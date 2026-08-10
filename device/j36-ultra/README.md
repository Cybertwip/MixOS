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

## Incremental build

After the R36 baseline build has finished, run:

```sh
./build-j36-ultra.sh
```

This reuses the existing `darkos-r36` Multipass VM but does **not** rerun
`make rg351mp`. The first J36 run creates a persistent ARMv7 Linux 6.12 LTS
workspace. Later runs reuse it and incrementally rebuild only changed kernel,
DTB, input-module, initramfs, and `boot.img` files.

Artifacts are copied to:

```text
../dArkOS-artifacts/j36-ultra/boot.img
../dArkOS-artifacts/j36-ultra/mt6592-j36-ultra.dtb
../dArkOS-artifacts/j36-ultra/j36_mt6592_input.ko
../dArkOS-artifacts/j36-ultra/manifest.txt
```

The generated `boot.img` is a first-stage serial/framebuffer/input bring-up
image sized for the stock 9 MiB BOOTIMG partition. It is not yet a complete
dArkOS image: native MT6592 MSDC/eMMC or SD storage must be ported before a
persistent Debian root filesystem can be mounted.
