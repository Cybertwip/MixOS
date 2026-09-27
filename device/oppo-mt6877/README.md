# OPPO MT6877 MixOS bring-up

Debian on OPPO Dimensity 900 (MT6877) phones, built the J36 way: a mainline
6.12 LTS arm64 kernel, a generated device tree, one out-of-tree module per
subsystem, and a payload that fails piece by piece instead of all at once.

Primary target: **20181** (Reno6 5G class). `devices.sh` lists the family;
adding a device is one row there plus its panel/touch extracts in `board/`.

## What is in this directory

- `devices.sh` -- the family table (`oppo_device_info`, `oppo_devices`).
- `board/` -- committed facts from the reference kernel
  (`reference/android_kernel_oppo_mt6877`, 4.14.186): SoC addresses, IRQ
  lines, TD4330 geometry, NT36672 wiring, eMMC caps, modem/consys outlines.
  See `board/PROVENANCE.txt`. The build never reads `reference/` itself.
- `generate_dts_oppo.py` -- parses `board/` into a standalone DTS (plain
  `dtc`, no kernel includes). Asserts the panel geometry and the LK
  framebuffer handoff.
- `linux/` -- the drivers plus two kernel patches:
  - `0001-mtk-sd-mt6877.patch` -- eMMC/SD (HS400, 8-bit).
  - `0002-pmic-wrap-mt6877.patch` -- pwrap so the MT6359 MFD can bind.
  - `oppo_mt6877_consys` + `oppo_mt6877_wifi` -- CONSYS_6877 power and the
    fullmac Wi-Fi driver (WMT + SDIO + cfg80211/wlan0).
  - `oppo_mt6877_panel_td4330` -- TD4330 1080x2280 DSI command-mode panel
    with DCS backlight (full vendor init table transcribed).
  - `oppo_mt6877_touch_nt36672` -- Novatek NT36672 SPI multitouch (10
    fingers; IRQ with a 100 Hz poll fallback until EINT lands).
  - `oppo_mt6877_audio` -- MT6877 AFE DL1 + `mt6359` machine (mainline
    codec).
  - `oppo_mt6877_modem` -- modem power/boot control, rfkill, READY uevent.
  - `oppo_mt6877_input` -- volume keys polled from GPIO DIN (GPIO120/114).
  - `oppo_mt6877_pmic` -- battery gauge over IIO + poweroff handling.
- `telephony/` -- userspace phone stack: modem boot, udev rules, systemd
  unit, APN template, self-test. See `telephony/README.md`.
- `firmware/` -- which stock blobs to extract and how (not vendored).
- `tests/` -- `test-dts.py` (every device + dtc), `test-panel-table.py`
  (init-table transcription guard), `wifi-bringup.sh` (on-device).
- `build-in-vm.sh` -- the VM half of `./build-oppo.sh`.

## Boot

`fastboot flash boot oppo-20181-boot.img`, with the rootfs unpacked once
onto a `ROOTFS`-labelled partition. The LK hands over with the panel lit;
`simple-framebuffer` adopts it, `/init` loads the `oppo.*` payload and
switches root. Command-line words (all default on in `boot.img`):

- `oppo.audio=1` / `oppo.wifi=1` / `oppo.modem=1` / `oppo.power=1` --
  load that subsystem; drop the word to skip it.
- `oppo.device=20181` -- informational; the image is per-device.

## Bring-up status (honest)

Expected to probe on first boot: serial, eMMC, framebuffer, keys, touch,
battery gauge. Wi-Fi registers `wlan0`; scan/connect forward to firmware
opcodes carried from the MT6592 driver and must be verified on hardware
(`tests/wifi-bringup.sh`). The modem boots to READY; packet data, SMS and
voice need the stage-2 CCIF/CLDMA channel port (`telephony/README.md`).
The panel driver binds when the MT6877 DSI host lands; until then the
console runs on the LK framebuffer. Power key, pinctrl/EINT and the MT6359
ADC are the three known gaps, each documented at its driver.
