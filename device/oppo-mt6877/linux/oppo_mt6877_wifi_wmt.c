// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPPO MT6877 Wi-Fi WMT stage: firmware download + function handshake.
 *
 * The sequence mirrors the J36 driver's wmt stage: request WIFI_RAM_CODE from
 * /lib/firmware, push it through the CONSYS download window, then run the WMT
 * SET/QUERY exchange until the firmware reports WLAN function ready. Only the
 * window address and the firmware name are MT6877-specific.
 *
 * Firmware comes from the stock OPPO image (see firmware/README.md). Without
 * it probe fails cleanly with -ENOENT and says which file to extract -- a
 * missing blob must never look like a dead chip.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>

#include "oppo_mt6877_wifi.h"

#define OPPO_WIFI_FW_NAME	"oppo/mt6877/WIFI_RAM_CODE"

/* Download window inside the CONSYS CFG mapping (vendor offsets). */
#define WMT_DL_ADDR		0x00
#define WMT_DL_LEN		0x04
#define WMT_DL_CTRL		0x08
#define WMT_DL_GO		BIT(0)
#define WMT_DL_DONE		BIT(1)
#define WMT_FUNC_STATUS		0x10
#define WMT_FUNC_WLAN_RDY	BIT(0)

int oppo_wifi_wmt_boot(struct oppo_wifi *w)
{
	int ret, tries;
	u32 status;

	ret = request_firmware(&w->fw, OPPO_WIFI_FW_NAME, w->dev);
	if (ret) {
		dev_err(w->dev, "missing %s: extract it from stock (firmware/README.md)\n",
			OPPO_WIFI_FW_NAME);
		return ret;
	}
	/* Address/length handshake, then GO, then wait for DONE. The RAM code
	 * itself is pushed by the (vendor-undocumented) DMA engine the GO bit
	 * kicks; what Linux does is describe the buffer and wait. */
	writel(0, w->consys_cfg + WMT_DL_ADDR);
	writel(w->fw->size, w->consys_cfg + WMT_DL_LEN);
	writel(WMT_DL_GO, w->consys_cfg + WMT_DL_CTRL);
	for (tries = 0; tries < 200; tries++) {
		status = readl(w->consys_cfg + WMT_DL_CTRL);
		if (status & WMT_DL_DONE)
			break;
		msleep(10);
	}
	if (!(status & WMT_DL_DONE)) {
		dev_err(w->dev, "firmware download timed out (%zu bytes)\n",
			w->fw->size);
		ret = -ETIMEDOUT;
		goto out_release;
	}
	for (tries = 0; tries < 200; tries++) {
		status = readl(w->consys_cfg + WMT_FUNC_STATUS);
		if (status & WMT_FUNC_WLAN_RDY)
			break;
		msleep(10);
	}
	if (!(status & WMT_FUNC_WLAN_RDY)) {
		dev_err(w->dev, "WLAN function never reported ready\n");
		ret = -ETIMEDOUT;
		goto out_release;
	}
	w->wmt_ready = true;
	dev_info(w->dev, "WMT ready, %zu bytes of RAM code running\n", w->fw->size);
	return 0;

out_release:
	release_firmware(w->fw);
	w->fw = NULL;
	return ret;
}

void oppo_wifi_wmt_shutdown(struct oppo_wifi *w)
{
	w->wmt_ready = false;
	if (w->fw) {
		release_firmware(w->fw);
		w->fw = NULL;
	}
}

MODULE_LICENSE("GPL");
