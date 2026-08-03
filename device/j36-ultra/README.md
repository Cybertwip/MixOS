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

Input still requires small MT6592 adapters:

1. GPIO controller access for the polled D-pad, stick clicks, and special key.
2. KPD register reader for the matrix-backed face/shoulder/start/select keys.
3. AUXADC IIO provider for joystick channels 15, 14, 12, and 13.

Those adapters can be rewired from the existing MVII driver files without
changing the generic Debian userspace.

## Implemented Linux adapter

`linux/j36_mt6592_input.c` now implements the first input layer as one minimal
polled platform driver. It consumes the DTB's `j36,j36-ultra-input` node and
covers the GPIO, KPD and AUXADC paths without touching display registers.
