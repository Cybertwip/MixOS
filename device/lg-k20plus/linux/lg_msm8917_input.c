// SPDX-License-Identifier: GPL-2.0-only
/*
 * LG K20 Plus keys: volume rocker + hall sensor polled from TLMM.
 *
 * WHY POLLED MMIO AND NOT gpio-keys. Same answer as the OPPO keys driver:
 * no mainline pinctrl data for this SoC, so no gpiochip and no interrupts.
 * The TLMM read path is trivial -- pin N's input bit is bit 0 of
 * base+N*0x1000+0x4 (pinctrl-msm8917.c: REG_SIZE 0x1000, io_reg +0x4,
 * in_bit 0) -- so this driver polls the two pins at 50 Hz with software
 * debounce and reports through the input subsystem.
 *
 * Pins and codes come from the DTS (board/keys_lv517.h): GPIO91 is the
 * rocker (VOL_UP, or VOL_DOWN on rev-0 where the PMIC takes VOL_UP) and
 * GPIO12 is the hall sensor (EV_SW/222, the vendor's type+code).
 */

#include <linux/input.h>
#include <linux/input-polldev.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define LV517_TLMM_STRIDE	0x1000
#define LV517_TLMM_IO_OFF	0x4
#define LV517_KEYS_POLL_MS	20
#define LV517_KEYS_DEBOUNCE	3

struct lv517_key {
	unsigned int gpio;
	unsigned int type;
	unsigned int code;
	unsigned int stable;
	bool pressed;
};

struct lv517_keys {
	void __iomem *tlmm;
	struct lv517_key keys[2];
};

static bool lv517_key_level(struct lv517_keys *lk, unsigned int gpio)
{
	u32 reg = readl(lk->tlmm + gpio * LV517_TLMM_STRIDE + LV517_TLMM_IO_OFF);

	/* Active low: vendor DTS marks both gpios 0x1. */
	return !(reg & BIT(0));
}

static void lv517_keys_poll(struct input_polled_dev *polldev)
{
	struct lv517_keys *lk = polldev->private;
	int i;

	for (i = 0; i < ARRAY_SIZE(lk->keys); i++) {
		struct lv517_key *k = &lk->keys[i];
		bool level = lv517_key_level(lk, k->gpio);

		if (level == k->pressed) {
			k->stable = 0;
			continue;
		}
		if (++k->stable < LV517_KEYS_DEBOUNCE)
			continue;
		k->pressed = level;
		k->stable = 0;
		input_event(polldev->input, k->type, k->code, level);
		input_sync(polldev->input);
	}
}

static int lv517_keys_probe(struct platform_device *pdev)
{
	struct lv517_keys *lk;
	struct input_polled_dev *polldev;
	struct device_node *np = pdev->dev.of_node;
	u32 pins[2], codes[2];
	int ret;

	if (of_property_read_u32_array(np, "lge,key-pins", pins, 2) ||
	    of_property_read_u32_array(np, "lge,key-codes", codes, 2)) {
		pins[0] = 91; codes[0] = 115; /* VOL_UP */
		pins[1] = 12; codes[1] = 222; /* hall */
	}
	lk = devm_kzalloc(&pdev->dev, sizeof(*lk), GFP_KERNEL);
	if (!lk)
		return -ENOMEM;
	lk->tlmm = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(lk->tlmm))
		return PTR_ERR(lk->tlmm);
	lk->keys[0].gpio = pins[0];
	lk->keys[0].type = EV_KEY;
	lk->keys[0].code = codes[0];
	lk->keys[1].gpio = pins[1];
	lk->keys[1].type = EV_SW;
	lk->keys[1].code = codes[1];

	polldev = devm_input_allocate_polled_device(&pdev->dev);
	if (!polldev)
		return -ENOMEM;
	polldev->private = lk;
	polldev->poll = lv517_keys_poll;
	polldev->poll_interval = LV517_KEYS_POLL_MS;
	polldev->input->name = "LG K20 Plus Keys";
	polldev->input->id.bustype = BUS_HOST;
	set_bit(EV_KEY, polldev->input->evbit);
	set_bit(EV_SW, polldev->input->evbit);
	set_bit(codes[0], polldev->input->keybit);
	set_bit(codes[1], polldev->input->swbit);

	ret = input_register_polled_device(polldev);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, polldev);
	dev_info(&pdev->dev, "polling TLMM GPIO%u/GPIO%u\n", pins[0], pins[1]);
	return 0;
}

static const struct of_device_id lv517_keys_match[] = {
	{ .compatible = "lge,lv517-keys-polled" },
	{ }
};
MODULE_DEVICE_TABLE(of, lv517_keys_match);

static struct platform_driver lv517_keys_driver = {
	.probe = lv517_keys_probe,
	.driver = {
		.name = "lg-lv517-keys",
		.of_match_table = lv517_keys_match,
	},
};
module_platform_driver(lv517_keys_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("LG K20 Plus polled keys (TLMM MMIO)");
