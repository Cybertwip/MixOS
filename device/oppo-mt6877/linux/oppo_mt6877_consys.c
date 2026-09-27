// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPPO MT6877 CONSYS (connectivity subsystem) power/reset driver.
 *
 * WHAT IT IS. The on-chip conn_infra block at 0x18000000 holds Wi-Fi, BT and
 * GPS behind one power domain and one reset line. Nothing answers -- not SDIO,
 * not the WMT channel -- until this block is powered, clocked and released
 * from reset, in that order. This driver does exactly that and then parks:
 * the wifi driver below consumes it (same device, probed first via load
 * order), runs the WMT handshake and owns the data path.
 *
 * WHY IT IS SEPARATE FROM THE WIFI DRIVER. Bluetooth and GPS share this block
 * and its firmware. When their drivers land they will attach to the same
 * "oppo,mt6877-consys" node instead of each replaying the power sequence and
 * racing each other at probe. One owner for the shared block, one client per
 * function -- the split the J36 wifi driver wishes it had made.
 *
 * Register outline from the consys@18000000 node of mt6877.dts and the
 * conn_infra RGU/CFG windows it maps (board/wifi_consys.h).
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/* conn_infra_rgu: bit 0 powers the block, bit 1 releases its reset. */
#define CONSYS_RGU_PWR		0x00
#define CONSYS_RGU_RST		0x04
#define CONSYS_RGU_PWR_ON	BIT(0)
#define CONSYS_RGU_RST_REL	BIT(1)
#define CONSYS_RGU_STATUS	0x08
#define CONSYS_RGU_RDY		BIT(0)

struct oppo_consys {
	void __iomem *rgu;
	void __iomem *cfg;
};

int oppo_consys_power_up(struct device *dev);
void oppo_consys_power_down(struct device *dev);
EXPORT_SYMBOL_GPL(oppo_consys_power_up);
EXPORT_SYMBOL_GPL(oppo_consys_power_down);

static struct oppo_consys *consys_singleton;

int oppo_consys_power_up(struct device *dev)
{
	struct oppo_consys *cs = consys_singleton;
	u32 status;
	int tries;

	if (!cs) {
		dev_err(dev, "consys block is not probed; load oppo_mt6877_consys first\n");
		return -EPROBE_DEFER;
	}
	/* Power, then clocks settle, then reset out -- the vendor order. */
	writel(CONSYS_RGU_PWR_ON, cs->rgu + CONSYS_RGU_PWR);
	udelay(100);
	writel(CONSYS_RGU_PWR_ON | CONSYS_RGU_RST_REL, cs->rgu + CONSYS_RGU_RST);
	for (tries = 0; tries < 100; tries++) {
		status = readl(cs->rgu + CONSYS_RGU_STATUS);
		if (status & CONSYS_RGU_RDY)
			return 0;
		udelay(50);
	}
	dev_err(dev, "consys never came ready (status %#x)\n", status);
	return -ETIMEDOUT;
}

void oppo_consys_power_down(struct device *dev)
{
	struct oppo_consys *cs = consys_singleton;

	/* Reset in first, then power off. Never touch it on the error path
	 * of a client that never powered up: writel on NULL is the bug. */
	if (!cs)
		return;
	writel(0, cs->rgu + CONSYS_RGU_RST);
	writel(0, cs->rgu + CONSYS_RGU_PWR);
	dev_info(dev, "consys powered down\n");
}

static int oppo_consys_probe(struct platform_device *pdev)
{
	struct oppo_consys *cs;

	cs = devm_kzalloc(&pdev->dev, sizeof(*cs), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;
	cs->rgu = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cs->rgu))
		return PTR_ERR(cs->rgu);
	/* The CFG window is optional on this pass; the power sequence only
	 * needs the RGU. A second reg entry can be added without changing
	 * this driver. */
	cs->cfg = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(cs->cfg))
		cs->cfg = NULL;
	consys_singleton = cs;
	platform_set_drvdata(pdev, cs);
	dev_info(&pdev->dev, "CONSYS_6877 block registered (not powered yet)\n");
	return 0;
}

static void oppo_consys_remove(struct platform_device *pdev)
{
	if (consys_singleton == platform_get_drvdata(pdev))
		consys_singleton = NULL;
}

static const struct of_device_id oppo_consys_match[] = {
	{ .compatible = "oppo,mt6877-consys" },
	{ }
};
MODULE_DEVICE_TABLE(of, oppo_consys_match);

static struct platform_driver oppo_consys_driver = {
	.probe = oppo_consys_probe,
	.remove_new = oppo_consys_remove,
	.driver = {
		.name = "oppo-mt6877-consys",
		.of_match_table = oppo_consys_match,
	},
};
module_platform_driver(oppo_consys_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("OPPO MT6877 CONSYS power/reset owner");
