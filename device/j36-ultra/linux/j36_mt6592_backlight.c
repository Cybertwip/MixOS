// SPDX-License-Identifier: GPL-2.0-only
/*
 * J36 Ultra panel backlight -- the MT6592 BLS block in front of a TPS61161.
 *
 * WHY THIS FILE EXISTS. Until it did, this board had no backlight device at all:
 * /sys/class/backlight was empty, the dashboard's Display page had nothing to
 * write to, and the panel simply ran at whatever level the MVII LK left it at
 * for the whole life of the boot. The device tree has carried a pwm-backlight
 * node the whole time and it has never once probed -- it consumes &disp_pwm and
 * &gpio, and neither of those has a driver in this profile, so an enabled
 * pwm-backlight does not fail, it defers until driver_deferred_probe_timeout
 * expires ten seconds into every boot. That node stays disabled. This driver
 * takes its place by binding the PWM block directly.
 *
 * WHAT THE HARDWARE IS. Three things in series, and the middle one is the only
 * one Linux can reach:
 *
 *   BLS at 0x1400a000  the MT6592 "backlight slice", a PWM generator inside
 *                      MMSYS. PWM_CON_1 holds a period in the low half and a
 *                      duty in the high half; the LK programs both to 1023,
 *                      which is a 10-bit scale at full on.
 *   GPIO pad 90        the BLS output, when the pad is muxed to mode 1. In
 *                      mode 0 it is a plain GPIO and the PWM goes nowhere.
 *   TPS61161           a white-LED boost driver whose CTRL pin is that pad. It
 *                      dims on the duty cycle of what it is fed, and it will
 *                      shut down if CTRL is held low for longer than a couple
 *                      of milliseconds -- which at this block's period is many
 *                      thousands of cycles away, so ordinary dimming is safe.
 *
 * THE STATE AT HANDOFF IS THE STATE THIS ADOPTS. The last thing the LK does
 * before it jumps to the kernel is mt6592_backlight_reassert(100), and that path
 * -- backlight.c's `full' branch -- muxes pad 90 to mode 1, writes period 1023
 * and duty 1023, and enables the block. So Linux inherits a live PWM at full
 * duty. probe() reads the duty back out of PWM_CON_1 and reports THAT as the
 * current brightness rather than picking a number and writing it, for the same
 * reason j36_jd9365_panel adopts the LK's panel instead of re-initialising it: a
 * driver that writes before it reads is a driver that can only ever be
 * discovered to be wrong by looking at the glass.
 *
 * WHAT IT DELIBERATELY DOES NOT COPY FROM THE LK. backlight.c has a second path
 * for anything under 100%: it puts pad 90 back into mode 0, drives it high as an
 * ordinary output, and -- the first time only -- sends the TPS61161's 32-pulse
 * one-wire wake train. That path does not dim. It cannot: a pad held statically
 * high is 100% duty by definition, and the pulse train it runs on the way there
 * is the EasyScale protocol, which on some panels latches a low current step and
 * leaves the backlight permanently dim (backlight.c says so itself, in the
 * comment above its `full' branch). The LK only ever needs full or park, so it
 * has never had to care. This driver only ever uses mode 1 and the duty field,
 * which is what the block is for.
 *
 * IT NEVER BLANKS ON ITS OWN. Not at remove, not at shutdown, not at unbind.
 * The panel is the only output this board has and the console is on it; a
 * backlight driver that goes dark when it is unloaded is a driver that can take
 * the machine away from whoever is trying to debug it. The one way to reach zero
 * is for something to ask for zero through the backlight class, which is a
 * deliberate act.
 */

#include <linux/backlight.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

/* ── the BLS block, at reg 0 of the node ──────────────────────────────────── */

#define J36_BLS_EN			0x0000
#define J36_BLS_ENABLE_BIT		BIT(16)
#define J36_BLS_PWM_CON_0		0x00a8
#define J36_BLS_PWM_CON_1		0x00ac
#define J36_BLS_DEBUG			0x00b0

/*
 * The three magic writes, taken verbatim from the LK's known-good sequence
 * rather than re-derived, because this block has no public documentation and the
 * board it is on is the only place these values have ever been proven.
 *
 * DEBUG bits [1:0] are the block's two "let the PWM out" enables -- with either
 * clear the counter runs and the pad does not move. CON_0 bits [25:16] are the
 * clock divider and bits [1:0] the source select; 2 and 2 are what the LK
 * writes, and they put the carrier far enough above the TPS61161's shutdown
 * timeout that no duty in range can trip it.
 */
#define J36_BLS_DEBUG_MASK		0x3
#define J36_BLS_DEBUG_VALUE		0x3
#define J36_BLS_CON_0_MASK		((0x3ffu << 16) | 0x3u)
#define J36_BLS_CON_0_VALUE		((2u << 16) | 0x2u)

/*
 * The period, and therefore the scale.
 *
 * 1023 is not a choice this driver gets to make: it is the value in PWM_CON_1's
 * low half when Linux starts, it is what the device tree's brightness-levels
 * were written against, and the duty field is compared against it by the
 * hardware. Ten bits of range on a panel this size is far more than the eye can
 * separate, so nothing is gained by rescaling it into something rounder.
 */
#define J36_BLS_PERIOD			1023
#define J36_BLS_DUTY_SHIFT		16
#define J36_BLS_DUTY_MASK		0xffffu

/* ── the GPIO controller, reached the way every other J36 driver reaches it ── */

/*
 * There is no gpiochip for MT6592 anywhere upstream, so the pad arrives as a
 * plain number plus a phandle to the register block -- the same idiom
 * j36_mt6592_input and j36_mt6592_usb_phy already use on this board. gpiod would
 * defer forever against a &gpio node that binds nothing.
 */
#define J36_GPIO_MODE_BASE		0x0600
#define J36_GPIO_MODE_STRIDE		0x0010
#define J36_GPIO_MODE_BITS		3
#define J36_GPIO_MODE_PER_REG		5
#define J36_GPIO_MODE_MASK		0x7
/* The highest pad the hardware decodes; past it the register arithmetic walks
 * off the end of the block into whatever is mapped next. */
#define J36_GPIO_PIN_MAX		168
/* Mode 1 on pad 90 is DISP_PWM. Mode 0 is plain GPIO, which is where the LK's
 * sub-100% path parks it and where the duty stops meaning anything. */
#define J36_PAD_MODE_DISP_PWM		1

/* ── MMSYS, whose clock gates the BLS counter lives behind ────────────────── */

/*
 * Write-1-to-ungate. Nothing in this kernel manages the MMSYS gates -- the
 * display nodes all take a fixed-factor disp_clk and there is no mt6592 MM clock
 * driver in this profile -- so they sit exactly as the LK left them, which is
 * open. Rewriting them here is therefore a no-op in the normal case and a repair
 * in the case where something has closed one, and it is the same pair of writes
 * the LK makes on the way into every backlight change.
 */
#define J36_MMSYS_CG_CLR0		0x0108
#define J36_MMSYS_CG_CLR1		0x0118
#define J36_MMSYS_CG_CLR0_VALUE		0x001fffffu
#define J36_MMSYS_CG_CLR1_VALUE		0x0000000fu

struct j36_backlight {
	struct device *dev;
	void __iomem *bls;
	/* Both optional. A NULL gpio means the pad is left however the LK muxed
	 * it, which at handoff is mode 1 and therefore already right; a NULL
	 * mmsys means the gates are trusted rather than re-opened. */
	void __iomem *gpio;
	void __iomem *mmsys;
	u32 pad;
	bool have_pad;
	/* The readback complaint is said once. A backlight whose every slider
	 * notch printed a line would bury the log it is trying to be. */
	bool readback_warned;
};

static void j36_update(void __iomem *base, u32 offset, u32 clear, u32 set)
{
	u32 value = readl(base + offset);

	value &= ~clear;
	value |= set;
	writel(value, base + offset);
}

static void j36_pad_set_mode(struct j36_backlight *b, u32 pad, u32 mode)
{
	u32 reg = J36_GPIO_MODE_BASE +
		  (pad / J36_GPIO_MODE_PER_REG) * J36_GPIO_MODE_STRIDE;
	u32 shift = (pad % J36_GPIO_MODE_PER_REG) * J36_GPIO_MODE_BITS;

	j36_update(b->gpio, reg, J36_GPIO_MODE_MASK << shift,
		   (mode & J36_GPIO_MODE_MASK) << shift);
}

/*
 * The whole sequence, every time, and not just the duty.
 *
 * The LK has a function called mt6592_backlight_reassert() whose entire purpose
 * is to rewrite registers that have not changed, and the comment on it says why:
 * the BLS shares this block with the video path, which can reset the PWM state
 * underneath it. Four register writes once per user action is nothing, and the
 * alternative is a slider that works until something else touches the display
 * and then silently stops.
 */
static int j36_backlight_apply(struct j36_backlight *b, u32 duty)
{
	u32 want, got;

	if (duty > J36_BLS_PERIOD)
		duty = J36_BLS_PERIOD;

	if (b->mmsys) {
		writel(J36_MMSYS_CG_CLR0_VALUE, b->mmsys + J36_MMSYS_CG_CLR0);
		writel(J36_MMSYS_CG_CLR1_VALUE, b->mmsys + J36_MMSYS_CG_CLR1);
	}

	if (b->gpio && b->have_pad)
		j36_pad_set_mode(b, b->pad, J36_PAD_MODE_DISP_PWM);

	j36_update(b->bls, J36_BLS_DEBUG, J36_BLS_DEBUG_MASK, J36_BLS_DEBUG_VALUE);
	j36_update(b->bls, J36_BLS_PWM_CON_0, J36_BLS_CON_0_MASK, J36_BLS_CON_0_VALUE);

	want = J36_BLS_PERIOD | (duty << J36_BLS_DUTY_SHIFT);
	writel(want, b->bls + J36_BLS_PWM_CON_1);
	j36_update(b->bls, J36_BLS_EN, 0, J36_BLS_ENABLE_BIT);

	/*
	 * Read it back, because the one failure this driver can plausibly have
	 * is invisible from here otherwise: if the BLS clock ever were gated,
	 * every write above would be accepted by the bus and dropped by the
	 * block, and the symptom on the glass would be a slider that moves and
	 * a panel that does not. Said once, with both numbers, so the report is
	 * "brightness does nothing" plus a line in dmesg that explains it.
	 */
	got = readl(b->bls + J36_BLS_PWM_CON_1);
	if (got != want) {
		if (!b->readback_warned) {
			b->readback_warned = true;
			dev_warn(b->dev,
				 "PWM_CON_1 does not hold: wrote 0x%08x, read 0x%08x -- the BLS block is not taking writes, so brightness will not move\n",
				 want, got);
		}
		return -EIO;
	}

	return 0;
}

/* ── the backlight class ──────────────────────────────────────────────────── */

static int j36_backlight_update_status(struct backlight_device *bd)
{
	struct j36_backlight *b = bl_get_data(bd);

	/*
	 * backlight_get_brightness() rather than props.brightness: it folds in
	 * the blank and power states, so "0 because somebody asked for 0" and
	 * "0 because the class was told to go dark" arrive here as the same
	 * number, which is the only thing the hardware can express anyway.
	 */
	return j36_backlight_apply(b, backlight_get_brightness(bd));
}

/*
 * actual_brightness, straight out of the register.
 *
 * Worth implementing rather than letting the core echo props.brightness back:
 * this is the number that is genuinely on the pad, so a sysfs read after a write
 * that the block dropped shows the old value instead of confirming a change that
 * did not happen.
 */
static int j36_backlight_get_brightness(struct backlight_device *bd)
{
	struct j36_backlight *b = bl_get_data(bd);
	u32 duty = (readl(b->bls + J36_BLS_PWM_CON_1) >> J36_BLS_DUTY_SHIFT) &
		   J36_BLS_DUTY_MASK;

	return min_t(u32, duty, J36_BLS_PERIOD);
}

static const struct backlight_ops j36_backlight_ops = {
	.update_status = j36_backlight_update_status,
	.get_brightness = j36_backlight_get_brightness,
};

/* ── probe ────────────────────────────────────────────────────────────────── */

/*
 * Same helper and same reasoning as j36_mt6592_input and j36_mt6592_usb_phy: the
 * region is NOT claimed. Both windows this reaches through a phandle are shared
 * -- the GPIO block with the input and USB PHY drivers, MMSYS with whatever the
 * DRM stack does -- and asking for exclusive ownership of either would fail the
 * probe of whichever driver happened to be second.
 */
static void __iomem *j36_iomap_phandle(struct device *dev, const char *property)
{
	struct device_node *node;
	struct resource resource;
	void __iomem *base;
	int ret;

	node = of_parse_phandle(dev->of_node, property, 0);
	if (!node)
		return ERR_PTR(-EINVAL);

	ret = of_address_to_resource(node, 0, &resource);
	of_node_put(node);
	if (ret)
		return ERR_PTR(ret);

	base = devm_ioremap(dev, resource.start, resource_size(&resource));
	if (!base)
		return ERR_PTR(-ENOMEM);
	return base;
}

static int j36_backlight_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct backlight_properties props = { 0 };
	struct backlight_device *bd;
	struct j36_backlight *b;
	u32 con1, duty;

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;

	b->bls = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(b->bls))
		return dev_err_probe(dev, PTR_ERR(b->bls), "map the BLS window\n");

	/*
	 * Optional, and the absence is survivable rather than fatal: the LK
	 * hands the pad over already muxed to DISP_PWM, so a device tree with no
	 * phandle gives a working backlight on this boot chain and loses only
	 * the ability to put the pad back if something else moved it.
	 */
	b->gpio = j36_iomap_phandle(dev, "j36,gpio-controller");
	if (IS_ERR(b->gpio)) {
		dev_info(dev, "no j36,gpio-controller phandle: the pad is left as the LK muxed it\n");
		b->gpio = NULL;
	} else if (of_property_read_u32(dev->of_node, "mediatek,pwm-pin", &b->pad)) {
		dev_info(dev, "no mediatek,pwm-pin: the pad is left as the LK muxed it\n");
	} else if (b->pad > J36_GPIO_PIN_MAX) {
		dev_warn(dev, "mediatek,pwm-pin %u is above pad %u: ignored\n",
			 b->pad, J36_GPIO_PIN_MAX);
	} else {
		b->have_pad = true;
	}

	b->mmsys = j36_iomap_phandle(dev, "j36,mmsys-controller");
	if (IS_ERR(b->mmsys)) {
		dev_info(dev, "no j36,mmsys-controller phandle: the MM clock gates are trusted as the LK left them\n");
		b->mmsys = NULL;
	}

	/*
	 * Adopt, do not assert. The duty in the register is what the panel is
	 * actually running at, and reporting it as the starting brightness means
	 * the first thing anyone reads out of sysfs is the truth about the glass
	 * in front of them rather than this driver's opinion.
	 *
	 * A duty of zero is the one reading that cannot be adopted: it would
	 * mean the panel is dark, and if it were, nobody would be looking at
	 * this. Far more likely is a block that has not been programmed on this
	 * boot at all, so full is the safe answer -- it is also what every path
	 * through the LK leaves behind.
	 */
	con1 = readl(b->bls + J36_BLS_PWM_CON_1);
	duty = (con1 >> J36_BLS_DUTY_SHIFT) & J36_BLS_DUTY_MASK;
	if (duty == 0 || duty > J36_BLS_PERIOD)
		duty = J36_BLS_PERIOD;

	props.type = BACKLIGHT_RAW;
	props.max_brightness = J36_BLS_PERIOD;
	props.brightness = duty;
	/*
	 * props.scale says what the numbers mean to a human, and LINEAR is the
	 * honest answer: the duty is linear in LED current. It is emphatically
	 * not linear in perceived brightness, which is why the dashboard's
	 * slider bends it and this driver does not -- a gamma curve baked in
	 * here would be a curve nothing could see past.
	 *
	 * props.power is left at its zeroed value on purpose. Zero is "on"
	 * under both the old FB_BLANK_UNBLANK spelling and the newer
	 * BACKLIGHT_POWER_ON one, so not naming it is the one way to say it
	 * that does not care which kernel this is built against.
	 */
	props.scale = BACKLIGHT_SCALE_LINEAR;

	bd = devm_backlight_device_register(dev, "j36-backlight", dev, b,
					    &j36_backlight_ops, &props);
	if (IS_ERR(bd))
		return dev_err_probe(dev, PTR_ERR(bd), "register the backlight\n");

	platform_set_drvdata(pdev, b);

	/*
	 * No apply() here, and that is deliberate. The hardware is already at
	 * `duty' -- that is where the number came from -- so writing it back
	 * would buy nothing and would put the first PWM programming of the boot
	 * inside probe, where a mistake takes the panel out before there is a
	 * console to say so on. The first write happens when something asks for
	 * a different level.
	 */
	if (b->have_pad)
		dev_info(dev, "adopted the LK's backlight at %u of %u, DISP_PWM on pad %u\n",
			 duty, J36_BLS_PERIOD, b->pad);
	else
		dev_info(dev, "adopted the LK's backlight at %u of %u, pad left as it was found\n",
			 duty, J36_BLS_PERIOD);

	return 0;
}

static const struct of_device_id j36_backlight_of_match[] = {
	{ .compatible = "j36,mt6592-disp-pwm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, j36_backlight_of_match);

static struct platform_driver j36_backlight_driver = {
	.probe = j36_backlight_probe,
	.driver = {
		.name = "j36-mt6592-backlight",
		.of_match_table = j36_backlight_of_match,
	},
};
module_platform_driver(j36_backlight_driver);

MODULE_DESCRIPTION("J36 Ultra panel backlight (MT6592 BLS into a TPS61161)");
MODULE_LICENSE("GPL v2");
