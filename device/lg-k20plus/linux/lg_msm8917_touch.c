// SPDX-License-Identifier: GPL-2.0-only
/*
 * LG K20 Plus touch: SiW LG4894 over I2C3.
 *
 * WHAT IT IS. An MT protocol-B driver for the LG4894 at 0x28: each IRQ reads
 * the lg4894_touch_info struct at register 0x200 (status words + ten
 * 12-byte finger records) and reports DOWN/MOVE fingers. The I2C framing is
 * the SiW two-byte header -- [0x20|(addr>>8), addr&0xff] for reads over 4
 * bytes -- taken straight from lg4894_reg_read().
 *
 * WHAT IT LEAVES OUT. The vendor driver is 2600 lines of factory test
 * (PRD), firmware update, swipe/knock-on gestures and debugfs. This driver
 * reports fingers and nothing else: no LPWG, no fw update, no test modes.
 * Those are factory and suspend-path features, not the input path.
 *
 * THE OTHER GLASS. Tovis phones carry a Synaptics TD4100 at 0x20, which is
 * an RMI4 device served by mainline rmi4-i2c ("synaptics,rmi4-i2c") -- see
 * the DTS. Both nodes stay enabled like the vendor tree; only the populated
 * IC probes.
 *
 * No-IRQ fallback: like the OPPO touch driver, polls at 100 Hz when the
 * (TLMM/EINT-dependent) IRQ is unavailable.
 *
 * From: drivers/input/touchscreen/lge/lgsic/touch_lg4894.{c,h},
 *       msm8917-lv517_gsm_us-touch.dtsi.
 */

#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/workqueue.h>

#define LG4894_REG_REPORT	0x200
#define LG4894_MAX_FINGERS	10
#define LG4894_COORD_X		720
#define LG4894_COORD_Y		1280
#define LG4894_POLL_MS		10

#define LG4894_EVT_DOWN		1
#define LG4894_EVT_MOVE		2

struct lg4894_finger {
	u8 tool_type:4;
	u8 event:4;
	s8 track_id;
	__le16 x;
	__le16 y;
	u8 pressure;
	u8 angle;
	__le16 width_major;
	__le16 width_minor;
} __packed;

struct lg4894_report {
	__le32 ic_status;
	__le32 device_status;
	/* wakeup:8, touch_cnt:5, button_cnt:3, palm:16 -- one u32, read as
	 * bytes; only the finger array below is consumed. */
	u8 wakeup_type;
	u8 counts;
	__le16 palm;
	struct lg4894_finger fingers[LG4894_MAX_FINGERS];
} __packed;

struct lg4894 {
	struct i2c_client *client;
	struct input_dev *input;
	struct delayed_work poll_work;
	struct lg4894_report report;
};

static int lg4894_read_report(struct lg4894 *ts)
{
	u8 hdr[2] = { 0x20 | ((LG4894_REG_REPORT >> 8) & 0x0f),
		      LG4894_REG_REPORT & 0xff };
	struct i2c_msg msgs[2] = {
		{
			.addr = ts->client->addr,
			.flags = 0,
			.len = sizeof(hdr),
			.buf = hdr,
		},
		{
			.addr = ts->client->addr,
			.flags = I2C_M_RD,
			.len = sizeof(ts->report),
			.buf = (u8 *)&ts->report,
		},
	};

	return i2c_transfer(ts->client->adapter, msgs, 2) == 2 ? 0 : -EIO;
}

static void lg4894_report(struct lg4894 *ts)
{
	struct input_dev *input = ts->input;
	bool pressed[LG4894_MAX_FINGERS] = { false };
	int i;

	if (lg4894_read_report(ts)) {
		dev_err_ratelimited(&ts->client->dev, "report read failed\n");
		return;
	}
	for (i = 0; i < LG4894_MAX_FINGERS; i++) {
		struct lg4894_finger *f = &ts->report.fingers[i];
		unsigned int x, y, w;

		if (f->track_id < 0 || f->track_id >= LG4894_MAX_FINGERS)
			continue;
		if (f->event != LG4894_EVT_DOWN && f->event != LG4894_EVT_MOVE)
			continue;
		x = le16_to_cpu(f->x);
		y = le16_to_cpu(f->y);
		if (x > LG4894_COORD_X || y > LG4894_COORD_Y)
			continue;
		w = max(le16_to_cpu(f->width_major), (u16)1);
		input_mt_slot(input, f->track_id);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
		input_report_abs(input, ABS_MT_POSITION_X, x);
		input_report_abs(input, ABS_MT_POSITION_Y, y);
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, w);
		input_report_abs(input, ABS_MT_PRESSURE,
				 f->pressure ? f->pressure : 1);
		pressed[f->track_id] = true;
	}
	for (i = 0; i < LG4894_MAX_FINGERS; i++) {
		if (pressed[i])
			continue;
		input_mt_slot(input, i);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
	}
	input_sync(input);
}

static irqreturn_t lg4894_irq(int irq, void *dev_id)
{
	lg4894_report(dev_id);
	return IRQ_HANDLED;
}

static void lg4894_poll(struct work_struct *work)
{
	struct lg4894 *ts = container_of(work, struct lg4894, poll_work.work);

	lg4894_report(ts);
	schedule_delayed_work(&ts->poll_work, msecs_to_jiffies(LG4894_POLL_MS));
}

static int lg4894_probe(struct i2c_client *client)
{
	struct lg4894 *ts;
	struct input_dev *input;
	int ret;

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->client = client;
	i2c_set_clientdata(client, ts);

	input = devm_input_allocate_device(&client->dev);
	if (!input)
		return -ENOMEM;
	input->name = "LG K20 Plus Touchscreen";
	input->id.bustype = BUS_I2C;
	input_set_capability(input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, LG4894_COORD_X, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, LG4894_COORD_Y, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	ret = input_mt_init_slots(input, LG4894_MAX_FINGERS, INPUT_MT_DIRECT);
	if (ret)
		return ret;
	ts->input = input;

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, lg4894_irq,
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						"lg4894", ts);
		if (ret)
			return ret;
	} else {
		dev_warn(&client->dev, "no IRQ; falling back to %d ms poll\n",
			 LG4894_POLL_MS);
		INIT_DELAYED_WORK(&ts->poll_work, lg4894_poll);
		schedule_delayed_work(&ts->poll_work,
				      msecs_to_jiffies(LG4894_POLL_MS));
	}
	return input_register_device(input);
}

static void lg4894_remove(struct i2c_client *client)
{
	struct lg4894 *ts = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&ts->poll_work);
	/* The input device is devm-allocated: devres unregisters it. */
}

static const struct of_device_id lg4894_match[] = {
	{ .compatible = "lge,lg4894" },
	{ }
};
MODULE_DEVICE_TABLE(of, lg4894_match);

static const struct i2c_device_id lg4894_ids[] = {
	{ "lg4894", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, lg4894_ids);

static struct i2c_driver lg4894_driver = {
	.driver = {
		.name = "lg-lv517-touch",
		.of_match_table = lg4894_match,
	},
	.probe = lg4894_probe,
	.remove = lg4894_remove,
	.id_table = lg4894_ids,
};
module_i2c_driver(lg4894_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("LG K20 Plus LG4894 I2C multitouch");
