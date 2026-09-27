# OPPO MT6877 telephony

The kernel side is `oppo_mt6877_modem.ko`: it boots the baseband and reports
`ready`. Everything from the SIM PIN to a voice call lives here, in
userspace, because that is where Debian already solved it.

## Stage 1: what works now

1. `modem-boot.sh` unblocks the `oppo-mdm` rfkill, writes `1` to the modem's
   `boot` sysfs file and waits for `state` to read `ready`.
2. `oppo-modem.rules` (udev) tags the modem device and starts
   `oppo-telephony.service` on its ONLINE uevent.

That is the whole stage-1 contract: a booted modem and a hook. There is no
AT port and no data interface yet, so ModemManager has nothing to drive --
installing it now would only add a daemon that polls for hardware that is
not there.

## Stage 2: the channel port (tracked, not claimed)

Packet data, SMS and voice ride the CCIF control channel and the CLDMA data
channels of the ECCCI stack (`drivers/misc/mediatek/eccci/` in the
reference kernel). The port order is:

1. CCIF control channel as a `/dev/ccmni`-style char device (AT proxy),
2. CLDMA data channel as a WWAN netdev (`wwan0`),
3. ModemManager driving both (its `mtk` plugin handles the MTK extensions),
4. voice-call audio through the AFE modem-PCM loopback (see the audio
   driver's loopback note).

Only when (2) exists does the rootfs gain `modemmanager`, `libmbim`,
`libqmi` is NOT needed (that is the Qualcomm path; see the LG tree) and the
APN chat scripts. `apn.conf.template` already reserves the shape so the
rootfs stage does not have to be redesigned later.

## Files

- `modem-boot.sh` -- boot the modem and wait for READY.
- `oppo-modem.rules` -- udev: tag + start the telephony service.
- `oppo-telephony.service` -- systemd unit (stage-1: logs readiness; stage-2:
  depends on ModemManager).
- `apn.conf.template` -- APN shape for the stage-2 data scripts.
- `check-telephony.sh` -- on-device self-test: modem state, rfkill, SIM
  presence once the channel port exposes it.
