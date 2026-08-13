# Connectivity firmware, J36 Ultra

Two blobs, both MediaTek's, both taken off this device's own stock system image at
`/system/etc/firmware/`. They are the ROM patches the MT6592's connectivity MCU
loads before it has anything patched to run — `j36_mt6592_wifi.ko` sends them down
the BTIF link during bring-up and there is no radio without them.

| file | size | sha256 |
| --- | --- | --- |
| `mediatek/mt6592/ROMv1_patch_1_0_hdr.bin` | 80776 | `14903c0b08ede70537c50ba8c7c5579c360297f3ce234c8f99e6794421e7c3fd` |
| `mediatek/mt6592/ROMv1_patch_1_1_hdr.bin` | 24592 | `591c06814b384c63b717ce9b8219c04383c1eef1ab5b3b489c637679451b1d24` |

The path under `mediatek/mt6592/` is the name the driver asks `request_firmware()`
for, so the layout here is the layout on the card. `build-in-vm.sh` copies this
directory into `j36/wifi/firmware/` and `/init` points
`/sys/module/firmware_class/parameters/path` at it before it insmods the module —
which is why nothing needs to be written into the shared rootfs's `/lib/firmware`,
and why deleting `j36/wifi/` from the card takes the radio off it cleanly.

## The names lie about the order

`ROMv1_patch_1_1_hdr.bin` is patch **1** of 2 and `ROMv1_patch_1_0_hdr.bin` is
patch **2** of 2. Byte 24 of each 28-byte header is `(count << 4) | sequence` —
`0x21` and `0x22` respectively — and the driver sorts on that byte rather than on
the filenames for exactly this reason. Sending them in filename order makes the
connectivity MCU stop answering rather than complain.

## What is not here

`WIFI_RAM_CODE_SOC` (263072 bytes) is the WLAN firmware proper. It goes down the
AHB HIF, which this build does not have, so requesting it would only produce a
missing-firmware warning for something nothing could use yet. It is in the same
directory of the stock image when the HIF stage lands.

`WMT_SOC.cfg` is not here either, and will not be: it is 80 bytes of key/value
settings that the stock driver parses at probe, and the four values this board's
copy holds (`coex_wmt_ant_mode=1`, `wmt_gps_lna_pin=0`, `wmt_gps_lna_enable=0`,
`co_clock_flag=1`) are compiled into `j36_mt6592_wifi.h` and the WMT command
tables, each quoted where it is used. A parser for four constants that cannot
change without new silicon is a parser that can disagree with the code.

## Licence

These are MediaTek's, redistributed here as they were shipped on the device, with
no modification and no reverse engineering of their contents. They are not covered
by this repository's licence. Everything under `device/j36-ultra/linux/` is
original work and is GPL-2.0 as marked.
