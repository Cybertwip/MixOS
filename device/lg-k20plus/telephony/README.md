# LG K20 Plus telephony (QMI over QRTR)

Unlike the OPPO tree, this phone's modem path is almost entirely mainline:
`qcom_q6v5_mss` boots the X6 (with the msm8917 patch), `qrtr` carries QMI,
and ModemManager's `qmi` plugin does registration, packet data, SMS and
voice signalling. This directory is the board policy around that stack.

## The boot order

1. `modem-boot.sh` unblocks the `lg-mss` rfkill, binds the remoteproc
   (`qcom,msm8917-mss-pil`), and waits for `qrtr-lookup` to announce the
   modem services (wms, voice, dms, nas).
2. On success it writes `ready` to the glue driver's `state` sysfs file,
   which emits the ONLINE uevent; `lg-modem.rules` (udev) starts
   `lg-telephony.service`, which starts ModemManager if it is installed.
3. `check-telephony.sh` verifies each stage for the serial console.

## The two traps

- **rmtfs.** The modem reads NV/EFS from the `rmtfs` partitions. Without
  them it boots, finds no calibration, and quietly never registers --
  indistinguishable from a dead modem except in the remoteproc log.
  `modem-boot.sh` refuses to start when `/dev/disk/by-partlabel/modemst1`
  is missing and says which partitions to flash.
- **qrtr-ns.** The QRTR name service must be running before the modem
  announces, or the announcements go nowhere. `lg-telephony.service`
  orders itself after `qrtr-ns.service`.

## Files

- `modem-boot.sh` -- rmtfs check, remoteproc bind, qrtr wait, state flip.
- `lg-modem.rules` -- udev: tag + start the telephony service.
- `lg-telephony.service` -- systemd unit (qrtr-ns -> modem-boot -> MM).
- `apn.conf.template` -- APN shape for the data scripts.
- `check-telephony.sh` -- on-device self-test.
