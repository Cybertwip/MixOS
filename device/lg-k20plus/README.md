# LG K20 Plus (MSM8917) MixOS bring-up

Debian on the LG K20 Plus (MP260, lv517), built the J36 way: a mainline
6.12 LTS arm64 kernel, a generated device tree, one out-of-tree module per
subsystem, and a payload that fails piece by piece instead of all at once.

NOTE: this phone is a **Qualcomm Snapdragon 425** (MSM8917), not MediaTek.
Its modem speaks QMI over QRTR, its Wi-Fi is a Pronto WCN3660, its PMIC is
a PMI8950 over SPMI. The drivers below target that hardware, and lean on
mainline QCOM drivers wherever they exist -- which, on this SoC, is most
of the way.

Targets: **lv517** (rev-b, LGD panel, default), **lv517-rev0** (volume
remap), **lv517-tovis** (TD4100 panel + Synaptics touch). `devices.sh`
lists the family.

## What is in this directory

- `devices.sh` -- the family table.
- `board/` -- committed facts from the reference kernel
  (`reference/NetHunter_K20plus_arm64_Kernel-Source`, 3.18.31): SoC
  addresses, IRQs, carveouts, panel geometries, touch protocol, key
  wiring. See `board/PROVENANCE.txt`.
- `generate_dts_lg.py` -- parses `board/` into a standalone DTS (plain
  `dtc`). Asserts panel geometry, emits the rev-specific key map, and the
  LK splash framebuffer node (0x90000000) when `--lk-fb-base` is set.
- `linux/` -- the drivers plus one kernel patch:
  - `0001-remoteproc-q6v5-mss-msm8917.patch` -- modem remoteproc.
  - `lg_msm8917_panel` -- LG4894 + TD4100 720x1280 DSI video panels
    (full LGD init table transcribed; WLED backlight via DT).
  - `lg_msm8917_touch` -- SiW LG4894 I2C multitouch (IRQ + poll
    fallback). TD4100 glass uses mainline `rmi4-i2c`.
  - `lg_msm8917_audio` -- `lv517-snd-card` machine (binds when LPASSCC
    lands; USB audio covers v1).
  - `lg_msm8917_wcnss` -- Pronto PIL boot (MDT + SCM PAS); MAC is
    mainline `wcn36xx`.
  - `lg_msm8917_modem` -- modem glue: rfkill, state, READY uevent.
    The modem itself is mainline `qcom_q6v5_mss` + `qrtr`.
  - `lg_msm8917_input` -- volume rocker + hall sensor polled from TLMM.
  - `lg_msm8917_pmic` -- battery gauge over IIO + poweroff handling.
  - Mainline, DT-only: SDHCI (eMMC HS400), SPMI, WLED backlight,
    PMIC pwrkey/resin, PSHOLD, RMI4 touch, i2c-qup, msm-serial.
- `telephony/` -- QMI userspace stack: rmtfs check, remoteproc bind,
  qrtr wait, ModemManager. See `telephony/README.md`.
- `firmware/` -- which stock blobs to extract and how (not vendored).
- `tests/` -- `test-dts.py` (every device + dtc), `test-panel-table.py`
  (init-table guard), `wifi-bringup.sh` (on-device).
- `build-in-vm.sh` -- the VM half of `./build-lg.sh`.

## Boot

One `MixOS_arm64_trixie_<commit>.img` per device: a GPT container with
the Android boot.img bytes (p1, BOOT) and the ext4 rootfs (p2, ROOTFS).
Split it with the `dd` lines the build prints, `fastboot flash boot` the
boot part, and `dd` the rootfs onto a `ROOTFS`-labelled partition big
enough to hold it. Never write the container to the eMMC whole.
Command-line words (default on):

- `lg.audio=1` / `lg.wifi=1` / `lg.modem=1` / `lg.power=1`
- `lg.device=lv517` / `lg.panel=lg4894` (informational)

## Bring-up status (honest)

Expected to probe on first boot: serial, eMMC, splash framebuffer,
backlight, keys, touch, battery gauge (once the ADC binding is
confirmed). Wi-Fi boots Pronto and hands to `wcn36xx`. The modem node
ships disabled until the CX/MX power domains are proven (see the DTS
comment); the remoteproc patch, glue driver and QMI userspace are ready
for the flip. Speaker audio waits on the LPASS clock controller. MDSS,
TLMM/pinctrl and GCC are the three known mainline gaps, each documented
at its consumer.
