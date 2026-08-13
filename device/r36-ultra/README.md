# R36 Ultra (RK3326) base build

```sh
./build-r36-ultra.sh
```

One native **armhf** Debian userspace, with no front end on it, on the existing
**arm64** RK3326 kernel and boot chain. On macOS it builds inside a persistent
Ubuntu 24.04 Multipass VM (`darkos-r36`); on Linux the same checkpointed build
runs directly.

## The architecture split, and why it is deliberate

| | value | asserted by |
| --- | --- | --- |
| kernel | arm64 (`Image`) | `verify_boot_kernel_arch` — arm64 magic at 0x38 |
| userspace | armhf, no foreign arch | `verify_native_userspace` — `dpkg --print-architecture` |

The mix is the point. The RK3326 is arm64 and its kernel has to be; the userspace
is 32-bit so the **same rootfs also runs on the ARMv7 J36 Ultra**. What is never
shared is the kernel: the MVII LK reads the arm64 boot magic and refuses this
`Image` on an MT6592 by design. A J36 card gets its own 32-bit `zImage` from
`device/j36-ultra/`; see that README.

## The BOOT partition

`install_boot.sh` owns partition 1 end to end, because it used to be a side
effect and that shipped an image with an all-zero BPB. It formats
`mkfs.vfat -F 32 -n BOOT`, installs the reference payload, then overwrites it
with the freshly built kernel payload, and leaves the partition mounted with
`LOOP_BOOT` exported so `finishing_touches.sh` can write `boot.ini` and detach it.

What ends up there:

```text
Image                    arm64 kernel        from BOOT_STASH (this build)
uInitrd                  initramfs           from BOOT_STASH (this build)
rk3326-r36s-linux.dtb    device tree         built if the tree has it, else reference
rk3326-rg351mp-linux.dtb                     built
rg351mp-uboot.dtb / rg351v-uboot.dtb         reference (u-boot panel variants)
boot.ini                 u-boot script       finishing_touches.sh
logo.bmp, charge bitmaps                     reference
```

`Image` and `uInitrd` are always this build's: a reference kernel would not match
the modules installed into this rootfs. The kernel payload is kept in
`$STATE_DIR/boot` rather than only inside the disk image, because `image_setup.sh`
recreates `$DISK` with a plain `dd` — the partition contents vanish while a
kernel checkpoint in the state directory survives.

### `DARKOS_R36_BOOT_PAYLOAD`

The BOOT partition of a working R36 image, which supplies what this
pipeline never produced: the R36S device tree, the u-boot panel variants, the
off-charging bitmaps and the dtb selector. It defaults to
`<artifacts>/Reference/BOOT`, and on macOS it **must** live under the artifact
directory — that is the only host path the build VM can read. Point it elsewhere
and you get a warning plus a boot partition holding only what the build made.

### `verify_boot.py`

Reads the MBR and the FAT of the finished image directly, without a loop device,
so it sees what is actually inside the archived artifact rather than what an
earlier stage believed it wrote:

```sh
python3 device/r36-ultra/verify_boot.py IMAGE --require Image --require uInitrd --require boot.ini
```

The resume runner runs it before trusting an existing archive. An image that
archived cleanly with an unformatted BOOT partition is now discarded and rebuilt
rather than released.

## Resuming, and what is not redone

Stages stamp `$STATE_DIR/<name>.done`, where `$STATE_DIR` is keyed by Debian
release, userspace architecture and profile. A failed build resumes at the first
incomplete stage; userspace components stamp individually, so a retry skips the
ones that finished.

Three things used to be redone anyway, and are not any more:

- **The VM's own apt setup** ran on every invocation of either wrapper.
  `darkos_vm_prepare_once` stamps it inside the VM instead
  (`DARKOS_FORCE_HOST_PREP=1` to run it anyway).
- **Debian, unpacked twice.** `clean_mounts.sh` deletes `$FILESYSTEM` at the end
  of a successful run to give back the working copy's disk, which meant the next
  build from a changed profile or a deleted image had to debootstrap and
  reinstall every chroot build dependency from scratch. The rootfs is now copied
  once immediately before finalization — the last moment it is still a usable
  build root, since `cleanup_filesystem.sh` strips it and `write_rootfs.sh`
  shrinks it — and a later run whose `$FILESYSTEM` is gone restores that copy and
  keeps its bootstrap/userspace checkpoints. `DARKOS_R36_SNAPSHOT_ROOTFS=0`
  disables it; it is skipped automatically when the disk cannot spare the space.
- **The kernel.** A kernel checkpoint means the kernel was compiled, not that its
  output still exists. `boot_stash_ready` checks, and a checkpoint with no payload
  behind it is cleared rather than believed.

The copy is crash-consistent rather than quiesced: `MixOSBuild` is still mounted, so
it is synced and then copied. That is what btrfs' log replay is for, and a build
root is not a database.

## Overrides

```sh
BUILD_JOBS=8 ./build-r36-ultra.sh
USERSPACE_ARCH=arm64 ./build-r36-ultra.sh
DARKOS_VM_CPUS=4 DARKOS_VM_MEMORY=8G ./build-r36-ultra.sh
```

`BUILD_BUNDLED_APPS` used to pick between this image and one carrying the
emulators, the ports and their compatibility libraries.  That source is out of
the tree, so there is one profile and the variable is ignored if it is set.

## The one artifact

A build produces exactly one file, uncompressed and named after the commit it was
built from:

```
MixOS_<arch>_<debian codename>_<commit>.img
```

`dd` it and go. There is no `.7z` any more — the volumes were split, CRC'd, copied,
verified by decompressing all 8 GiB again, and then unpacked by hand before every
flash, all so a file could cross a local mount slightly smaller. `DARKOS_COPY_RAW_IMAGE`
went with them: the raw image is not an extra, it is the deliverable.

The date in the name went too. It answered the wrong question — two cards flashed a
fortnight apart from identical sources got different names, and two flashed the same
afternoon from different commits got the same one. The commit id is also what the boot
image and the dashboard stamp themselves with, so a running card can be matched to the
build that made it (`/etc/j36-build`).

Because the raw image is the deliverable, an oversized OS partition is no longer free:
every unused megabyte of it is a megabyte of zeros in the file, on the card, and in
every copy either of them travels in. So `STORAGE_SIZE` in `build-in-vm.sh` is sized by
the rootfs and not by a guess — the number `write_rootfs.sh` prints ("Root filesystem
shrank to N MB"), rounded up to the next whole 1000 MB. At 3384 MB of rootfs that is
4000, and the image is 4417 MB rather than the 7917 MB the old flat 7500 produced. If
the rootfs ever passes it, `write_rootfs.sh` stops the build and names the value to set
rather than dd'ing p2 over p3.
