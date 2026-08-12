// SPDX-License-Identifier: GPL-2.0-only
/*
 * J36 Ultra JD9365 DSI panel, as a mipi_dsi_driver.
 *
 * This module exists because mainline's mtk_dsi cannot finish binding without
 * it. mtk_dsi_host_attach is where component_add for the DSI is called, and
 * host_attach only runs when a mipi_dsi_driver has probed on the panel child
 * node and called mipi_dsi_attach(). No panel driver, no DRM master, no
 * /dev/dri/card0 -- however correct the rest of the device tree is.
 *
 * WHAT IT DOES NOT DO IS THE POINT. The MVII LK has already powered this panel,
 * released its reset, pushed all 155 records of its init table, put it in
 * SYNC_EVENT video mode, assembled the DDP and switched the backlight on. The
 * panel is live and showing the boot logo at the instant Linux starts. So
 * prepare/enable/disable/unprepare are deliberately empty and this driver's only
 * real job is to hand mtk_dsi the mode, the lane count and the format, and to
 * exist so host_attach fires.
 *
 * That is not laziness, it is the only thing that can work in this profile.
 * Three of the properties the panel node carries name providers that have no
 * driver here, and every one of them would defer the probe forever rather than
 * fail it:
 *
 *   - backlight would be a pwm-backlight on &disp_pwm, which is
 *     j36,mt6592-disp-pwm and registers no pwm_chip. of_find_backlight returns
 *     -EPROBE_DEFER until a backlight device registers on the node it was
 *     pointed at. j36_mt6592_backlight.ko does register one on &disp_pwm now,
 *     and the phandle is still deliberately absent: that module ships in the
 *     power payload and is allowed to be missing, so linking to it would make
 *     the DRM master conditional on it. The panel node therefore has no
 *     backlight phandle and this driver never calls drm_panel_of_backlight.
 *   - reset-gpios and mediatek,power-gpios point at &gpio, which is
 *     j36,mt6592-gpio and binds nothing either. devm_gpiod_get would defer the
 *     same way, so this driver never asks gpiod for anything. Those properties
 *     record what the LK does; the sibling input driver reaches the same
 *     controller by ioremapping it directly, which is what a cold-start path
 *     here would have to do too.
 *   - mediatek,pmic-power-on-sequence is a list of MT6323 registers reached over
 *     PWRAP. There is no MT6323 regulator driver in this profile.
 *
 * Hence the refusal in probe: without j36,preserve-lk-state this driver stops
 * and says so, rather than pretending to bring a dark panel up. The DT keeps the
 * init table, the PMIC sequences and the GPIO sequence so that a cold-start path
 * has everything it needs the day something has to boot this panel without the
 * MVII LK in front of it.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

struct j36_jd9365 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_display_mode mode;
};

static inline struct j36_jd9365 *to_j36_jd9365(struct drm_panel *panel)
{
	return container_of(panel, struct j36_jd9365, panel);
}

/*
 * All four are empty, and each one is empty for a reason worth stating once.
 *
 * prepare/unprepare are the panel's power and reset, which the LK owns and which
 * this profile has no way to reach -- see the header. enable/disable are the
 * display-on/off DCS pair; sending MIPI_DCS_SET_DISPLAY_OFF at disable would
 * blank a panel this driver cannot then power-cycle back to a known state, and
 * the LK already left it on. mtk_dsi's own poweron/poweroff cycle around these
 * callbacks resets and reprograms the DSI HOST, not the panel controller, and the
 * JD9365 keeps its register state across that because RESX is never pulsed.
 */
static int j36_jd9365_prepare(struct drm_panel *panel)
{
	return 0;
}

static int j36_jd9365_enable(struct drm_panel *panel)
{
	return 0;
}

static int j36_jd9365_disable(struct drm_panel *panel)
{
	return 0;
}

static int j36_jd9365_unprepare(struct drm_panel *panel)
{
	return 0;
}

static int j36_jd9365_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct j36_jd9365 *ctx = to_j36_jd9365(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &ctx->mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	/*
	 * width_mm/height_mm stay 0. The panel-timing node carries no width-mm or
	 * height-mm because nothing has measured the glass, and a plausible
	 * millimetre figure would be an invention. Zero is what the DRM core reads
	 * as "unknown physical size", which is true.
	 */
	connector->display_info.width_mm = 0;
	connector->display_info.height_mm = 0;

	return 1;
}

static const struct drm_panel_funcs j36_jd9365_funcs = {
	.prepare = j36_jd9365_prepare,
	.enable = j36_jd9365_enable,
	.disable = j36_jd9365_disable,
	.unprepare = j36_jd9365_unprepare,
	.get_modes = j36_jd9365_get_modes,
};

/*
 * The timing comes out of panel-timing rather than out of this file.
 *
 * of_get_drm_panel_display_mode() would be the obvious helper and it is not used
 * here: it returns the error from reading width-mm and height-mm, which this
 * node does not have, AFTER it has already filled the mode in. Leaning on that
 * would mean treating -EINVAL as success. The three calls it makes before that
 * point are all exported, so they are made directly instead.
 */
static int j36_jd9365_read_mode(struct device *dev, struct drm_display_mode *mode)
{
	struct display_timing timing;
	struct videomode vm;
	int ret;

	ret = of_get_display_timing(dev->of_node, "panel-timing", &timing);
	if (ret)
		return dev_err_probe(dev, ret, "no usable panel-timing node\n");

	videomode_from_timing(&timing, &vm);
	memset(mode, 0, sizeof(*mode));
	drm_display_mode_from_videomode(&vm, mode);

	return 0;
}

static int j36_jd9365_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct device_node *np = dev->of_node;
	struct j36_jd9365 *ctx;
	u32 lanes, format, flags;
	bool preserve;
	int ret;

	/*
	 * j36,preserve-lk-state lives on the DSI host node, because it is a
	 * statement about the whole pipe, but accept it on the panel node too so
	 * either placement works.
	 */
	preserve = of_property_read_bool(np, "j36,preserve-lk-state") ||
		   (np->parent &&
		    of_property_read_bool(np->parent, "j36,preserve-lk-state"));
	if (!preserve)
		return dev_err_probe(dev, -EINVAL,
				     "cold start is not implemented: this driver only adopts a panel the MVII LK already lit. Bringing it up from dark needs the MT6323 rails over PWRAP, the reset and power pads on a GPIO controller with no driver in this profile, and the 155-record init table -- all of which the device tree carries. Add j36,preserve-lk-state to adopt instead.\n");

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ret = j36_jd9365_read_mode(dev, &ctx->mode);
	if (ret)
		return ret;

	/*
	 * Lanes, format and flags are the three fields mtk_dsi_host_attach copies
	 * straight out of this device, and all three are measured values from the
	 * LK's board header rather than defaults: 4 lanes, RGB888, and VIDEO with
	 * no SYNC_PULSE so that mtk_dsi_set_mode programs SYNC_EVENT -- mode 2,
	 * which is the DSI_MODE_CTRL value the LK leaves behind.
	 */
	if (of_property_read_u32(np, "dsi,lanes", &lanes))
		return dev_err_probe(dev, -EINVAL, "dsi,lanes is missing\n");
	if (of_property_read_u32(np, "dsi,format", &format))
		return dev_err_probe(dev, -EINVAL, "dsi,format is missing\n");
	if (of_property_read_u32(np, "dsi,flags", &flags))
		return dev_err_probe(dev, -EINVAL, "dsi,flags is missing\n");

	dsi->lanes = lanes;
	dsi->format = format;
	dsi->mode_flags = flags;

	drm_panel_init(&ctx->panel, dev, &j36_jd9365_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&ctx->panel);

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	/*
	 * And this is the call the whole module is here for. mtk_dsi_host_attach
	 * resolves port 0 / endpoint 0 of the host node back to this panel, wraps
	 * it in a panel_bridge, and only then does component_add.
	 */
	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach to the DSI host\n");
	}

	dev_info(dev, "adopted the LK's %ux%u@%u panel: %u lanes, format %u, flags 0x%x\n",
		 ctx->mode.hdisplay, ctx->mode.vdisplay,
		 drm_mode_vrefresh(&ctx->mode), lanes, format, flags);

	return 0;
}

static void j36_jd9365_remove(struct mipi_dsi_device *dsi)
{
	struct j36_jd9365 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id j36_jd9365_of_match[] = {
	{ .compatible = "j36,jd9365-qc-190227" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, j36_jd9365_of_match);

static struct mipi_dsi_driver j36_jd9365_driver = {
	.probe = j36_jd9365_probe,
	.remove = j36_jd9365_remove,
	.driver = {
		.name = "j36-jd9365-panel",
		.of_match_table = j36_jd9365_of_match,
	},
};
module_mipi_dsi_driver(j36_jd9365_driver);

MODULE_DESCRIPTION("J36 Ultra JD9365 DSI panel (adopts the MVII LK's state)");
MODULE_LICENSE("GPL v2");
