/*
 * vsock-repro guest: dials the host on vsock port 5600 and echoes every byte
 * back until the host closes. Run by libkrun's init as the guest workload.
 *
 * A watchdog thread reports when the echo has made no progress for 3 s, with
 * the socket's SIOCOUTQ/SIOCINQ, so a stall shows which side the guest is
 * waiting on. In the deadlock it is blocked in write() with SIOCOUTQ=0: it has
 * no credit, and nothing is queued.
 *
 * It runs either under libkrun's init (a virtiofs root) or as the kernel's
 * init itself (an ext4 root), where it sets up /dev/console for its output
 * and reboots at the end; with reboot=k that ends the VM.
 *
 * Build static for the guest's architecture:
 *   gcc -static -O2 -pthread -o root/vsock-repro-guest guest.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/sockios.h>
#include <linux/vm_sockets.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define PORT 5600
#define BUF (4u << 20)

static volatile int phase; /* 0 idle, 1 in read(), 2 in write() */
static volatile int sfd = -1;
static volatile unsigned long long echoed_in, echoed_out;

static void *watchdog(void *arg)
{
	unsigned long long in = 0, out = 0;
	int still = 0;

	(void)arg;
	for (;;) {
		sleep(1);
		if (phase == 0 || echoed_in != in || echoed_out != out) {
			in = echoed_in;
			out = echoed_out;
			still = 0;
			continue;
		}
		if (++still == 3) {
			int unsent = -1, unread = -1;
			ioctl(sfd, SIOCOUTQ, &unsent);
			ioctl(sfd, SIOCINQ, &unread);
			fprintf(stderr,
				"vsock-repro guest: STUCK in %s() for 3 s: read %llu, written back %llu, "
				"SIOCOUTQ=%d SIOCINQ=%d\n",
				phase == 1 ? "read" : "write", in, out, unsent, unread);
		}
	}
	return NULL;
}

/* As PID 1 there is no stdio until /dev is there. */
static void as_init(void)
{
	mkdir("/dev", 0755);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL); /* may already be mounted */
	int fd = open("/dev/console", O_RDWR);
	if (fd >= 0) {
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}
}

static int finish(int code)
{
	if (getpid() == 1) {
		sync();
		reboot(RB_AUTOBOOT);
	}
	return code;
}

int main(void)
{
	struct sockaddr_vm a = { .svm_family = AF_VSOCK, .svm_cid = VMADDR_CID_HOST, .svm_port = PORT };
	char *buf = malloc(BUF);
	pthread_t w;
	int fd = -1;

	if (getpid() == 1)
		as_init();
	setvbuf(stderr, NULL, _IONBF, 0);
	for (int tries = 0; tries < 500 && fd < 0; tries++) {
		fd = socket(AF_VSOCK, SOCK_STREAM, 0);
		if (fd < 0) {
			perror("vsock-repro guest: socket");
			return finish(1);
		}
		if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
			close(fd);
			fd = -1;
			usleep(10000);
		}
	}
	if (fd < 0) {
		fprintf(stderr, "vsock-repro guest: cannot reach the host on port %d\n", PORT);
		return finish(1);
	}
	fprintf(stderr, "vsock-repro guest: connected, echoing\n");
	sfd = fd;
	pthread_create(&w, NULL, watchdog, NULL);

	for (;;) {
		phase = 1;
		ssize_t r = read(fd, buf, BUF);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		echoed_in += (unsigned long long)r;
		phase = 2;
		for (ssize_t off = 0; off < r;) {
			ssize_t n = write(fd, buf + off, (size_t)(r - off));
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				perror("vsock-repro guest: write");
				return finish(1);
			}
			off += n;
			echoed_out += (unsigned long long)n;
		}
	}
	phase = 0;
	fprintf(stderr, "vsock-repro guest: host closed, %llu bytes echoed\n", echoed_out);
	return finish(0);
}
