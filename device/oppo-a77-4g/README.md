# OPPO A77 4G (2022, MT6765) -- bring-up scaffold

MixOS for the CPH2385 (Helio G35 / MT6765, 6.56in 720x1612 60Hz,
3 or 4GB RAM, stock Android 12). **This tree builds but does not boot
yet**: every hardware fact the drivers and subsystem nodes need is
still unknown, and nothing here invents any. Read `BRINGUP.md` before
anything else; the wrapper refuses to build without
`OPPO_A77_4G_BRINGUP_ACK=1`.

## Status

| Subsystem | Status | Grounding |
|---|---|---|
| SoC identity | MT6765 (Helio G35) | Seller listings + launch coverage |
| PMIC identity | MT6357 suspected, no drivers | Confirm from stock DTB (BRINGUP 2) |
| Memory map | 3GB default row @ conventional base (prior) | Retail size; base unverified; 4GB row opt-in |
| Serial/MMC/clocks/pinctrl | Unknown, no nodes emitted | Stock DTB (BRINGUP 2) |
| Display/touch | Models unknown, no drivers | Stock DTB + vendor (BRINGUP 3) |
| WiFi/modem | Names expected from mt6877 tree, unconfirmed | adb/OFP extraction (BRINGUP 4) |
| Image pipeline | Full: kernel + DTB + initramfs + rootfs + MixOS image | Shared `device/common` tooling |

## Layout

`devices.sh` (two rows: `cph2385` 3GB default + `cph2385-4gb`),
`generate_dts_a77_4g.py` (thin wrapper over shared
`device/common/mtk_bringup_dts.py`), `build-in-vm.sh` (trimmed phone
build: no drivers, no patches, defconfig kernel), `linux/` (empty
Makefile + landing plan), `firmware/` (adb + OFP extract scripts with
same-vendor expectations), `tests/`, `BRINGUP.md` (the playbook).

## Build

`OPPO_A77_4G_BRINGUP_ACK=1 sh ./build-oppo-a77-4g.sh` -- lands one
`MixOS_arm64_trixie_<commit>.img` in
`MixOS-Artifacts/oppo/<device>/`. `--mix-only` exports `boot/`
(boot.img + DTB + DTS source) for inspection and `fastboot boot`
trials. Do not flash blindly: without serial output the first boots
are diagnosed via `fastboot boot` + observed behavior (BRINGUP.md
step 5).

RAM rows: the default `cph2385` row describes 3GB, which is safe on a
4GB unit (wastes 1GB until you switch rows) -- the reverse would
describe RAM that is not there. Confirm your unit in Settings > About
(BRINGUP.md step 0) before building the full image.
