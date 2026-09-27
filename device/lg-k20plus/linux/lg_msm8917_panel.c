// SPDX-License-Identifier: GPL-2.0-only
/*
 * LG K20 Plus panels: LGD LG4894 and Tovis TD4100, both 720x1280 DSI video.
 *
 * WHAT IT IS. One drm_panel driver with two variants, picked by compatible:
 * "lge,lg4894-video" (the default glass) and "lge,td4100-video" (the second
 * source). The LG4894 carries a 26-command init table transcribed from the
 * vendor's qcom,mdss-dsi-on-command (dtype/last/vc/flags/wait/len header
 * stripped, waits kept); the TD4100 needs only sleep-out and display-on.
 * Both run video mode -- burst for the LGD, sync-pulse for the Tovis --
 * at the porch timings from their panel DTSI.
 *
 * THE BACKLIGHT IS NOT HERE. Both variants dim through the PMI8950 WLED,
 * which is mainline (qcom,pmi8950-wled) and bound in the DTS. This driver
 * only points drm at it. A panel driver that also owned the backlight IC
 * would be two drivers sharing one power rail's probe order.
 *
 * THE HANDOFF RULE (from the J36/OPPO panel drivers): enable() sends init
 * only when the panel is not already prepared, so the LK's lit panel is
 * adopted rather than re-initialised.
 *
 * From: dsi-panel-lv5-lgd-lg4894-hd-video.dtsi,
 *       dsi-panel-lv5-tovis-td4100-hd-video.dtsi.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct lg_panel_cmd {
	u8 cmd;
	u8 len;
	u8 data[24];
	unsigned int wait_ms;
};

#define LG_CMD(c, w, ...) { c, sizeof((u8[]){__VA_ARGS__}), {__VA_ARGS__}, w }

static const struct lg_panel_cmd lg4894_init[] = {
	LG_CMD(0xB0, 0, 0xAC),
	LG_CMD(0xB1, 0, 0x10, 0x30, 0x16, 0x00),
	LG_CMD(0xB2, 0, 0x01, 0x00, 0x21, 0x20, 0x58, 0x00, 0xCF, 0x10, 0x1F, 0x1F, 0x34, 0x34),
	LG_CMD(0xB4, 0, 0x00, 0x9F, 0x00),
	LG_CMD(0xB5, 0, 0x41, 0xC0, 0x80, 0x00, 0x00),
	LG_CMD(0xB6, 0, 0x77, 0x34, 0x48),
	LG_CMD(0xBD, 0, 0xD0, 0x02, 0xF4, 0x01, 0x04, 0x04, 0x12, 0x20, 0x22),
	LG_CMD(0xBE, 0, 0xE0, 0xE0, 0xC9, 0xC9, 0xC8, 0xF8, 0x00),
	LG_CMD(0xC1, 0, 0x01, 0xE8, 0xD8, 0xC2, 0xC1, 0x00),
	LG_CMD(0xC2, 0, 0x3B, 0x13, 0x13),
	LG_CMD(0xC3, 0, 0x15, 0x2F, 0x2F, 0x00, 0x66, 0x21),
	LG_CMD(0xC4, 0, 0x51, 0x00, 0x37),
	LG_CMD(0xC5, 0, 0x25, 0x20, 0x20, 0x14, 0x14),
	LG_CMD(0xC7, 0, 0x10, 0x22, 0x00, 0x28, 0x00, 0x62, 0x62, 0x62, 0x00, 0xB1, 0x01, 0x00),
	LG_CMD(0xC8, 0, 0x01, 0x00, 0x03, 0x8C),
	LG_CMD(0xCC, 0, 0x22, 0x2F, 0x11, 0x26, 0x21, 0x24, 0x02, 0x62, 0x62, 0x62, 0x00, 0xB1, 0x01, 0x00),
	LG_CMD(0xD0, 0, 0x10, 0x67, 0x43, 0x43, 0x32, 0x51, 0x00, 0x00, 0x00, 0x42, 0x46, 0x03),
	LG_CMD(0xD1, 0, 0x10, 0x67, 0x43, 0x43, 0x32, 0x51, 0x00, 0x00, 0x00, 0x42, 0x46, 0x03),
	LG_CMD(0xD2, 0, 0x10, 0x67, 0x43, 0x43, 0x32, 0x51, 0x00, 0x00, 0x00, 0x42, 0x46, 0x03),
	LG_CMD(0xD3, 0, 0x10, 0x67, 0x43, 0x43, 0x32, 0x51, 0x00, 0x00, 0x00, 0x42, 0x46, 0x03),
	LG_CMD(0xD4, 0, 0x10, 0x67, 0x43, 0x43, 0x32, 0x70, 0x0F, 0x0F, 0x00, 0x21, 0x46, 0x03),
	LG_CMD(0xD5, 0, 0x10, 0x67, 0x43, 0x43, 0x32, 0x70, 0x0F, 0x0F, 0x00, 0x21, 0x46, 0x03),
	LG_CMD(0xE4, 0, 0x2F, 0xC3, 0x2F, 0x60, 0x41, 0xC5, 0xC6, 0xCC, 0xCC, 0xCB, 0x2F, 0xCB, 0xCA, 0xCA, 0xC9, 0xC9, 0xC7, 0xC8, 0x2F, 0x41, 0x2F),
	LG_CMD(0xE5, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x13, 0xEF, 0xFF, 0xFF, 0xFF, 0xFF, 0x03),
	LG_CMD(0x11, 100),
	LG_CMD(0x29, 0),
};

static const struct lg_panel_cmd lg4894_off[] = {
	LG_CMD(0x28, 100),
	LG_CMD(0x10, 120),
};

static const struct lg_panel_cmd td4100_init[] = {
	LG_CMD(0x11, 120),
	LG_CMD(0x29, 0),
};

static const struct lg_panel_cmd td4100_off[] = {
	LG_CMD(0x28, 0),
	LG_CMD(0x10, 120),
};

static const struct drm_display_mode lg4894_mode = {
	.clock = 88363,
	.hdisplay = 720,
	.hsync_start = 720 + 24,
	.hsync_end = 720 + 24 + 4,
	.htotal = 720 + 24 + 4 + 72,
	.vdisplay = 1280,
	.vsync_start = 1280 + 500,
	.vsync_end = 1280 + 500 + 1,
	.vtotal = 1280 + 500 + 1 + 15,
};

static const struct drm_display_mode td4100_mode = {
	.clock = 83113,
	.hdisplay = 720,
	.hsync_start = 720 + 78,
	.hsync_end = 720 + 78 + 4,
	.htotal = 720 + 78 + 4 + 32,
	.vdisplay = 1280,
	.vsync_start = 1280 + 127,
	.vsync_end = 1280 + 127 + 127,
	.vtotal = 1280 + 127 + 127 + 127,
};

struct lg_panel_desc {
	const struct lg_panel_cmd *init;
	unsigned int init_len;
	const struct lg_panel_cmd *off;
	unsigned int off_len;
	const struct drm_display_mode *mode;
	unsigned long mode_flags;
};

static const struct lg_panel_desc lg4894_desc = {
	.init = lg4894_init,
	.init_len = ARRAY_SIZE(lg4894_init),
	.off = lg4894_off,
	.off_len = ARRAY_SIZE(lg4894_off),
	.mode = &lg4894_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_CLOCK_NON_CONTINUOUS,
};

static const struct lg_panel_desc td4100_desc = {
	.init = td4100_init,
	.init_len = ARRAY_SIZE(td4100_init),
	.off = td4100_off,
	.off_len = ARRAY_SIZE(td4100_off),
	.mode = &td4100_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
		      MIPI_DSI_CLOCK_NON_CONTINUOUS,
};

struct lg_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	const struct lg_panel_desc *desc;
	struct gpio_desc *reset;
	bool prepared;
};

static inline struct lg_panel *to_lg_panel(struct drm_panel *panel)
{
	return container_of(panel, struct lg_panel, panel);
}

static int lg_panel_push(struct lg_panel *p, const struct lg_panel_cmd *cmds,
			 unsigned int n)
{
	unsigned char buf[25];
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		buf[0] = cmds[i].cmd;
		memcpy(buf + 1, cmds[i].data, cmds[i].len);
		ret = mipi_dsi_dcs_write_buffer(p->dsi, buf, 1 + cmds[i].len);
		if (ret < 0) {
			dev_err(&p->dsi->dev, "DCS %#x failed: %d\n",
				cmds[i].cmd, ret);
			return ret;
		}
		if (cmds[i].wait_ms)
			msleep(cmds[i].wait_ms);
	}
	return 0;
}

static int lg_panel_prepare(struct drm_panel *panel)
{
	struct lg_panel *p = to_lg_panel(panel);

	if (p->prepared)
		return 0;
	if (p->reset) {
		gpiod_set_value(p->reset, 1);
		msleep(10);
		gpiod_set_value(p->reset, 0);
		msleep(20);
	}
	return lg_panel_push(p, p->desc->init, p->desc->init_len) ?:
		(p->prepared = true, 0);
}

static int lg_panel_unprepare(struct drm_panel *panel)
{
	struct lg_panel *p = to_lg_panel(panel);

	return lg_panel_push(p, p->desc->off, p->desc->off_len);
}

static int lg_panel_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct lg_panel *p = to_lg_panel(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, p->desc->mode);
	if (!mode)
		return 0;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);
	return 1;
}

static const struct drm_panel_funcs lg_panel_funcs = {
	.prepare = lg_panel_prepare,
	.unprepare = lg_panel_unprepare,
	.get_modes = lg_panel_get_modes,
};

static int lg_panel_probe(struct mipi_dsi_device *dsi)
{
	struct lg_panel *p;
	const struct lg_panel_desc *desc;

	desc = of_device_get_match_data(&dsi->dev);
	if (!desc)
		return -ENODEV;
	p = devm_kzalloc(&dsi->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dsi = dsi;
	p->desc = desc;
	p->reset = devm_gpiod_get_optional(&dsi->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(p->reset))
		return PTR_ERR(p->reset);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = desc->mode_flags;
	drm_panel_init(&p->panel, &dsi->dev, &lg_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	p->panel.prepare_prev_first = true;
	mipi_dsi_set_drvdata(dsi, p);
	return drm_panel_of_backlight(&p->panel);
}

static void lg_panel_remove(struct mipi_dsi_device *dsi)
{
	struct lg_panel *p = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&p->panel);
}

static const struct of_device_id lg_panel_match[] = {
	{ .compatible = "lge,lg4894-video", .data = &lg4894_desc },
	{ .compatible = "lge,td4100-video", .data = &td4100_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, lg_panel_match);

static struct mipi_dsi_driver lg_panel_driver = {
	.probe = lg_panel_probe,
	.remove = lg_panel_remove,
	.driver = {
		.name = "lg-lv517-panel",
		.of_match_table = lg_panel_match,
	},
};
module_mipi_dsi_driver(lg_panel_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("LG K20 Plus DSI video panels (LG4894/TD4100)");
