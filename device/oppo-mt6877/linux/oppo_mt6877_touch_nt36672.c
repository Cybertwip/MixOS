// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPPO MT6877 touch: Novatek NT36672 over SPI4.
 *
 * WHAT IT IS. An MT protocol-B multitouch driver for the 10-finger NT36672 on
 * SPI4 (chip-select 0, 4.8 MHz, falling-edge IRQ on GPIO 14). Each IRQ clocks
 * a 66-byte frame out of the controller and reports the fingers through the
 * input subsystem; coordinates arrive in the 1080x2400 touch space and are
 * reported raw, because the glass really is that size and scaling belongs to
 * userspace (libinput already does it).
 *
 * The frame layout is touch_event_handler() from the vendor NT36xxx driver,
 * minus the vendor's kernel thread: a threaded IRQ does the same job with
 * the scheduler's priorities instead of SCHED_RR prio 4. Gesture wakeup,
 * firmware update and MP test are deliberately absent -- they are factory and
 * suspend-path features, and this driver stays on the input path the way the
 * J36 input driver does.
 *
 * From: drivers/input/touchscreen/mediatek/NT36xxx/nt36xxx.c,
 *       cust_mt6877_touch_nt36672.dtsi (wiring + geometry).
 */

#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>

#define NT36672_MAX_FINGERS	10
#define NT36672_REPORT_SIZE	66
#define NT36672_COORD_X		1080
#define NT36672_COORD_Y		2400

#define NT36672_STATUS_MASK	0x07
#define NT36672_STATUS_DOWN	0x01
#define NT36672_STATUS_MOVE	0x02

struct nt36672 {
	struct spi_device *spi;
	struct input_dev *input;
	struct delayed_work poll_work;
	u8 *buf;
};

/* 100 Hz fallback poll, used only until the EINT/pinctrl port lands. */
#define NT36672_POLL_MS 10

static void nt36672_report(struct nt36672 *ts)
{
	struct input_dev *input = ts->input;
	struct spi_transfer xfer = {
		.rx_buf = ts->buf,
		.len = NT36672_REPORT_SIZE,
	};
	struct spi_message msg;
	unsigned int pos, x, y, w, p;
	u8 id, status;
	bool pressed[NT36672_MAX_FINGERS] = { false };
	int i, ret;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);
	ret = spi_sync(ts->spi, &msg);
	if (ret) {
		dev_err_ratelimited(&ts->spi->dev, "frame read failed: %d\n", ret);
		return;
	}

	for (i = 0; i < NT36672_MAX_FINGERS; i++) {
		pos = 1 + 6 * i;
		id = ts->buf[pos] >> 3;
		status = ts->buf[pos] & NT36672_STATUS_MASK;
		if (id == 0 || id > NT36672_MAX_FINGERS)
			continue;
		if (status != NT36672_STATUS_DOWN && status != NT36672_STATUS_MOVE)
			continue;
		x = (ts->buf[pos + 1] << 4) + (ts->buf[pos + 3] >> 4);
		y = (ts->buf[pos + 2] << 4) + (ts->buf[pos + 3] & 0x0F);
		if (x > NT36672_COORD_X || y > NT36672_COORD_Y)
			continue;
		w = ts->buf[pos + 4];
		if (w == 0)
			w = 1;
		if (i < 2)
			p = ts->buf[pos + 5] + (ts->buf[i + 63] << 8);
		else
			p = ts->buf[pos + 5];
		if (p == 0)
			p = 1;
		input_mt_slot(input, id - 1);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
		input_report_abs(input, ABS_MT_POSITION_X, x);
		input_report_abs(input, ABS_MT_POSITION_Y, y);
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, w);
		input_report_abs(input, ABS_MT_PRESSURE, p);
		pressed[id - 1] = true;
	}
	for (i = 0; i < NT36672_MAX_FINGERS; i++) {
		if (pressed[i])
			continue;
		input_mt_slot(input, i);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
	}
	input_sync(input);
}

static irqreturn_t nt36672_irq(int irq, void *dev_id)
{
	nt36672_report(dev_id);
	return IRQ_HANDLED;
}

static void nt36672_poll(struct work_struct *work)
{
	struct nt36672 *ts = container_of(work, struct nt36672, poll_work.work);

	nt36672_report(ts);
	schedule_delayed_work(&ts->poll_work, msecs_to_jiffies(NT36672_POLL_MS));
}

static int nt36672_probe(struct spi_device *spi)
{
	struct nt36672 *ts;
	struct input_dev *input;
	int ret;

	ts = devm_kzalloc(&spi->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->buf = devm_kzalloc(&spi->dev, NT36672_REPORT_SIZE, GFP_KERNEL);
	if (!ts->buf)
		return -ENOMEM;
	ts->spi = spi;
	spi_set_drvdata(spi, ts);

	input = devm_input_allocate_device(&spi->dev);
	if (!input)
		return -ENOMEM;
	input->name = "OPPO NT36672 Touchscreen";
	input->id.bustype = BUS_SPI;
	input->id.vendor = 0x29a4; /* Novatek */
	input->id.product = 0x6672;
	input_set_capability(input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, NT36672_COORD_X, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, NT36672_COORD_Y, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0, 1023, 0, 0);
	ret = input_mt_init_slots(input, NT36672_MAX_FINGERS, INPUT_MT_DIRECT);
	if (ret)
		return ret;
	ts->input = input;

	if (spi->irq > 0) {
		ret = devm_request_threaded_irq(&spi->dev, spi->irq, NULL,
						nt36672_irq,
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						"nt36672", ts);
		if (ret)
			return ret;
	} else {
		/* No EINT yet (pinctrl port pending): poll the frame at
		 * 100 Hz instead. Slower to wake, but the glass works. */
		dev_warn(&spi->dev, "no IRQ; falling back to %d ms poll\n",
			 NT36672_POLL_MS);
		INIT_DELAYED_WORK(&ts->poll_work, nt36672_poll);
		schedule_delayed_work(&ts->poll_work,
				      msecs_to_jiffies(NT36672_POLL_MS));
	}
	return input_register_device(input);
}

static void nt36672_remove(struct spi_device *spi)
{
	struct nt36672 *ts = spi_get_drvdata(spi);

	cancel_delayed_work_sync(&ts->poll_work);
	/* The input device is devm-allocated: devres unregisters it. */
}

static const struct of_device_id nt36672_match[] = {
	{ .compatible = "oppo,nt36672" },
	{ }
};
MODULE_DEVICE_TABLE(of, nt36672_match);

static const struct spi_device_id nt36672_ids[] = {
	{ "nt36672", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, nt36672_ids);

static struct spi_driver nt36672_driver = {
	.driver = {
		.name = "oppo-nt36672",
		.of_match_table = nt36672_match,
	},
	.probe = nt36672_probe,
	.remove = nt36672_remove,
	.id_table = nt36672_ids,
};
module_spi_driver(nt36672_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("OPPO NT36672 SPI multitouch");
