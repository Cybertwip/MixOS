# LG K20 Plus firmware

Same rule as the OPPO tree: the drivers are open, the blobs are LG's and
are NOT vendored. Place them under `/lib/firmware/lg/lv517/` via
`LG_FIRMWARE_DIR`.

## Needed files

| File | Used by | Stock location |
|---|---|---|
| `modem.mdt` + `modem.bXX` | MSS remoteproc (X6 boot) | `/firmware/image/modem.*` |
| `wcnss.mdt` + `wcnss.bXX` | Pronto PIL (Wi-Fi boot) | `/firmware/image/wcnss.*` |
| `wlan/prima/WCNSS_qcom_wlan_nv.bin` | WCNSS NV config | `/persist/WCNSS_qcom_wlan_nv.bin` |

`extract-stock.sh` pulls them over adb from a rooted stock phone. Unlike the
OPPO `.ofp` images, LG `.kdz` firmware can be unpacked offline (several open
tools do it); either source works as long as the `.mdt` and all its `.bXX`
segments arrive together -- a partial set fails PIL auth with an error that
looks exactly like a driver bug, which is why the build checks the set.

## Why `.mdt` sets fail

PIL authenticates the whole chain: the `.mdt` header names every segment
and TrustZone verifies each hash. One missing `.bXX` file aborts the boot
with `-EIO` from `qcom_scm_pas_auth_and_reset`. When Wi-Fi or the modem
fails at that call, count the segments before suspecting the driver.
