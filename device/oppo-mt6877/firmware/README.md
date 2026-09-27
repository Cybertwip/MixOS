# OPPO MT6877 firmware

The MixOS drivers are open source; the blobs they download into the
coprocessors are OPPO's and are NOT in this tree. Each file below is taken
from a stock phone and placed under `/lib/firmware/oppo/mt6877/` on the
Debian rootfs by the build's firmware stage (`FIRMWARE_DIR`).

## Needed files

| File | Used by | Stock location |
|---|---|---|
| `modem.img` | modem DSP+baseband boot | `/vendor/firmware/modem.img` |
| `dsp.img` | modem DSP support | `/vendor/firmware/dsp.img` |
| `WIFI_RAM_CODE` | CONSYS Wi-Fi RAM code | `/vendor/firmware/WIFI_RAM_CODE_*` |
| `WMT_SOC.cfg` | WMT coexistence config | `/vendor/firmware/WMT_SOC.cfg` |

Two routes fetch them into `stock/`, and they are interchangeable:

- `extract-stock.sh` pulls them over adb from the phone itself and writes a
  `MANIFEST.txt` with sizes and hashes next to them. Stock firmware
  directories are usually world-readable, so root is only needed if your
  build hid them. Two minutes, exact-match blobs, needs the phone.
- `fetch-ofp.sh /path/to/stock.ofp` decrypts a stock `.ofp` offline with
  [oppo_decrypt](https://github.com/oneseeker279/oppo_decrypt)
  (`ofp_mtk_decrypt.py`, MIT) and lifts the blobs out of the vendor image
  with 7z. The `.ofp` (several GB) comes from OPPO's official firmware
  downloads or a community mirror -- search the CPH2251 build -- and is a
  browser download. Newer builds rotate the OFP keys; if the decrypter
  reports unknown keys, extract from the phone instead.

Set `OPPO_OFP=/path/to/stock.ofp` when invoking `./build-oppo.sh` to run
the OFP route as build preflight (reused when `stock/` already holds the
blobs; ignored when `OPPO_FIRMWARE_DIR` is set).

Nothing here is vendored: the reference kernel tree
(`reference/android_kernel_oppo_mt6877`) is a kernel-only drop -- it carries
no Wi-Fi driver, no modem image, and no `WIFI_RAM_CODE` under any name, so
there was nothing to pick. Do not re-search it; extract from the phone.

## Why the tree does not vendor them

The J36 tree vendors its MT6592 Wi-Fi firmware because that blob is
redistributable. The OPPO blobs have never been published under any
redistribution licence, so the build refuses to run the modem/wifi stages
without them and fails at the firmware check with the extraction command to
run -- a missing blob must never look like a driver bug. CI builds therefore
stop after the kernel+module stages unless `FIRMWARE_DIR` is set.
