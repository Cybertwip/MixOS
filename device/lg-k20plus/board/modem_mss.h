/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * lv517 modem + Wi-Fi outline.
 *
 * MODEM. The X6 LTE baseband is an MSS (modem subsystem) rebooted by PIL.
 * Mainline boots it with qcom_q6v5_mss, which knows msm8916 but not msm8917;
 * linux/0001-remoteproc-q6v5-mss-msm8917.patch adds the compatible reusing
 * the 8916 sequence (same Hexagon generation, same PIL handshake). Control
 * and data then speak QMI over QRTR (mainline qrtr + ModemManager `qmi`).
 * linux/lg_msm8917_modem.c is the board glue: rfkill, READY tracking, and
 * the rmtfs/EFS checks without which the modem silently refuses to boot.
 *
 * WI-FI. Pronto/WCN3660 at 0xa21b000 (CONFIG_PRONTO_WLAN in the reference
 * defconfig). Mainline serves it with wcnss_ctrl ("qcom,wcnss") for the SMD
 * bring-up plus wcn36xx for the MAC -- no custom driver. Firmware
 * (wcnss.mdt/.bXX) comes from stock; see firmware/README.md.
 */
#ifndef LG_MODEM_MSS_H
#define LG_MODEM_MSS_H

#define LV517_MODEM_FW		"lg/lv517/modem.mdt"
#define LV517_WCNSS_FW		"lg/lv517/wcnss.mdt"

#endif /* LG_MODEM_MSS_H */
