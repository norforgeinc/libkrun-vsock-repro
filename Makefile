# vsock-repro needs libkrun main (the builder API: krun_vmm_builder_*,
# libkrun_init). Either it is installed, or:
#
#   make libkrun-main   # clone upstream main here, unpatched, and build it
#   make                # then picks up ./libkrun-main automatically
#
# A branch or tag to build instead of main, e.g. one carrying a fix:
#
#   make libkrun-main LIBKRUN_URL=https://github.com/<you>/libkrun.git LIBKRUN_REF=<branch>
#
# or point at any libkrun tree you have built: make LIBKRUN=~/src/libkrun
#
# libkrun's default guest kernel comes from libkrunfw (libkrunfw.so.5). If it
# is not installed (Linux):
#
#   make libkrunfw                          # libkrunfw main, its own kernel
#   make libkrunfw LIBKRUNFW_KERNEL=6.12.115   # same patches and config, this kernel
#
# and `make` links vsock-repro so libkrun finds ./libkrunfw at run time.
#
# The guest program must be a static Linux binary for the guest's
# architecture. On a Linux host that is the host compiler; on macOS set
# GUEST_CC to a cross compiler, e.g. GUEST_CC="zig cc -target aarch64-linux-musl".

LIBKRUN ?= $(wildcard libkrun-main)
LIBKRUN_URL ?= https://github.com/containers/libkrun.git
LIBKRUN_REF ?=
LIBKRUNFW ?= $(wildcard libkrunfw)
LIBKRUNFW_URL ?= https://github.com/containers/libkrunfw.git
LIBKRUNFW_KERNEL ?=
GUEST_CC ?= $(CC)
CFLAGS ?= -O2 -g -Wall
ifneq ($(LIBKRUN),)
LIBDIR := $(abspath $(LIBKRUN))/target/release
CPPFLAGS += -I$(LIBKRUN)/include
LDFLAGS += -L$(LIBDIR) -Wl,-rpath,$(LIBDIR)
endif
# libkrun dlopen()s libkrunfw.so.5. A DT_RPATH on the executable (unlike the
# default DT_RUNPATH) is also searched for that, so no LD_LIBRARY_PATH.
ifneq ($(LIBKRUNFW),)
LDFLAGS += -Wl,--disable-new-dtags -Wl,-rpath,$(abspath $(LIBKRUNFW))
endif

all: vsock-repro root/vsock-repro-guest

vsock-repro: repro.c
ifneq ($(LIBKRUN),)
	@test -e $(LIBDIR)/libkrun_init.so -o -e $(LIBDIR)/libkrun_init.dylib || { \
	  echo "$(LIBKRUN): libkrun is not fully built (no libkrun_init in $(LIBDIR))."; \
	  echo "Run 'make libkrun-main' (or 'make -C $(LIBKRUN) BLK=1') and look for the error."; \
	  exit 1; }
endif
	$(CC) $(CFLAGS) $(CPPFLAGS) -pthread -o $@ $< $(LDFLAGS) -lkrun -lkrun_init
ifeq ($(shell uname -s),Darwin)
	codesign --entitlements entitlements.plist --force -s - $@
endif

root/vsock-repro-guest: guest.c
	mkdir -p root
	$(GUEST_CC) -static -O2 -Wall -pthread -o $@ $<

# The same program as the kernel's init on an ext4 disk, for a kernel without
# virtiofs (Linux host: mkfs.ext4 -d).
root.ext4: root/vsock-repro-guest
	mkfs.ext4 -q -F -d root $@ 32M

# Upstream libkrun main, unpatched. BLK=1 for the ext4-image root (block
# devices are opt-in upstream). The library's SONAME is libkrun.so.2 and
# `make` there leaves only libkrun.so and libkrun.so.2.0.0, so link the name
# the loader looks for.
# The clone and the build are separate steps, so a build that failed (a
# missing Rust musl target, say) is retried by the next `make libkrun-main`
# instead of being taken for done.
libkrun-main: libkrun-main/.built

libkrun-main/.git:
	git clone --depth 1 $(if $(LIBKRUN_REF),--branch $(LIBKRUN_REF)) $(LIBKRUN_URL) libkrun-main

libkrun-main/.built: | libkrun-main/.git
	$(MAKE) -C libkrun-main BLK=1
ifeq ($(shell uname -s),Darwin)
	ln -sf libkrun.dylib libkrun-main/target/release/libkrun.2.dylib
else
	ln -sf libkrun.so libkrun-main/target/release/libkrun.so.2
endif
	touch $@

# libkrunfw: the kernel libkrun boots by default, with libkrunfw's patches and
# config. LIBKRUNFW_KERNEL builds another kernel.org version with them (the
# patches must still apply). The stamp names the version, so changing it
# rebuilds; the last build is the one in libkrunfw.so.5.
LIBKRUNFW_STAMP = libkrunfw/.built-$(or $(LIBKRUNFW_KERNEL),default)

libkrunfw: $(LIBKRUNFW_STAMP)

libkrunfw/.git:
	git clone --depth 1 $(LIBKRUNFW_URL) libkrunfw

$(LIBKRUNFW_STAMP): | libkrunfw/.git
	rm -f libkrunfw/.built-*
	$(MAKE) -C libkrunfw $(if $(LIBKRUNFW_KERNEL),KERNEL_VERSION=linux-$(LIBKRUNFW_KERNEL))
	cd libkrunfw && ln -sf "$$(ls libkrunfw.so.5.*.* | head -1)" libkrunfw.so.5
	touch $@

clean:
	rm -rf vsock-repro root root.ext4 vsock-repro.log

.PHONY: all clean libkrun-main libkrunfw
