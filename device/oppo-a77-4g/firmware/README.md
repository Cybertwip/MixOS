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

`DA/MTK_DA_mt6765.bin` is the MT6765 Download Agent entry (570588 bytes,
sha256 `156aaf9cdb02bc4fdcd5ee59c740aa3735aa9cc3af2450c64b7e8c308bb84f41`),
lifted with `extract-mtk-da.py` out of the AllInOne bundle the mtkclient
reference tree carries (`Loader/MTK_DA_V5.bin`); `fetch-da-mt6765.sh`
re-derives the same bytes from the pinned third-party copy and is the
provenance record. Use it with `./flash -da-loader
device/oppo-a77-4g/firmware/DA/MTK_DA_mt6765.bin ...`. It does not by
itself enable BROM eMMC writes: a retail phone may still demand SLA
authentication (the stock package ships `auth_sv5.auth`; the tool stops at
that wall today), DRAM needs the preloader EMI, and eMMC writes follow the
stock scatter, not the J36 layout.
