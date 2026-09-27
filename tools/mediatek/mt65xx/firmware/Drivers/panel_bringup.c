/*
 * panel_bringup.c — shared J36 Ultra panel bring-up (see panel_bringup.h)
 *
 * Moved out of mvii_lk_main.c unchanged in order, so the resident BROM payload
 * can run the identical sequence from its live console.
 */
#include "panel_bringup.h"

#include "backlight.h"
#include "dsi_drv.h"
#include "lcd_drv.h"
#include "mt6592_disp_hw.h"

#ifndef MVII_MT6592_LK_FB_ADDR
#define MVII_MT6592_LK_FB_ADDR 0x82700000u
#endif
#ifndef MVII_MT6592_LK_FB_PITCH
#define MVII_MT6592_LK_FB_PITCH 2560u
#endif
/* MVII_MT6592_LK_BACKLIGHT_PCT comes from panel_bringup.h now — it had a second
 * copy here, and the two silently disagreeing is a brightness step on the panel. */

const char* const mt6592_dsi_readback_names[12] = {
    "dsi pll_con0=",  "dsi pll_con2=",   "dsi pll_pwr=",   "dsi com_ctrl=",
    "dsi mode_ctrl=", "dsi psctrl=",     "dsi txrx_ctrl=", "dsi intsta=",
    "dsi start=",     "dsi sta=",        "dsi phy_lccon=", "dsi vm_cmd_con=",
};

const char* const mt6592_lcd_readback_names[16] = {
    "ddp ovl0_mout_en=", "ddp disp_out_sel=", "ddp ovl_en=",      "ddp ovl_src_con=",
    "ddp rdma_gcon=",    "ddp ovl_bgclr=",    "ddp mutex_mod=",   "ddp bls_en=",
    "ddp layer=",        "ddp layer_con=",    "ddp layer_addr=",  "ddp layer_pitch=",
    "ddp layer_size=",   "ddp layer_rdma=",   "ddp mutex0=",      "ddp mutex0_sof=",
};

static void emit(mt6592_panel_log_fn log, void* ctx, const char* label)
{
    if (log) log(ctx, label, 0, 0u);
}

static void emit_hex(mt6592_panel_log_fn log, void* ctx, const char* label, uint32_t value)
{
    if (log) log(ctx, label, 1, value);
}

static void gpio_out(uint32_t pin, uint32_t high)
{
    mtk_gpio_set_mode(pin, 0u);
    mtk_gpio_dir_output(pin);
    mtk_gpio_write(pin, high);
}

/*
 * Same sequence, one line per step.
 *
 * The steps are unchanged and deliberately so; what was missing was any way to see
 * where inside them a board stops. This is ~300 ms of GPIO toggling and PMIC LDO
 * writes that used to emit nothing at all, so a boot that ended in here left a log
 * whose last line was "display clocks on" and no way to tell a hung rail write from
 * a board that browned out when the panel supplies came up. Which of those it is
 * decides whether the bug is in software at all.
 *
 * Every emit is placed AFTER the action it names, so the last line printed is the
 * step that completed and the failing step is the one that never printed.
 */
uint32_t mt6592_panel_power_on_logged(mt6592_panel_log_fn log, void* ctx)
{
    uint32_t fail;

    gpio_out(MT6592_PANEL_GPIO_RESET, 0u);
    mtk_delay_ms(10u);
    emit(log, ctx, "panel: reset low");
    gpio_out(MT6592_PANEL_GPIO_RESET, 1u);
    mtk_delay_ms(10u);
    emit(log, ctx, "panel: reset high");

    (void)mt6592_lcm_power_rails_off();
    mtk_delay_ms(10u);
    emit(log, ctx, "panel: rails off");
    fail = mt6592_lcm_power_rails_on();
    mtk_delay_ms(10u);
    /* Printed every time, not just on failure: on a board with no cell this is the
     * moment the panel supplies load an unclamped VBAT, and "rails on mask=0" is
     * the proof the writes landed before whatever happens next. */
    emit_hex(log, ctx, "panel: rails on, fail mask=", fail);

    gpio_out(MT6592_PANEL_GPIO_PWR0, 1u);
    mtk_delay_ms(10u);
    gpio_out(MT6592_PANEL_GPIO_PWR0, 0u);
    mtk_delay_ms(150u);
    gpio_out(MT6592_PANEL_GPIO_PWR0, 1u);
    mtk_delay_ms(100u);
    emit(log, ctx, "panel: pwr0 pulsed");

    gpio_out(MT6592_PANEL_GPIO_PWR1, 1u);
    emit(log, ctx, "panel: pwr1 high");
    return fail;
}

uint32_t mt6592_panel_power_on(void)
{
    return mt6592_panel_power_on_logged(0, 0);
}

void mt6592_panel_bringup_defaults(mt6592_panel_bringup_cfg_t* cfg)
{
    if (!cfg) return;
    cfg->fb_addr = (uint32_t)MVII_MT6592_LK_FB_ADDR;
    cfg->fb_pitch = (uint32_t)MVII_MT6592_LK_FB_PITCH;
    cfg->bg_color = 0xff000000u;
    cfg->backlight_pct = (uint32_t)MVII_MT6592_LK_BACKLIGHT_PCT;
    cfg->skip_clocks = 0u;
}

void mt6592_panel_dump(mt6592_panel_log_fn log, void* ctx)
{
    uint32_t ddp[16];
    uint32_t dsi[12];

    if (!log) return;

    mt6592_lcd_readback(ddp);
    for (uint32_t i = 0; i < 16u; ++i) emit_hex(log, ctx, mt6592_lcd_readback_names[i], ddp[i]);

    mt6592_dsi_readback(dsi);
    for (uint32_t i = 0; i < 12u; ++i) emit_hex(log, ctx, mt6592_dsi_readback_names[i], dsi[i]);
}

int mt6592_panel_bringup(const mt6592_panel_bringup_cfg_t* cfg,
                         mt6592_panel_log_fn log, void* ctx)
{
    mt6592_panel_bringup_cfg_t local;
    int rc;

    if (cfg) {
        local = *cfg;
    } else {
        mt6592_panel_bringup_defaults(&local);
    }

    if (!local.skip_clocks) {
        mt6592_lcd_clocks_on();
        emit(log, ctx, "display clocks on");
    }

    (void)mt6592_panel_power_on_logged(log, ctx);
    emit(log, ctx, "panel power + reset");

    /* The backlight used to come on here, dim, as a sign of life before the link
     * was up. That is what the white flash at the start of every boot was: the
     * JD9365 has not been programmed yet at this point, so lighting it shows the
     * panel's own uninitialised output — white — for as long as the DSI setup
     * and the whole DCS program take. Nothing about that is seamless, and it is
     * the first thing anyone sees.
     *
     * It is no longer a trade worth making. The argument for it was that a lit
     * panel is the only sign a cable-less board gives that the image is running;
     * that stopped being true once the USB debug console existed, and a failed
     * bring-up now reports itself over a link that can also say *why*. So the
     * panel stays dark until there is something on it, and the only backlight
     * call in this function is the one after the scanout is committed. */

    mt6592_dsi_video_setup();
    emit(log, ctx, "mipitx pll + dsi host");

    rc = mt6592_dsi_jd9365_stock_init();
    if (rc != 0) {
        uint32_t rb[12];

        emit_hex(log, ctx, "jd9365 program stopped at index=", mt6592_dsi_jd9365_last_index());

        /* Index 0 is a statement about the DSI block, not about the panel: the
         * host refused the very first packet, so nothing the JD9365 does or
         * does not do can be involved yet. Dump the twelve registers that tell
         * the possible causes apart — PLL unpowered, host still in reset, host
         * never enabled, BUSY latched by a byte clock that never arrived —
         * because from the front of the device every one of them looks the
         * same: a lit panel with nothing on it. */
        mt6592_dsi_readback(rb);
        for (uint32_t i = 0; i < 12u; ++i) emit_hex(log, ctx, mt6592_dsi_readback_names[i], rb[i]);
        return -1;
    }
    emit(log, ctx, "jd9365 program sent");

    /* The transport no longer treats DSI_INTSTA.BUSY as a gate, so this is the
     * only place the bit gets reported. Equal to the program length means the
     * host never once said it finished a packet — worth knowing, and no longer
     * worth stopping for. */
    emit_hex(log, ctx, "dsi busy stalls=", mt6592_dsi_busy_stalls());

    if (local.fb_addr != 0u) {
        rc = mt6592_lcd_scanout_lk_argb8888_layer2(local.fb_addr, local.fb_pitch, local.bg_color);
    } else {
        /* No DRAM behind us: let the OVL emit bg_color from its ROI background
         * so the link can be proven without a framebuffer to read. */
        rc = mt6592_lcd_scanout(0u, local.bg_color ? local.bg_color : 0xff0000ffu);
    }
    if (rc != 0) {
        emit(log, ctx, "ddp scanout failed");
        return -2;
    }

    /* The BLS pixel path shares the DISP_PWM block and resets its duty when the
     * video route is committed. */
    mt6592_backlight_reassert(local.backlight_pct);
    return 0;
}
