// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPPO MT6877 keys: volume up/down polled from the GPIO block.
 *
 * WHY POLLED MMIO AND NOT gpio-keys/EINT. The volume keys are GPIO120 (up)
 * and GPIO114 (down), configured as EINTs by the vendor. EINT needs the
 * pinctrl driver, and mainline has no mt6877 pinctrl data -- that port is a
 * 200-pin table for another day. The GPIO DIN registers, on the other hand,
 * are readable from the moment the kernel maps them (Paris layout: DIN at
 * base+0x200, 32 pins per register, 0x10 stride -- see the di_range table in
 * the vendor pinctrl-mt6877.c). So this driver polls the two DIN bits at
 * 50 Hz and debounces in software, the way the J36 input driver polls its
 * matrix. When pinctrl lands, this driver is replaced by a gpio-keys node
 * and deleted; the DT compatible documents that ("...-polled").
 *
 * POWER IS NOT HERE. Power is the MT6359 PMIC key, reachable only through
 * pwrap+MFD. It arrives with the PMIC-keys binding (mainline) once the
 * pwrap patch in this directory is verified on hardware. Until then the
 * power button wakes nothing -- but it also cannot suspend the machine by
 * accident, which is the failure mode a guessed-at power key would have.
 */

#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/input.h>
#include <linux/input-polldev.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define OPPO_GPIO_DIN_BASE	0x200
#define OPPO_GPIO_PINS_PER_REG	32
#define OPPO_GPIO_REG_STRIDE	0x10
#define OPPO_KEYS_POLL_MS	20
#define OPPO_KEYS_DEBOUNCE	3 /* consecutive identical polls to accept */

struct oppo_key {
	unsigned int gpio;
	unsigned int code;
	unsigned int stable;
	bool pressed;
};

struct oppo_keys {
	void __iomem *gpio;
	struct oppo_key keys[2];
};

static bool oppo_key_level(struct oppo_keys *ok, unsigned int gpio)
{
	u32 reg = readl(ok->gpio + OPPO_GPIO_DIN_BASE +
			(gpio / OPPO_GPIO_PINS_PER_REG) * OPPO_GPIO_REG_STRIDE);

	/* Active low: the vendor arms pull-ups, idle reads 1. */
	return !(reg & BIT(gpio % OPPO_GPIO_PINS_PER_REG));
}

static void oppo_keys_poll(struct input_polled_dev *polldev)
{
	struct oppo_keys *ok = polldev->private;
	int i;

	for (i = 0; i < ARRAY_SIZE(ok->keys); i++) {
		struct oppo_key *k = &ok->keys[i];
		bool level = oppo_key_level(ok, k->gpio);

		if (level == k->pressed) {
			k->stable = 0;
			continue;
		}
		if (++k->stable < OPPO_KEYS_DEBOUNCE)
			continue;
		k->pressed = level;
		k->stable = 0;
		input_event(polldev->input, EV_KEY, k->code, level);
		input_sync(polldev->input);
	}
}

static int oppo_keys_probe(struct platform_device *pdev)
{
	struct oppo_keys *ok;
	struct input_polled_dev *polldev;
	struct device_node *np = pdev->dev.of_node;
	u32 gpios[2];
	int ret;

	if (of_property_read_u32_array(np, "oppo,key-pins", gpios, 2)) {
		/* The 20181 wiring, from oplus6877_20181_v1.dts. */
		gpios[0] = 120; /* VOL_UP */
		gpios[1] = 114; /* VOL_DOWN */
	}
	ok = devm_kzalloc(&pdev->dev, sizeof(*ok), GFP_KERNEL);
	if (!ok)
		return -ENOMEM;
	ok->gpio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ok->gpio))
		return PTR_ERR(ok->gpio);
	ok->keys[0].gpio = gpios[0];
	ok->keys[0].code = KEY_VOLUMEUP;
	ok->keys[1].gpio = gpios[1];
	ok->keys[1].code = KEY_VOLUMEDOWN;

	polldev = devm_input_allocate_polled_device(&pdev->dev);
	if (!polldev)
		return -ENOMEM;
	polldev->private = ok;
	polldev->poll = oppo_keys_poll;
	polldev->poll_interval = OPPO_KEYS_POLL_MS;
	polldev->input->name = "OPPO MT6877 Volume Keys";
	polldev->input->id.bustype = BUS_HOST;
	set_bit(EV_KEY, polldev->input->evbit);
	set_bit(KEY_VOLUMEUP, polldev->input->keybit);
	set_bit(KEY_VOLUMEDOWN, polldev->input->keybit);

	ret = input_register_polled_device(polldev);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, polldev);
	dev_info(&pdev->dev, "polling GPIO%u/GPIO%u for volume\n",
		 gpios[0], gpios[1]);
	return 0;
}

static const struct of_device_id oppo_keys_match[] = {
	{ .compatible = "oppo,mt6877-keys-polled" },
	{ }
};
MODULE_DEVICE_TABLE(of, oppo_keys_match);

static void oppo_keys_remove(struct platform_device *pdev)
{
	input_unregister_polled_device(platform_get_drvdata(pdev));
}

static struct platform_driver oppo_keys_driver = {
	.probe = oppo_keys_probe,
	.remove_new = oppo_keys_remove,
	.driver = {
		.name = "oppo-mt6877-keys",
		.of_match_table = oppo_keys_match,
	},
};
module_platform_driver(oppo_keys_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("OPPO MT6877 polled volume keys");
