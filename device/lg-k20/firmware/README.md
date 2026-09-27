# LG K20 (MT6739) firmware

Nothing is known yet: no blobs have been seen on this phone, and the
names `extract-stock.sh` tries (`modem.img`, `dsp.img`, `WIFI_RAM_CODE*`,
`WMT_SOC.cfg`) are MediaTek-standard priors, not measured facts. The day
adb reaches the phone, run it: hits land in `stock/` (git-ignored) with
a `MANIFEST.txt`, misses print the fix (a `/vendor/firmware` listing that
teaches the script the real names), and `BRINGUP.md` step 4 turns the
results into driver firmware paths.

No KDZ/fetch route yet: LG's MTK stock-ROM format is unverified
(BRINGUP.md step 1 confirms it). No vendored blobs: nothing has been
picked from anywhere, and nothing will be until a source exists.
