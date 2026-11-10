/*
 * vsock-repro: reproduces a libkrun vsock deadlock under heavy two-way
 * traffic on one guest-initiated unix-socket port.
 *
 * One process: it listens on a unix socket, maps guest vsock port 5600 to it
 * (krun_vsock_device_add_unix_port, listen=false, no TSI), boots the guest
 * with `vsock-repro-guest` as its workload, and echoes large messages through
 * the guest from a host thread:
 *
 *   1. N round trips of one 1 MiB message (send it, read it all back, repeat);
 *   2. pipelined echo: 256 KiB messages with 16 in flight, 8 GiB in total.
 *
 * Reading and writing are interleaved with poll() on the host, so the host
 * itself never blocks a direction. If neither direction moves for 10 s that
 * is the stall: it is reported and the process exits 3. Exit 0 means no stall.
 *
 *   vsock-repro [options] ROOT
 *
 * ROOT holds vsock-repro-guest (static) and is either
 *   - a directory: shared as the guest's root over virtiofs, and libkrun's
 *     init runs the program (libkrunfw, or a --kernel with virtiofs); or
 *   - an ext4 image: the guest's /dev/vda, with the program as the kernel's
 *     init (needs --kernel; any kernel with virtio-blk and ext4).
 * See README.md.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libkrun.h>
#include <libkrun_init.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PORT 5600
#define STALL_SECS 10

#if defined(__x86_64__)
#define KERNEL_FORMAT KRUN_KERNEL_FORMAT_ELF
#else
#define KERNEL_FORMAT KRUN_KERNEL_FORMAT_RAW
#endif

static struct {
	const char *root, *kernel, *log;
	int vcpus, mem;
	uint32_t msg;
	uint64_t trips;
	uint32_t pipe_msg;
	int window;
	uint64_t pipe_bytes;
} opt = {
	.log = "vsock-repro.log",
	.vcpus = 2,
	.mem = 1024,
	.msg = 1u << 20,
	.trips = 4000,
	.pipe_msg = 256u << 10,
	.window = 16,
	.pipe_bytes = 8ull << 30,
};

static int lfd;
static char sock_path[108];

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/*
 * Echo `count` messages of `msg` bytes through the guest, keeping at most
 * `window` messages outstanding (window 1 is a plain round trip). Returns 0,
 * or -1 after a stall.
 */
static int echo(int fd, const char *name, uint32_t msg, int window, uint64_t count)
{
	uint64_t total = (uint64_t)msg * count, sent = 0, got = 0;
	char *wbuf = malloc(msg), *rbuf = malloc(4u << 20);
	double t0 = now(), last = t0;

	memset(wbuf, 0x5a, msg);
	while (got < total) {
		uint64_t limit = (got / msg + (uint64_t)window) * msg;
		if (limit > total)
			limit = total;

		struct pollfd p = { .fd = fd, .events = POLLIN | (sent < limit ? POLLOUT : 0) };
		if (poll(&p, 1, 1000) < 0 && errno != EINTR) {
			perror("poll");
			exit(1);
		}
		if (p.revents & POLLOUT) {
			uint64_t want = limit - sent, room = msg - sent % msg;
			ssize_t n = write(fd, wbuf + sent % msg, want < room ? want : room);
			if (n > 0) {
				sent += (uint64_t)n;
				last = now();
			} else if (n < 0 && errno != EAGAIN && errno != EINTR) {
				perror("write");
				exit(1);
			}
		}
		if (p.revents & (POLLIN | POLLHUP | POLLERR)) {
			uint64_t want = total - got;
			ssize_t n = read(fd, rbuf, want < (4u << 20) ? want : (4u << 20));
			if (n > 0) {
				got += (uint64_t)n;
				last = now();
			} else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
				fprintf(stderr, "%s: the guest closed the connection (see %s)\n", name, opt.log);
				exit(1);
			}
		}
		if (now() - last > STALL_SECS) {
			printf("%s: STALLED in message %llu of %llu: host sent %llu bytes, got %llu back; "
			       "nothing moved for %d s\n",
			       name, (unsigned long long)(got / msg + 1), (unsigned long long)count,
			       (unsigned long long)sent, (unsigned long long)got, STALL_SECS);
			return -1;
		}
	}
	double secs = now() - t0;
	printf("%s: ok, %llu MiB each way in %.1f s (%.0f MB/s)\n", name, (unsigned long long)(total >> 20), secs,
	       (double)total / secs / 1e6);
	free(wbuf);
	free(rbuf);
	return 0;
}

static void *host_side(void *arg)
{
	struct pollfd p = { .fd = lfd, .events = POLLIN };
	int fd, sz = 8 << 20;
	char name[64];

	(void)arg;
	if (poll(&p, 1, 120000) != 1) {
		fprintf(stderr, "vsock-repro: no guest connection within 120 s (see %s)\n", opt.log);
		_exit(1);
	}
	fd = accept(lfd, NULL, NULL);
	if (fd < 0) {
		perror("accept");
		_exit(1);
	}
	unlink(sock_path);
	/* Keep the host's socket buffers out of the picture. */
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	printf("vsock-repro: guest connected\n");
	fflush(stdout);

	snprintf(name, sizeof name, "round trips, %u KiB", opt.msg >> 10);
	int stalled = echo(fd, name, opt.msg, 1, opt.trips) < 0;
	if (!stalled && opt.pipe_bytes) {
		snprintf(name, sizeof name, "pipelined echo, %u KiB x %d in flight", opt.pipe_msg >> 10, opt.window);
		stalled = echo(fd, name, opt.pipe_msg, opt.window, opt.pipe_bytes / opt.pipe_msg) < 0;
	}
	if (stalled) {
		printf("\nDEADLOCK: both directions idle. The guest's view is in %s (\"STUCK in write()\" with "
		       "SIOCOUTQ=0 is the credit stall).\n",
		       opt.log);
		/* STALL_HOLD=secs: keep the VM up, to inspect it. */
		if (getenv("STALL_HOLD"))
			sleep((unsigned)atoi(getenv("STALL_HOLD")));
	} else {
		printf("\nno stall\n");
	}
	fflush(stdout);
	_exit(stalled ? 3 : 0);
}

static bool push_to_stderr(void *userdata, KrunStr s)
{
	(void)userdata;
	fwrite(s.data, 1, s.len, stderr);
	return true;
}

/* As upstream's examples/chroot_vm.c does it on main (c9327bd). */
static KrunPushStrVtable krun_stderr_vtable = KRUN_VTABLE(KrunPushStrVtable, .drop = NULL, .push = push_to_stderr);
static KrunVtableHandle krun_stderr_writer = KRUN_TEMP_OBJ(KRUN_PUSH_STR_TYPE_TAG, &krun_stderr_vtable, NULL);
static KrunInitPushStrVtable init_stderr_vtable =
	KRUN_VTABLE(KrunInitPushStrVtable, .drop = NULL, .push = push_to_stderr);
static KrunVtableHandle init_stderr_writer = KRUN_TEMP_OBJ(KRUN_INIT_PUSH_STR_TYPE_TAG, &init_stderr_vtable, NULL);

#define CHECK(call)                                                      \
	krun_err = NULL;                                                 \
	call;                                                            \
	if (krun_err) {                                                  \
		fprintf(stderr, "%s failed: ", #call);                   \
		krun_error_message(krun_err, &krun_stderr_writer);       \
		fputc('\n', stderr);                                     \
		return 1;                                                \
	}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [options] ROOT    (a directory, or an ext4 image with --kernel)\n"
		"  --kernel PATH     boot this kernel instead of libkrunfw's\n"
		"  --vcpus N         guest vCPUs (default 2)\n"
		"  --mem MiB         guest memory (default 1024)\n"
		"  --trips N         1 MiB round trips (default 4000)\n"
		"  --msg BYTES       round-trip message size (default 1048576)\n"
		"  --pipe-gib N      pipelined echo volume, 0 to skip (default 8)\n"
		"  --log PATH        guest console and libkrun log (default vsock-repro.log)\n",
		argv0);
	exit(2);
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "kernel", required_argument, NULL, 'k' }, { "vcpus", required_argument, NULL, 'c' },
		{ "mem", required_argument, NULL, 'm' },    { "trips", required_argument, NULL, 'n' },
		{ "msg", required_argument, NULL, 's' },    { "pipe-gib", required_argument, NULL, 'p' },
		{ "log", required_argument, NULL, 'l' },    { NULL, 0, NULL, 0 },
	};
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	KrunError krun_err;
	pthread_t host;
	int c;

	while ((c = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
		switch (c) {
		case 'k': opt.kernel = optarg; break;
		case 'c': opt.vcpus = atoi(optarg); break;
		case 'm': opt.mem = atoi(optarg); break;
		case 'n': opt.trips = strtoull(optarg, NULL, 0); break;
		case 's': opt.msg = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'p': opt.pipe_bytes = strtoull(optarg, NULL, 0) << 30; break;
		case 'l': opt.log = optarg; break;
		default: usage(argv[0]);
		}
	}
	if (optind != argc - 1 || opt.msg == 0 || opt.msg > (4u << 20))
		usage(argv[0]);
	opt.root = argv[optind];

	struct stat st;
	if (stat(opt.root, &st) < 0) {
		perror(opt.root);
		return 1;
	}
	bool disk = !S_ISDIR(st.st_mode);
	if (disk && !opt.kernel) {
		fprintf(stderr, "%s is not a directory: an ext4 image root needs --kernel\n", opt.root);
		return 2;
	}

	/* The host end of guest port 5600. */
	snprintf(sock_path, sizeof sock_path, "%s/vsock-repro-%d.sock", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
		 (int)getpid());
	snprintf(a.sun_path, sizeof a.sun_path, "%s", sock_path);
	unlink(sock_path);
	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 1) < 0) {
		perror(sock_path);
		return 1;
	}

	int logfd = open(opt.log, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
	int nullfd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (logfd < 0 || nullfd < 0) {
		perror(opt.log);
		return 1;
	}
	/* libkrun's warnings (the vsock muxer's among them) go to the log too. */
	CHECK(krun_init_log(logfd, KRUN_LOG_LEVEL_WARN, KRUN_LOG_STYLE_NEVER, 0, &krun_err));

	KrunPayload payload;
	if (disk) {
		/* The program is PID 1 on an ext4 root. */
		CHECK(payload = krun_payload_load_external(
			      KRUN_STR(opt.kernel), KERNEL_FORMAT, KRUN_STR(NULL) /* no initrd */,
			      KRUN_STR("reboot=k panic=-1 console=hvc0 root=/dev/vda rw init=/vsock-repro-guest"),
			      &krun_err));
	} else if (opt.kernel) {
		CHECK(payload = krun_payload_load_external(KRUN_STR(opt.kernel), KERNEL_FORMAT, KRUN_STR(NULL) /* no initrd */,
							   KRUN_STR("reboot=k panic=-1 console=hvc0 rootfstype=virtiofs rw"),
							   &krun_err));
	} else {
		krun_err = NULL;
		payload = krun_payload_load_krunfw(&krun_err);
		if (krun_err) {
			fprintf(stderr, "cannot load libkrunfw's kernel: ");
			krun_error_message(krun_err, &krun_stderr_writer);
			fprintf(stderr, "\nlibkrun main loads libkrunfw.so.5 (libkrunfw 5.x) from the library path: install it,\n"
					"or point LD_LIBRARY_PATH (DYLD_LIBRARY_PATH on macOS) at a built one, or use --kernel.\n");
			return 1;
		}
	}

	/* With a directory root, libkrun's init runs the guest program. */
	KrunFsOverlay overlay = krun_fs_overlay_new();
	if (!disk) {
		KrunInitError err = NULL;
		KrunInitBuilder b = krun_init_config_builder();
		krun_init_builder_arg(&b, KRUN_STR("/vsock-repro-guest"));
		krun_init_builder_workdir(&b, KRUN_STR("/"));
		KrunInitConfig config = krun_init_builder_build(&b);
		krun_init_config_apply(config, overlay, payload, &err);
		if (err) {
			fprintf(stderr, "krun_init_config_apply failed: ");
			krun_init_error_message(err, &init_stderr_writer);
			fputc('\n', stderr);
			return 1;
		}
	}

	KrunMmioDeviceManager devices = krun_mmio_device_manager_new();
	{
		KrunConsoleBuilder cb = krun_console_device_builder();
		CHECK(krun_console_builder_add_default_console(cb, nullfd, logfd, logfd, &krun_err));
		KrunConsoleDevice console;
		CHECK(console = krun_console_builder_build(cb, &krun_err));
		krun_mmio_device_manager_add(devices, console);
	}
	if (disk) {
		KrunBlockDevice blk;
		CHECK(blk = krun_block_device_new(KRUN_STR("root"), KRUN_STR(opt.root), KRUN_DISK_FORMAT_RAW, &krun_err));
		krun_mmio_device_manager_add(devices, blk);
	} else {
		KrunFsDevice rootfs;
		CHECK(rootfs = krun_fs_device_new(KRUN_STR("/dev/root"), KRUN_STR(opt.root), &krun_err));
		krun_fs_device_set_overlay(rootfs, overlay);
		krun_mmio_device_manager_add(devices, rootfs);
	}
	{
		/* No TSI: a plain vsock port mapped to a host unix socket, guest-initiated. */
		KrunVsockDevice vsock;
		CHECK(vsock = krun_vsock_device_new(3, 0, &krun_err));
		const char *path = sock_path;
		krun_vsock_device_add_unix_port(vsock, PORT, KRUN_STR(path), false);
		krun_mmio_device_manager_add(devices, vsock);
	}

	KrunVmmBuilder builder = krun_vmm_builder_new();
	CHECK(krun_vmm_builder_vcpus(&builder, (uint8_t)opt.vcpus, &krun_err));
	CHECK(krun_vmm_builder_ram_mib(&builder, (uint32_t)opt.mem, &krun_err));
	krun_vmm_builder_payload(&builder, payload);
	krun_vmm_builder_devices(&builder, devices);
	KrunVmm vmm;
	CHECK(vmm = krun_vmm_builder_build(&builder, &krun_err));

	printf("vsock-repro: %d vCPU, %d MiB, %s kernel; guest log in %s\n", opt.vcpus, opt.mem,
	       opt.kernel ? opt.kernel : "libkrunfw", opt.log);
	fflush(stdout);
	pthread_create(&host, NULL, host_side, NULL);
	krun_vmm_run(vmm); /* never returns: the host thread exits the process */
	return 0;
}
