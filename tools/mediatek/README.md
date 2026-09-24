# MediaTek flashing tool and J36 firmware

This directory contains a standalone copy of PowerEngine's `Deployment/cmd/mvii-flash` Go source, with its `go.mod` and `go.sum`, for the J36 Ultra MT6592. The original PowerEngine directory was left in place. Run `./tools/mediatek/flash -h` from the dArkOS checkout, or build the CLI with `cd tools/mediatek && go build -o /path/to/flash ./mvii-flash`. Go 1.23 or newer is required. On macOS, install `libusb-1.0` and `pkg-config` for the native USB transport.

`firmware/` is the J36 LK, the release LK and the in-target flash payload. `./tools/mediatek/build.sh` builds `lk.bin`, `lk-release.bin`, `MVIIFlash.bin` and the flash CLI. `./tools/mediatek/build.sh --without-battery` disables the charger watchdog, widens the brownout limit, skips the charge screen, and does not rewrite the preloader's charger mode. Pair that image with `./build-j36-ultra.sh --without-battery`, which writes `j36.power=external` so Linux does the same after handoff.

The flashing commands require their usual board-specific inputs and a connected device. The first `go run` may download the dependencies in `go.mod`. The firmware build needs CMake, Python 3 and an LLVM that provides `clang` and `ld.lld` (`MVII_LLVM_ROOT` overrides discovery). It does not replace the board's stock preloader.

For SD boot, install the resulting `build/mediatek/j36-ultra/without-battery/boot/lk-release.bin`
in the device's LK/UBOOT slot using the existing flashing procedure. Writing a
MixOS `.img` to removable media does not update this slot. Update the Linux
payload too: the previous batteryless driver would re-arm the four-second timer.
The new LK and Linux driver log `charger watchdog OFF (verified)` only after
checking the PMIC readback. Register-model regression checks can be run with:

```sh
cc -std=c99 -Wall -Wextra -Werror device/j36-ultra/tests/external-power.c -o /tmp/j36-power-test
/tmp/j36-power-test
```
