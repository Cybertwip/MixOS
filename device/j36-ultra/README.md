# J36 Ultra MT6592 bring-up DTB

Build from the repository root:

```sh
./build-j36-ultra-dtb.sh
```

The generator reads only the canonical MVII J36 Ultra driver directory:

```text
PowerEngine/OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers
```

It extracts the board constants and the exact compact 155-record JD9365 table
from `mt6592_board_j36.h`, `panel_bringup.h`, `mt6592_disp_hw.h`,
`mt6592_keys.c`, and `dsi_drv.c`.

Outputs:

```text
device/j36-ultra/generated/mt6592-j36-ultra.dts
device/j36-ultra/generated/mt6592-j36-ultra.dtb
device/j36-ultra/generated/mt6592-j36-ultra.roundtrip.dts
```

This is a **bring-up hardware description**, not a complete Linux port. It
contains the panel timing/init records, power sequence, framebuffer handoff,
GPIO keys, keypad matrix, and AUXADC joystick wiring. Linux still needs the
small MT6592 adapter drivers named by the `j36,*` compatible strings before the
DTB can execute panel initialization or report input events.

## Boot-chain integration

The stock J36 Ultra Android `boot.img` format does not carry a separate DTB.
For the first Linux bring-up, keep the stock LK and build the ARM kernel with:

```text
CONFIG_OF=y
CONFIG_ARM_APPENDED_DTB=y
CONFIG_ARM_ATAG_DTB_COMPAT=y
CONFIG_ARM_ATAG_DTB_COMPAT_CMDLINE_FROM_BOOTLOADER=y
```

Then append this DTB to the ARM `zImage` before wrapping it as the MediaTek
`boot.img` kernel payload:

```sh
cat arch/arm/boot/zImage \
    device/j36-ultra/generated/mt6592-j36-ultra.dtb \
    > zImage-j36-ultra
```

That preserves the stock LK/ATAG handoff while the kernel discovers the new
board description. A later custom LK can instead pass the DTB address in ARM
register `r2`.

## Minimum-driver strategy

For the first visible Linux boot, preserve the stock LK display state and use
the `simple-framebuffer` node at `0x82700000`; do not reset DSI/MMSYS yet. The
full JD9365 program remains in the DTB for the later native panel adapter.

The first input layer is implemented by `linux/j36_mt6592_input.c` as one
minimal polled platform driver. It consumes the DTB's
`j36,j36-ultra-input` node and covers:

1. GPIO DIN for the D-pad, stick clicks, and special key, with a pull-up armed so
   "active low" has a defined idle to be low against.
2. KPD scan memory for face/shoulder/start/select/menu/volume keys.
3. AUXADC channels 15, 14, 12, and 13 for both analog sticks.

It also does the two things without which the matrix half of that list is dead:
it muxes the three keypad pads the boot chain leaves parked (KPROW3 pad 11 at
mode 3, KPCOL3 pad 12 at mode 3, KPCOL4 pad 2 at mode 6, from the DT keypad
node), and it ungates the keypad's 32 kHz clock in the MT6323 PMIC over PWRAP.
Both failures look identical from the SoC side -- scan memories reading their
idle all-ones pattern -- so both were chased in the keymap first. Pad mux writes
are confined to a pad whose mode is measurably wrong, and every one is logged
with its before and after. See `linux/README.md`.

It touches no MMSYS, DSI, MIPI-TX, panel, or backlight register.

## 32-bit, and where that is enforced

An MT6592 is ARMv7. An RK3326 is arm64. The two boot chains in this repository
now share an SD card, so neither architecture is left to memory:

| | R36 Ultra (RK3326) | J36 Ultra (MT6592) |
| --- | --- | --- |
| kernel | arm64 `Image` | ARMv7 `zImage` |
| toolchain | `aarch64-linux-gnu-` | `arm-linux-gnueabihf-` |
| userspace | armhf (32-bit) | armhf (32-bit), shared |

The rootfs is shared; the kernel never is. `build-r36-ultra.sh` asserts both
halves — `verify_native_userspace` for the armhf rootfs, `verify_gui_architecture`
for an ELF32/ARM EmulationStation, and `verify_boot_kernel_arch` for the arm64
boot magic in its own kernel. `device/j36-ultra/build-in-vm.sh` runs the MVII LK's
own test on its own artifact: `readelf` for ELF32/ARM, then the arm64 magic at
offset 0x38 and the zImage magic at 0x24, on the exact bytes the LK will read.
That check is the LK's `sd_kernel_is_armv7()`, moved to the build machine so "no"
is a build failure instead of a board that quietly fell back to the eMMC.

## Two kernel payloads, and they are not interchangeable

The **eMMC BOOTIMG** path is entered the way the stock LK enters a kernel: an
ATAG list in `r2` and no device tree anywhere. That payload
(`zImage-j36-ultra`) therefore carries its tree **appended**, and
`CONFIG_ARM_ATAG_DTB_COMPAT` folds the ATAGs into it.

The **SD hand-off** path passes the tree itself in `r2` and patches `/chosen`
first: `bootargs`, `linux,initrd-start`, `linux,initrd-end`. An appended tree
would silently win that argument — `arch/arm/boot/compressed/head.S` takes the
appended DTB whenever its magic is present and consults `r2` only to fold ATAGs
in, so a DTB in `r2` is discarded along with the initramfs range, and the kernel
boots and then cannot find `/init`. The SD payload is therefore the **plain**
`zImage` with the tree beside it as a separate file.

## Incremental build

```sh
./build-j36-ultra.sh
```

This is an extension of `build-r36-ultra.sh` rather than a second build system.
It regenerates the DTB on the host first — so a keymap or pad-mux regression
fails in a second — then resumes the checkpointed R36 base build (already
finished ones cost seconds; `J36_RESUME_R36=0` skips it), then builds the J36
layer in the same `darkos-r36` VM. The first J36 run creates a persistent ARMv7
Linux 6.12 LTS workspace; later runs rebuild only changed kernel, DTB,
input-module, initramfs and `boot.img` files.

Artifacts are copied to:

```text
../dArkOS-artifacts/j36-ultra/boot.img            eMMC BOOTIMG payload (9 MiB slot)
../dArkOS-artifacts/j36-ultra/zImage              plain 32-bit ARM kernel
../dArkOS-artifacts/j36-ultra/mt6592-j36-ultra.dtb
../dArkOS-artifacts/j36-ultra/j36_mt6592_input.ko
../dArkOS-artifacts/j36-ultra/sd-boot/            copy onto the BOOT partition
../dArkOS-artifacts/j36-ultra/manifest.txt
```

## The SD BOOT payload

Copy `sd-boot/` into the root of the FAT partition labelled `BOOT` and the MVII
LK boots the card instead of the eMMC. Nothing already there is disturbed: an
R36S card keeps its `Image`, `uInitrd`, rk3326 trees and `boot.ini`.

```text
zImage                 plain ARMv7 kernel, no appended tree
mt6592-j36-ultra.dtb   the tree the LK loads separately and patches
initrd.img             bring-up initramfs (busybox + the input module)
mvii/boot.conf         filenames and command line for the MVII LK
```

`mvii/boot.conf` exists because a dArkOS card already carries a `boot.ini`, and
that `boot.ini` names the arm64 `Image` and an rk3326 tree. The LK parses
`boot.ini` first and `/mvii/boot.conf` second precisely so this file gets the
last word; without it the LK would load the arm64 kernel, refuse it at the magic
check, and fall back to the eMMC. Load addresses are deliberately absent from
it: those are the LK's business, and it knows this SoC's DRAM map and the
address of the framebuffer the DTB hands to `simple-framebuffer`.

The command line carries no `root=`, and must not yet. This bring-up profile has
no MMC, block or network drivers — they are off so the payload fits the 9 MiB
BOOTIMG slot — so a `root=` this kernel cannot honour is a panic instead of a
shell. It boots to its initramfs. When native MT6592 MSDC lands, add
`root=LABEL=ROOTFS rootwait rw` and drop `rdinit=`; the armhf rootfs the R36
build produces is the one it will mount.
