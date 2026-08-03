// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal J36 Ultra input adapter for MediaTek MT6592.
 *
 * This intentionally mirrors the non-destructive polling paths in PowerEngine
 * OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers/mt6592_keys.c:
 *   - direct active-low buttons are read from GPIO DIN only;
 *   - matrix buttons are read from the five KPD_MEM scan words;
 *   - joystick axes are read from AUXADC channels after a fresh stop/start;
 *   - no pinmux writes are performed, preserving the preloader/LK setup.
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

#define J36_GPIO_DIN_BASE             0x0500
#define J36_GPIO_BANK_STRIDE          0x0010
#define J36_GPIO_PINS_PER_BANK        16

#define J36_KPD_MEM1                  0x0004
#define J36_KPD_DEBOUNCE              0x0018
#define J36_KPD_EN                    0x0024
#define J36_KPD_DEBOUNCE_DEFAULT      0x0400

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

struct j36_key_map {
	u32 source;
	u32 code;
	bool state;
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
	struct input_dev *input;
	struct delayed_work poll_work;
	unsigned int poll_ms;

	struct j36_key_map *direct;
	unsigned int direct_count;
	struct j36_key_map *matrix;
	unsigned int matrix_count;
	struct j36_axis_map *axes;
	unsigned int axis_count;

	u32 raw_min;
	u32 raw_max;
	u32 fallback_center;
	u32 deadzone;
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

static bool j36_gpio_pressed(struct j36_input *j36, u32 gpio)
{
	u32 bank = gpio / J36_GPIO_PINS_PER_BANK;
	u32 bit = gpio % J36_GPIO_PINS_PER_BANK;
	u32 value = readl(j36->gpio + J36_GPIO_DIN_BASE +
			  bank * J36_GPIO_BANK_STRIDE);

	return !(value & BIT(bit));
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

static void j36_poll(struct work_struct *work)
{
	struct j36_input *j36 = container_of(to_delayed_work(work),
					     struct j36_input, poll_work);
	unsigned int i;
	bool changed = false;

	for (i = 0; i < j36->direct_count; ++i) {
		bool state = j36_gpio_pressed(j36, j36->direct[i].source);

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

	if (changed)
		input_sync(j36->input);
	schedule_delayed_work(&j36->poll_work,
			      msecs_to_jiffies(j36->poll_ms));
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

	ret = input_register_device(input);
	if (ret)
		return ret;

	/*
	 * Match the validated MVII initialization: enable the scan memory only if
	 * needed, and ungate the AUXADC peripheral clock without touching DSI.
	 */
	writew(J36_KPD_DEBOUNCE_DEFAULT, j36->keypad + J36_KPD_DEBOUNCE);
	writew(1, j36->keypad + J36_KPD_EN);
	writel(J36_PERI_PDN0_AUXADC_BITS,
	       j36->pericfg + J36_PERI_PDN0_CLR);

	INIT_DELAYED_WORK(&j36->poll_work, j36_poll);
	ret = devm_add_action_or_reset(dev, j36_cancel_poll, j36);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, j36);
	schedule_delayed_work(&j36->poll_work, msecs_to_jiffies(100));

	dev_info(dev, "polling %u GPIO keys, %u matrix keys and %u axes every %u ms\n",
		 j36->direct_count, j36->matrix_count, j36->axis_count,
		 j36->poll_ms);
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
MODULE_AUTHOR("dArkOS / PowerEngine integration");
MODULE_LICENSE("GPL");
