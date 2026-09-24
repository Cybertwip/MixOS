/*
 * backlight.h — MT6592 / J36 Ultra panel backlight + LCD power rails
 *
 * Surgical extraction of the backlight path from the MediaTek kernel
 * (Reference/.../video + leds/pwm): PMIC DIGLDO LCD rails, DISP_PWM (BLS),
 * and the TPS61161 GPIO90 one-shot wake pulse train.
 */
#ifndef MT6592_BACKLIGHT_H
#define MT6592_BACKLIGHT_H

#include <stdint.h>

/* Enable the PMIC DIGLDO rails that power the LCD (VIO28/VIO18 plus the
 * regulator triplet used by the stock J36 Ultra LK LCM driver).
 * Returns 0 on success, or a bitmask of the rails that failed. */
uint32_t mt6592_backlight_power_rails(void);

/* Exact regulator cycle recovered from Reference/J36-ULTRA/lk.bin
 * jd9365_qc_190227_lcm_drv::lcm_init(). */
uint32_t mt6592_lcm_power_rails_off(void);
uint32_t mt6592_lcm_power_rails_on(void);

/* Diagnostic: read a 16-bit PMIC (MT6323) register over PWRAP/WACS2. Returns 0
 * and stores the value on success, non-zero on a wrapper error. Used by the
 * boot-status telemetry to confirm the LCD rails (VIO18/VIO28/VGP1) are on. */
int mt6592_pmic_read16_pub(uint32_t addr, uint32_t *out);

/* Program DISP_PWM duty + wake the panel backlight to `pct` (0..100). */
void mt6592_backlight_on(uint32_t pct);

/* Re-write the DISP_PWM registers even when the requested duty is unchanged.
 * The BLS video path shares the same block and can reset the PWM state. */
void mt6592_backlight_reassert(uint32_t pct);

#endif /* MT6592_BACKLIGHT_H */
