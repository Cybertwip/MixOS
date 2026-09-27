# OPPO A77 5G (2022, MT6833) -- bring-up scaffold

MixOS for the CPH2381 (Dimensity 810 / MT6833, 6.56in 720x1612 90Hz,
4 or 6GB RAM, stock Android 12). **This tree builds but does not boot
yet**: every hardware fact the drivers and subsystem nodes need is
still unknown, and nothing here invents any. Read `BRINGUP.md` before
anything else; the wrapper refuses to build without
`OPPO_A77_BRINGUP_ACK=1`.

## Status

| Subsystem | Status | Grounding |
|---|---|---|
| SoC identity | MT6833 (Dimensity 810) | Retail specs (GSMArena + launch coverage) |
| PMIC identity | Unknown, no drivers | Stock DTB (BRINGUP 2) |
| Memory map | 4GB default row @ conventional base (prior) | Retail size; base unverified; 6GB row opt-in |
| Serial/MMC/clocks/pinctrl | Unknown, no nodes emitted | Stock DTB (BRINGUP 2) |
| Display/touch | Models unknown, no drivers | Stock DTB + vendor (BRINGUP 3) |
| WiFi/modem | Names expected from mt6877 tree, unconfirmed | adb/OFP extraction (BRINGUP 4) |
| Image pipeline | Full: kernel + DTB + initramfs + rootfs + MixOS image | Shared `device/common` tooling |

## Layout

`devices.sh` (two rows: `cph2381` 4GB default + `cph2381-6gb`),
`generate_dts_a77.py` (thin wrapper over shared
`device/common/mtk_bringup_dts.py`), `build-in-vm.sh` (trimmed phone
build: no drivers, no patches, defconfig kernel), `linux/` (empty
Makefile + landing plan), `firmware/` (adb + OFP extract scripts with
same-vendor expectations), `tests/`, `BRINGUP.md` (the playbook).

## Build

`OPPO_A77_BRINGUP_ACK=1 sh ./build-oppo-a77.sh` -- lands one
`MixOS_arm64_trixie_<commit>.img` in
`MixOS-Artifacts/oppo/<device>/`. `--mix-only` exports `boot/`
(boot.img + DTB + DTS source) for inspection and `fastboot boot`
trials. Do not flash blindly: without serial output the first boots
are diagnosed via `fastboot boot` + observed behavior (BRINGUP.md
step 5).

RAM rows: the default `cph2381` row describes 4GB, which is safe on a
6GB unit (wastes 2GB until you switch rows) -- the reverse would
describe RAM that is not there. Confirm your unit in Settings > About
(BRINGUP.md step 0) before building the full image.
