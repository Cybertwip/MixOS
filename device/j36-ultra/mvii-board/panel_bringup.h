/*
 * panel_bringup.h — the one J36 Ultra panel bring-up sequence
 *
 * Both users of the display path share this file on purpose:
 *
 *   - mvii_lk_main.c, the LK-slot loader, runs it once at boot;
 *   - flash_stage.c, the resident MVIIFlash BROM payload, runs it on demand
 *     from the live command console.
 *
 * The console exists so a register fix can be tried in seconds over the USB
 * bridge instead of via flash-reboot-read-log, and that only proves anything if
 * the console drives the *same* code the loader will run. Keeping the order in
 * one place is what makes a "panel" command on the bench equivalent to a boot.
 */
#ifndef MT6592_PANEL_BRINGUP_H
#define MT6592_PANEL_BRINGUP_H

#include <stdint.h>

/* Panel sideband GPIOs, from the stock LCM driver (jd9365_qc_190227_lcm_drv
 * init() @0x81e1c54c, transcribed in Reference/J36-ULTRA/src/panel.c). The PMIC
 * half of that sequence lives in backlight.c as mt6592_lcm_power_rails_*. */
#define MT6592_PANEL_GPIO_RESET 6u
#define MT6592_PANEL_GPIO_PWR0  112u /* 0x70 */
#define MT6592_PANEL_GPIO_PWR1  113u /* 0x71 */

/* Reset pin, then the six DIGLDO writes cleared and re-applied, then the two
 * enable pins — 1:1 with the stock LCM init(). Returns 0, or a bitmask of the
 * PMIC rails whose write failed. */
uint32_t mt6592_panel_power_on(void);

/* Optional trace sink. `label` is always non-null; when `has_value` is 0 the
 * value is meaningless. Pass a null sink for a silent run. */
typedef void (*mt6592_panel_log_fn)(void* ctx, const char* label, int has_value, uint32_t value);

/* mt6592_panel_power_on() with a line per step. The sequence is identical; the
 * difference is that a board that stops partway through says where. Roughly 300 ms
 * of it is unconditional delay, so a silent run of it is indistinguishable from a
 * hang, and the last-printed step names the one that completed. Null sink is fine
 * and is exactly what mt6592_panel_power_on() passes. */
uint32_t mt6592_panel_power_on_logged(mt6592_panel_log_fn log, void* ctx);

typedef struct {
    /* Framebuffer to scan out. When fb_addr is 0 the OVL paints bg_color out of
     * its ROI background instead and reads no DRAM at all — which is the only
     * way the BROM payload can light the panel, since nothing has trained the
     * memory controller at that point. */
    uint32_t fb_addr;
    uint32_t fb_pitch;
    uint32_t bg_color;

    uint32_t backlight_pct;       /* after the route is committed */

    /* Skip mt6592_lcd_clocks_on(). Only for a repeat run inside one session,
     * where the domain is already powered and re-gating it is wasted time. */
    uint32_t skip_clocks;
} mt6592_panel_bringup_cfg_t;

/* Fill `cfg` with the LK boot defaults (DRAM framebuffer, full backlight). */
void mt6592_panel_bringup_defaults(mt6592_panel_bringup_cfg_t* cfg);

/* clocks -> panel power -> dim backlight -> DSI host+PHY -> JD9365 program ->
 * video mode -> DDP route -> full backlight.
 *
 * Returns 0 on success. Negative values are stage failures:
 *   -1  JD9365 program refused (mt6592_dsi_jd9365_last_index() says where)
 *   -2  DDP scanout setup failed
 * The panel program failure path dumps the twelve DSI registers through the
 * sink before returning, because from the front of the device a stalled host
 * and a dead panel look identical. */
int mt6592_panel_bringup(const mt6592_panel_bringup_cfg_t* cfg,
                         mt6592_panel_log_fn log, void* ctx);

/* Register-name tables for the two readback dumps, so the loader and the live
 * console label the same words the same way. */
extern const char* const mt6592_dsi_readback_names[12];
extern const char* const mt6592_lcd_readback_names[16];

/* Dump mt6592_dsi_readback() + mt6592_lcd_readback() through the sink. */
void mt6592_panel_dump(mt6592_panel_log_fn log, void* ctx);

#endif /* MT6592_PANEL_BRINGUP_H */
