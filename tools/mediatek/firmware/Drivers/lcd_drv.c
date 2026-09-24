/*
 * lcd_drv.c — MT6592 LCD / DDP display path (J36 Ultra)
 *
 * 1:1 surgical extraction from
 *   Reference/android/mediatek/platform/mt6592/kernel/drivers/video/lcd_drv.c
 *   (and supporting core clock/mtcmos). Freestanding, trimmed.
 * Cold APMIXED path is now gated by PRESERVE_USB so that the BROM VCOM
 * stays configured during live stage1 breadcrumbs (fixes "device not configured").
 */
#include "lcd_drv.h"
#include "dsi_drv.h"
#include "mt6592_disp_hw.h"

#include <stdint.h>

/* Fault-guard globals defined in mt6592_dram.c (resumed by the stage1 entry data
 * abort handler) so a protected SPM read returns instead of hanging the bus. */
extern volatile uint32_t mt6592_stage1_abort_flag;
extern volatile uint32_t mt6592_stage1_abort_resume;

int mt6592_mmio_safe_read32(uint32_t addr, uint32_t *out) __attribute__((noinline));
int mt6592_mmio_safe_read32(uint32_t addr, uint32_t *out)
{
    uint32_t v = 0u;

    mt6592_stage1_abort_flag = 0u;
    mt6592_stage1_abort_resume = (uint32_t)(uintptr_t)&&faulted;
    __asm__ volatile("dsb sy\nisb" ::: "memory");

    v = mtk_read32(addr);

    __asm__ volatile("dsb sy\nisb" ::: "memory");
    mt6592_stage1_abort_resume = 0u;
    if (mt6592_stage1_abort_flag) goto faulted;

    if (out) *out = v;
    return 1;

faulted:
    mt6592_stage1_abort_resume = 0u;
    if (out) *out = 0xdab0da7au;
    return 0;
}

/* Local alias so the rest of this file keeps reading the way it did. */
#define lcd_safe_read32 mt6592_mmio_safe_read32

int mt6592_mmio_safe_write32(uint32_t addr, uint32_t value) __attribute__((noinline));
int mt6592_mmio_safe_write32(uint32_t addr, uint32_t value)
{
    mt6592_stage1_abort_flag = 0u;
    mt6592_stage1_abort_resume = (uint32_t)(uintptr_t)&&faulted;
    __asm__ volatile("dsb sy\nisb" ::: "memory");

    mtk_write32(addr, value);

    __asm__ volatile("dsb sy\nisb" ::: "memory");
    mt6592_stage1_abort_resume = 0u;
    if (mt6592_stage1_abort_flag) goto faulted;
    return 1;

faulted:
    mt6592_stage1_abort_resume = 0u;
    return 0;
}

void mt6592_lcd_clean_fb(uintptr_t start, uintptr_t len)
{
    uintptr_t end = start + len;
    if (end < start) return;
    start &= ~(uintptr_t)31u;
    while (start < end) {
        /* DCCMVAC: clean each cache line by VA to PoC. Posted writeback — it
         * does NOT stall on the DRAM write buffer. The OVL reads from DRAM, so
         * the cleaned data reaches scanout without a dsb. (A dsb sy here would
         * hang the AXI bus when the DQS read-gate is untrained.) */
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(start) : "memory");
        start += 32u;
    }
}

void mt6592_lcd_clean_sram_fb(uintptr_t start, uintptr_t len)
{
    uintptr_t end = start + len;
    if (end < start) return;
    start &= ~(uintptr_t)31u;
    while (start < end) {
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(start) : "memory");
        start += 32u;
    }
    __asm__ volatile("dsb sy\nisb" ::: "memory");
}

/* ── Display MTCMOS power domain ── */

/* Spin until `ready`, but never forever.
 *
 * These three polls used to be bare `while (...) {}`. In a stage image that is
 * merely rude — the stock LK has already powered SYS_DIS and every one of them
 * falls straight through. In the minimal LK it is a brick: this is the first
 * code after the preloader to touch SPM, and a domain that does not come up
 * takes the whole boot with it, with a lit panel and no OS and no way to say
 * why. Time out instead and let the caller carry on; a display that then does
 * not work is a bad boot, and a bootloader stuck in a poll is no boot at all.
 *
 * Bound is in read attempts, not time: SPM sits behind the same slow bus either
 * way and this code runs before any timer is guaranteed to be counting. */
#define MTK_DISP_POLL_LIMIT 200000u

static int mtk_poll_clear(uint32_t addr, uint32_t mask)
{
    for (uint32_t i = 0; i < MTK_DISP_POLL_LIMIT; ++i) {
        if (!(mtk_read32(addr) & mask)) return 1;
    }
    return 0;
}

static void mtk_mtcmos_disp_on(void)
{
    /* Ported from mt_spm_mtcmos.c spm_mtcmos_ctrl_disp(STA_POWER_ON). Without
     * this the DSI/MMSYS bus reads back zero even when APMIXED/TOPCKGEN are up.
     * The first SPM access is fault-guarded; if SPM is unreachable we skip and
     * rely on the preloader having left DISP powered. */
    uint32_t pwr_con;

    if (!lcd_safe_read32(MTK_SPM_BASE + MTK_SPM_DIS_PWR_CON, &pwr_con)) {
        return;
    }

    if ((pwr_con & (MTK_PWR_ON | MTK_PWR_ON_S)) == (MTK_PWR_ON | MTK_PWR_ON_S) &&
        (pwr_con & MTK_PWR_RST_B) && !(pwr_con & MTK_PWR_ISO) &&
        !(pwr_con & MTK_PWR_CLK_DIS)) {
        /* Already on: just make sure the AXI protection is clear. */
        mtk_update32(MTK_INFRACFG_AO_BASE + MTK_TOPAXI_PROT_EN, MTK_DISP_PROT_MASK, 0u);
        (void)mtk_poll_clear(MTK_INFRACFG_AO_BASE + MTK_TOPAXI_PROT_STA1, MTK_DISP_PROT_MASK);
        return;
    }

    /* Enable SPM register access (project code key + REG_CPU_CLR). */
    mtk_write32(MTK_SPM_BASE + MTK_SPM_POWERON_CONFIG_SET, (0xb16u << 16) | 1u);

    /* PWR_ON / PWR_ON_S */
    mtk_update32(MTK_SPM_BASE + MTK_SPM_DIS_PWR_CON, 0u, MTK_PWR_ON | MTK_PWR_ON_S);
    for (uint32_t i = 0; i < MTK_DISP_POLL_LIMIT; ++i) {
        if ((mtk_read32(MTK_SPM_BASE + MTK_SPM_PWR_STATUS) & MTK_DIS_PWR_STA_MASK) &&
            (mtk_read32(MTK_SPM_BASE + MTK_SPM_PWR_STATUS_S) & MTK_DIS_PWR_STA_MASK)) {
            break;
        }
    }

    /* Release CLK_DIS, ISO, assert RST_B, release SRAM_PDN. */
    mtk_update32(MTK_SPM_BASE + MTK_SPM_DIS_PWR_CON, MTK_PWR_CLK_DIS, 0u);
    mtk_update32(MTK_SPM_BASE + MTK_SPM_DIS_PWR_CON, MTK_PWR_ISO, 0u);
    mtk_update32(MTK_SPM_BASE + MTK_SPM_DIS_PWR_CON, 0u, MTK_PWR_RST_B);
    mtk_update32(MTK_SPM_BASE + MTK_SPM_DIS_PWR_CON, MTK_SRAM_PDN, 0u);

    /* Clear TOPAXI protection so display masters can reach the bus. */
    mtk_update32(MTK_INFRACFG_AO_BASE + MTK_TOPAXI_PROT_EN, MTK_DISP_PROT_MASK, 0u);
    (void)mtk_poll_clear(MTK_INFRACFG_AO_BASE + MTK_TOPAXI_PROT_STA1, MTK_DISP_PROT_MASK);
}

static void mtk_clkmgr_disp_on(void)
{
    /* Enable INFRA clocks feeding the display/SMI/M4U fabric and ungate every
     * DISP0/DISP1 clock gate (mt_clkmgr.c enable_clock for MT_CG_DISP*).
     *
     * For PRESERVE_USB (live VCOM) we deliberately avoid a full 0xffffffff
     * INFRA_PDN_CLR because the BROM USB session may rely on specific infra
     * clock states left by the download mode; only touch MMSYS (display) CGs
     * which are the ones the 1:1 android disp bring-up actually needs. */
#if MVII_MT6592_PRESERVE_USB
    /* Targeted: only the MMSYS display fabric. Full INFRA left alone. */
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR0, 0x001fffffu);
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR1, 0x0000000fu);
#else
    mtk_write32(MTK_INFRACFG_AO_BASE + MTK_INFRA_PDN_CLR, 0xffffffffu);
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR0, 0x001fffffu);
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR1, 0x0000000fu);
#endif
}

/* Safe TOPCKGEN mux for MIPI_26M ref. Called both from cold apmixed and from
 * the PRESERVE_USB / WARM paths (no UNIVPLL power cycle). */
static void mtk_topckgen_enable_mipi_ref(void)
{
    /* The MIPITX D-PHY is fed by MIPI_26M from TOPCKGEN: select CLK26M and
     * enable the gate bits (preserve every other bit). */
    mtk_update32(MTK_TOPCKGEN_BASE + MTK_CLK_CFG_1, 0x00000107u, 0x00000100u);
    mtk_update32(MTK_TOPCKGEN_BASE + MTK_CLK_CFG_8, 0x01010300u, 0x01010300u);
    mtk_delay(1000u);
}

#if !MVII_MT6592_WARM_LK && !MVII_MT6592_PRESERVE_USB
static void mtk_apmixed_enable_mipi_ref(void)
{
    /* Cold-boot UNIVPLL bring-up for the MIPITX HS byte-clock reference. Values
     * from the MT6592 preloader (drivers/pll.c). Skipped for PRESERVE_USB live
     * VCOM (and WARM LK) to avoid disturbing the USB phy clock. */
    uint32_t v;

    mtk_write32(MTK_APMIXED_BASE + MTK_AP_PLL_CON3, 0x00780000u);

    v = mtk_read32(MTK_APMIXED_BASE + MTK_UNIVPLL_PWR_CON0);
    mtk_write32(MTK_APMIXED_BASE + MTK_UNIVPLL_PWR_CON0, v | 0x1u);
    mtk_delay_ms(1u);
    v = mtk_read32(MTK_APMIXED_BASE + MTK_UNIVPLL_PWR_CON0);
    mtk_write32(MTK_APMIXED_BASE + MTK_UNIVPLL_PWR_CON0, v & ~0x2u);

    mtk_write32(MTK_APMIXED_BASE + MTK_UNIVPLL_CON1, 0x800c0000u);

    v = mtk_read32(MTK_APMIXED_BASE + MTK_UNIVPLL_CON0);
    mtk_write32(MTK_APMIXED_BASE + MTK_UNIVPLL_CON0, v | 0x1u);
    mtk_delay_ms(1u);

    v = mtk_read32(MTK_APMIXED_BASE + MTK_UNIVPLL_CON0);
    mtk_write32(MTK_APMIXED_BASE + MTK_UNIVPLL_CON0, v | 0x01000000u);

    v = mtk_read32(MTK_APMIXED_BASE + MTK_AP_PLL_CON1);
    mtk_write32(MTK_APMIXED_BASE + MTK_AP_PLL_CON1, v & 0xffccfdccu);
    v = mtk_read32(MTK_APMIXED_BASE + MTK_AP_PLL_CON2);
    mtk_write32(MTK_APMIXED_BASE + MTK_AP_PLL_CON2, v & 0xfffffffcu);
    mtk_write32(MTK_APMIXED_BASE + MTK_AP_PLL_CON0, 0x00000133u);
    mtk_delay_ms(1u);

    mtk_topckgen_enable_mipi_ref();

    mtk_write32(MTK_PERICFG_BASE + MTK_PERI_PDN0_CLR, 0x000fffffu);
    mtk_delay(1000u);
}
#endif /* !WARM && !PRESERVE */

void mt6592_lcd_clocks_on(void)
{
    mtk_mtcmos_disp_on();   /* power up SYS_DIS before touching clocks */
    mtk_clkmgr_disp_on();   /* INFRA + DISP_CG gates */

    /* Ensure every DSI/MMSYS clock gate is open before the MIPI reference PLL
     * and any MIPITX/DSI PHY register access. Use targeted mask when preserving
     * USB VCOM so we don't touch unrelated fabric clocks. */
#if MVII_MT6592_PRESERVE_USB
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR0, 0x001fffffu);
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR1, 0x0000000fu);
#else
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR0, 0xffffffffu);
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_CG_CLR1, 0xffffffffu);
#endif

#if !MVII_MT6592_WARM_LK && !MVII_MT6592_PRESERVE_USB
    mtk_apmixed_enable_mipi_ref();   /* cold full PLL; skipped to keep USB VCOM live */
#else
    mtk_topckgen_enable_mipi_ref();  /* just mux, preserve USB ref clock */
#endif
    /* Do NOT clear CLK_CFG_1[2:0] here: the MIPI_26M source is already
     * selected; re-gating it would kill the DSI HS byte clock. */
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_LCM_RST_B, 0u);
    mtk_delay(100000u);
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_LCM_RST_B, 1u);
    mtk_delay(200000u);
}

/* ── DDP route ── */
static uint32_t mtk_ovl_layer_reg(uint32_t layer, uint32_t l0_reg);

static int __attribute__((unused)) mtk_ddp_mutex_take(void)
{
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0, 0u);
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0_EN, 0u);
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0_RST, 1u);
    mtk_delay(100u);
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0_RST, 0u);
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0_MOD, MTK_MUTEX_MOD_DSI_VDO);
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0_SOF, 1u);
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX_INTSTA, MTK_MUTEX_INT_MUTEX0 | MTK_MUTEX_INT_TO0);
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX_INTEN, 0x0000ffffu);
    __asm__ volatile("dsb sy\nisb" ::: "memory");

    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0_EN, 1u);
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0, 1u);
    for (uint32_t i = 0; i < 20000u; ++i) {
        if ((mtk_read32(MTK_MUTEX_BASE + MTK_MUTEX0) & MTK_MUTEX_ACQUIRED) == MTK_MUTEX_ACQUIRED) return 0;
        mtk_delay(64u);
    }
    return -1;
}

static void mtk_color_bypass(void)
{
    mtk_write32(MTK_COLOR_BASE + 0x0400u, 0x2000323cu);
    mtk_write32(MTK_COLOR_BASE + 0x0f00u, 0x00000001u);
    mtk_write32(MTK_COLOR_BASE + 0x0f50u, DISP_WIDTH);
    mtk_write32(MTK_COLOR_BASE + 0x0f54u, DISP_HEIGHT);
}

static void mtk_bls_video_bypass(void)
{
    /* Stock LK DOES program the BLS pixel-path in the DSI video route: the DSI
     * setup (FUN_81e0d5c0) calls FUN_81e14900 right after the engine config,
     * which writes the four registers below (lk.bin decompile :17000-17003).
     * Although the mutex MOD mask (0x488 = OVL|COLOR|RDMA) does not list the BLS
     * bit, the COLOR->DSI datapath still runs through BLS on this board, so an
     * un-sized BLS passes black even with the backlight lit. Mirror the stock
     * pixel-path enable exactly:
     *   0x1400a0a0 = 0            (source offset / GMC reset)
     *   0x1400a0a8 = height << 16 (source ROI size)
     *   0x1400a000 = 0x00010000   (BLS pixel-path master enable, bit16)
     *   0x1400a0b0 = 0            (BLS_DEBUG cleared) */
    mtk_write32(MTK_BLS_BASE + 0x00a0u, 0u);
    mtk_write32(MTK_BLS_BASE + 0x00a8u, (uint32_t)DISP_HEIGHT << 16);
    mtk_write32(MTK_BLS_BASE + MTK_BLS_EN, 0x00010000u);
    mtk_write32(MTK_BLS_BASE + MTK_BLS_DEBUG, 0u);
}

/* Which OVL layer the last scanout configured. The J36 route uses layer 2, so a
 * dump hard-wired to layer 0 reports the registers of a layer nobody programmed:
 * con=0x000000ff, addr/pitch/size all zero. That is exactly what a broken layer
 * 2 would look like, which cost a hardware round trip to find out it was the
 * instrument and not the patient. Recorded here so the dump follows the route. */
static uint32_t g_lcd_layer;

void mt6592_lcd_readback(uint32_t out[16])
{
    const uint32_t layer = g_lcd_layer & 3u;

    if (!out) return;
    (void)lcd_safe_read32(MTK_MMSYS_BASE + MTK_MMSYS_OVL0_MOUT_EN, &out[0]);
    (void)lcd_safe_read32(MTK_MMSYS_BASE + MTK_MMSYS_DISP_OUT_SEL, &out[1]);
    (void)lcd_safe_read32(MTK_OVL0_BASE + MTK_OVL_EN, &out[2]);
    (void)lcd_safe_read32(MTK_OVL0_BASE + MTK_OVL_SRC_CON, &out[3]);
    (void)lcd_safe_read32(MTK_RDMA0_BASE + MTK_RDMA_GLOBAL_CON, &out[4]);
    (void)lcd_safe_read32(MTK_OVL0_BASE + MTK_OVL_ROI_BGCLR, &out[5]);
    (void)lcd_safe_read32(MTK_MUTEX_BASE + MTK_MUTEX0_MOD, &out[6]);
    (void)lcd_safe_read32(MTK_BLS_BASE + MTK_BLS_EN, &out[7]);
    out[8] = layer;
    (void)lcd_safe_read32(MTK_OVL0_BASE + mtk_ovl_layer_reg(layer, MTK_OVL_L0_CON), &out[9]);
    (void)lcd_safe_read32(MTK_OVL0_BASE + mtk_ovl_layer_reg(layer, MTK_OVL_L0_ADDR), &out[10]);
    (void)lcd_safe_read32(MTK_OVL0_BASE + mtk_ovl_layer_reg(layer, MTK_OVL_L0_PITCH), &out[11]);
    (void)lcd_safe_read32(MTK_OVL0_BASE + mtk_ovl_layer_reg(layer, MTK_OVL_L0_SRC_SIZE), &out[12]);
    (void)lcd_safe_read32(MTK_OVL0_BASE + MTK_OVL_RDMA0_CTRL + layer * 0x20u, &out[13]);
    (void)lcd_safe_read32(MTK_MUTEX_BASE + MTK_MUTEX0, &out[14]);
    (void)lcd_safe_read32(MTK_MUTEX_BASE + MTK_MUTEX0_SOF, &out[15]);
}

static void mtk_ddp_lk_video_route(void)
{
    /* Reference/J36-ULTRA lk.bin dsi_config_ddp(): DSI video route is
     * OVL0 -> RDMA0 -> COLOR -> DSI0. BLS/PWM is not in this mutex. */
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0_MOD, MTK_MUTEX_MOD_DSI_VDO);
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0_SOF, 1u);
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX_INTSTA, MTK_MUTEX_INT_MUTEX0 | MTK_MUTEX_INT_TO0);
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX_INTEN, 0x0000ffffu);
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0_EN, 1u);
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_OVL0_MOUT_EN, 0x00000001u);
    mtk_write32(MTK_MMSYS_BASE + MTK_MMSYS_DISP_OUT_SEL, 0x00000000u);
}

static void mtk_ovl_stop_lk(void)
{
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_INTEN, 0u);
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_EN, 0u);
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_INTSTA, 0u);
}

static void mtk_ovl_roi_lk(uint32_t width, uint32_t height, uint32_t bg_color)
{
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_ROI_SIZE, (height << 16) | width);
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_ROI_BGCLR, bg_color);
}

static uint32_t mtk_ovl_layer_reg(uint32_t layer, uint32_t l0_reg)
{
    return l0_reg + layer * 0x20u;
}

static void mtk_ovl_layer_enable_lk(uint32_t layer, uint32_t enable)
{
    uint32_t bit = 1u << (layer & 3u);
    uint32_t src = mtk_read32(MTK_OVL0_BASE + MTK_OVL_SRC_CON);
    src = enable ? (src | bit) : (src & ~bit);
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_SRC_CON, src);
}

static void mtk_ovl_layers_disable_all_lk(void)
{
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_SRC_CON, 0u);
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_RDMA0_CTRL, 0u);
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_RDMA1_CTRL, 0u);
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_RDMA2_CTRL, 0u);
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_RDMA3_CTRL, 0u);
}

static void mtk_ovl_layer_config_lk(uint32_t layer, uint32_t fb_addr,
                                    uint32_t width, uint32_t height,
                                    uint32_t pitch, uint32_t fmt)
{
    uint32_t con;
    uint32_t con_reg;
    uint32_t pitch_reg;

    layer &= 3u;
    con_reg = mtk_ovl_layer_reg(layer, MTK_OVL_L0_CON);
    pitch_reg = mtk_ovl_layer_reg(layer, MTK_OVL_L0_PITCH);

    mtk_write32(MTK_OVL0_BASE + mtk_ovl_layer_reg(layer, MTK_OVL_L0_SRC_SIZE),
                (height << 16) | width);
    /* Per-layer RDMA-enable registers are at OVL+0xc0 + layer*0x20
     * (0xc0/0xe0/0x100/0x120), matching the stock LK +0x20 layer stride —
     * NOT a packed 0xc0/0xc4/0xc8/0xcc block. */
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_RDMA0_CTRL + layer * 0x20u, 1u);
    mtk_write32(MTK_OVL0_BASE + mtk_ovl_layer_reg(layer, MTK_OVL_L0_OFFSET), 0u);

    con = mtk_read32(MTK_OVL0_BASE + con_reg);
    con &= 0x8fff0e00u;
    con |= (fmt & 0x0fu) << 12;
    con |= 0xffu;
    mtk_write32(MTK_OVL0_BASE + con_reg, con);

    mtk_write32(MTK_OVL0_BASE + pitch_reg,
                (pitch & 0xffffu) | (mtk_read32(MTK_OVL0_BASE + pitch_reg) & 0xffff0000u));
    mtk_write32(MTK_OVL0_BASE + mtk_ovl_layer_reg(layer, MTK_OVL_L0_SRCKEY), 0u);
    mtk_write32(MTK_OVL0_BASE + mtk_ovl_layer_reg(layer, MTK_OVL_L0_ADDR), fb_addr);
}

static void mtk_ovl_start_lk(void)
{
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_INTEN, 0x0fu);
    mtk_write32(MTK_OVL0_BASE + MTK_OVL_EN, 1u);
}

static void mtk_rdma_stop_lk(void)
{
    mtk_update32(MTK_RDMA0_BASE + MTK_RDMA_GLOBAL_CON, 0x00000001u, 0u);
    mtk_write32(MTK_RDMA0_BASE + MTK_RDMA_INT_ENABLE, 0u);
    mtk_write32(MTK_RDMA0_BASE + MTK_RDMA_INT_STATUS, 0u);
}

static void mtk_rdma_reset_lk(void)
{
    mtk_update32(MTK_RDMA0_BASE + MTK_RDMA_GLOBAL_CON, 0u, 0x00000010u);
    for (uint32_t i = 0; i < 0x2711u; ++i) {
        if ((mtk_read32(MTK_RDMA0_BASE + MTK_RDMA_GLOBAL_CON) & 0x00000700u) != 0x00000100u) {
            break;
        }
    }
    mtk_update32(MTK_RDMA0_BASE + MTK_RDMA_GLOBAL_CON, 0x00000010u, 0u);
}

static void mtk_rdma_config_direct_lk(uint32_t format, uint32_t width,
                                      uint32_t height, uint32_t pitch)
{
    uint32_t size0 = mtk_read32(MTK_RDMA0_BASE + MTK_RDMA_SIZE_CON_0);
    uint32_t size1 = mtk_read32(MTK_RDMA0_BASE + MTK_RDMA_SIZE_CON_1);

    mtk_update32(MTK_RDMA0_BASE + MTK_RDMA_GLOBAL_CON, 0x00000002u, 0u);
    mtk_update32(MTK_RDMA0_BASE + MTK_RDMA_MEM_CON, 0x000001f0u, (format & 0x1fu) << 4);
    mtk_write32(MTK_RDMA0_BASE + MTK_RDMA_MEM_START, 0u);
    mtk_write32(MTK_RDMA0_BASE + MTK_RDMA_MEM_SRC_PITCH, pitch);

    size0 &= 0x1ffff000u;
    size0 |= width & 0x00000fffu;
    size1 &= 0xfff00000u;
    size1 |= height & 0x000fffffu;
    mtk_write32(MTK_RDMA0_BASE + MTK_RDMA_SIZE_CON_0, size0);
    mtk_write32(MTK_RDMA0_BASE + MTK_RDMA_SIZE_CON_1, size1);
}

static void mtk_rdma_start_lk(void)
{
    mtk_write32(MTK_RDMA0_BASE + MTK_RDMA_INT_ENABLE, 0x3fu);
    mtk_update32(MTK_RDMA0_BASE + MTK_RDMA_GLOBAL_CON, 0u, MTK_RDMA_ENGINE_EN);
}

static void mtk_ddp_commit_lk(void)
{
    /* 0x1400e024 is not a "commit" register, it is a lock, and both halves of it
     * have now been observed failing on this board:
     *
     *  - Writing 0 without ever having written 1 (the old mtk_ddp_release_lk(),
     *    called at the end of the scanout setup) releases a lock nobody holds.
     *    Nothing is latched, scanout never begins: pure black behind a perfectly
     *    configured DSI stream.
     *  - Writing 1 and leaving it set latches the path for exactly one frame and
     *    then holds it there. That is the state this function used to leave the
     *    hardware in, and its signature on the J36 is unmistakable: the panel
     *    shows LK_BEACON_SCANOUT dark blue forever, while every later beacon —
     *    teal, dark green, green, and the white hand-off — repaints the same
     *    framebuffer to no visible effect, and so does everything the OS draws
     *    into it afterwards. One frame reached the panel; the mutex ate the rest.
     *
     * Stock does the pair, and only the pair: FUN_81e161e8 takes the lock
     * (MUTEX0_EN=1, MUTEX0=1, poll bit1 ACQUIRED, up to 0x2711 spins) and
     * FUN_81e14adc gives it back (MUTEX0=0) once the layers are configured. The
     * J36 satisfies the two-layer condition guarding that release call, so on
     * this board the stock LK provably ends with the lock free.
     *
     * The acquire is kept after the OVL/RDMA writes rather than around them
     * because on a cold path there is no SOF to grant it against until the
     * engines are running — the poll is the latch boundary, the release is what
     * lets the path free-run past it. */
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0_EN, 1u);
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0, 1u);
    for (uint32_t i = 0; i < 0x2711u; ++i) {
        if (mtk_read32(MTK_MUTEX_BASE + MTK_MUTEX0) & MTK_MUTEX_ACQUIRED) break;
        mtk_delay(64u);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    mtk_write32(MTK_MUTEX_BASE + MTK_MUTEX0, 0u);
}

static int mt6592_lcd_scanout_layer_config(uint32_t layer, uint32_t fb_addr,
                                           uint32_t layer_width, uint32_t layer_height,
                                           uint32_t layer_pitch, uint32_t ovl_format,
                                           uint32_t rdma_format, uint32_t bg_color,
                                           uint32_t enable_layer)
{
    /* The DSI host + MIPITX PLL are already configured (mt6592_dsi_video_setup)
     * and the panel program has been sent in LP. This routine only builds the
     * DDP pixel path (OVL -> RDMA -> COLOR -> DSI), enables the LK-style mutex,
     * and leaves DSI in continuous video mode. */
    if (layer_width == 0u || layer_width > DISP_WIDTH) layer_width = DISP_WIDTH;
    if (layer_height == 0u || layer_height > DISP_HEIGHT) layer_height = DISP_HEIGHT;
    if (layer_pitch == 0u) layer_pitch = DISP_PITCH;
    g_lcd_layer = layer & 3u;

    /* Stock LK switches DSI_MODE_CTRL to video before dsi_config_ddp(); the
     * mutex/RDMA path then drives continuous scanout. */
    mt6592_dsi_video_start();

    mtk_ddp_lk_video_route();
    mtk_ovl_stop_lk();
    mtk_ovl_roi_lk(DISP_WIDTH, DISP_HEIGHT, bg_color ? bg_color : 0xff000000u);
    /* Stock LK never writes OVL DATAPATH_CON (0x14007024) — it only reads it for
     * its debug dump. Leave it at the preloader/reset default; forcing
     * 0x60000000 here is an un-validated divergence that can blank scanout. */
    mtk_ovl_layers_disable_all_lk();
    if (enable_layer) {
        mtk_ovl_layer_config_lk(layer, fb_addr, layer_width, layer_height, layer_pitch, ovl_format);
        mtk_ovl_layer_enable_lk(layer, 1u);
    }
    mtk_ovl_start_lk();

    mtk_color_bypass();
    mtk_bls_video_bypass();

    /* RDMA still has to run in direct-link mode for OVL -> RDMA -> DSI, even
     * when OVL is sourcing only ROI_BGCLR. Solid mode avoids DRAM by disabling
     * OVL layers, not by removing RDMA from the DDP route. */
    mtk_rdma_stop_lk();
    mtk_rdma_reset_lk();
    mtk_rdma_config_direct_lk(rdma_format, DISP_WIDTH, DISP_HEIGHT, layer_pitch);
    mtk_rdma_start_lk();
    /* Take the DDP mutex, let it latch the assembled OVL->RDMA->COLOR path, and
     * give it straight back so continuous DSI-video scanout can run (stock
     * FUN_81e161e8 + FUN_81e14adc). Holding it is what pinned the panel to a
     * single frame; see mtk_ddp_commit_lk(). */
    mtk_ddp_commit_lk();

    /* Transmitter last: mt6592_dsi_video_start() had to run first so the mutex
     * had a DSI0 start-of-frame to be granted against, but that put the engine
     * on air while the OVL was still half-programmed. Restart it now that the
     * path behind it is complete. */
    mt6592_dsi_video_kick();

    return 0;
}

static int mt6592_lcd_scanout_config(uint32_t fb_addr, uint32_t layer_width,
                                     uint32_t layer_height, uint32_t layer_pitch,
                                     uint32_t ovl_format, uint32_t rdma_format,
                                     uint32_t bg_color, uint32_t enable_layer)
{
    return mt6592_lcd_scanout_layer_config(0u, fb_addr, layer_width, layer_height,
                                           layer_pitch, ovl_format, rdma_format,
                                           bg_color, enable_layer);
}

int mt6592_lcd_scanout(uint32_t fb_addr, uint32_t solid_color)
{
    if (solid_color) {
        return mt6592_lcd_scanout_config(0u, DISP_WIDTH, DISP_HEIGHT, DISP_PITCH,
                                         MTK_OVL_INPUT_ARGB8888, MTK_RDMA_INPUT_RGB888,
                                         solid_color, 0u);
    }

    return mt6592_lcd_scanout_config(fb_addr, DISP_WIDTH, DISP_HEIGHT, DISP_PITCH,
                                     MTK_OVL_INPUT_ARGB8888, MTK_RDMA_INPUT_RGB888,
                                     0xff000000u, 1u);
}

int mt6592_lcd_scanout_rgb888_patch(uint32_t fb_addr, uint32_t width,
                                    uint32_t height, uint32_t pitch,
                                    uint32_t bg_color)
{
    return mt6592_lcd_scanout_config(fb_addr, width, height, pitch,
                                     MTK_OVL_INPUT_RGB888, MTK_RDMA_INPUT_RGB888,
                                     bg_color, 1u);
}

int mt6592_lcd_scanout_lk_rgb565_layer3(uint32_t fb_addr, uint32_t pitch,
                                        uint32_t bg_color)
{
    return mt6592_lcd_scanout_layer_config(3u, fb_addr, DISP_WIDTH, DISP_HEIGHT,
                                           pitch, MTK_OVL_INPUT_RGB565,
                                           MTK_RDMA_INPUT_RGB888, bg_color, 1u);
}

int mt6592_lcd_scanout_lk_argb8888_layer2(uint32_t fb_addr, uint32_t pitch,
                                          uint32_t bg_color)
{
    /* The stock J36 Ultra LK scans its boot framebuffer out on OVL layer 2
     * (memory source, color format 1), verified in the lk.bin decompile
     * (FUN_81e13e38 caller: layer=2, src=0). Mirror that exact layer so the
     * LK-replacement uses the proven-visible scanout. */
    return mt6592_lcd_scanout_layer_config(2u, fb_addr, DISP_WIDTH, DISP_HEIGHT,
                                           pitch ? pitch : DISP_PITCH,
                                           MTK_OVL_INPUT_ARGB8888,
                                           MTK_RDMA_INPUT_RGB888, bg_color, 1u);
}
