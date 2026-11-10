# vsock-repro: libkrun vsock deadlock under heavy two-way traffic

A standalone reproducer for libkrun upstream that uses only libkrun main's
public C API.

## The setup

- **Port:** one guest-initiated vsock port (5600), mapped to a host Unix
  socket with `krun_vsock_device_add_unix_port(..., listen = false)`.
- **No TSI.**
- **Guest:** `vsock-repro-guest` connects and echoes every byte back. It
  runs in one of two ways:
  - from a directory root shared over virtiofs, started by libkrun's init.
    This works with libkrunfw's kernel.
  - as the kernel's init on an ext4 disk. This works with any kernel that has
    virtio-blk and ext4.
- **Host:** a thread in the same process pushes large messages through the
  connection, both ways at once. It runs two tests on one connection:
  1. **Round trips:** 4000 of them, each sending one 1 MiB message and reading
     all of it back.
  2. **Pipelined echo:** 256 KiB messages, 16 in flight, 8 GiB in total.

The host interleaves reads and writes with `poll()`, so it never blocks either
direction itself. If neither direction moves for 10 s, the run reports a stall
and exits with status 3. Exit status 0 means no stall.

## Build and run

Needs libkrun main, which has the builder API (`krun_vmm_builder_*`,
`libkrun_init`). Released 1.x versions don't have it. If libkrun main isn't
installed, have the Makefile fetch and build it, unpatched, into
`./libkrun-main`:

```sh
make libkrun-main         # git clone --depth 1 + make; needs Rust and the musl target
make                      # vsock-repro, root/vsock-repro-guest
```

For a libkrun tree you've already built, run `make LIBKRUN=/path/to/libkrun`.

To build a branch or tag instead of main, for example one with a fix, pass
`LIBKRUN_URL` and `LIBKRUN_REF` to `make libkrun-main`. `libkrun-main` is
cloned once, so remove it first to switch:

```sh
rm -rf libkrun-main
make libkrun-main LIBKRUN_URL=https://github.com/<you>/libkrun.git LIBKRUN_REF=<branch>
rm -f vsock-repro && make
```

Then pick a guest kernel:

```sh
# libkrunfw's kernel (needs libkrunfw installed)
./vsock-repro root

# any plain kernel, with the guest program as init on an ext4 disk (Linux host)
make root.ext4
./vsock-repro --kernel /path/to/vmlinux root.ext4
```

A run takes about a minute when it doesn't stall.

- **macOS:** the guest program must be a static Linux aarch64 binary. Build it
  with `make GUEST_CC="zig cc -target aarch64-linux-musl"` or something
  equivalent. The launcher is codesigned with the hypervisor entitlement. If
  libkrun isn't installed, run with
  `DYLD_LIBRARY_PATH=libkrun-main/target/release`.

### Options

| Option | Effect |
|---|---|
| `--vcpus N` | number of guest vCPUs (default 2) |
| `--mem MiB` | guest memory (default 1024) |
| `--trips N` | number of round trips (default 4000) |
| `--msg BYTES` | round-trip message size |
| `--pipe-gib N` | pipelined echo volume; `0` skips it |
| `--kernel PATH` | boot this kernel instead of libkrunfw's. It needs virtio-vsock, plus virtiofs for a directory root or virtio-blk and ext4 for a disk root |
| `--log PATH` | where the guest console and libkrun's warnings go |

Set `STALL_HOLD=600` to keep the VM up after a stall, so you can inspect it.

## What a stall looks like

On stdout:

```
round trips, 1024 KiB: STALLED in message N of 4000: host sent X bytes, got Y back; nothing moved for 10 s

DEADLOCK: both directions idle. ...
```

In `vsock-repro.log`:

```
vsock-repro guest: STUCK in write() for 3 s: read ..., written back ..., SIOCOUTQ=0 SIOCINQ=0
```

The guest is blocked in `write()` with nothing queued, meaning it has no
credit. No thread is blocked, and both vCPUs are idle.

## What we saw

With this program, on 2026-11-10:

- **libkrun:** upstream main at c9327bd, unpatched, built with `BLK=1`.
- **Guest:** plain Linux kernels from kernel.org with no libkrunfw patches:
  x86_64 `defconfig` with a guest config fragment merged in (virtio-mmio,
  virtio-vsock, virtio-blk, ext4, the virtio console), without debug info. Each had 2 vCPUs and 1 GiB, and booted
  from an ext4 image.
- **Host:** x86_64 under nested KVM (a VM inside a VM), 8 vCPUs.

| guest kernel | libkrun | result |
|---|---|---|
| 6.12.112 | main | stalled at round trip 24 |
| 6.12.115 | main | stalled 3 of 3 runs, at round trips 237, 316 and 109 |
| 6.12.115 | main + fix 1 | 2 of 2 runs passed: 4000 round trips plus 8 GiB of pipelined echo |
| 6.12.112 | main + fix 1 | 1 of 1 run passed: 4000 round trips plus 8 GiB of pipelined echo |
| libkrunfw (main, e552102) built with 6.12.115 | main | stalled 3 of 3 runs, at round trips 68, 51 and 115 |
| libkrunfw (main, e552102) built with 6.12.115 | main + fix 1 | stalled 2 of 10 runs of round trips (and 1 of 2 full runs) |
| libkrunfw (main, e552102) built with 6.12.115 | main + fixes 1 and 2 | 20 of 20 runs of round trips passed, and 5 of 5 full runs |
| 6.12.115 | main + fixes 1 and 2 | 20 of 20 runs of round trips passed, and 5 of 5 full runs |
| 6.12.112 | main + fixes 1 and 2 | 5 of 5 full runs passed |

On every stall the guest log shows `STUCK in write() ... SIOCOUTQ=0`. (On the
plain kernels the `SIOCINQ=-1` beside it means that ioctl failed on the vsock
socket. libkrunfw's patches add it.)

The libkrunfw rows boot the default way, with `./vsock-repro root`: a virtiofs
root and libkrun's init. They use libkrunfw's own patches and config on
kernel.org 6.12.115 (`make libkrunfw LIBKRUNFW_KERNEL=6.12.115`).

A full run is 4000 round trips plus 8 GiB of pipelined echo. The two fixes:

1. **Keep `rxq` in order.** `drain_rxq()` puts an undeliverable packet back at
   the head, new packets queue behind parked ones, and parked credit packets
   for a connection are merged up to the newest `fwd_cnt`. This alone left
   the libkrunfw stalls above.
2. **Never deliver an older `fwd_cnt` than data already carried.** Data
   packets skip `rxq` and carry the current count. A credit update parked in
   `rxq` meanwhile still holds the count from when it was made, so it can
   reach the guest after newer data and move its view back. Logging showed
   that happening 27 to 295 times per run with only fix 1, and in 572 of 579
   cases it was a parked credit update arriving after a data packet. `rxq`
   now remembers the newest count sent to the guest on each connection, and
   raises a credit packet to it on delivery.

Throughput with the fixes was 420-550 MB/s each way under nested KVM.

We first saw the stall on bare metal (x86_64 i7-1185G7, host kernel 7.2.6)
with libkrun main at a4d501ef and a 6.12.112 guest. There a 1 MiB echo loop
of the same shape stalled on every run, after 62 to 334 round trips.

Timing differs between machines and guest kernels, so a run that doesn't stall
doesn't by itself mean the bug is gone. Try several runs, or `--vcpus 4`.

## A check that doesn't depend on the guest

Log the `fwd_cnt` of every packet that carries credit, per connection, as
libkrun writes it into the guest's RX queue. Flag any value lower than the one
delivered before it for the same connection. Without a fix, this shows
`fwd_cnt` going backwards well before any stall.

## The mechanism

1. Host → guest data uses up the guest's RX descriptors, so `push_packet()`
   parks control packets, including credit updates, in `MuxerRxQ`.
2. `drain_rxq()` puts a packet it cannot deliver back at the tail. With two
   credit updates for the same connection parked, their order flips on every
   retry, and the older one can arrive last.
3. Each update carries an absolute `fwd_cnt`. Linux keeps the last one it
   receives, so its view of libkrun's buffer moves backwards.
4. Linux limits its send credit to its own `buf_alloc` (256 KiB by default),
   not libkrun's 8 MiB. Falling back by two updates leaves it with zero credit.
5. libkrun sends the next update only after forwarding `peer_buf_alloc / 2`
   more bytes, which the blocked guest never sends. No host → guest data is
   left to carry a fresh `fwd_cnt` either.
