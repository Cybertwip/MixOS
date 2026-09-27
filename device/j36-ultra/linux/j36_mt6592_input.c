// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal J36 Ultra input adapter for MediaTek MT6592.
 *
 * This intentionally mirrors the polling paths in PowerEngine
 * OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers/mt6592_keys.c:
 *   - direct active-low buttons are read from GPIO DIN, with a pull-up armed so
 *     "active low" has a defined idle to be low against;
 *   - matrix buttons are read from the five KPD_MEM scan words;
 *   - joystick axes are read from AUXADC channels after a fresh stop/start.
 *
 * IT DOES WRITE PINMUX, and it has to. This driver used to say "no pinmux writes
 * are performed, preserving the preloader/LK setup", and that was a description
 * of a broken keypad: the boot chain leaves three of the block's eight pads
 * parked as plain GPIO -- KPROW3 (11), KPCOL3 (12) and KPCOL4 (2) -- so the block
 * scans two rows against three columns and only matrix bits {0,1,2,9,10,11} ever
 * change. VOL-, VOL+, SELECT, START, MENU, R2 and A read as never pressed, with a
 * perfectly correct keymap sitting above them. The pads and their per-pad modes
 * come from the keypad node (j36,kpd-strobe-pads / -sense-pads), and the rule is
 * as narrow as MVII's: touch a pad only if its mode is wrong, and log the before
 * and after of every write. A pad already in its wanted mode is left exactly as
 * found, floating sense line or not.
 *
 * AND ONE PAD CANNOT BE READ AT ALL WITHOUT DRIVING IT. GPIO 93 is D-pad UP's
 * EINT (it is KPROW2's pad, which is why matrix row 2 is dead on this board) and
 * it reads 0 with the internal pull-up armed and verified in the register
 * readback, held or released -- something loads it harder than that resistor can
 * fight. A pad that already reads 0 has nowhere to move when its switch closes,
 * so it gets driven high for about a microsecond per poll and sampled while
 * driven. Which pads need that is MEASURED at probe, not hardcoded: every mapped
 * button pad is armed, given time to settle, and read. That keeps a board
 * revision which populates the missing pull-up off this path entirely.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define J36_GPIO_DIR_BASE             0x0000
#define J36_GPIO_PULLEN_BASE          0x0100
#define J36_GPIO_PULLSEL_BASE         0x0200
#define J36_GPIO_DOUT_BASE            0x0400
#define J36_GPIO_DIN_BASE             0x0500
#define J36_GPIO_BANK_STRIDE          0x0010
#define J36_GPIO_PINS_PER_BANK        16
/* Every 16-pin GPIO register has a write-1-to-set at +4 and a write-1-to-reset at
 * +8, so one pad is changed without reading the register and without disturbing
 * its neighbours. MODE is the exception and is read-modify-written below. */
#define J36_GPIO_BANK_SET             0x0004
#define J36_GPIO_BANK_RST             0x0008
#define J36_GPIO_MODE_BASE            0x0600
#define J36_GPIO_MODE_STRIDE          0x0010
#define J36_GPIO_MODE_BITS            3
#define J36_GPIO_MODE_PER_REG         5
#define J36_GPIO_MODE_MASK            0x7
/* The highest pad the hardware decodes: both the preloader's mt_set_gpio_mode and
 * stock LK's reject anything above this before touching a register. */
#define J36_GPIO_MAX                  168
/* Roughly a microsecond. Long enough for a pad to settle against its own few tens
 * of picofarads, short enough that driving a held button's pad to ground for the
 * duration is a duty cycle under a thousandth of the 5 ms poll. */
#define J36_GPIO_SETTLE_US            1
#define J36_PULLUP_SETTLE_US          64
#define J36_PAD_MAX                   16

#define J36_KPD_MEM1                  0x0004
#define J36_KPD_DEBOUNCE              0x0018
#define J36_KPD_SEL                   0x0020
#define J36_KPD_EN                    0x0024
#define J36_KPD_DEBOUNCE_DEFAULT      0x0400
#define J36_KPD_DEBOUNCE_MASK         0x3fff
#define J36_KPD_SEL_DOUBLE_KEY        BIT(0)

/*
 * PWRAP, WACS2 channel only. The full bring-up from reset is not here and is not
 * needed: the preloader and LK both leave the wrapper at INIT_DONE, and this
 * driver checks that rather than assuming it. If the wrapper is not ready the
 * failure is logged and the probe continues -- if the PMIC bus is broken, the
 * buttons are the least of it, and bailing out would also skip the KP_EN write
 * that at least leaves the block as the boot chain left it.
 */
#define J36_PWRAP_WACS2_CMD           0x009c
#define J36_PWRAP_WACS2_RDATA         0x00a0
#define J36_PWRAP_WACS2_VLDCLR        0x00a4
#define J36_PWRAP_FSM_IDLE            0x0
#define J36_PWRAP_FSM_WFVLDCLR        0x6
#define J36_PWRAP_POLL_LIMIT          10000

/* MT6323 register 0x40, bit 0: the keypad's 32 kHz clock gate. Read-modify-write
 * rather than an assignment, because 0x40 gates more than the keypad and the
 * vendor only ever clears this one bit. */
#define J36_PMIC_KPD_CLK_GATE_REG     0x0040
#define J36_PMIC_KPD_CLK_GATE_BIT     BIT(0)

#define J36_AUXADC_CON1_SET           0x0008
#define J36_AUXADC_CON1_CLR           0x000c
#define J36_AUXADC_CON2               0x0010
#define J36_AUXADC_CON2_BUSY          BIT(0)
#define J36_AUXADC_DAT0               0x0014
#define J36_AUXADC_DAT_STRIDE         0x0004
#define J36_AUXADC_DAT_READY          BIT(12)
#define J36_AUXADC_DAT_MASK           0x0fff
#define J36_AUXADC_CHANNELS           16
#define J36_AUXADC_POLL_LIMIT         96
#define J36_AUXADC_SETTLE_US          25

#define J36_PERI_PDN0_CLR             0x0010
#define J36_PERI_PDN0_AUXADC_BITS     0x0ff00000

#define J36_AXIS_FULL_SCALE           4096

/*
 * ── THE HEADPHONE JACK, AND WHY IT IS HERE OF ALL PLACES ────────────────────
 *
 * It is here because this is the only driver on the board that already has both
 * instruments a plug can be noticed with: the SoC AUXADC, which it converts four
 * channels of every five milliseconds for the sticks, and the GPIO block, with
 * the pull-up arming and the driven read the buttons needed. A detect line is
 * either a pad that changes level or a divider that changes voltage, and both of
 * those are a few lines on top of a loop that is already running.
 *
 * It is NOT in the audio driver, which is where it belongs on paper. That driver
 * has no ADC and no GPIO, so it would have to reach one of them through an
 * exported symbol -- and an exported symbol is a modprobe dependency, which on
 * this image means j36.audio would drag in whichever module owned the detect,
 * and the rule the whole payload is built on is that each command-line word can
 * be deleted without touching anything else. So the kernel NOTICES here and
 * userspace ACTS: this reports SW_HEADPHONE_INSERT on the gamepad's own input
 * device, and the dashboard -- which already owns "Speaker Amp" and "Headphone"
 * and shows both on Settings > Sound -- is what turns the speaker off. That also
 * keeps those two switches honest, which a kernel muting them behind the
 * dashboard's back would not.
 *
 * ── AND WHERE THE LINE IS, WHICH NOTHING IN THIS TREE KNOWS ─────────────────
 *
 * Every file that could have said has been asked and none of them says. The MVII
 * board header describes eighteen buttons, four ADC axes, the panel, the LED and
 * the charger pin, and it has no audio in it at all. Its keypad driver has no
 * jack. The vendor HAL drives the MT6592's ACCDET block, but through an ioctl on
 * /dev/accdet -- a kernel driver's interface, not a register map -- so there is
 * no base address to program even if the block is wired.
 *
 * So the wiring is a MEASUREMENT and this driver takes it. jack_scan=<ms> prints
 * every AUXADC channel and every GPIO input bank on a timer; plug the headphones
 * in and out a few times with dmesg -w running, and whichever number moves is the
 * line. Then jack_adc= or jack_gpio= on the insmod line pins it down, and the
 * detect starts working with no code change. If nothing moves, the board truly
 * brings no detect out, the scan says so in the only way that settles it, and the
 * two switches stay what they have always been.
 */
#define J36_JACK_NONE                 0
#define J36_JACK_ADC                  1
#define J36_JACK_GPIO                 2
#define J36_JACK_SCAN_MIN_MS          200
#define J36_JACK_DEBOUNCE_MS          120
/*
 * The window that means "plugged", in raw 12-bit counts, and the default is the
 * bottom sixth of the range for one reason: a jack's detect contact is a switch
 * to ground in every wiring anybody uses, so the plugged state is the LOW one and
 * the open state is wherever the pull-up sits. It is a window and not a threshold
 * because a four-pole headset puts the microphone's own resistance in the divider
 * and lands in the middle, which is a plug and not an open circuit -- widening
 * the top of this is how that board is handled without new code.
 */
#define J36_JACK_ADC_LOW              0
#define J36_JACK_ADC_HIGH             600

static int jack_adc = -1;
module_param(jack_adc, int, 0444);
MODULE_PARM_DESC(jack_adc,
		 "AUXADC channel carrying the headphone-detect line, or -1 for "
		 "none. Overrides the j36,jack-adc device-tree property. Find it "
		 "with jack_scan.");

static int jack_gpio = -1;
module_param(jack_gpio, int, 0444);
MODULE_PARM_DESC(jack_gpio,
		 "GPIO pad carrying the headphone-detect line, or -1 for none. "
		 "Overrides j36,jack-gpio. Takes precedence over jack_adc.");

static bool jack_gpio_active_low = true;
module_param(jack_gpio_active_low, bool, 0444);
MODULE_PARM_DESC(jack_gpio_active_low,
		 "the jack GPIO reads 0 with a plug in it (the usual wiring)");

static int jack_adc_low = J36_JACK_ADC_LOW;
module_param(jack_adc_low, int, 0644);
MODULE_PARM_DESC(jack_adc_low,
		 "bottom of the raw AUXADC window that means a plug is in");

static int jack_adc_high = J36_JACK_ADC_HIGH;
module_param(jack_adc_high, int, 0644);
MODULE_PARM_DESC(jack_adc_high,
		 "top of the raw AUXADC window that means a plug is in");

static int jack_debounce_ms = J36_JACK_DEBOUNCE_MS;
module_param(jack_debounce_ms, int, 0644);
MODULE_PARM_DESC(jack_debounce_ms,
		 "how long a new jack reading has to hold before it is believed");

/*
 * The instrument rather than a setting: echo 500 into it, work the jack, read
 * dmesg, echo 0 back. Writable like the window and the debounce above it, which
 * are the knobs an operator turns while the answer is still being found.
 *
 * The three that choose the SOURCE are the 0444 ones, and that is not caution --
 * the capability this driver advertises on its input device is decided once, at
 * probe, and evdev hands a node's capabilities to userspace when the node is
 * opened. A switch that appeared halfway through the life of an open descriptor
 * is not something a reader has any way to notice, so the choice has to be made
 * before there is anything to notice it with.
 */
static int jack_scan;
module_param(jack_scan, int, 0644);
MODULE_PARM_DESC(jack_scan,
		 "print every AUXADC channel and GPIO input bank this often in ms "
		 "(0 off, 200 minimum): the way to find which line the jack is on");

struct j36_key_map {
	u32 source;
	u32 code;
	bool state;
	/* Set by the probe below for a pad that will not idle high on its own
	 * pull-up, and therefore has to be driven high to be read at all. */
	bool driven;
};

struct j36_axis_map {
	u32 channel;
	u32 code;
	bool invert;
	u32 center;
	bool center_valid;
};

struct j36_input {
	struct device *dev;
	void __iomem *gpio;
	void __iomem *keypad;
	void __iomem *auxadc;
	void __iomem *pericfg;
	void __iomem *pwrap;
	struct input_dev *input;
	struct delayed_work poll_work;
	unsigned int poll_ms;

	struct j36_key_map *direct;
	unsigned int direct_count;
	struct j36_key_map *matrix;
	unsigned int matrix_count;
	struct j36_axis_map *axes;
	unsigned int axis_count;

	/* Pads the keypad block owns. Nothing else in this driver may drive, pull
	 * or read them as GPIO; the one exception is the mux pass at probe, which
	 * runs before the scanner is enabled and only on a pad whose mode is
	 * wrong. The last time a loop here ran over a list that happened to
	 * include a column pad, that column stopped scanning. */
	u32 owned_pads[J36_PAD_MAX];
	unsigned int owned_pad_count;

	u32 raw_min;
	u32 raw_max;
	u32 fallback_center;
	u32 deadzone;

	/* The jack. jack_source is J36_JACK_NONE unless a line was named, and
	 * everything below it is dead weight when it is. */
	unsigned int jack_source;
	u32 jack_line;			/* AUXADC channel, or GPIO pad */
	bool jack_active_low;
	bool jack_reported;		/* what SW_HEADPHONE_INSERT last said */
	bool jack_raw;			/* the last sample, undebounced */
	unsigned long jack_settled_at;	/* when jack_raw last changed */
	unsigned long jack_scan_at;
};

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

	/*
	 * Do not claim the region: these are shared SoC blocks whose eventual
	 * native providers may map the same registers. This adapter only reads the
	 * GPIO/KPD paths and performs the vendor AUXADC conversion sequence.
	 */
	base = devm_ioremap(dev, resource.start, resource_size(&resource));
	if (!base)
		return ERR_PTR(-ENOMEM);
	return base;
}

static int j36_read_map(struct device *dev, const char *property,
			unsigned int tuple_cells, struct j36_key_map **out,
			unsigned int *out_count)
{
	struct j36_key_map *map;
	u32 *cells;
	int count;
	int ret;
	unsigned int i;

	count = of_property_count_u32_elems(dev->of_node, property);
	if (count <= 0 || count % tuple_cells)
		return count < 0 ? count : -EINVAL;

	cells = devm_kcalloc(dev, count, sizeof(*cells), GFP_KERNEL);
	if (!cells)
		return -ENOMEM;
	ret = of_property_read_u32_array(dev->of_node, property, cells, count);
	if (ret)
		return ret;

	map = devm_kcalloc(dev, count / tuple_cells, sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	for (i = 0; i < count / tuple_cells; ++i) {
		map[i].source = cells[i * tuple_cells];
		map[i].code = cells[i * tuple_cells + 1];
		if (map[i].code > KEY_MAX)
			return -EINVAL;
	}

	*out = map;
	*out_count = count / tuple_cells;
	return 0;
}

static int j36_read_axes(struct j36_input *j36)
{
	struct device *dev = j36->dev;
	struct j36_axis_map *axes;
	u32 *cells;
	int count;
	int ret;
	unsigned int i;

	count = of_property_count_u32_elems(dev->of_node, "j36,axis-map");
	if (count <= 0 || count % 3)
		return count < 0 ? count : -EINVAL;

	cells = devm_kcalloc(dev, count, sizeof(*cells), GFP_KERNEL);
	if (!cells)
		return -ENOMEM;
	ret = of_property_read_u32_array(dev->of_node, "j36,axis-map", cells, count);
	if (ret)
		return ret;

	axes = devm_kcalloc(dev, count / 3, sizeof(*axes), GFP_KERNEL);
	if (!axes)
		return -ENOMEM;

	for (i = 0; i < count / 3; ++i) {
		axes[i].channel = cells[i * 3];
		axes[i].code = cells[i * 3 + 1];
		axes[i].invert = !!cells[i * 3 + 2];
		axes[i].center = j36->fallback_center;
		if (axes[i].channel >= J36_AUXADC_CHANNELS || axes[i].code > ABS_MAX)
			return -EINVAL;
	}

	j36->axes = axes;
	j36->axis_count = count / 3;
	return 0;
}

static u32 j36_gpio_read_bit(struct j36_input *j36, u32 base, u32 gpio)
{
	u32 bank = gpio / J36_GPIO_PINS_PER_BANK;
	u32 value = readl(j36->gpio + base + bank * J36_GPIO_BANK_STRIDE);

	return (value >> (gpio % J36_GPIO_PINS_PER_BANK)) & 1;
}

static void j36_gpio_write_bit(struct j36_input *j36, u32 base, u32 gpio, bool on)
{
	u32 bank = gpio / J36_GPIO_PINS_PER_BANK;

	writel(BIT(gpio % J36_GPIO_PINS_PER_BANK),
	       j36->gpio + base + bank * J36_GPIO_BANK_STRIDE +
	       (on ? J36_GPIO_BANK_SET : J36_GPIO_BANK_RST));
}

static u32 j36_gpio_get_mode(struct j36_input *j36, u32 gpio)
{
	u32 reg = J36_GPIO_MODE_BASE +
		  (gpio / J36_GPIO_MODE_PER_REG) * J36_GPIO_MODE_STRIDE;
	u32 shift = (gpio % J36_GPIO_MODE_PER_REG) * J36_GPIO_MODE_BITS;

	return (readl(j36->gpio + reg) >> shift) & J36_GPIO_MODE_MASK;
}

/* Five pads per 16-pin-spaced register and no atomic set/reset pair, so this one
 * register family has to be read back. */
static void j36_gpio_set_mode(struct j36_input *j36, u32 gpio, u32 mode)
{
	u32 reg = J36_GPIO_MODE_BASE +
		  (gpio / J36_GPIO_MODE_PER_REG) * J36_GPIO_MODE_STRIDE;
	u32 shift = (gpio % J36_GPIO_MODE_PER_REG) * J36_GPIO_MODE_BITS;
	u32 value = readl(j36->gpio + reg);

	value &= ~(J36_GPIO_MODE_MASK << shift);
	value |= (mode & J36_GPIO_MODE_MASK) << shift;
	writel(value, j36->gpio + reg);
}

/*
 * Input, pull-up, in that order -- PULLSEL before PULLEN so the resistor is never
 * briefly enabled in the wrong direction.
 *
 * A PARKED PULL-DOWN IS OVERRIDDEN, NOT PRESERVED. Every button pad on this
 * connector idles low on a parked pull-down and only the three keypad sense lines
 * idle high; a pad that idles low cannot report a closure because it already reads
 * 0 and has nowhere to move. So the pull-up is not hardening, it is the thing that
 * makes a press observable. MODE is deliberately not touched here: the four D-pad
 * pads are mode-0 EINTs already and reprogramming a button pad's mode is what
 * wedged the earlier bring-up.
 */
static void j36_gpio_arm_pullup(struct j36_input *j36, u32 gpio)
{
	j36_gpio_write_bit(j36, J36_GPIO_DIR_BASE, gpio, false);
	j36_gpio_write_bit(j36, J36_GPIO_PULLSEL_BASE, gpio, true);
	j36_gpio_write_bit(j36, J36_GPIO_PULLEN_BASE, gpio, true);
}

/*
 * Supply the high with the output driver, for a microsecond at a time: drive the
 * pad high, let it settle, sample DIN while it is still driven, put it straight
 * back to an input. Released, the driver wins and DIN reads 1; held, the closed
 * switch wins and DIN reads 0 -- which is the point, the driver is deliberately
 * not strong enough to beat a dead short, so the switch still decides.
 *
 * Yes, this shorts the driver to ground while the button is held, and that is the
 * honest cost. It is bounded: output for one settle plus one read, once per poll.
 * The alternative was leaving D-pad UP permanently dead.
 *
 * PULLEN off first, so the pull-down this pad is parked on is not still fighting
 * while the driver ramps. DOUT before DIR, always -- setting the direction first
 * would drive whatever DOUT happened to hold, which on a button pad means a
 * deliberate short to ground for as long as it takes to notice.
 */
static u32 j36_gpio_read_driven(struct j36_input *j36, u32 gpio)
{
	u32 value;

	j36_gpio_write_bit(j36, J36_GPIO_PULLEN_BASE, gpio, false);
	j36_gpio_write_bit(j36, J36_GPIO_DOUT_BASE, gpio, true);
	j36_gpio_write_bit(j36, J36_GPIO_DIR_BASE, gpio, true);
	udelay(J36_GPIO_SETTLE_US);
	value = j36_gpio_read_bit(j36, J36_GPIO_DIN_BASE, gpio);
	/* Input again before anything else: the pad must stop driving the instant
	 * the sample is taken. Everything after this is housekeeping. */
	j36_gpio_write_bit(j36, J36_GPIO_DIR_BASE, gpio, false);
	j36_gpio_write_bit(j36, J36_GPIO_PULLSEL_BASE, gpio, true);
	j36_gpio_write_bit(j36, J36_GPIO_PULLEN_BASE, gpio, true);
	return value;
}

static bool j36_pad_is_owned(struct j36_input *j36, u32 gpio)
{
	unsigned int i;

	for (i = 0; i < j36->owned_pad_count; ++i) {
		if (j36->owned_pads[i] == gpio)
			return true;
	}
	return false;
}

/*
 * Apply one pad list from the keypad node. Cells are <pad mux-mode> pairs.
 *
 * The rule is narrow on purpose: touch a pad only if its mode is wrong. The five
 * pads the preloader already muxes work, and a "corrective" pull on a sense line
 * that reads fine without one would be a change with no evidence behind it. Mode
 * is written before direction, so a row pad is never a GPIO output for the width
 * of two stores -- muxed the wrong way round it would drive its whole row low and
 * every key on it would read pressed for as long as that lasted.
 *
 * Strobes get no pull (the block restores them after each pulse); senses get a
 * pull-up; reserved pads are buttons and want mode 0, an input, pulled up.
 */
static int j36_apply_pads(struct j36_input *j36, struct device_node *node,
			  const char *property, bool output, bool pullup,
			  bool remember)
{
	struct device *dev = j36->dev;
	int count = of_property_count_u32_elems(node, property);
	unsigned int pairs;
	unsigned int i;

	if (count == -EINVAL) {
		dev_warn(dev, "%s is missing; leaving those pads as the boot chain left them\n",
			 property);
		return 0;
	}
	if (count < 0)
		return count;
	if (count == 0 || count % 2)
		return -EINVAL;
	pairs = (unsigned int)count / 2;

	for (i = 0; i < pairs; ++i) {
		u32 pad, want, mode;

		if (of_property_read_u32_index(node, property, i * 2, &pad) ||
		    of_property_read_u32_index(node, property, i * 2 + 1, &want))
			return -EINVAL;
		if (pad > J36_GPIO_MAX || want > J36_GPIO_MODE_MASK) {
			dev_err(dev, "%s[%u]: pad %u mode %u is out of range\n",
				property, i, pad, want);
			return -EINVAL;
		}
		if (remember) {
			if (j36->owned_pad_count >= J36_PAD_MAX)
				return -ENOSPC;
			j36->owned_pads[j36->owned_pad_count++] = pad;
		}

		mode = j36_gpio_get_mode(j36, pad);
		if (mode == want) {
			dev_dbg(dev, "%s pad %u already mode %u\n", property, pad, mode);
			continue;
		}
		j36_gpio_set_mode(j36, pad, want);
		j36_gpio_write_bit(j36, J36_GPIO_DIR_BASE, pad, output);
		if (pullup) {
			j36_gpio_write_bit(j36, J36_GPIO_PULLSEL_BASE, pad, true);
			j36_gpio_write_bit(j36, J36_GPIO_PULLEN_BASE, pad, true);
		} else {
			j36_gpio_write_bit(j36, J36_GPIO_PULLEN_BASE, pad, false);
		}
		dev_info(dev, "%s pad %u: mode %u -> %u, now mode %u din %u\n",
			 property, pad, mode, want,
			 j36_gpio_get_mode(j36, pad),
			 j36_gpio_read_bit(j36, J36_GPIO_DIN_BASE, pad));
	}
	return 0;
}

/*
 * One WACS2 transaction. The leftover-state recovery at the top is not optional
 * and the stock kernel's pwrap_wacs2_hal does exactly the same thing: a read whose
 * caller timed out before collecting RDATA -- or a hand-off from the preloader
 * mid-transaction -- leaves the FSM parked in WFVLDCLR, and without writing VLDCLR
 * the engine never returns to IDLE, so every later PMIC transaction times out.
 * Clearing a stale VLDCLR here makes that wedge self-heal.
 */
static int j36_pwrap_xfer(struct j36_input *j36, bool write, u32 adr, u32 wdata,
			  u32 *rdata)
{
	unsigned int i;
	u32 value;

	if (adr & ~0xffffu || wdata & ~0xffffu)
		return -EINVAL;
	if (!write && !rdata)
		return -EINVAL;

	value = readl(j36->pwrap + J36_PWRAP_WACS2_RDATA);
	if (((value >> 16) & 0x7) == J36_PWRAP_FSM_WFVLDCLR)
		writel(1, j36->pwrap + J36_PWRAP_WACS2_VLDCLR);

	for (i = 0; i < J36_PWRAP_POLL_LIMIT; ++i) {
		value = readl(j36->pwrap + J36_PWRAP_WACS2_RDATA);
		if (((value >> 16) & 0x7) == J36_PWRAP_FSM_IDLE)
			break;
		cpu_relax();
	}
	if (i == J36_PWRAP_POLL_LIMIT)
		return -ETIMEDOUT;

	writel(((u32)write << 31) | ((adr >> 1) << 16) | wdata,
	       j36->pwrap + J36_PWRAP_WACS2_CMD);
	if (write)
		return 0;

	for (i = 0; i < J36_PWRAP_POLL_LIMIT; ++i) {
		value = readl(j36->pwrap + J36_PWRAP_WACS2_RDATA);
		if (((value >> 16) & 0x7) == J36_PWRAP_FSM_WFVLDCLR) {
			*rdata = value & 0xffff;
			writel(1, j36->pwrap + J36_PWRAP_WACS2_VLDCLR);
			return 0;
		}
		cpu_relax();
	}
	return -ETIMEDOUT;
}

static int j36_pwrap_read(struct j36_input *j36, u32 adr, u32 *rdata)
{
	return j36_pwrap_xfer(j36, false, adr, 0, rdata);
}

static int j36_pwrap_write(struct j36_input *j36, u32 adr, u32 wdata)
{
	return j36_pwrap_xfer(j36, true, adr, wdata, NULL);
}

/* INIT_DONE0 is bit 21 of WACS2_RDATA, and it only refreshes after a transaction,
 * so a cold read of it right after hand-off can be stale. */
static bool j36_pwrap_ready(struct j36_input *j36)
{
	return !!(readl(j36->pwrap + J36_PWRAP_WACS2_RDATA) & BIT(21));
}

/*
 * Ungate the keypad's 32 kHz clock in the PMIC. Best effort: a PWRAP failure is
 * logged and otherwise ignored, for the reason given beside the register defines.
 */
static void j36_kpd_clock_ungate(struct j36_input *j36)
{
	struct device *dev = j36->dev;
	u32 value = 0;
	int ret;

	if (!j36_pwrap_ready(j36)) {
		dev_warn(dev, "PWRAP is not at INIT_DONE; leaving the KPD clock gate alone\n");
		return;
	}
	ret = j36_pwrap_read(j36, J36_PMIC_KPD_CLK_GATE_REG, &value);
	if (ret) {
		dev_warn(dev, "KPD clock ungate: PMIC read failed (%d)\n", ret);
		return;
	}
	if (!(value & J36_PMIC_KPD_CLK_GATE_BIT)) {
		dev_info(dev, "KPD clock already ungated (PMIC 0x40 = 0x%04x)\n", value);
		return;
	}
	ret = j36_pwrap_write(j36, J36_PMIC_KPD_CLK_GATE_REG,
			      value & ~J36_PMIC_KPD_CLK_GATE_BIT);
	if (ret) {
		dev_warn(dev, "KPD clock ungate: PMIC write failed (%d)\n", ret);
		return;
	}
	if (j36_pwrap_read(j36, J36_PMIC_KPD_CLK_GATE_REG, &value))
		return;
	dev_info(dev, "KPD clock ungated (PMIC 0x40 now 0x%04x)\n", value);
}

static int j36_setup_pads(struct j36_input *j36)
{
	struct device_node *node;
	int ret;

	node = of_parse_phandle(j36->dev->of_node, "j36,keypad-controller", 0);
	if (!node)
		return -EINVAL;

	/* Rows first, then columns, then the pad this board took back off the
	 * block: the vendor order is pads, then clock, then KPD_EN. */
	ret = j36_apply_pads(j36, node, "j36,kpd-strobe-pads", true, false, true);
	if (!ret)
		ret = j36_apply_pads(j36, node, "j36,kpd-sense-pads", false, true, true);
	if (!ret)
		ret = j36_apply_pads(j36, node, "j36,kpd-reserved-pads", false, true, false);
	of_node_put(node);
	return ret;
}

/*
 * Ask each button pad whether its pull-up actually took, because on pad 93 it does
 * not. The pull needs a moment against the pad's own capacitance before the answer
 * means anything, so the settle below is generous by orders of magnitude rather
 * than cut fine. A false positive -- a pad marked because the user was holding
 * that button at probe -- is harmless: the driven read is correct for a healthy pad
 * too, it just costs the microsecond.
 */
static void j36_probe_idle_pads(struct j36_input *j36)
{
	unsigned int i;
	unsigned int driven = 0;

	for (i = 0; i < j36->direct_count; ++i) {
		if (j36_pad_is_owned(j36, j36->direct[i].source))
			continue;
		j36_gpio_arm_pullup(j36, j36->direct[i].source);
	}
	udelay(J36_PULLUP_SETTLE_US);
	for (i = 0; i < j36->direct_count; ++i) {
		u32 pad = j36->direct[i].source;

		if (j36_pad_is_owned(j36, pad))
			continue;
		if (j36_gpio_read_bit(j36, J36_GPIO_DIN_BASE, pad))
			continue;
		j36->direct[i].driven = true;
		++driven;
		dev_info(j36->dev,
			 "pad %u will not idle high on its pull-up; switching it to a driven read\n",
			 pad);
	}
	dev_info(j36->dev, "pads needing a driven read: %u\n", driven);
}

static bool j36_gpio_pressed(struct j36_input *j36, const struct j36_key_map *key)
{
	u32 value = key->driven ? j36_gpio_read_driven(j36, key->source)
				: j36_gpio_read_bit(j36, J36_GPIO_DIN_BASE, key->source);

	return !value;
}

static bool j36_matrix_pressed(struct j36_input *j36, u32 matrix_bit)
{
	u32 bank = matrix_bit / 16;
	u32 bit = matrix_bit % 16;
	u16 value;

	if (bank >= 5)
		return false;
	value = readw(j36->keypad + J36_KPD_MEM1 + bank * sizeof(u32));
	if (bank == 4)
		value &= 0x00ff;
	return !(value & BIT(bit));
}

static int j36_auxadc_read(struct j36_input *j36, u32 channel, u32 *raw)
{
	void __iomem *data_reg;
	u32 data = 0;
	unsigned int i;

	if (channel >= J36_AUXADC_CHANNELS)
		return -EINVAL;
	data_reg = j36->auxadc + J36_AUXADC_DAT0 +
		   channel * J36_AUXADC_DAT_STRIDE;

	for (i = 0; i < J36_AUXADC_POLL_LIMIT; ++i) {
		if (!(readl(j36->auxadc + J36_AUXADC_CON2) &
		      J36_AUXADC_CON2_BUSY))
			break;
		cpu_relax();
	}

	writel(BIT(channel), j36->auxadc + J36_AUXADC_CON1_CLR);
	for (i = 0; i < J36_AUXADC_POLL_LIMIT; ++i) {
		if (!(readl(data_reg) & J36_AUXADC_DAT_READY))
			break;
		cpu_relax();
	}
	udelay(J36_AUXADC_SETTLE_US);

	writel(BIT(channel), j36->auxadc + J36_AUXADC_CON1_SET);
	udelay(J36_AUXADC_SETTLE_US);
	for (i = 0; i < J36_AUXADC_POLL_LIMIT; ++i) {
		data = readl(data_reg);
		if (data & J36_AUXADC_DAT_READY) {
			writel(BIT(channel),
			       j36->auxadc + J36_AUXADC_CON1_CLR);
			*raw = data & J36_AUXADC_DAT_MASK;
			return 0;
		}
		cpu_relax();
	}

	writel(BIT(channel), j36->auxadc + J36_AUXADC_CON1_CLR);
	return -ETIMEDOUT;
}

static int j36_scale_axis(struct j36_input *j36,
			   struct j36_axis_map *axis, u32 raw)
{
	int delta = (int)raw - (int)axis->center;
	unsigned int span;
	unsigned int magnitude;
	int value;

	if (axis->invert)
		delta = -delta;
	magnitude = abs(delta);
	if (magnitude <= j36->deadzone)
		return 0;

	span = max(axis->center - j36->raw_min,
		   j36->raw_max - axis->center);
	if (span <= j36->deadzone)
		return 0;

	magnitude = min(magnitude, span);
	value = (magnitude - j36->deadzone) * J36_AXIS_FULL_SCALE /
		(span - j36->deadzone);
	return delta < 0 ? -value : value;
}

/*
 * One jack sample, undebounced, false if it could not be taken.
 *
 * The GPIO arm at probe is what makes the pad case mean anything: an unconnected
 * detect pin with no pull is an antenna, and this is the same pull-up the buttons
 * get for the same reason.
 */
static bool j36_jack_sample(struct j36_input *j36, bool *plugged)
{
	u32 raw;

	switch (j36->jack_source) {
	case J36_JACK_ADC:
		if (j36_auxadc_read(j36, j36->jack_line, &raw))
			return false;
		*plugged = (int)raw >= jack_adc_low && (int)raw <= jack_adc_high;
		return true;
	case J36_JACK_GPIO:
		raw = j36_gpio_read_bit(j36, J36_GPIO_DIN_BASE, j36->jack_line);
		*plugged = j36->jack_active_low ? !raw : !!raw;
		return true;
	default:
		return false;
	}
}

/*
 * A plug is a mechanical contact wiped across a springy one, so it bounces for
 * as long as the hand takes -- tens of milliseconds of make and break, and every
 * one of them would otherwise be an output switch thrown. The debounce is
 * therefore on the SAMPLE and not on the report: the clock restarts on every
 * change of the raw reading, and only a reading that has held still for the whole
 * window is allowed to become the state.
 */
static bool j36_jack_update(struct j36_input *j36)
{
	bool plugged;

	if (!j36_jack_sample(j36, &plugged))
		return false;

	if (plugged != j36->jack_raw) {
		j36->jack_raw = plugged;
		j36->jack_settled_at = jiffies;
		return false;
	}
	if (plugged == j36->jack_reported)
		return false;
	if (time_before(jiffies, j36->jack_settled_at +
				 msecs_to_jiffies(max(jack_debounce_ms, 0))))
		return false;

	j36->jack_reported = plugged;
	input_report_switch(j36->input, SW_HEADPHONE_INSERT, plugged);
	dev_info(j36->dev, "headphone jack: %s\n",
		 plugged ? "plugged in" : "empty");
	return true;
}

/*
 * The instrument. Sixteen conversions and eleven register reads, on a timer the
 * operator sets, printed as two lines so a scrollback of them can be read down a
 * column. A channel the converter will not answer for prints ---- rather than a
 * number, because a nought there would read as a measurement.
 *
 * It costs about a millisecond per pass and it is off by default, so the price of
 * having it is nothing and the price of not having it was that nobody could find
 * the line at all.
 */
static void j36_jack_scan_once(struct j36_input *j36)
{
	char adc[J36_AUXADC_CHANNELS * 5 + 1];
	char din[16 * 6 + 1];
	unsigned int banks = J36_GPIO_MAX / J36_GPIO_PINS_PER_BANK + 1;
	unsigned int i;
	int n = 0;
	u32 raw;

	for (i = 0; i < J36_AUXADC_CHANNELS; ++i) {
		if (j36_auxadc_read(j36, i, &raw))
			n += scnprintf(adc + n, sizeof(adc) - n, " ----");
		else
			n += scnprintf(adc + n, sizeof(adc) - n, " %4u", raw);
	}
	dev_info(j36->dev, "jack scan: adc%s\n", adc);

	n = 0;
	if (banks > 16)
		banks = 16;
	for (i = 0; i < banks; ++i)
		n += scnprintf(din + n, sizeof(din) - n, " %04x",
			       readl(j36->gpio + J36_GPIO_DIN_BASE +
				     i * J36_GPIO_BANK_STRIDE) & 0xffff);
	dev_info(j36->dev, "jack scan: din%s\n", din);
}

static void j36_poll(struct work_struct *work)
{
	struct j36_input *j36 = container_of(to_delayed_work(work),
					     struct j36_input, poll_work);
	unsigned int i;
	bool changed = false;

	for (i = 0; i < j36->direct_count; ++i) {
		bool state = j36_gpio_pressed(j36, &j36->direct[i]);

		if (state != j36->direct[i].state) {
			j36->direct[i].state = state;
			input_report_key(j36->input, j36->direct[i].code, state);
			changed = true;
		}
	}

	for (i = 0; i < j36->matrix_count; ++i) {
		bool state = j36_matrix_pressed(j36, j36->matrix[i].source);

		if (state != j36->matrix[i].state) {
			j36->matrix[i].state = state;
			input_report_key(j36->input, j36->matrix[i].code, state);
			changed = true;
		}
	}

	for (i = 0; i < j36->axis_count; ++i) {
		struct j36_axis_map *axis = &j36->axes[i];
		u32 raw;

		if (j36_auxadc_read(j36, axis->channel, &raw))
			continue;
		if (!axis->center_valid) {
			axis->center = clamp(raw, j36->raw_min, j36->raw_max);
			axis->center_valid = true;
		}
		input_report_abs(j36->input, axis->code,
				 j36_scale_axis(j36, axis, raw));
		changed = true;
	}

	/* After the axes, because on the ADC path it is a conversion on the same
	 * converter and taking it last keeps the sticks' timing exactly as it
	 * was. */
	if (j36_jack_update(j36))
		changed = true;

	if (changed)
		input_sync(j36->input);

	/* Outside the change accounting entirely: this reports nothing, it only
	 * prints. Its own clock, so turning it on does not disturb the poll. */
	if (jack_scan > 0 &&
	    time_after_eq(jiffies, j36->jack_scan_at)) {
		j36_jack_scan_once(j36);
		j36->jack_scan_at = jiffies +
			msecs_to_jiffies(max(jack_scan, J36_JACK_SCAN_MIN_MS));
	}

	schedule_delayed_work(&j36->poll_work,
			      msecs_to_jiffies(j36->poll_ms));
}

/*
 * Which line, from the device tree first and the insmod line second.
 *
 * That order round, and not the other, because the module parameter is the one a
 * person can change without regenerating a DTB and reflashing a card -- which is
 * exactly the position anybody is in while they are still finding the line. Once
 * it is found, j36,jack-adc in the tree makes it the board's own fact and the
 * insmod line goes back to being empty.
 *
 * A GPIO beats an ADC channel when both are named: a level is a cheaper and more
 * certain answer than a window, so if the board turns out to have one, that is
 * the one to use.
 */
static void j36_jack_configure(struct j36_input *j36)
{
	struct device *dev = j36->dev;
	u32 cells[3];

	if (!of_property_read_u32_array(dev->of_node, "j36,jack-adc", cells, 3)) {
		j36->jack_source = J36_JACK_ADC;
		j36->jack_line = cells[0];
		jack_adc_low = (int)cells[1];
		jack_adc_high = (int)cells[2];
	}
	if (!of_property_read_u32_array(dev->of_node, "j36,jack-gpio", cells, 2)) {
		j36->jack_source = J36_JACK_GPIO;
		j36->jack_line = cells[0];
		j36->jack_active_low = !!cells[1];
	}
	if (jack_adc >= 0) {
		j36->jack_source = J36_JACK_ADC;
		j36->jack_line = (u32)jack_adc;
	}
	if (jack_gpio >= 0) {
		j36->jack_source = J36_JACK_GPIO;
		j36->jack_line = (u32)jack_gpio;
		j36->jack_active_low = jack_gpio_active_low;
	}

	if (j36->jack_source == J36_JACK_ADC &&
	    j36->jack_line >= J36_AUXADC_CHANNELS) {
		dev_warn(dev, "jack: AUXADC channel %u does not exist; detection off\n",
			 j36->jack_line);
		j36->jack_source = J36_JACK_NONE;
	}
	if (j36->jack_source == J36_JACK_GPIO && j36->jack_line > J36_GPIO_MAX) {
		dev_warn(dev, "jack: pad %u does not exist; detection off\n",
			 j36->jack_line);
		j36->jack_source = J36_JACK_NONE;
	}
	/* A pad the keypad block owns is not available to be read as a level, and
	 * finding that out here is better than a detect that quietly tracks a
	 * column strobe. */
	if (j36->jack_source == J36_JACK_GPIO && j36_pad_is_owned(j36, j36->jack_line)) {
		dev_warn(dev, "jack: pad %u belongs to the keypad block; detection off\n",
			 j36->jack_line);
		j36->jack_source = J36_JACK_NONE;
	}
}

static void j36_cancel_poll(void *data)
{
	struct j36_input *j36 = data;

	cancel_delayed_work_sync(&j36->poll_work);
}

static int j36_input_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct j36_input *j36;
	struct input_dev *input;
	unsigned int i;
	int ret;

	j36 = devm_kzalloc(dev, sizeof(*j36), GFP_KERNEL);
	if (!j36)
		return -ENOMEM;
	j36->dev = dev;
	j36->poll_ms = 5;
	j36->raw_min = 800;
	j36->raw_max = 3900;
	j36->fallback_center = 2350;
	j36->deadzone = 150;
	of_property_read_u32(dev->of_node, "poll-interval-ms", &j36->poll_ms);
	of_property_read_u32(dev->of_node, "j36,raw-min", &j36->raw_min);
	of_property_read_u32(dev->of_node, "j36,raw-max", &j36->raw_max);
	of_property_read_u32(dev->of_node, "j36,fallback-center",
			     &j36->fallback_center);
	of_property_read_u32(dev->of_node, "j36,deadzone", &j36->deadzone);
	j36->poll_ms = clamp(j36->poll_ms, 1U, 100U);

	j36->gpio = j36_iomap_phandle(dev, "j36,gpio-controller");
	if (IS_ERR(j36->gpio))
		return dev_err_probe(dev, PTR_ERR(j36->gpio), "map GPIO\n");
	j36->keypad = j36_iomap_phandle(dev, "j36,keypad-controller");
	if (IS_ERR(j36->keypad))
		return dev_err_probe(dev, PTR_ERR(j36->keypad), "map KPD\n");
	j36->auxadc = j36_iomap_phandle(dev, "j36,auxadc-controller");
	if (IS_ERR(j36->auxadc))
		return dev_err_probe(dev, PTR_ERR(j36->auxadc), "map AUXADC\n");
	j36->pericfg = j36_iomap_phandle(dev, "j36,pericfg-controller");
	if (IS_ERR(j36->pericfg))
		return dev_err_probe(dev, PTR_ERR(j36->pericfg), "map PERICFG\n");
	j36->pwrap = j36_iomap_phandle(dev, "j36,pwrap-controller");
	if (IS_ERR(j36->pwrap))
		return dev_err_probe(dev, PTR_ERR(j36->pwrap), "map PWRAP\n");

	ret = j36_read_map(dev, "j36,direct-key-map", 2,
			    &j36->direct, &j36->direct_count);
	if (ret)
		return dev_err_probe(dev, ret, "read direct key map\n");
	ret = j36_read_map(dev, "j36,matrix-key-map", 2,
			    &j36->matrix, &j36->matrix_count);
	if (ret)
		return dev_err_probe(dev, ret, "read matrix key map\n");
	ret = j36_read_axes(j36);
	if (ret)
		return dev_err_probe(dev, ret, "read axis map\n");

	/*
	 * Before the input device and not after it, which is a change of order
	 * from what this used to do and is the jack's fault. The pad pass is what
	 * fills owned_pads, the jack cannot be allowed to sit on a pad the keypad
	 * block owns, and whether the jack exists decides one capability bit on a
	 * device that must be fully described before it is registered -- an evdev
	 * node's capabilities are read once, at open, and userspace has no way to
	 * be told they grew. It still runs before the clock ungate and KP_EN, so
	 * the MVII order this was matched against -- pads, clock, enable -- is
	 * intact; input_register_device between them touches no hardware.
	 */
	ret = j36_setup_pads(j36);
	if (ret)
		return dev_err_probe(dev, ret, "apply keypad pads\n");
	j36_jack_configure(j36);

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	j36->input = input;
	input->name = "J36 Ultra built-in gamepad";
	input->phys = "j36/input0";
	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x2454;
	input->id.product = 0x6500;
	input->id.version = 0x0001;

	for (i = 0; i < j36->direct_count; ++i)
		input_set_capability(input, EV_KEY, j36->direct[i].code);
	for (i = 0; i < j36->matrix_count; ++i)
		input_set_capability(input, EV_KEY, j36->matrix[i].code);
	for (i = 0; i < j36->axis_count; ++i)
		input_set_abs_params(input, j36->axes[i].code,
				     -J36_AXIS_FULL_SCALE, J36_AXIS_FULL_SCALE,
				     16, 0);
	/* Only when there is something behind it. A switch that is advertised and
	 * never moves is worse than an absent one: it reads as a board that has
	 * detection and has nothing plugged in, which is the wrong half of the
	 * two things a reader needs to tell apart. */
	if (j36->jack_source != J36_JACK_NONE)
		input_set_capability(input, EV_SW, SW_HEADPHONE_INSERT);

	ret = input_register_device(input);
	if (ret)
		return ret;

	/*
	 * Match the validated MVII initialization order: pads, then clock, then
	 * KPD_EN, and ungate the AUXADC peripheral clock without touching DSI. The
	 * pad pass ran above and has to be before the scanner is enabled -- that
	 * is the one window in which writing a pad the block owns is legitimate.
	 */
	j36_kpd_clock_ungate(j36);
	writew(J36_KPD_DEBOUNCE_DEFAULT & J36_KPD_DEBOUNCE_MASK,
	       j36->keypad + J36_KPD_DEBOUNCE);
	/* Double-key off exactly as the preloader does it: read KP_SEL, clear bit 0,
	 * write it back. A clear rather than an assignment, because KP_SEL's upper
	 * bits are column enables on parts that populate them. */
	writew(readw(j36->keypad + J36_KPD_SEL) & ~J36_KPD_SEL_DOUBLE_KEY,
	       j36->keypad + J36_KPD_SEL);
	writew(1, j36->keypad + J36_KPD_EN);
	writel(J36_PERI_PDN0_AUXADC_BITS,
	       j36->pericfg + J36_PERI_PDN0_CLR);
	j36_probe_idle_pads(j36);
	/* The same pull-up the buttons get, for the same reason: an open detect
	 * contact with nothing holding it is an antenna, and a level read off one
	 * is a reading of the room. */
	if (j36->jack_source == J36_JACK_GPIO)
		j36_gpio_arm_pullup(j36, j36->jack_line);

	INIT_DELAYED_WORK(&j36->poll_work, j36_poll);
	ret = devm_add_action_or_reset(dev, j36_cancel_poll, j36);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, j36);
	j36->jack_settled_at = jiffies;
	j36->jack_scan_at = jiffies;
	schedule_delayed_work(&j36->poll_work, msecs_to_jiffies(100));

	dev_info(dev, "polling %u GPIO keys, %u matrix keys and %u axes every %u ms\n",
		 j36->direct_count, j36->matrix_count, j36->axis_count,
		 j36->poll_ms);
	switch (j36->jack_source) {
	case J36_JACK_ADC:
		dev_info(dev, "headphone jack: AUXADC channel %u, plugged when the raw count is %d..%d\n",
			 j36->jack_line, jack_adc_low, jack_adc_high);
		break;
	case J36_JACK_GPIO:
		dev_info(dev, "headphone jack: pad %u, plugged when it reads %u\n",
			 j36->jack_line, j36->jack_active_low ? 0 : 1);
		break;
	default:
		dev_info(dev, "headphone jack: no detect line is configured -- nothing here notices a plug. Set jack_scan=500 and work the jack to find one\n");
		break;
	}
	return 0;
}

static const struct of_device_id j36_input_of_match[] = {
	{ .compatible = "j36,j36-ultra-input" },
	{ }
};
MODULE_DEVICE_TABLE(of, j36_input_of_match);

static struct platform_driver j36_input_driver = {
	.probe = j36_input_probe,
	.driver = {
		.name = "j36-mt6592-input",
		.of_match_table = j36_input_of_match,
	},
};
module_platform_driver(j36_input_driver);

MODULE_DESCRIPTION("J36 Ultra MT6592 polled gamepad adapter");
MODULE_AUTHOR("MixOS / PowerEngine integration");
MODULE_LICENSE("GPL");
