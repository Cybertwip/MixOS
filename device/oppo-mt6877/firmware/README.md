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

`extract-stock.sh` pulls them over adb from a rooted stock device. OPPO
factory images (`.ofp`) are encrypted; there is no offline unpack path, so
extraction needs the phone itself -- which the person building a phone image
has.

## Why the tree does not vendor them

The J36 tree vendors its MT6592 Wi-Fi firmware because that blob is
redistributable. The OPPO blobs have never been published under any
redistribution licence, so the build refuses to run the modem/wifi stages
without them and fails at the firmware check with the extraction command to
run -- a missing blob must never look like a driver bug. CI builds therefore
stop after the kernel+module stages unless `FIRMWARE_DIR` is set.
