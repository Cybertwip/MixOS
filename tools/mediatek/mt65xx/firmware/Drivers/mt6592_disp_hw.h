/*
 * mt6592_disp_hw.h — MT6592 display hardware definitions (J36 Ultra)
 *
 * Shared register map + freestanding MMIO helpers for the surgically-extracted
 * MediaTek display drivers (backlight / dsi_drv / lcd_drv).  The register
 * offsets and field layouts mirror the stock MediaTek kernel headers
 *   Reference/android/mediatek/platform/mt6592/kernel/drivers/video/
 *     dsi_reg.h / lcd_reg.h / disp_drv_platform.h
 * but stripped of the Linux dependencies (mach headers, cmdq, m4u, proc_fs,
 * mutex) so the block compiles for armv7a-none-eabi -ffreestanding -nostdlib.
 *
 * The register VALUES / sequences carried by the .c files are aligned with
 * Reference/J36-ULTRA/lk.bin (jd9365_qc_190227_lcm_drv, MIPITX base
 * 0x10010000).
 */
#ifndef MT6592_DISP_HW_H
#define MT6592_DISP_HW_H

#include <stdint.h>
#include <stddef.h>

#include "mt6592_board_j36.h"

/* ── Panel geometry (JD9365 QC 190227, 640x480 RGB888) ── */
#define DISP_WIDTH   640u
#define DISP_HEIGHT  480u
#define DISP_PITCH   (DISP_WIDTH * 4u)

/* ── SoC peripheral bases ── */
#define MTK_TOPCKGEN_BASE      0x10000000u
#define MTK_GPIO_BASE          0x10005000u
#define MTK_WDT_BASE           0x10007000u
#define MTK_TOPRGU_BASE        0x10000500u
#define MTK_PWRAP_BASE         0x1000d000u
#define MTK_PERICFG_BASE       0x10003000u
#define MTK_INFRACFG_AO_BASE   0x10001000u
#define MTK_SPM_BASE           0x10006000u
#define MTK_APMIXED_BASE       0x10209000u

#define MTK_WDT_MODE           0x0000u
#define MTK_WDT_LENGTH         0x0004u
#define MTK_WDT_RESTART        0x0008u
#define MTK_WDT_INTERVAL       0x0010u
#define MTK_WDT_SWRST          0x0014u
#define MTK_WDT_REQ_MODE       0x0024u
#define MTK_WDT_MODE_KEY       0x22000000u
#define MTK_WDT_RESTART_KEY    0x1971u

#define MTK_AP_PLL_CON0        0x0000u
#define MTK_AP_PLL_CON1        0x0004u
#define MTK_AP_PLL_CON2        0x0008u
#define MTK_AP_PLL_CON3        0x000cu
#define MTK_UNIVPLL_CON0       0x0220u
#define MTK_UNIVPLL_CON1       0x0224u
#define MTK_UNIVPLL_PWR_CON0   0x022cu

/* ── Display sub-system (MMSYS / DDP) ── */
#define MTK_MMSYS_BASE         0x14000000u
#define MTK_OVL0_BASE          0x14007000u
#define MTK_RDMA0_BASE         0x14008000u
#define MTK_BLS_BASE           0x1400a000u
#define MTK_COLOR_BASE         0x1400b000u
#define MTK_DSI0_BASE          0x1400c000u
#define MTK_MUTEX_BASE         0x1400e000u

#define MTK_CLK_CFG_1          0x0040u
#define MTK_CLK_CFG_2          0x0060u
#define MTK_CLK_CFG_8          0x0080u
#define MTK_PERI_PDN0_CLR      0x0010u
#define MTK_INFRA_PDN_CLR      0x0044u
#define MTK_MMSYS_CG_CON0      0x0100u
#define MTK_MMSYS_CG_CLR0      0x0108u
#define MTK_MMSYS_CG_CON1      0x0110u
#define MTK_MMSYS_CG_CLR1      0x0118u
#define MTK_MMSYS_LCM_RST_B    0x013cu
#define MTK_MMSYS_OVL0_MOUT_EN 0x0030u
#define MTK_MMSYS_DISP_OUT_SEL 0x004cu

/* ── SPM / TOPAXI (display MTCMOS power domain) ── */
#define MTK_SPM_POWERON_CONFIG_SET 0x0000u
#define MTK_SPM_DIS_PWR_CON        0x023cu
#define MTK_SPM_PWR_STATUS         0x060cu
#define MTK_SPM_PWR_STATUS_S       0x0610u
#define MTK_TOPAXI_PROT_EN         0x0220u
#define MTK_TOPAXI_PROT_STA1       0x0228u

#define MTK_PWR_RST_B          (1u << 0)
#define MTK_PWR_ISO            (1u << 1)
#define MTK_PWR_ON             (1u << 2)
#define MTK_PWR_ON_S           (1u << 3)
#define MTK_PWR_CLK_DIS        (1u << 4)
#define MTK_SRAM_PDN           (0xfu << 8)
#define MTK_DIS_PWR_STA_MASK   (1u << 3)
#define MTK_DISP_PROT_MASK     0x0002u

/* ── DDP mutex ── */
#define MTK_MUTEX_INTEN        0x0000u
#define MTK_MUTEX_INTSTA       0x0004u
#define MTK_MUTEX_REG_COMMIT   0x000cu
#define MTK_MUTEX0_EN          0x0020u
#define MTK_MUTEX0             0x0024u
#define MTK_MUTEX0_RST         0x0028u
#define MTK_MUTEX0_MOD         0x002cu
#define MTK_MUTEX0_SOF         0x0030u
#define MTK_MUTEX_ACQUIRED     0x00000002u
#define MTK_MUTEX_INT_MUTEX0   0x00000001u
#define MTK_MUTEX_INT_TO0      0x00000040u
#define MTK_MUTEX_INTEN0       0x00000101u
#define MTK_MUTEX_MOD_OVL      (1u << 3)
#define MTK_MUTEX_MOD_COLOR    (1u << 7)
#define MTK_MUTEX_MOD_BLS      (1u << 9)
#define MTK_MUTEX_MOD_RDMA     (1u << 10)
#define MTK_MUTEX_MOD_DSI0     (1u << 13)
/* Stock J36 Ultra LK writes 0x488 for the visible video path. DSI is already
 * put into video mode by the host block; the DDP mutex only owns
 * OVL -> RDMA -> COLOR. BLS/PWM is configured separately. */
#define MTK_MUTEX_MOD_DSI_VDO  (MTK_MUTEX_MOD_OVL | MTK_MUTEX_MOD_RDMA | MTK_MUTEX_MOD_COLOR)

/* ── GPIO ── */
#define MTK_GPIO_DIR_BASE      0x0000u
#define MTK_GPIO_DOUT_BASE     0x0400u
#define MTK_GPIO_DIN_BASE      0x0500u
#define MTK_GPIO_MODE_BASE     0x0600u
#define MTK_GPIO_STRIDE        0x0010u
#define MTK_GPIO_SET           0x0004u
#define MTK_GPIO_RST           0x0008u
#define MTK_GPIO_PIN_PER_REG   16u
#define MTK_GPIO_MODE_BITS     3u
#define MTK_GPIO_MODE_PER_REG  5u
#define MTK_GPIO_MODE_MASK     0x7u
#define MTK_BACKLIGHT_GPIO     90u   /* DISP_PWM / TPS61161 control (known good) */

/* ── Peripheral bases used by the non-destructive LK handoff scan ── */
#define MTK_USB0_BASE          0x11200000u
#define MTK_AUDIO_AFE_BASE     0x11220000u
#define MTK_MSDC0_BASE         0x11230000u
#define MTK_MSDC1_BASE         0x11240000u
#define MTK_MSDC2_BASE         0x11250000u
#define MTK_KPD_BASE           0x10011000u

#define MTK_MSDC_CFG           0x0000u
#define MTK_MSDC_PS            0x0008u
#define MTK_MSDC_INT           0x000cu
#define MTK_MSDC_SDC_STS       0x003cu

#define MTK_USB_DEVCTL         0x0060u
#define MTK_USB_POWER          0x0068u
#define MTK_USB_INTRTX         0x006cu

#define MTK_AFE_AUDIO_TOP_CON0 0x0000u
#define MTK_AFE_DAC_CON0       0x0010u
#define MTK_AFE_I2S_CON        0x0018u
#define MTK_AFE_CONN0          0x0020u

#define MTK_KPD_STA            0x0000u
#define MTK_KPD_MEM1           0x0004u
#define MTK_KPD_MEM2           0x0008u
#define MTK_KPD_MEM3           0x000cu
#define MTK_KPD_MEM4           0x0010u
#define MTK_KPD_MEM5           0x0014u
#define MTK_KPD_DEBOUNCE       0x0018u
#define MTK_KPD_SCAN_TIMING    0x001cu
#define MTK_KPD_SEL            0x0020u
#define MTK_KPD_EN             0x0024u

/* LCD sideband GPIO candidates. Start at/near the known backlight GPIO and count.
 * These are used for direct GPIO reset of a generic MIPI LCD 30 panel and for
 * the GPIO scanner that emits logs so you can match test point labels on the board
 * photos to actual pin numbers while watching UART/bridge output. */
#define MTK_LCD_RST_GPIO_TRY0   89u
#define MTK_LCD_RST_GPIO_TRY1   91u
#define MTK_LCD_RST_GPIO_TRY2   88u
#define MTK_LCD_RST_GPIO_TRY3   92u
#define MTK_LCD_RST_GPIO_TRY4   85u

/* ── PMIC wrapper (PWRAP) WACS2 channel ── */
#define MTK_PWRAP_HIPRIO_ARB_EN 0x0050u
#define MTK_PWRAP_WACS2_EN      0x0094u
#define MTK_PWRAP_WACS2_CMD     0x009cu
#define MTK_PWRAP_WACS2_RDATA   0x00a0u
#define MTK_PWRAP_WACS2_VLDCLR  0x00a4u
#define MTK_PWRAP_WACS2         (1u << 3)

/* ── PMIC (MT6323) DIGLDO rails feeding the LCD ── */
#define MTK_PMIC_DIGLDO_CON0    0x0500u
#define MTK_PMIC_DIGLDO_CON7    0x050au
#define MTK_PMIC_DIGLDO_CON28   0x0530u
#define MTK_PMIC_DIGLDO_CON49   0x0556u

/* ── BLS (backlight / PWM) ── */
#define MTK_BLS_EN             0x0000u
#define MTK_BLS_PWM_CON_0      0x00a8u
#define MTK_BLS_PWM_CON_1      0x00acu
#define MTK_BLS_DEBUG          0x00b0u
#define MTK_BLS_ENABLE_BIT     (1u << 16)

/* ── DISP_PWM (same block as BLS on MT6592 J36 Ultra) ── */
#define MTK_DISP_PWM_CON_0     0x0010u
#define MTK_DISP_PWM_CON_1     0x0014u

/* ── OVL (overlay) ── */
#define MTK_OVL_INTEN          0x0004u
#define MTK_OVL_INTSTA         0x0008u
#define MTK_OVL_EN             0x000cu
#define MTK_OVL_RST            0x0014u
#define MTK_OVL_ROI_SIZE       0x0020u
#define MTK_OVL_DATAPATH_CON   0x0024u
#define MTK_OVL_ROI_BGCLR      0x0028u
#define MTK_OVL_SRC_CON        0x002cu
#define MTK_OVL_L0_CON         0x0030u
#define MTK_OVL_L0_SRCKEY      0x0034u
#define MTK_OVL_L0_SRC_SIZE    0x0038u
#define MTK_OVL_L0_OFFSET      0x003cu
#define MTK_OVL_L0_ADDR        0x0040u
#define MTK_OVL_L0_PITCH       0x0044u
#define MTK_OVL_L1_CON         0x0050u
#define MTK_OVL_L1_SRCKEY      0x0054u
#define MTK_OVL_L1_SRC_SIZE    0x0058u
#define MTK_OVL_L1_OFFSET      0x005cu
#define MTK_OVL_L1_ADDR        0x0060u
#define MTK_OVL_L1_PITCH       0x0064u
#define MTK_OVL_L2_CON         0x0070u
#define MTK_OVL_L2_SRCKEY      0x0074u
#define MTK_OVL_L2_SRC_SIZE    0x0078u
#define MTK_OVL_L2_OFFSET      0x007cu
#define MTK_OVL_L2_ADDR        0x0080u
#define MTK_OVL_L2_PITCH       0x0084u
#define MTK_OVL_L3_CON         0x0090u
#define MTK_OVL_L3_SRCKEY      0x0094u
#define MTK_OVL_L3_SRC_SIZE    0x0098u
#define MTK_OVL_L3_OFFSET      0x009cu
#define MTK_OVL_L3_ADDR        0x00a0u
#define MTK_OVL_L3_PITCH       0x00a4u
/* Per-layer RDMA-enable registers use the same +0x20 layer stride as the OVL
 * Lx_CON block (stock LK: 0x140070c0 / 0e0 / 100 / 120), not a packed word. */
#define MTK_OVL_RDMA0_CTRL     0x00c0u
#define MTK_OVL_RDMA1_CTRL     0x00e0u
#define MTK_OVL_RDMA2_CTRL     0x0100u
#define MTK_OVL_RDMA3_CTRL     0x0120u
#define MTK_OVL_INPUT_RGB565     0u
#define MTK_OVL_INPUT_RGB888     1u
#define MTK_OVL_INPUT_ARGB8888   2u
#define MTK_OVL_INPUT_PARGB8888  3u
#define MTK_OVL_INPUT_XRGB8888   4u

/* ── RDMA (direct-link reader) ── */
#define MTK_RDMA_INT_ENABLE    0x0000u
#define MTK_RDMA_INT_STATUS    0x0004u
#define MTK_RDMA_GLOBAL_CON    0x0010u
#define MTK_RDMA_SIZE_CON_0    0x0014u
#define MTK_RDMA_SIZE_CON_1    0x0018u
#define MTK_RDMA_MEM_CON       0x0024u
#define MTK_RDMA_MEM_START     0x0028u
#define MTK_RDMA_MEM_SRC_PITCH 0x002cu
#define MTK_RDMA_MEM_GMC_0     0x0030u
#define MTK_RDMA_FIFO_CON      0x0040u
#define MTK_RDMA_INPUT_RGB888  8u
#define MTK_RDMA_INPUT_ARGB    16u
#define MTK_RDMA_MODE_DIRECT   0u
#define MTK_RDMA_ENGINE_EN     0x00000001u

/* ── DSI host controller (mirror of dsi_reg.h DSI_REGS) ── */
#define MTK_DSI_START          0x0000u
#define MTK_DSI_STA            0x0004u
#define MTK_DSI_INTEN          0x0008u
#define MTK_DSI_INTSTA         0x000cu
#define MTK_DSI_CMD_DONE_FLAG  (1u << 1)
#define MTK_DSI_COM_CTRL       0x0010u
#define MTK_DSI_MODE_CTRL      0x0014u
#define MTK_DSI_TXRX_CTRL      0x0018u
#define MTK_DSI_PSCTRL         0x001cu
#define MTK_DSI_VSA_NL         0x0020u
#define MTK_DSI_VBP_NL         0x0024u
#define MTK_DSI_VFP_NL         0x0028u
#define MTK_DSI_VACT_NL        0x002cu
#define MTK_DSI_HSA_WC         0x0050u
#define MTK_DSI_HBP_WC         0x0054u
#define MTK_DSI_HFP_WC         0x0058u
#define MTK_DSI_BLLP_WC        0x005cu
#define MTK_DSI_CMDQ_SIZE      0x0060u
#define MTK_DSI_HSTX_CKL_WC    0x0064u
#define MTK_DSI_PHY_LCCON      0x0104u
#define MTK_DSI_PHY_LD0CON     0x0108u
#define MTK_DSI_PHY_TIMECON0   0x0110u
#define MTK_DSI_PHY_TIMECON1   0x0114u
#define MTK_DSI_PHY_TIMECON2   0x0118u
#define MTK_DSI_PHY_TIMECON3   0x011cu
#define MTK_DSI_PHY_TIMECON4   0x0120u
#define MTK_DSI_VM_CMD_CON     0x0130u
#define MTK_DSI_CMDQ           0x0180u
#define MTK_DSI_CMDQ_WORDS     32u

#define MTK_DSI_COM_DPHY_RESET (1u << 2)
#define MTK_DSI_LCCON_HS_TX_EN (1u << 0)
#define MTK_DSI_LCCON_WAKEUP   (1u << 2)
#define MTK_DSI_LD0CON_WAKEUP  (1u << 2)
#define MTK_DSI_VM_CMD_EN      (1u << 0)
#define MTK_DSI_VM_TS_VFP_EN   (1u << 5)

/* Panel link parameters from Reference/J36-ULTRA lk.bin get_params(). */
#define MTK_DSI_LANES          MT6592_J36_PANEL_DSI_LANES
#define MTK_DSI_LINK_BPP       24u
#define MTK_DSI_DOT_CLOCK_KHZ  (MT6592_J36_PANEL_PIXEL_CLOCK_HZ / 1000u)
#define MTK_DSI_LANE_FIELD     ((((1u << MTK_DSI_LANES) - 1u)) << 2)

/* ── MIPITX D-PHY PLL (mirror of dsi_reg.h DSI_PHY_REGS) ──
 * Reference/J36-ULTRA lk.bin uses physical 0x10010000 for PLL_CONx. */
#define MTK_MIPI_BASE          0x10010000u
#define MTK_MIPITX_DSI0_CON    0x0000u
#define MTK_MIPITX_CLOCK_LANE  0x0004u
#define MTK_MIPITX_DATA_LANE0  0x0008u
#define MTK_MIPITX_DATA_LANE1  0x000cu
#define MTK_MIPITX_DATA_LANE2  0x0010u
#define MTK_MIPITX_DATA_LANE3  0x0014u
#define MTK_MIPITX_TOP_CON     0x0040u
#define MTK_MIPITX_BG_CON      0x0044u
#define MTK_MIPITX_PLL_CON0    0x0050u
#define MTK_MIPITX_PLL_CON1    0x0054u
#define MTK_MIPITX_PLL_CON2    0x0058u
#define MTK_MIPITX_PLL_CON3    0x005cu
#define MTK_MIPITX_PLL_CHG     0x0060u
#define MTK_MIPITX_PLL_TOP     0x0064u
#define MTK_MIPITX_PLL_PWR     0x0068u
#define MTK_MIPITX_SW_CTRL     0x0080u

#define MTK_MIPITX_PLL_LOCK_BIT       (1u << 31)
#define MTK_MIPITX_PLL_LOCK_TIMEOUT   1000u
#define MTK_MIPITX_PLL_SDM_PWR_ON     (1u << 0)
#define MTK_MIPITX_PLL_SDM_ISO_EN     (1u << 1)
#define MTK_MIPITX_PLL_EN             (1u << 0)
#define MTK_MIPITX_PLL_VDO_EN         (1u << 12)
#define MTK_MIPITX_PLL_PREDIV_MASK    0x00000006u
#define MTK_MIPITX_PLL_TXDIV0_SHIFT   3u
#define MTK_MIPITX_PLL_TXDIV0_MASK    0x00000018u
#define MTK_MIPITX_PLL_TXDIV1_SHIFT   5u
#define MTK_MIPITX_PLL_TXDIV1_MASK    0x00000060u
#define MTK_MIPITX_PLL_POSDIV_MASK    0x00000380u
#define MTK_MIPITX_PLL_SDM_FRA_EN     (1u << 0)
#define MTK_MIPITX_PLL_SDM_PH_INIT    (1u << 1)
#define MTK_MIPITX_PLL_SDM_SSC_EN     (1u << 2)
#define MTK_MIPITX_PLL_SDM_PRD_SHIFT  16u
#define MTK_MIPITX_PLL_SDM_PRD_MASK   0xffff0000u
#define MTK_MIPITX_PLL_PCW_H_SHIFT    24u
#define MTK_MIPITX_PLL_PCW_H_MASK     0x7f000000u
#define MTK_MIPITX_PLL_PCW_16_23_SHIFT 16u
#define MTK_MIPITX_PLL_PCW_16_23_MASK 0x00ff0000u
#define MTK_MIPITX_PLL_PCW_8_15_SHIFT 8u
#define MTK_MIPITX_PLL_PCW_8_15_MASK  0x0000ff00u
#define MTK_MIPITX_PLL_PCW_0_7_MASK   0x000000ffu
#define MTK_MIPITX_PLL_SSC_DELTA_SHIFT 16u
#define MTK_MIPITX_PLL_SSC_DELTA_MASK 0xffff0000u
#define MTK_MIPITX_PLL_SSC_DELTA1_MASK 0x0000ffffu
#define MTK_MIPITX_CON_LDOCORE_EN     (1u << 0)
#define MTK_MIPITX_CON_CKG_LDOOUT_EN  (1u << 1)
#define MTK_MIPITX_LANE_LDOOUT_EN     (1u << 0)
#define MTK_MIPITX_BG_CORE_EN         (1u << 0)
#define MTK_MIPITX_BG_CKEN            (1u << 1)
#define MTK_MIPITX_TOP_HS_BIAS_EN     (1u << 1)
#define MTK_MIPITX_TOP_IMP_CAL_CODE   (8u << 4)
#define MTK_MIPITX_TOP_PAD_TIE_LOW    (1u << 11)
#define MTK_MIPITX_PLL_TOP_PRESERVE_SHIFT 8u
#define MTK_MIPITX_PLL_TOP_PRESERVE_MASK  0x00000300u
#define MTK_MIPITX_PLL_SDM_PCW_CHG    (1u << 0)
#define MTK_DSI_PLL_REF_MHZ           26u
#define MTK_DSI_PLL_PCW_NUM           13u

/* ── DCS commands ── */
#define DSI_DCS_SWRESET 0x01u
#define DSI_DCS_SLPOUT  0x11u
#define DSI_DCS_CASET   0x2Au
#define DSI_DCS_PASET   0x2Bu
#define DSI_DCS_RAMWR   0x2Cu
#define DSI_DCS_COLMOD  0x3Au
#define DSI_DCS_RAMWRC  0x3Cu
#define DSI_DCS_DISPON  0x29u

/* Warm LK-takeover build: the preloader already brought up the parent PLLs and
 * DRAM, so do NOT replay the cold APMIXED/TOPCKGEN MIPI-ref writes.
 * PRESERVE_USB is for the MVIIFlash live stage1 breadcrumb run over VCOM:
 * skip full UNIVPLL power-up so the BROM USB download session stays
 * configured (avoids "device not configured" on macOS /dev/cu.*). */
#ifndef MVII_MT6592_WARM_LK
#define MVII_MT6592_WARM_LK 0
#endif
#ifndef MVII_MT6592_PRESERVE_USB
#define MVII_MT6592_PRESERVE_USB 0
#endif

/* ── Freestanding MMIO helpers ── */
static inline uint32_t mtk_read32(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void mtk_write32(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

static inline void mtk_update32(uint32_t addr, uint32_t clear_mask, uint32_t set_mask)
{
    uint32_t value = mtk_read32(addr);
    value &= ~clear_mask;
    value |= set_mask;
    mtk_write32(addr, value);
}

static inline void mtk_irq_disable(void)
{
    __asm__ volatile("cpsid if\n"
                     "dsb sy\n"
                     "isb\n" ::: "memory");
}

static inline void mtk_watchdog_kick(void)
{
    mtk_write32(MTK_WDT_BASE + MTK_WDT_RESTART, MTK_WDT_RESTART_KEY);
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void mtk_watchdog_disable(void)
{
    uint32_t mode = mtk_read32(MTK_WDT_BASE + MTK_WDT_MODE);
    uint32_t toprgu = mtk_read32(MTK_TOPRGU_BASE);

    /* LK's watchdog disable path preserves unrelated mode bits, clears the
     * active watchdog control bits, and writes the key. Then pet the watchdog
     * on the confirmed LK restart register (0x10007008). */
    mtk_write32(MTK_WDT_BASE + MTK_WDT_MODE, (mode & ~0x4fu) | MTK_WDT_MODE_KEY);
    mtk_write32(MTK_TOPRGU_BASE, (toprgu & ~1u) | MTK_WDT_MODE_KEY);
    mtk_watchdog_kick();
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void mtk_delay(uint32_t cycles)
{
    for (volatile uint32_t i = 0; i < cycles; ++i) {}
}

static inline void mtk_delay_ms(uint32_t ms)
{
    while (ms--) mtk_delay(12000u);
}

/* ── Generic GPIO accessors (mode 0 = GPIO, dir 1=output). Used to scan
 * sideband lines (reset, enable) starting from known backlight GPIO90 and
 * to drive a reset line directly when the generic MIPI LCD 30 panel does not
 * respond to the stock MMSYS_LCM_RST_B path or JD9365 init. */
static inline void mtk_gpio_set_mode(uint32_t pin, uint32_t mode)
{
    uint32_t reg = MTK_GPIO_BASE + MTK_GPIO_MODE_BASE + (pin / MTK_GPIO_MODE_PER_REG) * MTK_GPIO_STRIDE;
    uint32_t shift = (pin % MTK_GPIO_MODE_PER_REG) * MTK_GPIO_MODE_BITS;
    mtk_update32(reg, MTK_GPIO_MODE_MASK << shift, (mode & MTK_GPIO_MODE_MASK) << shift);
}

static inline void mtk_gpio_dir_output(uint32_t pin)
{
    uint32_t reg = MTK_GPIO_BASE + MTK_GPIO_DIR_BASE + (pin / MTK_GPIO_PIN_PER_REG) * MTK_GPIO_STRIDE;
    uint32_t bit = 1u << (pin % MTK_GPIO_PIN_PER_REG);
    mtk_write32(reg + MTK_GPIO_SET, bit);
}

static inline void mtk_gpio_write(uint32_t pin, uint32_t high)
{
    uint32_t reg = MTK_GPIO_BASE + MTK_GPIO_DOUT_BASE + (pin / MTK_GPIO_PIN_PER_REG) * MTK_GPIO_STRIDE;
    uint32_t bit = 1u << (pin % MTK_GPIO_PIN_PER_REG);
    mtk_write32(reg + (high ? MTK_GPIO_SET : MTK_GPIO_RST), bit);
}

/* Pulse a GPIO as reset (active-low typical for LCM XRES). */
static inline void mtk_gpio_pulse_reset(uint32_t pin, uint32_t low_us, uint32_t high_us)
{
    mtk_gpio_set_mode(pin, 0u);
    mtk_gpio_dir_output(pin);
    mtk_gpio_write(pin, 0u);
    mtk_delay(low_us);
    mtk_gpio_write(pin, 1u);
    mtk_delay(high_us);
}

/* Simple GPIO scanner: walk a small range around the known backlight pin,
 * force each to output and emit a distinguishable pulse pattern + log hook.
 * The caller prints breadcrumbs so the operator can correlate UART/bridge
 * output with probing the test points on the MIPI LCD 30 FPC area. */
static inline void mtk_gpio_scan_range(uint32_t start, uint32_t count, void (*log_fn)(uint32_t pin, uint32_t phase))
{
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t pin = start + i;
        mtk_gpio_set_mode(pin, 0u);
        mtk_gpio_dir_output(pin);
        if (log_fn) log_fn(pin, 0);
        mtk_gpio_write(pin, 0u);
        mtk_delay(8000u);
        if (log_fn) log_fn(pin, 1);
        mtk_gpio_write(pin, 1u);
        mtk_delay(16000u);
        if (log_fn) log_fn(pin, 2);
        mtk_gpio_write(pin, 0u);
        mtk_delay(8000u);
        /* leave high for visibility */
        mtk_gpio_write(pin, 1u);
    }
}

#endif /* MT6592_DISP_HW_H */
