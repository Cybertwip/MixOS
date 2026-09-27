// SPDX-License-Identifier: GPL-2.0-only
/*
 * LG K20 Plus power: battery gauge + poweroff.
 *
 * WHAT IT IS. The same IIO-channel gauge contract as the OPPO power driver:
 * capacity from the "vbat" channel through lg_battery_curve.h, status from
 * "vbus", present always true. On this phone the channels come from the
 * PMI8950 ADC (mainline qcom-spmi-adc5/rradc class -- the exact PMI8950
 * binding is the CHECK in the DTS, and this driver defers until it binds).
 * Charging current/voltage is the charger driver's business (mainline
 * pm8941-charger family); poweroff goes through PSHOLD (qcom,pshold),
 * with the same halt-not-reboot refusal as the OPPO driver until then.
 */

#include <linux/iio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/reboot.h>

#include "lg_battery_curve.h"

struct lv517_power {
	struct device *dev;
	struct power_supply *batt;
	struct iio_channel *vbat;
	struct iio_channel *vbus;
};

static int lv517_power_get_prop(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct lv517_power *pw = power_supply_get_drvdata(psy);
	int ret, uv;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = iio_read_channel_processed(pw->vbus, &uv);
		if (ret)
			return ret;
		val->intval = uv > 4000000 ? POWER_SUPPLY_STATUS_CHARGING :
					      POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = iio_read_channel_processed(pw->vbat, &uv);
		if (ret)
			return ret;
		val->intval = lg_batt_pct(uv / 1000);
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = iio_read_channel_processed(pw->vbat, &uv);
		if (ret)
			return ret;
		val->intval = uv;
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	default:
		return -EINVAL;
	}
}

static enum power_supply_property lv517_power_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_PRESENT,
};

static void lv517_power_off(void)
{
	pr_emerg("lg-lv517-power: PSHOLD not wired yet; halting instead\n");
	kernel_halt();
}

static int lv517_power_probe(struct platform_device *pdev)
{
	struct lv517_power *pw;
	struct power_supply_config cfg = {};

	pw = devm_kzalloc(&pdev->dev, sizeof(*pw), GFP_KERNEL);
	if (!pw)
		return -ENOMEM;
	pw->dev = &pdev->dev;
	platform_set_drvdata(pdev, pw);

	pw->vbat = devm_iio_channel_get(&pdev->dev, "vbat");
	if (IS_ERR(pw->vbat))
		return dev_err_probe(&pdev->dev, PTR_ERR(pw->vbat),
				     "no vbat IIO channel\n");
	pw->vbus = devm_iio_channel_get(&pdev->dev, "vbus");
	if (IS_ERR(pw->vbus))
		return dev_err_probe(&pdev->dev, PTR_ERR(pw->vbus),
				     "no vbus IIO channel\n");

	cfg.drv_data = pw;
	cfg.of_node = pdev->dev.of_node;
	pw->batt = devm_power_supply_register(&pdev->dev,
					     &(const struct power_supply_desc){
						.name = "lg-battery",
						.type = POWER_SUPPLY_TYPE_BATTERY,
						.properties = lv517_power_props,
						.num_properties = ARRAY_SIZE(lv517_power_props),
						.get_property = lv517_power_get_prop,
					     }, &cfg);
	if (IS_ERR(pw->batt))
		return PTR_ERR(pw->batt);
	if (pm_power_off == NULL)
		pm_power_off = lv517_power_off;
	dev_info(&pdev->dev, "battery gauge on IIO vbat/vbus\n");
	return 0;
}

static const struct of_device_id lv517_power_match[] = {
	{ .compatible = "lge,lv517-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, lv517_power_match);

static struct platform_driver lv517_power_driver = {
	.probe = lv517_power_probe,
	.driver = {
		.name = "lg-lv517-power",
		.of_match_table = lv517_power_match,
	},
};
module_platform_driver(lv517_power_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("LG K20 Plus battery gauge and poweroff");
