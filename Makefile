SHELL := /bin/bash

.PHONY: all r36-ultra j36-ultra devenv devenv32 clean clean_devenv clean_devenv32 clean_complete

DEBIAN_CODE_NAME ?= trixie
ENABLE_CACHE ?= y
BUILD_ARMHF ?= y
BUILD_BLUEALSA ?= y
BUILD_JOBS ?= 4

# The R36 Ultra bring-up build produces a plain Debian image with no front end on
# it.  Its native userspace defaults to armhf.  BUILD_BUNDLED_APPS used to select a
# second profile that built the emulators, the ports and their compatibility
# libraries; none of that source is in this tree any more, so there is one profile.
R36_USERSPACE_ARCH = $(if $(strip $(USERSPACE_ARCH)),$(USERSPACE_ARCH),armhf)

# Ensure system binaries like parted are in the path, and silence strict GCC warnings
PATH := $(PATH):/usr/sbin:/sbin
KCFLAGS := -w

export DEBIAN_CODE_NAME
export ENABLE_CACHE
export BUILD_ARMHF
export BUILD_BLUEALSA
export BUILD_JOBS
export PATH
export KCFLAGS

ifeq ($(DEBIAN_CODE_NAME),)
  $(error DEBIAN_CODE_NAME is not set. Please run with DEBIAN_CODE_NAME=suite (e.g., trixie))
endif

all:
	@echo "Please specify a build target: make r36-ultra (the base image) or make j36-ultra (that image with the J36 layer folded in)"

# ONE DEVICE FAMILY, TWO TARGETS.  There were twelve more -- a10mini, g350, miniloong,
# rgb10, rgb20pro, rgb30, rg351mp, rg353m, rg353v, rg503, rk2023 -- each calling its own
# ./build_<unit>.sh.  Not one of those scripts is in the tree; the targets outlived them
# and every one of them failed with "no such file or directory" after printing a banner
# and sleeping for five seconds.  The base image is still built as UNIT=rg351mp, because
# that is the RK3326 board the R36 Ultra's kernel and u-boot come from, but it is reached
# through r36-ultra and not through a target of its own.
r36-ultra:
	$(info MixOS R36 Ultra base will use Debian $(DEBIAN_CODE_NAME).)
	$(info parallel build jobs: $(BUILD_JOBS))
	$(info Debian userspace architecture: $(R36_USERSPACE_ARCH))
	env USERSPACE_ARCH="$(R36_USERSPACE_ARCH)" ./build-r36-ultra.sh

# The finished article: resumes the base build above and folds the MT6592 kernel, DTB,
# drivers and the mixdash dashboard into the same image.  macOS only -- it drives
# Multipass, which is where the ARMv7 workspace lives.
j36-ultra:
	$(info MixOS J36 Ultra will use Debian $(DEBIAN_CODE_NAME).)
	$(info parallel build jobs: $(BUILD_JOBS))
	$(info Debian userspace architecture: $(R36_USERSPACE_ARCH))
	env USERSPACE_ARCH="$(R36_USERSPACE_ARCH)" ./build-j36-ultra.sh

devenv:
	$(info MixOS will be built using the $(DEBIAN_CODE_NAME) release of Debian.)
	$(info debian building caching enabled? ${ENABLE_CACHE})
	@sleep 5
	./build_devenv.sh

devenv32:
	$(info MixOS will be built using the $(DEBIAN_CODE_NAME) release of Debian.)
	$(info debian building caching enabled? ${ENABLE_CACHE})
	@sleep 5
	./build_devenv.sh 32

clean:
	[ -d "mnt/boot" ] && sudo umount mnt/boot && sudo rm -rf mnt/boot || true
	[ -d "mnt/data" ] && sudo umount mnt/data && sudo rm -rf mnt/data || true
# mnt/roms was what finishing_touches.sh called that mount point before p3 became a home
# directory instead of a rom library.  It stays for one more release for the same reason
# the Arkbuild line below does: a tree interrupted mid-build by an older checkout still
# has it MOUNTED, and the wholesale `rm -rf mnt' further down would then be deleting
# through a live mount rather than a directory.  Unmounting first is the entire point of
# these two lines.
	[ -d "mnt/roms" ] && sudo umount mnt/roms && sudo rm -rf mnt/roms || true
	[ -d "main" ] && sudo rm -rf main || true
	[ -d "initrd" ] && sudo rm -rf initrd || true
	[ -f "wget-log" ] && sudo rm -f wget-log* || true
	source utils.sh && remove_mixosbuild && remove_mixosbuild32
# MixOS_*.img and not MixOS_*: the devenv trees are called MixOS_devenv and
# MixOS_devenv32, they have their own clean targets, and a build environment that took an
# hour to make is not something `make clean' should take with it.  The ArkOS_ glob stays
# for one more release -- a build root written before the rename is 3 GB or more and
# nothing else deletes it.  Arkbuild/Arkbuild32 are here for the same one release: a
# cached debootstrap tarball written before the rename unpacked itself into Arkbuild/,
# and until bootstrap_rootfs.sh was taught to name its destination that is exactly what
# a resumed build did -- so there are trees out there holding a couple of gigabytes that
# no other target has ever heard of.  (Comments in column 0 rather than after the tab:
# make ignores these, whereas a tab-indented one is handed to the shell and echoed.)
	sudo rm -rf MixOSBuild MixOSBuild32 MixOSBuild-final Arkbuild Arkbuild32 main mnt odroidgoA-4.4.y MixOS_*.img ArkOS_*.img rg351 wget-*
# -m 1 on BOTH greps.  Without it on the second, two matching loop devices come back as
# one newline-separated argument, losetup -d rejects it, the device stays attached, and
# the loop condition still matches -- so the while never ends.
	while losetup -a | grep -m 1 -E 'MixOS_|ArkOS_'; do sudo losetup -d "$$(losetup -a | grep -m 1 -E 'MixOS_|ArkOS_' | cut -d ':' -f 1)"; done
	@echo "Done!"

clean_devenv:
	./clean_mounts_devenv.sh
	sudo rm -rf MixOS_devenv/
	@echo "Done!"

clean_devenv32:
	./clean_mounts_devenv.sh 32
	sudo rm -rf MixOS_devenv32/
	@echo "Done!"

clean_complete: clean
	[ -d "$${PWD}/MixOSBuild_ccache" ] && sudo umount $${PWD}/MixOSBuild_ccache || true
	[ -d "$${PWD}/MixOSBuild_ccache" ] && sudo rm -rf MixOSBuild_ccache || true
	[ -d "$${PWD}/MixOSBuild_package_cache" ] && sudo rm -rf MixOSBuild_package_cache || true
	sudo rm -f build.log*
