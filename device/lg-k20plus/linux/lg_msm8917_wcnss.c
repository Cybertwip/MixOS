// SPDX-License-Identifier: GPL-2.0-only
/*
 * LG K20 Plus Wi-Fi bring-up: Pronto (WCN3660) PIL boot.
 *
 * WHAT IT IS. The WCN3660 firmware (wcnss.mdt + segments) has to be
 * authenticated and started through TrustZone PIL before the SMD control
 * channel exists. Mainline's wcnss_ctrl speaks the channel and wcn36xx
 * speaks the MAC, but nothing in mainline boots Pronto on this SoC family
 * -- so this driver does it: load the .mdt into the wcnss reserved region
 * with the MDT loader, run PAS init/auth/reset (peripheral 6, PAS_WCNSS
 * from the reference subsys-pil-tz.c), wait for the WCNSS_CTRL channel,
 * and then get out of the way. From there on, wlan0 is mainline's.
 *
 * POWER. Pronto's rails and clocks are left on by the LK on this phone;
 * this driver does not own a regulator or a GCC clock and says so at
 * probe. If a board revision powers Pronto down in LK, this is the driver
 * that grows the regulator calls -- the failure mode (PIL timeout with
 * the region mapped) points straight here.
 *
 * Firmware: /lib/firmware/lg/lv517/wcnss.mdt + wcnss.bXX, from stock
 * (firmware/README.md). Missing blobs fail probe with -ENOENT and the
 * extraction pointer, never silently.
 */

#include <linux/firmware.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/mdt_loader.h>

#define LV517_WCNSS_FW		"lg/lv517/wcnss.mdt"
#define LV517_PAS_WCNSS		6

struct lv517_wcnss {
	struct device *dev;
	struct qcom_scm_pas_metadata pas_ctx;
	phys_addr_t mem_phys;
	void *mem_region;
	size_t mem_size;
};

static int lv517_wcnss_probe(struct platform_device *pdev)
{
	struct lv517_wcnss *w;
	struct reserved_mem *rmem;
	const struct firmware *fw;
	phys_addr_t reloc_base;
	int ret;

	w = devm_kzalloc(&pdev->dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->dev = &pdev->dev;
	platform_set_drvdata(pdev, w);

	rmem = of_reserved_mem_lookup(pdev->dev.of_node);
	if (!rmem) {
		dev_err(&pdev->dev, "no memory-region (wcnss carveout)\n");
		return -ENODEV;
	}
	w->mem_phys = rmem->base;
	w->mem_size = rmem->size;
	w->mem_region = devm_ioremap_wc(&pdev->dev, w->mem_phys, w->mem_size);
	if (!w->mem_region)
		return -ENOMEM;

	ret = request_firmware(&fw, LV517_WCNSS_FW, &pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "missing %s: extract it from stock (firmware/README.md)\n",
			LV517_WCNSS_FW);
		return ret;
	}
	ret = qcom_mdt_pas_init(&pdev->dev, fw, LV517_WCNSS_FW, LV517_PAS_WCNSS,
				w->mem_phys, &w->pas_ctx);
	if (ret) {
		dev_err(&pdev->dev, "PAS init failed: %d\n", ret);
		goto out_release;
	}
	ret = qcom_mdt_load(&pdev->dev, fw, LV517_WCNSS_FW, LV517_PAS_WCNSS,
			    w->mem_region, w->mem_phys, w->mem_size,
			    &reloc_base);
	if (ret) {
		dev_err(&pdev->dev, "segment load failed: %d\n", ret);
		goto out_release;
	}
	ret = qcom_scm_pas_auth_and_reset(LV517_PAS_WCNSS);
	if (ret) {
		dev_err(&pdev->dev, "PIL auth+reset failed: %d (power? see above)\n",
			ret);
		goto out_release;
	}
	dev_info(&pdev->dev, "Pronto running; wcnss_ctrl/wcn36xx take it from here\n");
	ret = 0;
out_release:
	release_firmware(fw);
	return ret;
}

static void lv517_wcnss_remove(struct platform_device *pdev)
{
	qcom_scm_pas_shutdown(LV517_PAS_WCNSS);
}

static const struct of_device_id lv517_wcnss_match[] = {
	{ .compatible = "lge,pronto-wcnss" },
	{ }
};
MODULE_DEVICE_TABLE(of, lv517_wcnss_match);

static struct platform_driver lv517_wcnss_driver = {
	.probe = lv517_wcnss_probe,
	.remove_new = lv517_wcnss_remove,
	.driver = {
		.name = "lg-pronto-wcnss",
		.of_match_table = lv517_wcnss_match,
	},
};
module_platform_driver(lv517_wcnss_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("LG K20 Plus Pronto WCNSS PIL boot");
