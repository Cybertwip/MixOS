/*
 * backlight.c — MT6592 / J36 Ultra panel backlight + LCD power rails
 *
 * Extracted from the MediaTek kernel backlight/leds/pwm + PMIC paths and the
 * validated J36 Ultra bring-up. Self-contained: the PMIC accesses use a local
 * PWRAP WACS2 helper (file-static, so no clash with mt6592_pwrap.c).
 */
#include "backlight.h"
#include "mt6592_disp_hw.h"

#include <stdint.h>

#define MTK_PMIC_DIGLDO_CON29   0x0532u
#define MTK_PMIC_DIGLDO_CON31   0x0536u
#define MTK_PMIC_DIGLDO_CON32   0x0538u

/* ── State: the TPS61161 wake pulse is a one-shot; the PWM regs are written
 * once and only re-programmed on a duty change. ── */
static uint32_t g_backlight_woke = 0;
static uint32_t g_backlight_pwm_written = 0;
static uint32_t g_backlight_last_pct = 0xffffffffu;

/* ── PMIC over PWRAP WACS2 ── */
static int mtk_pwrap_wait_idle(void)
{
    for (uint32_t i = 0; i < 100000u; ++i) {
        uint32_t status = mtk_read32(MTK_PWRAP_BASE + MTK_PWRAP_WACS2_RDATA);
        uint32_t fsm = (status >> 16) & 0x7u;
        uint32_t init_done = (status >> 21) & 0x1u;
        if (!init_done) return -1;
        if (fsm == 0x6u) {
            mtk_write32(MTK_PWRAP_BASE + MTK_PWRAP_WACS2_VLDCLR, 1u);
            continue;
        }
        if (fsm == 0u) return 0;
    }
    return -1;
}

static int mtk_pmic_write16(uint32_t addr, uint32_t value)
{
    uint32_t cmd;
    if (mtk_pwrap_wait_idle() != 0) return -1;
    mtk_write32(MTK_PWRAP_BASE + MTK_PWRAP_HIPRIO_ARB_EN,
                mtk_read32(MTK_PWRAP_BASE + MTK_PWRAP_HIPRIO_ARB_EN) | MTK_PWRAP_WACS2);
    mtk_write32(MTK_PWRAP_BASE + MTK_PWRAP_WACS2_EN, 1u);
    cmd = (1u << 31) | ((addr >> 1) << 16) | (value & 0xffffu);
    mtk_write32(MTK_PWRAP_BASE + MTK_PWRAP_WACS2_CMD, cmd);
    return mtk_pwrap_wait_idle();
}

static int mtk_pmic_read16(uint32_t addr, uint32_t *value)
{
    uint32_t cmd;
    if (mtk_pwrap_wait_idle() != 0) return -1;
    mtk_write32(MTK_PWRAP_BASE + MTK_PWRAP_HIPRIO_ARB_EN,
                mtk_read32(MTK_PWRAP_BASE + MTK_PWRAP_HIPRIO_ARB_EN) | MTK_PWRAP_WACS2);
    mtk_write32(MTK_PWRAP_BASE + MTK_PWRAP_WACS2_EN, 1u);
    cmd = ((addr >> 1) << 16);
    mtk_write32(MTK_PWRAP_BASE + MTK_PWRAP_WACS2_CMD, cmd);
    for (uint32_t i = 0; i < 100000u; ++i) {
        uint32_t status = mtk_read32(MTK_PWRAP_BASE + MTK_PWRAP_WACS2_RDATA);
        uint32_t fsm = (status >> 16) & 0x7u;
        if (fsm == 0x6u) {
            if (value) *value = status & 0xffffu;
            mtk_write32(MTK_PWRAP_BASE + MTK_PWRAP_WACS2_VLDCLR, 1u);
            return 0;
        }
    }
    return -1;
}

static int mtk_pmic_config16(uint32_t addr, uint32_t value, uint32_t mask, uint32_t shift)
{
    uint32_t current = 0;
    if (mtk_pmic_read16(addr, &current) != 0) return -1;
    current &= ~(mask << shift);
    current |= (value & mask) << shift;
    return mtk_pmic_write16(addr, current);
}

int mt6592_pmic_read16_pub(uint32_t addr, uint32_t *out)
{
    return mtk_pmic_read16(addr, out);
}

static uint32_t mt6592_lcm_power_rails_set(uint32_t on)
{
    uint32_t fail = 0u;

    if (!on) {
        if (mtk_pmic_config16(0x050cu, 0u, 0x1u, 15u) != 0) fail |= 1u;
        if (mtk_pmic_config16(MTK_PMIC_DIGLDO_CON29, 0u, 0x7u, 5u) != 0) fail |= 2u;
        if (mtk_pmic_config16(MTK_PMIC_DIGLDO_CON7, 0u, 0x1u, 15u) != 0) fail |= 4u;
        if (mtk_pmic_config16(MTK_PMIC_DIGLDO_CON28, 0u, 0x7u, 5u) != 0) fail |= 8u;
        return fail;
    }

    if (mtk_pmic_config16(MTK_PMIC_DIGLDO_CON29, 3u, 0x7u, 5u) != 0) fail |= 1u;
    if (mtk_pmic_config16(0x050cu, 1u, 0x1u, 15u) != 0) fail |= 2u;
    if (mtk_pmic_config16(MTK_PMIC_DIGLDO_CON28, 7u, 0x7u, 5u) != 0) fail |= 4u;
    if (mtk_pmic_config16(MTK_PMIC_DIGLDO_CON7, 1u, 0x1u, 15u) != 0) fail |= 8u;
    if (mtk_pmic_config16(MTK_PMIC_DIGLDO_CON32, 6u, 0x7u, 5u) != 0) fail |= 16u;
    if (mtk_pmic_config16(MTK_PMIC_DIGLDO_CON31, 1u, 0x1u, 15u) != 0) fail |= 32u;
    return fail;
}

uint32_t mt6592_lcm_power_rails_off(void)
{
    return mt6592_lcm_power_rails_set(0u);
}

uint32_t mt6592_lcm_power_rails_on(void)
{
    return mt6592_lcm_power_rails_set(1u);
}

uint32_t mt6592_backlight_power_rails(void)
{
    uint32_t fail = 0u;
    /* Do NOT enable VIO28 (DIGLDO_CON0) or VIO18 (DIGLDO_CON49): the stock LK
     * never writes either — the only PMIC rail writes in lk.bin are the six
     * DIGLDO regs in FUN_81e1c700 (== mt6592_lcm_power_rails_on). VIO28 is the
     * front power LED stock LK leaves dark, and driving these extra rails clamps
     * the panel bias node. The panel's 1.8V I/O comes from the VGP rails below. */
    fail |= mt6592_lcm_power_rails_on();
    mtk_delay(200000u);
    return fail;
}

static void mtk_gpio_output(uint32_t pin)
{
    uint32_t reg = MTK_GPIO_BASE + MTK_GPIO_DIR_BASE + (pin / MTK_GPIO_PIN_PER_REG) * MTK_GPIO_STRIDE;
    uint32_t bit = 1u << (pin % MTK_GPIO_PIN_PER_REG);
    mtk_write32(reg + MTK_GPIO_SET, bit);
}

void mt6592_backlight_on(uint32_t pct)
{
    uint32_t duty = (pct > 100u) ? 1023u : ((pct * 1023u) / 100u);
    uint32_t full = (pct >= 100u) ? 1u : 0u;

    /* Preserve the MIPI_26M reference: re-gating CLK_CFG_1 here would drop the
     * DSI byte clock mid-operation, so only re-open the MMSYS CG.
     * Use safe mask when PRESERVE_USB to avoid disturbing other clocks. */
#if MVII_MT6592_PRESERVE_USB
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR0, 0x001fffffu);
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR1, 0x0000000fu);
#else
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR0, 0xffffffffu);
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR1, 0xffffffffu);
#endif

    if (!g_backlight_pwm_written || g_backlight_last_pct != pct) {
        mtk_gpio_set_mode(MTK_BACKLIGHT_GPIO, 1u); /* DISP_PWM on GPIO90 */
        mtk_update32(MTK_BLS_BASE + MTK_BLS_DEBUG, 0x3u, 0x3u);
        mtk_update32(MTK_BLS_BASE + MTK_BLS_PWM_CON_0, (0x3ffu << 16) | 0x3u, (2u << 16) | 0x2u);
        mtk_write32(MTK_BLS_BASE + MTK_BLS_PWM_CON_1, 1023u | (duty << 16));
        mtk_update32(MTK_BLS_BASE + MTK_BLS_EN, 0u, MTK_BLS_ENABLE_BIT);
        g_backlight_pwm_written = 1u;
        g_backlight_last_pct = pct;
    }

    if (full) {
        /* Full brightness should stay in DISP_PWM mode at 100% duty. The old
         * 32-pulse TPS61161 fallback can select a low one-wire current step on
         * some panels, which looks like a permanently dim backlight. */
        mtk_gpio_set_mode(MTK_BACKLIGHT_GPIO, 1u);
        mtk_update32(MTK_BLS_BASE + MTK_BLS_DEBUG, 0x3u, 0x3u);
        mtk_update32(MTK_BLS_BASE + MTK_BLS_PWM_CON_0, (0x3ffu << 16) | 0x3u, (2u << 16) | 0x2u);
        mtk_write32(MTK_BLS_BASE + MTK_BLS_PWM_CON_1, 1023u | (1023u << 16));
        mtk_update32(MTK_BLS_BASE + MTK_BLS_EN, 0u, MTK_BLS_ENABLE_BIT);
        g_backlight_woke = 1u;
        mtk_delay(40000u);
        return;
    }

    /* TPS61161 wake fallback: a one-shot pulse train. Repeating it during swaps
     * looks like dimming/flashing, so after wake leave EN/PWM high as plain GPIO. */
    mtk_gpio_set_mode(MTK_BACKLIGHT_GPIO, 0u);
    mtk_gpio_output(MTK_BACKLIGHT_GPIO);
    mtk_gpio_write(MTK_BACKLIGHT_GPIO, 1u);
    if (!g_backlight_woke) {
        mtk_delay(80000u);
        for (uint32_t i = 0; i < 32u; ++i) {
            mtk_gpio_write(MTK_BACKLIGHT_GPIO, 0u);
            mtk_delay(2500u);
            mtk_gpio_write(MTK_BACKLIGHT_GPIO, 1u);
            mtk_delay(2500u);
        }
        g_backlight_woke = 1u;
#if MVII_MT6592_PRESERVE_USB
        mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR0, 0x001fffffu);
        mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR1, 0x0000000fu);
#else
        mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR0, 0xffffffffu);
        mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR1, 0xffffffffu);
#endif
        mtk_delay(40000u);
    }
    mtk_gpio_write(MTK_BACKLIGHT_GPIO, 1u);
    mtk_delay(1000u);
}

void mt6592_backlight_reassert(uint32_t pct)
{
    g_backlight_pwm_written = 0u;
    mt6592_backlight_on(pct);
}
