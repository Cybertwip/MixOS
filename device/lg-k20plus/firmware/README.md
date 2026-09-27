# Pronto/WLAN config, LG K20 Plus

Three files, all Qualcomm's, all taken from the reference kernel drop at
`reference/NetHunter_K20plus_arm64_Kernel-Source/drivers/staging/prima/firmware_bin/`.
They are the calibration and default configuration the WCNSS WLAN stack
reads: `wcn36xx` asks `request_firmware()` for the NV item table by the
path below, and the prima `wlan` driver (when it lands) parses the config
pair the same way stock does.

| file | size | sha256 |
| --- | --- | --- |
| `wlan/prima/WCNSS_qcom_wlan_nv.bin` | 29816 | `93fc87d8233ffb0244037ba165efdfcdc8851e4e7345fc646f5d628d29d703d8` |
| `wlan/prima/WCNSS_qcom_cfg.ini` | 9850 | `a8a748e831b510e3d60f8fb46796ec770e7546560aabc35432ce602b696f7583` |
| `wlan/prima/WCNSS_cfg.dat` | 11514 | `66d8aa043111f6bdce04cfb5c8d09d43f65f853506f996da357d89122e0ac1ae` |

The path under `wlan/prima/` is the name the driver asks
`request_firmware()` for, so the layout here is the layout on the phone:
`build-in-vm.sh` copies this directory into `lib/firmware/wlan/prima/`
both in the initramfs and on the rootfs.

## The NV file here is the generic default

A phone carries its own per-unit calibration at `/persist/WCNSS_qcom_wlan_nv.bin`,
tuned for that unit's RF front end; this copy is the reference default that
boots the radio without it. `extract-stock.sh` pulls the per-unit file when
the phone is rooted, and the build overlays it onto the vendored one -- the
extracted file wins whenever it exists, the vendored one covers every other
case. A WLAN that associates but performs badly is the signature of the
default standing in for the tuning, not of a driver bug.

## What is not here

`modem.mdt` + `modem.bXX` and `wcnss.mdt` + `wcnss.bXX` -- the PIL sets the
modem and Pronto boot from -- are not in the reference tree (it is a
kernel-only drop; the images ship on the phone's `/firmware` partition).
Two routes fetch them into `stock/`, and they are interchangeable:

- `extract-stock.sh` pulls them over adb from the phone, discovering every
  `.bXX` segment by listing. Two minutes, exact-match blobs, needs the phone.
- `fetch-kdz.sh /path/to/stock.kdz` unpacks a stock KDZ offline with
  [kdztools](https://github.com/ehem/kdztools) and lifts the sets out of
  the modem partition image with 7z. The KDZ itself (~2 GB, wait-walled)
  is a browser download -- e.g. the MP26011K_00 build for the LGMP260 at
  <https://lgrom.com/firmware/LGMP260>.

Either way the build counts each set, because one missing segment fails
PIL auth with an error that looks exactly like a driver bug.

## Licence

These are Qualcomm's, redistributed here as they were shipped in the
reference drop, with no modification and no reverse engineering of their
contents. They are not covered by this repository's licence. Everything
under `device/lg-k20plus/linux/` is original work and is GPL-2.0 as marked.
