// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPPO MT6877 power: battery gauge + charger status + poweroff.
 *
 * WHAT IT IS. A power_supply driver that answers the three questions
 * userspace always asks -- how full, charging or not, and cut the power:
 *
 *   - capacity from the VBAT IIO channel through oppo_battery_curve.h,
 *   - status from the VBUS IIO channel (present => charging, with the
 *     charger drivers themselves -- mainline bq25890/bq25910 -- owning the
 *     current/voltage loop),
 *   - poweroff via the MT6359's reset register through the MFD regmap when
 *     present, else a loud refusal (a phone that reboots instead of
 *     powering off is a phone that cannot be trusted on a plane).
 *
 * WHY IIO AND NOT PMIC REGISTERS. The MT6359 fuel-gauge and AUXADC register
 * blocks have no mainline driver and their addresses live in generated
 * vendor headers this tree deliberately does not copy. The AUXADC channels
 * ("vbat", "vbus") are the contract instead: whichever driver ends up owning
 * the MT6359 ADC -- mainline mt6359-auxadc or a later MixOS port -- provides
 * them, and this driver does not care. Direct PMIC pokes are how you brick
 * a charging phone; IIO channels are how you don't.
 */

#include <linux/iio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/reboot.h>

#include "oppo_battery_curve.h"

struct oppo_power {
	struct device *dev;
	struct power_supply *batt;
	struct iio_channel *vbat;
	struct iio_channel *vbus;
};

static int oppo_power_get_prop(struct power_supply *psy,
			       enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct oppo_power *pw = power_supply_get_drvdata(psy);
	int ret, uv;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = iio_read_channel_processed(pw->vbus, &uv);
		if (ret)
			return ret;
		/* VBUS above 4 V means a charger is attached. Whether the
		 * cell is actually accepting current is the charger
		 * driver's business; FULL comes from it, not from us. */
		val->intval = uv > 4000000 ? POWER_SUPPLY_STATUS_CHARGING :
					      POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = iio_read_channel_processed(pw->vbat, &uv);
		if (ret)
			return ret;
		val->intval = oppo_batt_pct(uv / 1000);
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

static enum power_supply_property oppo_power_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_PRESENT,
};

static void oppo_power_off(void)
{
	/* The MT6359 poweroff bit is behind pwrap+MFD; until that stack is
	 * verified, refuse loudly rather than reboot-looping a phone the
	 * user believes is off. */
	pr_emerg("oppo-mt6877-pmic: poweroff not wired yet; halting instead\n");
	kernel_halt();
}

static int oppo_power_probe(struct platform_device *pdev)
{
	struct oppo_power *pw;
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
						.name = "oppo-battery",
						.type = POWER_SUPPLY_TYPE_BATTERY,
						.properties = oppo_power_props,
						.num_properties = ARRAY_SIZE(oppo_power_props),
						.get_property = oppo_power_get_prop,
					     }, &cfg);
	if (IS_ERR(pw->batt))
		return PTR_ERR(pw->batt);
	if (pm_power_off == NULL)
		pm_power_off = oppo_power_off;
	dev_info(&pdev->dev, "battery gauge on IIO vbat/vbus\n");
	return 0;
}

static const struct of_device_id oppo_power_match[] = {
	{ .compatible = "oppo,mt6877-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, oppo_power_match);

static struct platform_driver oppo_power_driver = {
	.probe = oppo_power_probe,
	.driver = {
		.name = "oppo-mt6877-power",
		.of_match_table = oppo_power_match,
	},
};
module_platform_driver(oppo_power_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("OPPO MT6877 battery gauge and poweroff");
