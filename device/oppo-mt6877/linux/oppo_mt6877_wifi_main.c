// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPPO MT6877 Wi-Fi main: SDIO HIF glue + the four-stage bring-up order.
 *
 * PROBE ORDER (enforced by load.order, defended by -EPROBE_DEFER):
 *   1. oppo_mt6877_consys.ko powers the conn_infra block,
 *   2. this driver maps the CONSYS CFG window and runs the WMT boot,
 *   3. the SDIO function appears; command/response queues attach here,
 *   4. cfg80211/wlan0 come up in the net stage.
 *
 * The SDIO command queue is deliberately small: one outstanding firmware
 * command at a time, serialized by a mutex, completed from the SDIO IRQ.
 * Throughput lives or dies in the TX/RX path, not here, and a queue depth of
 * one is a queue that cannot reorder scan against connect.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "oppo_mt6877_wifi.h"

static int oppo_wifi_probe(struct platform_device *pdev)
{
	struct oppo_wifi *w;
	int ret;

	w = devm_kzalloc(&pdev->dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->dev = &pdev->dev;
	platform_set_drvdata(pdev, w);

	/* Stage 1: the shared block. Defers cleanly when consys is staged
	 * later in load.order than this driver. */
	ret = oppo_consys_power_up(&pdev->dev);
	if (ret)
		return ret;

	w->consys_cfg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(w->consys_cfg)) {
		ret = PTR_ERR(w->consys_cfg);
		goto out_power;
	}

	/* Stage 2: firmware + WMT handshake. */
	ret = oppo_wifi_wmt_boot(w);
	if (ret)
		goto out_power;

	/* Stages 3+4: HIF queues, then cfg80211/wlan0. */
	ret = oppo_wifi_net_attach(w);
	if (ret)
		goto out_wmt;
	return 0;

out_wmt:
	oppo_wifi_wmt_shutdown(w);
out_power:
	oppo_consys_power_down(&pdev->dev);
	return ret;
}

static void oppo_wifi_remove(struct platform_device *pdev)
{
	struct oppo_wifi *w = platform_get_drvdata(pdev);

	oppo_wifi_net_detach(w);
	oppo_wifi_wmt_shutdown(w);
	oppo_consys_power_down(&pdev->dev);
}

static const struct of_device_id oppo_wifi_match[] = {
	{ .compatible = "oppo,mt6877-wifi" },
	{ }
};
MODULE_DEVICE_TABLE(of, oppo_wifi_match);

static struct platform_driver oppo_wifi_driver = {
	.probe = oppo_wifi_probe,
	.remove_new = oppo_wifi_remove,
	.driver = {
		.name = "oppo-mt6877-wifi",
		.of_match_table = oppo_wifi_match,
	},
};
module_platform_driver(oppo_wifi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("OPPO MT6877 CONSYS_6877 fullmac Wi-Fi");
