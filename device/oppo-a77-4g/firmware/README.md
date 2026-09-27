# OPPO A77 4G (MT6765) firmware

Nothing is confirmed on this model yet: no blobs have been seen on a
CPH2385, and the names both scripts try (`modem.img`, `dsp.img`,
`WIFI_RAM_CODE*`, `WMT_SOC.cfg`) are the proven oppo-mt6877 set (same
vendor, same MTK CONSYS/modem architecture) -- expected, not measured.
Two routes fetch them into `stock/` (git-ignored), and they are
interchangeable:

- `extract-stock.sh` pulls them over adb from the phone itself and writes
  a `MANIFEST.txt` with sizes and hashes next to them. Misses print the
  fix (a `/vendor/firmware` listing that teaches the script the real
  names). Needs the phone, ten minutes.
- `fetch-ofp.sh /path/to/CPH2385.ofp` decrypts a stock `.ofp` offline
  with oppo_decrypt and lifts the blobs out of the vendor image (copied
  from the proven `device/oppo-mt6877/firmware/fetch-ofp.sh`; untested
  against a CPH2385 OFP -- newer builds rotate the keys, and a miss
  means extracting from the phone instead). The same OFP also carries
  the stock boot image BRINGUP step 1 needs, so this route does double
  duty.

`BRINGUP.md` step 4 turns confirmed results into driver firmware paths.
No vendored blobs: nothing has been picked from anywhere, and nothing
will be until a source exists.
