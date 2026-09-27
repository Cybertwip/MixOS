# LG K20 (2019, MT6739) -- bring-up scaffold

MixOS for the LM-X120 (MT6739, 5.45in 480x960, 1GB/16GB, stock Android 9
Go). **This tree builds but does not boot yet**: every hardware fact the
drivers and subsystem nodes need is still unknown, and nothing here
invents any. Read `BRINGUP.md` before anything else; the wrapper refuses
to build without `LG_K20_BRINGUP_ACK=1`.

## Status

| Subsystem | Status | Grounding |
|---|---|---|
| SoC/PMIC identity | MT6739 + MT6357 (standard pairing) | Retail specs; confirm from stock DTB |
| Memory map | 1GB @ conventional base (prior) | Retail size; base unverified |
| Serial/MMC/clocks/pinctrl | Unknown, no nodes emitted | Stock DTB (BRINGUP 2) |
| Display/touch | Models unknown, no drivers | Stock DTB + vendor (BRINGUP 3) |
| WiFi/modem/PMIC | Names unknown, no drivers | adb extraction (BRINGUP 4) |
| PMIC bindings | `mediatek,mt6357` exists in mainline 6.12 | `arch/arm64/boot/dts/mediatek/mt6357.dtsi` |
| Image pipeline | Full: kernel + DTB + initramfs + rootfs + MixOS image | Shared `device/common` tooling |

## Layout

`devices.sh` (one row: `lm-x120`), `generate_dts_k20.py` (thin wrapper
over shared `device/common/mtk_bringup_dts.py`), `build-in-vm.sh`
(trimmed phone build: no drivers, no patches, defconfig kernel),
`linux/` (empty Makefile + landing plan), `firmware/` (extract script
with MTK-standard priors), `tests/`, `BRINGUP.md` (the playbook).

## Build

`LG_K20_BRINGUP_ACK=1 sh ./build-lg-k20.sh` -- lands one
`MixOS_arm64_trixie_<commit>.img` in `MixOS-Artifacts/lg/lm-x120/`.
`--mix-only` exports `boot/` (boot.img + DTB + DTS source) for inspection
and `fastboot boot` trials. Do not flash blindly: without serial output
the first boots are diagnosed via `fastboot boot` + observed behavior
(BRINGUP.md step 5).

1GB RAM note: the Debian rootfs fits the 16GB eMMC fine; at runtime the
console system is small enough, but anything graphical wants zram --
BRINGUP step 6, after first boot, not before.
