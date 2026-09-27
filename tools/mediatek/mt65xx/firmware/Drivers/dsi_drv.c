/*
 * dsi_drv.c — MT6592 DSI host + MIPITX D-PHY (J36 Ultra / JD9365)
 *
 * 1:1 surgical extraction from
 *   Reference/android/mediatek/platform/mt6592/kernel/drivers/video/dsi_drv.c
 * Register sequences, PHY PLL, TIMCON, mode switches, and panel params are
 * matched to Reference/J36-ULTRA/lk.bin (linux glue trimmed).
 */
#include "dsi_drv.h"
#include "dsi_reg.h"
#include "mt6592_board_j36.h"

#include <stdint.h>

/* ── Physical bases (bare metal). See dsi_reg.h header note. ── */
#define DSI_BASE          0x1400C000u
#define MIPI_CONFIG_BASE  0x10010000u
#define DSI_EFUSE_RES3    0x10206180u   /* IO_VIRT_TO_PHYS(0xF0206180) */

static PDSI_REGS     const DSI_REG     = (PDSI_REGS)DSI_BASE;
static PDSI_PHY_REGS const DSI_PHY_REG = (PDSI_PHY_REGS)MIPI_CONFIG_BASE;

/* DSI command queue (DSI_BASE + 0x180): 32 x 4 bytes. */
#define DSI_CMDQ(i)      (DSI_BASE + 0x180u + ((i) * 4u))
#define DSI_CMDQ_WORDS   32u

/* ── DSI host control registers ──
 * These match dsi_reg.h's struct layout, which the stock image confirms.  An
 * earlier revision of this file claimed MT6592 moved COM_CTRL to 0x008 and
 * INTEN to 0x010; that was wrong on every count and left DSI_EN permanently
 * clear, so the host never executed a single CMDQ packet.  From
 * Reference/j36-lk-reverse/lk.full-decompile.c:
 *   0x008 = DSI_INTEN                                       [:14798  "0x008 |= 3" arms RD_RDY]
 *   0x00c = DSI_INTSTA  (bit31 = BUSY)                      [:14812  ack, :14828 poll bit0]
 *   0x010 = DSI_COM_CTRL, bit0 = DSI_RESET, bit1 = DSI_EN   [FUN_81e0f274 :13480-13491 pulses
 *                                                            bit0 with a 5000-unit hold;
 *                                                            eight "0x1400c010 |= 2" sites
 *                                                            (:11722, :12459..:12499) set EN]
 *   0x014 = DSI_MODE_CTRL (low 2 bits = mode)               [FUN_81e0f2c8 :13508-13519]
 * Raw offsets are kept (rather than the struct fields) only because reset and
 * enable are whole-register RMWs; the addresses are the struct's. */
#define DSI_REG_COM_CTRL  (DSI_BASE + 0x010u)
#define DSI_COM_CTRL_RESET  0x00000001u   /* DSI_RESET (bit0) */
#define DSI_COM_CTRL_EN     0x00000002u   /* DSI_EN    (bit1) */

/* ── Panel parameters (the kernel's lcm_params.dsi). J36 Ultra / JD9365. ── */
typedef struct {
    unsigned int PLL_CLOCK;             /* MHz; data_rate = PLL_CLOCK*2 */
    unsigned int LANE_NUM;              /* 1..4 */
    unsigned int mode;                  /* DSI_*_VDO_MODE */
    unsigned int ssc_disable;
    unsigned int ssc_range;             /* 0 => default delta1 = 5 */
    unsigned int rgb565;                /* 0 => RGB888 (3 bytes/pixel) */

    unsigned int vertical_sync_active;
    unsigned int vertical_backporch;
    unsigned int vertical_frontporch;
    unsigned int vertical_active_line;

    unsigned int horizontal_sync_active;
    unsigned int horizontal_backporch;
    unsigned int horizontal_frontporch;
    unsigned int horizontal_active_pixel;
    unsigned int horizontal_bllp;
} dsi_lcm_params;

/* Values decoded from jd9365_qc_190227_lcm_drv::get_params() in
 * Reference/J36-ULTRA/lk.bin. */
static const dsi_lcm_params g_lcm = {
    .PLL_CLOCK = MT6592_J36_PANEL_DSI_PLL_CLOCK_MHZ,
    .LANE_NUM  = MT6592_J36_PANEL_DSI_LANES,
    .mode      = DSI_SYNC_EVENT_VDO_MODE,
    .ssc_disable = 1,
    .ssc_range   = 0,
    .rgb565      = 0,

    .vertical_sync_active   = MT6592_J36_PANEL_VSYNC,
    .vertical_backporch     = MT6592_J36_PANEL_VBP,
    .vertical_frontporch    = MT6592_J36_PANEL_VFP,
    .vertical_active_line   = MT6592_J36_PANEL_HEIGHT,

    .horizontal_sync_active = MT6592_J36_PANEL_HSYNC,
    .horizontal_backporch   = MT6592_J36_PANEL_HBP,
    .horizontal_frontporch  = MT6592_J36_PANEL_HFP,
    .horizontal_active_pixel= MT6592_J36_PANEL_WIDTH,
    .horizontal_bllp        = 0,
};

/* ── freestanding helpers (replace mdelay / ASSERT) ── */
static void mdelay(uint32_t ms)      { while (ms--) for (volatile uint32_t i = 0; i < 12000u; ++i) {} }
static void udelay_cycles(uint32_t c){ for (volatile uint32_t i = 0; i < c; ++i) {} }

/* ============================ DSI host ============================ */

void DSI_PowerOn(void)
{
    /* COM_CTRL.DSI_EN (0x010 bit1). Without this the host latches INTSTA.BUSY
     * and retires nothing: every CMDQ packet is written and never sent. */
    uint32_t v = INREG32(DSI_REG_COM_CTRL) | DSI_COM_CTRL_EN;
    OUTREG32(DSI_REG_COM_CTRL, v);
}

void DSI_Reset(void)
{
    /* COM_CTRL.DSI_RESET (0x010 bit0): assert, hold, release — preserving
     * DSI_EN. FUN_81e0f274 holds for 5000 delay units before releasing; the
     * release edge is what re-arms the host, so the hold is not optional. */
    uint32_t v = INREG32(DSI_REG_COM_CTRL);
    OUTREG32(DSI_REG_COM_CTRL, v | DSI_COM_CTRL_RESET);
    udelay_cycles(5000u);
    OUTREG32(DSI_REG_COM_CTRL, INREG32(DSI_REG_COM_CTRL) & ~DSI_COM_CTRL_RESET);
}

void DSI_SetMode(unsigned int mode)
{
    /* MODE_CTRL (0x014) low 2 bits — struct offset already correct. */
    OUTREGBIT(DSI_MODE_CTRL_REG, DSI_REG->DSI_MODE_CTRL, MODE, mode);
}

void DSI_EnableClk(void)
{
    uint32_t v = INREG32(DSI_REG_COM_CTRL) | DSI_COM_CTRL_EN;
    OUTREG32(DSI_REG_COM_CTRL, v);
}

void DSI_Start(void)
{
    OUTREGBIT(DSI_START_REG, DSI_REG->DSI_START, DSI_START, 0);
    OUTREGBIT(DSI_START_REG, DSI_REG->DSI_START, DSI_START, 1);
}

int DSI_clk_HS_state(void)
{
    return DSI_REG->DSI_PHY_LCCON.LC_HS_TX_EN ? 1 : 0;
}

void DSI_clk_HS_mode(int enter)
{
    DSI_PHY_LCCON_REG tmp = DSI_REG->DSI_PHY_LCCON;
    if (enter && !DSI_clk_HS_state()) {
        tmp.LC_HS_TX_EN = 1;
        OUTREG32(&DSI_REG->DSI_PHY_LCCON, AS_UINT32(&tmp));
    } else if (!enter && DSI_clk_HS_state()) {
        tmp.LC_HS_TX_EN = 0;
        OUTREG32(&DSI_REG->DSI_PHY_LCCON, AS_UINT32(&tmp));
    }
}

void DSI_Set_VM_CMD(void)
{
    OUTREGBIT(DSI_VM_CMD_CON_REG, DSI_REG->DSI_VM_CMD_CON, TS_VFP_EN, 1);
    OUTREGBIT(DSI_VM_CMD_CON_REG, DSI_REG->DSI_VM_CMD_CON, VM_CMD_EN, 1);
}

/* ── DSI_TXRX_Control (lane-enable mask encoding from the stock switch) ── */
void DSI_TXRX_Control(void)
{
    DSI_TXRX_CTRL_REG tmp = DSI_REG->DSI_TXRX_CTRL;

    switch (g_lcm.LANE_NUM) {
        case 1: tmp.LANE_NUM = 1;   break;
        case 2: tmp.LANE_NUM = 3;   break;
        case 3: tmp.LANE_NUM = 0x7; break;
        case 4: tmp.LANE_NUM = 0xF; break;
        default: tmp.LANE_NUM = 0x7; break;
    }
    tmp.VC_NUM       = 0;
    tmp.DIS_EOT      = 1;   /* MIPI_DSI_MODE_NO_EOT_PACKET */
    tmp.NULL_EN      = 0;
    tmp.MAX_RTN_SIZE = 0;
    /* Continuous HS clock. Stock LK computes HSTX_CKLP_EN = 1 - lcm.dsi[0x1f0],
     * and get_params (FUN_81e1c580) sets that field = 1, so LK programs 0
     * (continuous). A JD9365 in video mode loses its internal PLL lock between
     * bursts with a non-continuous clock and shows black. */
    tmp.HSTX_CKLP_EN = 0;
    OUTREG32(&DSI_REG->DSI_TXRX_CTRL, AS_UINT32(&tmp));
}

/* ── DSI_PS_Control: DSI_PS_SEL = (ps_type > LOOSELY) ? 5-ps_type : ps_type ── */
void DSI_PS_Control(void)
{
    DSI_PSCTRL_REG tmp = DSI_REG->DSI_PSCTRL;
    unsigned int ps_type = g_lcm.rgb565 ? PACKED_PS_16BIT_RGB565 : PACKED_PS_24BIT_RGB888;
    unsigned int dsiTmpBufBpp = g_lcm.rgb565 ? 2u : 3u;
    unsigned int ps_wc = g_lcm.horizontal_active_pixel * dsiTmpBufBpp;
    unsigned int vact_line = g_lcm.vertical_active_line;

    if (ps_type > LOOSELY_PS_18BIT_RGB666)
        tmp.DSI_PS_SEL = (5u - ps_type);   /* RGB888(2) => 3 */
    else
        tmp.DSI_PS_SEL = ps_type;
    tmp.DSI_PS_WC = ps_wc;

    OUTREG32(&DSI_REG->DSI_VACT_NL, AS_UINT32(&vact_line));
    OUTREG32(&DSI_REG->DSI_PSCTRL, AS_UINT32(&tmp));
    OUTREG32(&DSI_REG->DSI_HSTX_CKL_WC, ps_wc);
}

/* ── DSI_Config_VDO_Timing ── */
#define ALIGN_TO(x, n)  (((x) + ((n) - 1)) & ~((n) - 1))

void DSI_Config_VDO_Timing(void)
{
    unsigned int horizontal_sync_active_byte = 0;
    unsigned int horizontal_backporch_byte;
    unsigned int horizontal_frontporch_byte;
    unsigned int horizontal_bllp_byte;
    unsigned int dsiTmpBufBpp = g_lcm.rgb565 ? 2u : 3u;

    OUTREG32(&DSI_REG->DSI_VSA_NL, g_lcm.vertical_sync_active);
    OUTREG32(&DSI_REG->DSI_VBP_NL, g_lcm.vertical_backporch);
    OUTREG32(&DSI_REG->DSI_VFP_NL, g_lcm.vertical_frontporch);
    OUTREG32(&DSI_REG->DSI_VACT_NL, g_lcm.vertical_active_line);

    if (g_lcm.mode == DSI_SYNC_EVENT_VDO_MODE || g_lcm.mode == DSI_BURST_VDO_MODE) {
        horizontal_backporch_byte =
            ((g_lcm.horizontal_backporch + g_lcm.horizontal_sync_active) * dsiTmpBufBpp - 10);
    } else {
        horizontal_sync_active_byte = (g_lcm.horizontal_sync_active * dsiTmpBufBpp - 10);
        horizontal_backporch_byte   = (g_lcm.horizontal_backporch * dsiTmpBufBpp - 10);
    }

    horizontal_frontporch_byte = (g_lcm.horizontal_frontporch * dsiTmpBufBpp - 12);
    horizontal_bllp_byte       = (g_lcm.horizontal_bllp * dsiTmpBufBpp);

    OUTREG32(&DSI_REG->DSI_HSA_WC, ALIGN_TO(horizontal_sync_active_byte, 4));
    OUTREG32(&DSI_REG->DSI_HBP_WC, ALIGN_TO(horizontal_backporch_byte, 4));
    OUTREG32(&DSI_REG->DSI_HFP_WC, ALIGN_TO(horizontal_frontporch_byte, 4));
    OUTREG32(&DSI_REG->DSI_BLLP_WC, ALIGN_TO(horizontal_bllp_byte, 4));
}

/* ── DSI_PHY_clk_setting: MIPITX D-PHY PLL (RMW bitfields, stock order) ── */
void DSI_PHY_clk_setting(void)
{
    unsigned int data_Rate = g_lcm.PLL_CLOCK * 2;
    unsigned int txdiv = 1, pcw;
    unsigned int delta1 = 5;     /* default SSC range 0%~-5% */
    unsigned int pdelta1;
    uint32_t m_hw_res3, temp1, temp2, temp3, temp4, temp5;

    /* Per-lane RT_CODE from efuse 0x10206180 (default 0x8 when zero). */
    m_hw_res3 = INREG32(DSI_EFUSE_RES3);
    temp1 = (m_hw_res3 >> 28) & 0xF;
    temp2 = (m_hw_res3 >> 24) & 0xF;
    temp3 = (m_hw_res3 >> 20) & 0xF;
    temp4 = (m_hw_res3 >> 16) & 0xF;
    temp5 = (m_hw_res3 >> 12) & 0xF;

    OUTREGBIT(MIPITX_DSI_TOP_CON_REG, DSI_PHY_REG->MIPITX_DSI_TOP_CON, RG_DSI_LNT_IMP_CAL_CODE, 8);
    OUTREGBIT(MIPITX_DSI_TOP_CON_REG, DSI_PHY_REG->MIPITX_DSI_TOP_CON, RG_DSI_LNT_HS_BIAS_EN, 1);

    OUTREGBIT(MIPITX_DSI_BG_CON_REG, DSI_PHY_REG->MIPITX_DSI_BG_CON, RG_DSI_V032_SEL, 4);
    OUTREGBIT(MIPITX_DSI_BG_CON_REG, DSI_PHY_REG->MIPITX_DSI_BG_CON, RG_DSI_V04_SEL, 4);
    OUTREGBIT(MIPITX_DSI_BG_CON_REG, DSI_PHY_REG->MIPITX_DSI_BG_CON, RG_DSI_V072_SEL, 4);
    OUTREGBIT(MIPITX_DSI_BG_CON_REG, DSI_PHY_REG->MIPITX_DSI_BG_CON, RG_DSI_V10_SEL, 4);
    OUTREGBIT(MIPITX_DSI_BG_CON_REG, DSI_PHY_REG->MIPITX_DSI_BG_CON, RG_DSI_V12_SEL, 4);
    OUTREGBIT(MIPITX_DSI_BG_CON_REG, DSI_PHY_REG->MIPITX_DSI_BG_CON, RG_DSI_BG_CKEN, 1);
    OUTREGBIT(MIPITX_DSI_BG_CON_REG, DSI_PHY_REG->MIPITX_DSI_BG_CON, RG_DSI_BG_CORE_EN, 1);

    mdelay(10);

    OUTREGBIT(MIPITX_DSI0_CON_REG, DSI_PHY_REG->MIPITX_DSI0_CON, RG_DSI0_CKG_LDOOUT_EN, 1);
    OUTREGBIT(MIPITX_DSI0_CON_REG, DSI_PHY_REG->MIPITX_DSI0_CON, RG_DSI0_LDOCORE_EN, 1);

    OUTREGBIT(MIPITX_DSI_PLL_PWR_REG, DSI_PHY_REG->MIPITX_DSI_PLL_PWR, DA_DSI0_MPPLL_SDM_PWR_ON, 1);
    OUTREGBIT(MIPITX_DSI_PLL_PWR_REG, DSI_PHY_REG->MIPITX_DSI_PLL_PWR, DA_DSI0_MPPLL_SDM_ISO_EN, 1);
    mdelay(10);
    OUTREGBIT(MIPITX_DSI_PLL_PWR_REG, DSI_PHY_REG->MIPITX_DSI_PLL_PWR, DA_DSI0_MPPLL_SDM_ISO_EN, 0);

    OUTREGBIT(MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_PREDIV, 0);
    OUTREGBIT(MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_POSDIV, 0);

    if (data_Rate != 0) {
        if (data_Rate >= 500)      txdiv = 1;
        else if (data_Rate >= 250) txdiv = 2;
        else if (data_Rate >= 125) txdiv = 4;
        else if (data_Rate > 62)   txdiv = 8;
        else                       txdiv = 16;

        switch (txdiv) {
            case 1:
                OUTREGBIT(MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_TXDIV0, 0);
                OUTREGBIT(MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_TXDIV1, 0);
                break;
            case 2:
                OUTREGBIT(MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_TXDIV0, 1);
                OUTREGBIT(MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_TXDIV1, 0);
                break;
            case 4:
                OUTREGBIT(MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_TXDIV0, 2);
                OUTREGBIT(MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_TXDIV1, 0);
                break;
            case 8:
                OUTREGBIT(MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_TXDIV0, 2);
                OUTREGBIT(MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_TXDIV1, 1);
                break;
            case 16:
                OUTREGBIT(MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_TXDIV0, 2);
                OUTREGBIT(MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_TXDIV1, 2);
                break;
            default: break;
        }

        /* pcw = data_Rate*txdiv/13; Post-DIV=4 folded into the /13 ref math. */
        pcw = data_Rate * txdiv / 13;
        OUTREGBIT(MIPITX_DSI_PLL_CON2_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON2, RG_DSI0_MPPLL_SDM_PCW_H, (pcw & 0x7F));
        OUTREGBIT(MIPITX_DSI_PLL_CON2_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON2, RG_DSI0_MPPLL_SDM_PCW_16_23,
                  ((256 * (data_Rate * txdiv % 13) / 13) & 0xFF));
        OUTREGBIT(MIPITX_DSI_PLL_CON2_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON2, RG_DSI0_MPPLL_SDM_PCW_8_15,
                  ((256 * (256 * (data_Rate * txdiv % 13) % 13) / 13) & 0xFF));
        OUTREGBIT(MIPITX_DSI_PLL_CON2_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON2, RG_DSI0_MPPLL_SDM_PCW_0_7,
                  ((256 * (256 * (256 * (data_Rate * txdiv % 13) % 13) % 13) / 13) & 0xFF));

        if (g_lcm.ssc_disable != 1) {
            OUTREGBIT(MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON1, RG_DSI0_MPPLL_SDM_SSC_PH_INIT, 1);
            OUTREGBIT(MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON1, RG_DSI0_MPPLL_SDM_SSC_PRD, 0x1B1);
            if (g_lcm.ssc_range != 0)
                delta1 = g_lcm.ssc_range;
            pdelta1 = (delta1 * data_Rate * txdiv * 262144 + 281664) / 563329;
            OUTREGBIT(MIPITX_DSI_PLL_CON3_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON3, RG_DSI0_MPPLL_SDM_SSC_DELTA, pdelta1);
            OUTREGBIT(MIPITX_DSI_PLL_CON3_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON3, RG_DSI0_MPPLL_SDM_SSC_DELTA1, pdelta1);
        }
    }
    OUTREGBIT(MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON1, RG_DSI0_MPPLL_SDM_FRA_EN, 1);

    OUTREGBIT(MIPITX_DSI0_CLOCK_LANE_REG, DSI_PHY_REG->MIPITX_DSI0_CLOCK_LANE, RG_DSI0_LNTC_RT_CODE, (temp1 == 0) ? 0x8 : temp1);
    OUTREGBIT(MIPITX_DSI0_CLOCK_LANE_REG, DSI_PHY_REG->MIPITX_DSI0_CLOCK_LANE, RG_DSI0_LNTC_PHI_SEL, 0x1);
    OUTREGBIT(MIPITX_DSI0_CLOCK_LANE_REG, DSI_PHY_REG->MIPITX_DSI0_CLOCK_LANE, RG_DSI0_LNTC_LDOOUT_EN, 1);
    if (g_lcm.LANE_NUM > 0) {
        OUTREGBIT(MIPITX_DSI0_DATA_LANE0_REG, DSI_PHY_REG->MIPITX_DSI0_DATA_LANE0, RG_DSI0_LNT0_RT_CODE, (temp2 == 0) ? 0x8 : temp2);
        OUTREGBIT(MIPITX_DSI0_DATA_LANE0_REG, DSI_PHY_REG->MIPITX_DSI0_DATA_LANE0, RG_DSI0_LNT0_LDOOUT_EN, 1);
    }
    if (g_lcm.LANE_NUM > 1) {
        OUTREGBIT(MIPITX_DSI0_DATA_LANE1_REG, DSI_PHY_REG->MIPITX_DSI0_DATA_LANE1, RG_DSI0_LNT1_RT_CODE, (temp3 == 0) ? 0x8 : temp3);
        OUTREGBIT(MIPITX_DSI0_DATA_LANE1_REG, DSI_PHY_REG->MIPITX_DSI0_DATA_LANE1, RG_DSI0_LNT1_LDOOUT_EN, 1);
    }
    if (g_lcm.LANE_NUM > 2) {
        OUTREGBIT(MIPITX_DSI0_DATA_LANE2_REG, DSI_PHY_REG->MIPITX_DSI0_DATA_LANE2, RG_DSI0_LNT2_RT_CODE, (temp4 == 0) ? 0x8 : temp4);
        OUTREGBIT(MIPITX_DSI0_DATA_LANE2_REG, DSI_PHY_REG->MIPITX_DSI0_DATA_LANE2, RG_DSI0_LNT2_LDOOUT_EN, 1);
    }
    if (g_lcm.LANE_NUM > 3) {
        OUTREGBIT(MIPITX_DSI0_DATA_LANE3_REG, DSI_PHY_REG->MIPITX_DSI0_DATA_LANE3, RG_DSI0_LNT3_RT_CODE, (temp5 == 0) ? 0x8 : temp5);
        OUTREGBIT(MIPITX_DSI0_DATA_LANE3_REG, DSI_PHY_REG->MIPITX_DSI0_DATA_LANE3, RG_DSI0_LNT3_LDOOUT_EN, 1);
    }

    OUTREGBIT(MIPITX_DSI_PLL_CON0_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON0, RG_DSI0_MPPLL_PLL_EN, 1);
    mdelay(1);
    if ((data_Rate != 0) && (g_lcm.ssc_disable != 1))
        OUTREGBIT(MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON1, RG_DSI0_MPPLL_SDM_SSC_EN, 1);
    else
        OUTREGBIT(MIPITX_DSI_PLL_CON1_REG, DSI_PHY_REG->MIPITX_DSI_PLL_CON1, RG_DSI0_MPPLL_SDM_SSC_EN, 0);

    /* default POSDIV by 4 */
    OUTREGBIT(MIPITX_DSI_PLL_TOP_REG, DSI_PHY_REG->MIPITX_DSI_PLL_TOP, RG_MPPLL_PRESERVE_L, 3);
    OUTREGBIT(MIPITX_DSI_TOP_CON_REG, DSI_PHY_REG->MIPITX_DSI_TOP_CON, RG_DSI_PAD_TIE_LOW_EN, 0);
}

/* ── DSI_PHY_TIMCONFIG ── */
#define NS_TO_CYCLE(n, c) ((n) / (c))

void DSI_PHY_TIMCONFIG(void)
{
    DSI_PHY_TIMCON0_REG timcon0 = {0, 0, 0, 0};
    DSI_PHY_TIMCON1_REG timcon1 = {0, 0, 0, 0};
    DSI_PHY_TIMCON2_REG timcon2 = {0, 0, 0, 0};
    DSI_PHY_TIMCON3_REG timcon3 = {0, 0, 0, 0};
    unsigned int cycle_time, ui;
    unsigned int hs_trail_m, hs_trail_n;

    /* PLL_CLOCK path: ui/cycle_time from the bit clock (data_rate=PLL_CLOCK*2). */
    ui         = 1000 / (g_lcm.PLL_CLOCK * 2) + 0x01;
    cycle_time = 8000 / (g_lcm.PLL_CLOCK * 2) + 0x01;

    hs_trail_m = 1;
    hs_trail_n = NS_TO_CYCLE(((hs_trail_m * 0x4) + 0x60), cycle_time);
    timcon0.HS_TRAIL = ((hs_trail_m > hs_trail_n) ? hs_trail_m : hs_trail_n) + 0x0a;

    timcon0.HS_PRPR = NS_TO_CYCLE((0x40 + 0x5 * ui), cycle_time);
    if (timcon0.HS_PRPR == 0) timcon0.HS_PRPR = 1;

    timcon0.HS_ZERO = NS_TO_CYCLE((0xC8 + 0x0a * ui), cycle_time);
    if (timcon0.HS_ZERO > timcon0.HS_PRPR) timcon0.HS_ZERO -= timcon0.HS_PRPR;

    timcon0.LPX = NS_TO_CYCLE(0x50, cycle_time);
    if (timcon0.LPX == 0) timcon0.LPX = 1;

    timcon1.TA_GET     = 0x5 * timcon0.LPX;
    timcon1.TA_SURE    = 0x3 * timcon0.LPX / 0x2;
    timcon1.TA_GO      = 0x4 * timcon0.LPX;
    timcon1.DA_HS_EXIT = NS_TO_CYCLE((0x3c + 0x80 * ui), cycle_time);

    timcon2.CLK_TRAIL = NS_TO_CYCLE(0x64, cycle_time) + 0x0a;
    if (timcon2.CLK_TRAIL < 2) timcon2.CLK_TRAIL = 2;
    timcon2.CONT_DET = 0;
    timcon2.CLK_ZERO = NS_TO_CYCLE(0x190, cycle_time);

    timcon3.CLK_HS_PRPR = NS_TO_CYCLE(0x40, cycle_time);
    if (timcon3.CLK_HS_PRPR == 0) timcon3.CLK_HS_PRPR = 1;
    timcon3.CLK_HS_EXIT = 2 * timcon0.LPX;
    timcon3.CLK_HS_POST = NS_TO_CYCLE((0x3c + 0x80 * ui), cycle_time);

    OUTREGBIT(DSI_PHY_TIMCON0_REG, DSI_REG->DSI_PHY_TIMECON0, LPX, timcon0.LPX);
    OUTREGBIT(DSI_PHY_TIMCON0_REG, DSI_REG->DSI_PHY_TIMECON0, HS_PRPR, timcon0.HS_PRPR);
    OUTREGBIT(DSI_PHY_TIMCON0_REG, DSI_REG->DSI_PHY_TIMECON0, HS_ZERO, timcon0.HS_ZERO);
    OUTREGBIT(DSI_PHY_TIMCON0_REG, DSI_REG->DSI_PHY_TIMECON0, HS_TRAIL, timcon0.HS_TRAIL);

    OUTREGBIT(DSI_PHY_TIMCON1_REG, DSI_REG->DSI_PHY_TIMECON1, TA_GO, timcon1.TA_GO);
    OUTREGBIT(DSI_PHY_TIMCON1_REG, DSI_REG->DSI_PHY_TIMECON1, TA_SURE, timcon1.TA_SURE);
    OUTREGBIT(DSI_PHY_TIMCON1_REG, DSI_REG->DSI_PHY_TIMECON1, TA_GET, timcon1.TA_GET);
    OUTREGBIT(DSI_PHY_TIMCON1_REG, DSI_REG->DSI_PHY_TIMECON1, DA_HS_EXIT, timcon1.DA_HS_EXIT);

    OUTREGBIT(DSI_PHY_TIMCON2_REG, DSI_REG->DSI_PHY_TIMECON2, CONT_DET, timcon2.CONT_DET);
    OUTREGBIT(DSI_PHY_TIMCON2_REG, DSI_REG->DSI_PHY_TIMECON2, CLK_ZERO, timcon2.CLK_ZERO);
    OUTREGBIT(DSI_PHY_TIMCON2_REG, DSI_REG->DSI_PHY_TIMECON2, CLK_TRAIL, timcon2.CLK_TRAIL);

    OUTREGBIT(DSI_PHY_TIMCON3_REG, DSI_REG->DSI_PHY_TIMECON3, CLK_HS_PRPR, timcon3.CLK_HS_PRPR);
    OUTREGBIT(DSI_PHY_TIMCON3_REG, DSI_REG->DSI_PHY_TIMECON3, CLK_HS_POST, timcon3.CLK_HS_POST);
    OUTREGBIT(DSI_PHY_TIMCON3_REG, DSI_REG->DSI_PHY_TIMECON3, CLK_HS_EXIT, timcon3.CLK_HS_EXIT);
}

/* ============================ DCS transport ============================ */

/*
 * BUSY is observed here, and never obeyed. That is deliberate, and it took two
 * failed boots to earn.
 *
 * This DSI host latches DSI_INTSTA.BUSY on the first packet of the panel
 * program and does not drop it again, so any design that treats the bit as a
 * gate stalls on entry 0 of a 155-entry table and the panel is never programmed
 * at all. Both ways of gating were tried on the board and both lost:
 *
 *  - Waiting for BUSY to fall while START was still asserted is a wait that
 *    cannot be satisfied — START is a level, so holding it high *is* busy. The
 *    register dump said exactly that: START=0x00000001 with INTSTA=0x80000000.
 *  - Lowering START first and then waiting fixed the contradiction and changed
 *    nothing: START=0x00000000, INTSTA=0x80000000, still stopped at index 0.
 *
 * Nor is a bigger bound the answer. Raising the poll limit to 262144 to give a
 * cold MIPITX PLL room to settle made the LK stop booting entirely.
 * udelay_cycles() is a volatile loop, and with the MMU and caches off every
 * iteration is a strongly-ordered round trip — hundreds of nanoseconds, not the
 * few cycles the source suggests — so a nominal 1.6 ms wait is really tens of
 * milliseconds and 64x of that is seconds per packet. Nothing services the
 * charger FSM during display bring-up, and on a board with no cell an
 * unserviced stretch that long is how boots end.
 *
 * Both references agree the bit is not a gate. Reference/J36-ULTRA/src/dsi.c
 * dsi_cmdq_send() fires the queue, waits 200us, lowers START, and never reads
 * BUSY; the stock LK's DSI_Start (FUN_81e0f274) is the same toggle. So this
 * follows them and keeps a counter where the wait used to be: one register read
 * per packet, no stalling. A board where BUSY behaves leaves the count near
 * zero; a board where it is wedged runs the count up to the program length and
 * still gets its panel programmed.
 */
static uint32_t g_dsi_busy_stalls;

uint32_t mt6592_dsi_busy_stalls(void)
{
    return g_dsi_busy_stalls;
}

/* DSI_set_cmdq equivalent: write the cmdq words, set size, pulse START. */
static int dsi_send_cmdq(const uint32_t *words, uint32_t count)
{
    if (!words || count == 0 || count > DSI_CMDQ_WORDS) return -1;

    /* START low while the queue is rewritten, then the reference's pulse. */
    OUTREGBIT(DSI_START_REG, DSI_REG->DSI_START, DSI_START, 0);
    OUTREG32(&DSI_REG->DSI_INTSTA, 0);
    for (uint32_t i = 0; i < count; ++i)
        OUTREG32(DSI_CMDQ(i), words[i]);
    OUTREG32(&DSI_REG->DSI_CMDQ_SIZE, count);

    OUTREGBIT(DSI_START_REG, DSI_REG->DSI_START, DSI_START, 1);
    udelay_cycles(2400u); /* ~200us at the nominal 12000 loops/ms calibration */
    OUTREGBIT(DSI_START_REG, DSI_REG->DSI_START, DSI_START, 0);

    if (DSI_REG->DSI_INTSTA.BUSY) ++g_dsi_busy_stalls;
    return 0;
}

static int dsi_send_v3_packet(uint8_t data_id, uint8_t cmd, const uint8_t *params, uint32_t count)
{
    uint32_t words[DSI_CMDQ_WORDS];
    uint32_t word_count;

    if (count && !params) return -1;
    for (uint32_t i = 0; i < DSI_CMDQ_WORDS; ++i) words[i] = 0u;

    if (count < 2u) {
        words[0] = ((uint32_t)cmd << 16) | ((uint32_t)data_id << 8);
        if (count == 1u) words[0] |= (uint32_t)params[0] << 24;
        word_count = 1u;
    } else {
        uint32_t bytes = count + 1u;
        word_count = (count >> 2) + 2u;
        if (word_count > DSI_CMDQ_WORDS) return -1;
        words[0] = (bytes << 16) | ((uint32_t)data_id << 8) | 0x02u;
        words[1] = cmd;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t byte_index = i + 1u;
            words[1u + (byte_index >> 2)] |= (uint32_t)params[i] << ((byte_index & 3u) * 8u);
        }
    }
    return dsi_send_cmdq(words, word_count);
}

/* Pick the MIPI data type the stock LK uses for a short write (FUN_81e10b70):
 * commands in the DCS user range (< 0xB0) go out as DCS short writes
 * (0x05 no-param / 0x15 one-param); the manufacturer command set (>= 0xB0 — the
 * JD9365 0xFF page-select and the 0xB0..0xFF config/gamma registers) MUST go out
 * as GENERIC short writes (0x13 / 0x23). Sending the manufacturer commands as
 * DCS (our previous behaviour) makes the panel ignore the whole init: the pages
 * never switch and no config/gamma lands, so the panel stays dark even though a
 * perfect RGB888 video stream is being transmitted. */
static uint8_t dsi_short_dt(uint8_t cmd, uint32_t has_param)
{
    if (cmd >= 0xb0u) return has_param ? 0x23u : 0x13u;
    return has_param ? 0x15u : 0x05u;
}

/* Short write that selects DCS vs generic by command value, exactly like LK. */
static int __attribute__((unused)) dsi_send_short_auto(uint8_t cmd, uint32_t has_param, uint8_t param)
{
    if (has_param) return dsi_send_v3_packet(dsi_short_dt(cmd, 1u), cmd, &param, 1u);
    return dsi_send_v3_packet(dsi_short_dt(cmd, 0u), cmd, 0, 0u);
}

static int dsi_set_window(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint8_t x[4];
    uint8_t y[4];

    x[0] = (uint8_t)(x0 >> 8);
    x[1] = (uint8_t)x0;
    x[2] = (uint8_t)(x1 >> 8);
    x[3] = (uint8_t)x1;
    y[0] = (uint8_t)(y0 >> 8);
    y[1] = (uint8_t)y0;
    y[2] = (uint8_t)(y1 >> 8);
    y[3] = (uint8_t)y1;

    if (DSI_dcs_write_long(0x2Au, x, sizeof(x)) != 0) return -1;
    if (DSI_dcs_write_long(0x2Bu, y, sizeof(y)) != 0) return -1;
    return 0;
}

/* CONFG byte for a LP short/long DCS write: type[1:0], HS=0 (bit3), CL=0. */
int DSI_dcs_write_short(uint8_t cmd, int has_param, uint8_t param)
{
    uint32_t w0;
    /* DSI_DCS_SHORT_PACKET_ID_0 (0x05, no param) / _1 (0x15, one param). */
    if (has_param)
        w0 = ((uint32_t)param << 24) | ((uint32_t)cmd << 16) | (0x15u << 8) | 0x00u;
    else
        w0 = ((uint32_t)cmd << 16) | (0x05u << 8) | 0x00u;
    return dsi_send_cmdq(&w0, 1u);
}

int DSI_dcs_write_long(uint8_t cmd, const uint8_t *params, uint32_t count)
{
    uint32_t words[DSI_CMDQ_WORDS];
    uint32_t bytes = count + 1u;                  /* DCS cmd + params */
    uint32_t word_count = 1u + ((bytes + 3u) / 4u);

    if (count && !params) return -1;
    if (word_count > DSI_CMDQ_WORDS) return -1;

    for (uint32_t i = 0; i < DSI_CMDQ_WORDS; ++i) words[i] = 0u;
    /* DSI_DCS_LONG_PACKET_ID = 0x39, CONFG type=LONG_PACKET_W(2), CL_8BITS. */
    words[0] = (bytes << 16) | (0x39u << 8) | 0x02u;
    for (uint32_t i = 0; i < bytes; ++i) {
        uint8_t b = (i == 0u) ? cmd : params[i - 1u];
        words[1u + (i >> 2)] |= (uint32_t)b << ((i & 3u) * 8u);
    }
    return dsi_send_cmdq(words, word_count);
}

typedef struct {
    uint8_t data_id;
    uint8_t cmd;
    uint8_t count;
    uint8_t param0;
} jd9365_v3_entry_t;

#define JD9365_V3(id, cmd, count, param0) { (id), (cmd), (count), (param0) }

/* Extracted from Reference/J36-ULTRA boot.img:
 *   jd9365_qc_190227_lcm_drv::lcm_init()
 *   dsi_set_cmdq_V3(table=0xc0b6726c, size=0x9b, force_update=1)
 *
 * The original LCM_setting_table_V3 entries are 131 bytes each
 * (data_id, cmd, count, para_list[128]). This panel table only uses one-byte
 * writes plus two delay markers, so keep the freestanding copy compact. */
static const jd9365_v3_entry_t g_jd9365_stock_init[] = {
    JD9365_V3(0x15, 0xff, 0x01, 0x30), JD9365_V3(0x15, 0xff, 0x01, 0x52),
    JD9365_V3(0x15, 0xff, 0x01, 0x01), JD9365_V3(0x15, 0xe3, 0x01, 0x00),
    JD9365_V3(0x15, 0x25, 0x01, 0x10), JD9365_V3(0x15, 0x28, 0x01, 0x6f),
    JD9365_V3(0x15, 0x29, 0x01, 0x01), JD9365_V3(0x15, 0x2a, 0x01, 0xdf),
    JD9365_V3(0x15, 0x2c, 0x01, 0x22), JD9365_V3(0x15, 0xc3, 0x01, 0x0f),
    JD9365_V3(0x15, 0x30, 0x01, 0x58), JD9365_V3(0x15, 0x45, 0x01, 0x91),
    JD9365_V3(0x15, 0x37, 0x01, 0x9c), JD9365_V3(0x15, 0x38, 0x01, 0xa7),
    JD9365_V3(0x15, 0x39, 0x01, 0x41), JD9365_V3(0x15, 0x80, 0x01, 0x20),
    JD9365_V3(0x15, 0x91, 0x01, 0x67), JD9365_V3(0x15, 0x92, 0x01, 0x67),
    JD9365_V3(0x15, 0xa0, 0x01, 0x55), JD9365_V3(0x15, 0xa1, 0x01, 0x50),
    JD9365_V3(0x15, 0xa3, 0x01, 0x58), JD9365_V3(0x15, 0xa4, 0x01, 0x9c),
    JD9365_V3(0x15, 0xa7, 0x01, 0x02), JD9365_V3(0x15, 0xa8, 0x01, 0x01),
    JD9365_V3(0x15, 0xa9, 0x01, 0x21), JD9365_V3(0x15, 0xaa, 0x01, 0xfc),
    JD9365_V3(0x15, 0xab, 0x01, 0x28), JD9365_V3(0x15, 0xac, 0x01, 0x06),
    JD9365_V3(0x15, 0xad, 0x01, 0x06), JD9365_V3(0x15, 0xae, 0x01, 0x06),
    JD9365_V3(0x15, 0xaf, 0x01, 0x03), JD9365_V3(0x15, 0xb0, 0x01, 0x08),
    JD9365_V3(0x15, 0xb1, 0x01, 0x26), JD9365_V3(0x15, 0xb2, 0x01, 0x28),
    JD9365_V3(0x15, 0xb3, 0x01, 0x28), JD9365_V3(0x15, 0xb4, 0x01, 0x03),
    JD9365_V3(0x15, 0xb5, 0x01, 0x08), JD9365_V3(0x15, 0xb6, 0x01, 0x26),
    JD9365_V3(0x15, 0xb7, 0x01, 0x08), JD9365_V3(0x15, 0xb8, 0x01, 0x26),
    JD9365_V3(0x15, 0xc0, 0x01, 0x00), JD9365_V3(0x15, 0xc1, 0x01, 0x00),
    JD9365_V3(0x15, 0xc2, 0x01, 0x00), JD9365_V3(0x15, 0xff, 0x01, 0x30),
    JD9365_V3(0x15, 0xff, 0x01, 0x52), JD9365_V3(0x15, 0xff, 0x01, 0x02),
    JD9365_V3(0x15, 0xb0, 0x01, 0x02), JD9365_V3(0x15, 0xd0, 0x01, 0x02),
    JD9365_V3(0x15, 0xb1, 0x01, 0x0f), JD9365_V3(0x15, 0xd1, 0x01, 0x10),
    JD9365_V3(0x15, 0xb2, 0x01, 0x11), JD9365_V3(0x15, 0xd2, 0x01, 0x12),
    JD9365_V3(0x15, 0xb3, 0x01, 0x32), JD9365_V3(0x15, 0xd3, 0x01, 0x33),
    JD9365_V3(0x15, 0xb4, 0x01, 0x36), JD9365_V3(0x15, 0xd4, 0x01, 0x36),
    JD9365_V3(0x15, 0xb5, 0x01, 0x3c), JD9365_V3(0x15, 0xd5, 0x01, 0x3c),
    JD9365_V3(0x15, 0xb6, 0x01, 0x20), JD9365_V3(0x15, 0xd6, 0x01, 0x20),
    JD9365_V3(0x15, 0xb7, 0x01, 0x3e), JD9365_V3(0x15, 0xd7, 0x01, 0x3e),
    JD9365_V3(0x15, 0xb8, 0x01, 0x0e), JD9365_V3(0x15, 0xd8, 0x01, 0x0d),
    JD9365_V3(0x15, 0xb9, 0x01, 0x05), JD9365_V3(0x15, 0xd9, 0x01, 0x05),
    JD9365_V3(0x15, 0xba, 0x01, 0x11), JD9365_V3(0x15, 0xda, 0x01, 0x12),
    JD9365_V3(0x15, 0xbb, 0x01, 0x11), JD9365_V3(0x15, 0xdb, 0x01, 0x11),
    JD9365_V3(0x15, 0xbc, 0x01, 0x13), JD9365_V3(0x15, 0xdc, 0x01, 0x14),
    JD9365_V3(0x15, 0xbd, 0x01, 0x14), JD9365_V3(0x15, 0xdd, 0x01, 0x14),
    JD9365_V3(0x15, 0xbe, 0x01, 0x16), JD9365_V3(0x15, 0xde, 0x01, 0x18),
    JD9365_V3(0x15, 0xbf, 0x01, 0x0e), JD9365_V3(0x15, 0xdf, 0x01, 0x0f),
    JD9365_V3(0x15, 0xc0, 0x01, 0x17), JD9365_V3(0x15, 0xe0, 0x01, 0x17),
    JD9365_V3(0x15, 0xc1, 0x01, 0x07), JD9365_V3(0x15, 0xe1, 0x01, 0x08),
    JD9365_V3(0x15, 0xff, 0x01, 0x30), JD9365_V3(0x15, 0xff, 0x01, 0x52),
    JD9365_V3(0x15, 0xff, 0x01, 0x03), JD9365_V3(0x15, 0x08, 0x01, 0x8a),
    JD9365_V3(0x15, 0x09, 0x01, 0x8b), JD9365_V3(0x15, 0x30, 0x01, 0x00),
    JD9365_V3(0x15, 0x31, 0x01, 0x00), JD9365_V3(0x15, 0x32, 0x01, 0x00),
    JD9365_V3(0x15, 0x33, 0x01, 0x00), JD9365_V3(0x15, 0x34, 0x01, 0x61),
    JD9365_V3(0x15, 0x35, 0x01, 0xd4), JD9365_V3(0x15, 0x36, 0x01, 0x24),
    JD9365_V3(0x15, 0x37, 0x01, 0x03), JD9365_V3(0x15, 0x40, 0x01, 0x86),
    JD9365_V3(0x15, 0x41, 0x01, 0x87), JD9365_V3(0x15, 0x42, 0x01, 0x84),
    JD9365_V3(0x15, 0x43, 0x01, 0x85), JD9365_V3(0x15, 0x44, 0x01, 0x11),
    JD9365_V3(0x15, 0x45, 0x01, 0xde), JD9365_V3(0x15, 0x46, 0x01, 0xdd),
    JD9365_V3(0x15, 0x47, 0x01, 0x11), JD9365_V3(0x15, 0x48, 0x01, 0xe0),
    JD9365_V3(0x15, 0x49, 0x01, 0xdf), JD9365_V3(0x15, 0x50, 0x01, 0x82),
    JD9365_V3(0x15, 0x51, 0x01, 0x83), JD9365_V3(0x15, 0x52, 0x01, 0x80),
    JD9365_V3(0x15, 0x53, 0x01, 0x81), JD9365_V3(0x15, 0x54, 0x01, 0x11),
    JD9365_V3(0x15, 0x55, 0x01, 0xe2), JD9365_V3(0x15, 0x56, 0x01, 0xe1),
    JD9365_V3(0x15, 0x57, 0x01, 0x11), JD9365_V3(0x15, 0x58, 0x01, 0xe4),
    JD9365_V3(0x15, 0x59, 0x01, 0xe3), JD9365_V3(0x15, 0x82, 0x01, 0x0f),
    JD9365_V3(0x15, 0x83, 0x01, 0x0f), JD9365_V3(0x15, 0x84, 0x01, 0x00),
    JD9365_V3(0x15, 0x85, 0x01, 0x0f), JD9365_V3(0x15, 0x86, 0x01, 0x0f),
    JD9365_V3(0x15, 0x87, 0x01, 0x0e), JD9365_V3(0x15, 0x88, 0x01, 0x0e),
    JD9365_V3(0x15, 0x89, 0x01, 0x06), JD9365_V3(0x15, 0x8a, 0x01, 0x06),
    JD9365_V3(0x15, 0x8b, 0x01, 0x07), JD9365_V3(0x15, 0x8c, 0x01, 0x07),
    JD9365_V3(0x15, 0x8d, 0x01, 0x04), JD9365_V3(0x15, 0x8e, 0x01, 0x04),
    JD9365_V3(0x15, 0x8f, 0x01, 0x05), JD9365_V3(0x15, 0x90, 0x01, 0x05),
    JD9365_V3(0x15, 0x98, 0x01, 0x0f), JD9365_V3(0x15, 0x99, 0x01, 0x0f),
    JD9365_V3(0x15, 0x9a, 0x01, 0x00), JD9365_V3(0x15, 0x9b, 0x01, 0x0f),
    JD9365_V3(0x15, 0x9c, 0x01, 0x0f), JD9365_V3(0x15, 0x9d, 0x01, 0x0e),
    JD9365_V3(0x15, 0x9e, 0x01, 0x0e), JD9365_V3(0x15, 0x9f, 0x01, 0x06),
    JD9365_V3(0x15, 0xa0, 0x01, 0x06), JD9365_V3(0x15, 0xa1, 0x01, 0x07),
    JD9365_V3(0x15, 0xa2, 0x01, 0x07), JD9365_V3(0x15, 0xa3, 0x01, 0x04),
    JD9365_V3(0x15, 0xa4, 0x01, 0x04), JD9365_V3(0x15, 0xa5, 0x01, 0x05),
    JD9365_V3(0x15, 0xa6, 0x01, 0x05), JD9365_V3(0x15, 0xe0, 0x01, 0x02),
    JD9365_V3(0x15, 0xe1, 0x01, 0x52), JD9365_V3(0x15, 0xff, 0x01, 0x30),
    JD9365_V3(0x15, 0xff, 0x01, 0x52), JD9365_V3(0x15, 0xff, 0x01, 0x00),
    JD9365_V3(0x15, 0x36, 0x01, 0x02), JD9365_V3(0x05, 0x11, 0x00, 0x00),
    JD9365_V3(0x00, 0xff, 0xc8, 0x00), JD9365_V3(0x05, 0x29, 0x00, 0x00),
    JD9365_V3(0x00, 0xff, 0x64, 0x00),
};

static uint32_t g_jd9365_stock_last_index;

uint32_t mt6592_dsi_jd9365_stock_count(void)
{
    return (uint32_t)(sizeof(g_jd9365_stock_init) / sizeof(g_jd9365_stock_init[0]));
}

uint32_t mt6592_dsi_jd9365_last_index(void)
{
    return g_jd9365_stock_last_index;
}

int mt6592_dsi_jd9365_stock_init_range(uint32_t start, uint32_t end)
{
    const uint32_t count = mt6592_dsi_jd9365_stock_count();

    if (start > count) start = count;
    if (end > count) end = count;
    if (end < start) end = start;

    for (uint32_t i = start; i < end; ++i) {
        const jd9365_v3_entry_t *e = &g_jd9365_stock_init[i];
        int rc = 0;

        g_jd9365_stock_last_index = i;
        if (e->data_id == 0x00u && e->cmd == 0xffu) {
            mdelay(e->count);
            continue;
        }
        if (e->count == 0u) {
            /* DT chosen by command value, like stock LK (FUN_81e10b70) — the
             * table's stored data_id (0x15) is NOT what LK actually transmits
             * for the 0xFF page-select / >=0xB0 manufacturer registers. */
            rc = dsi_send_v3_packet(dsi_short_dt(e->cmd, 0u), e->cmd, 0, 0u);
        } else if (e->count == 1u) {
            rc = dsi_send_v3_packet(dsi_short_dt(e->cmd, 1u), e->cmd, &e->param0, 1u);
        } else {
            return -2;
        }
        if (rc != 0) return -1;
    }
    return 0;
}

int mt6592_dsi_jd9365_stock_init(void)
{
    return mt6592_dsi_jd9365_stock_init_range(0u, mt6592_dsi_jd9365_stock_count());
}

int mt6592_dsi_jd9365_force_rgb888_video_format(void)
{
    /* 0xFF is a >=0xB0 manufacturer command -> generic short write (like LK);
     * 0x3A/0x36 are standard DCS. dsi_send_short_auto picks the right DT. */
    if (dsi_send_short_auto(0xffu, 1, 0x30u) != 0) return -1;
    if (dsi_send_short_auto(0xffu, 1, 0x52u) != 0) return -1;
    if (dsi_send_short_auto(0xffu, 1, 0x00u) != 0) return -1;
    if (dsi_send_short_auto(0x3au, 1, 0x77u) != 0) return -1;
    if (dsi_send_short_auto(0x36u, 1, 0x02u) != 0) return -1;
    mdelay(5);
    return 0;
}

int mt6592_dsi_panel_init(void)
{
    return mt6592_dsi_jd9365_stock_init();
}

/* ============================ orchestration ============================ */

void mt6592_dsi_phy_pll_on(void)
{
    DSI_PHY_clk_setting();
    DSI_PHY_TIMCONFIG();
}

/* Single video-mode configuration, matching the stock init_dsi() order:
 *   PLL (clk_switch) -> DSI_Init/PowerOn -> TXRX -> TIMCONFIG -> PS ->
 *   Config_VDO_Timing -> Set_VM_CMD -> DSI_EN.
 * The clock lane is LEFT IN LP so the panel DCS init that follows goes out in
 * low-power escape mode. There is NO second reset/reconfigure afterwards — the
 * caller just sends the panel program, then DSI_SetMode(video)+HS+Start. */
void mt6592_dsi_video_setup(void)
{
    DSI_PHY_clk_setting();      /* MIPITX PLL — done first (DSI_PHY_clk_switch) */
    /* Stock LK DSI_Init writes DSI_MEM_CONTI (0x090) = 0x3c (:13412) then pulses
     * COM_CTRL.DSI_RESET (FUN_81e0f274, :13480-13491). */
    OUTREG32((uintptr_t)(DSI_BASE + 0x90u), 0x3cu);
    DSI_Reset();                /* COM_CTRL(0x010) bit0 assert+hold+release */
    OUTREG32(&DSI_REG->DSI_INTSTA, 0);
    OUTREG32(&DSI_REG->DSI_MODE_CTRL, 0);
    OUTREG32(&DSI_REG->DSI_TXRX_CTRL, 0);
    OUTREG32(&DSI_REG->DSI_PSCTRL, 0);
    OUTREG32(&DSI_REG->DSI_CMDQ_SIZE, 0);
    OUTREG32(&DSI_REG->DSI_PHY_LCCON, 0);
    OUTREG32(&DSI_REG->DSI_PHY_LD0CON, 0);
    OUTREG32(&DSI_REG->DSI_VM_CMD_CON, 0);
    DSI_PowerOn();              /* enable DSI host: COM_CTRL(0x010) bit1 */
    DSI_TXRX_Control();
    DSI_PHY_TIMCONFIG();
    DSI_PS_Control();
    DSI_Config_VDO_Timing();
    DSI_Set_VM_CMD();
    DSI_EnableClk();            /* MASKREG(DSI_COM_CTRL, DSI_EN) */
}

/* Arm video mode exactly where stock LK does: after the panel DCS init and
 * before dsi_config_ddp().
 *
 * The DSI_Start() on the end is not a command-mode leftover, and leaving it out
 * is what pinned the J36 to a single frame. Two independent readings of the
 * stock LK say the same thing:
 *
 *  - FUN_81e10b14 (lk.full-decompile :14484) is DSI_Start: it waits for
 *    not-busy only when MODE_CTRL says command mode, then clears and sets bit 0
 *    of DSI_START (0x1400c000) and *leaves it set*. Its sibling FUN_81e10884
 *    (:14315) is DSI_EnableVM_CMD and drives bit 16 instead — and the stock
 *    panel-program sender FUN_81e10ea0 picks between the two purely on
 *    MODE_CTRL, which is only coherent if bit 0 is the video engine's own run
 *    bit rather than a per-packet trigger.
 *  - Nothing in the stock image ever writes bit 1 of DSI_START, so
 *    Reference/J36-ULTRA/src/dsi.c's `setbits(DSI_START, 0x2)` is that file's
 *    own invention, as is the `setbits(DSI_MODE_CTRL, 0x21)` above it — 0x21 is
 *    the stock DSI_VM_CMD_CON value (MT6592_J36_LK_HANDOFF_DSI_VM_CMD_CON),
 *    written to the wrong register. Follow the decompile, not that file.
 *
 * With bit 0 clear the host emits the frame that is already staged and then
 * stops generating video timing, so the DDP has no SOF to fetch the next frame
 * against: the panel holds LK_BEACON_SCANOUT and every later repaint — the
 * LK's own beacons, then everything the OS draws — lands in a framebuffer
 * nobody is scanning any more. */
void mt6592_dsi_video_start(void)
{
    DSI_SetMode(g_lcm.mode);
    DSI_clk_HS_mode(1);
    DSI_Start();
}

/* Restart the video engine against a pixel path that is now fully assembled.
 *
 * mt6592_dsi_video_start() runs before the DDP is configured, because the DDP
 * mutex takes its start-of-frame from DSI0 and cannot be granted against a host
 * that is not generating timing. That leaves the first frames scanning out of an
 * OVL that is still being programmed, so the caller kicks the engine once more
 * on the way out: DSI_Start() clears and re-sets DSI_START bit 0, which is the
 * same clear-then-set the stock LK's FUN_81e10b14 performs, and the worst it can
 * cost is one torn frame in a bootloader. */
void mt6592_dsi_video_kick(void)
{
    DSI_Start();
}

/* Read back key MIPITX-PLL + DSI-host registers so the live breadcrumb stream
 * can tell whether the block is clocked, stuck busy, or still in command mode.
 * out[] must hold 12 words. */
void mt6592_dsi_readback(uint32_t out[12])
{
    out[0] = INREG32(&DSI_PHY_REG->MIPITX_DSI_PLL_CON0); /* PLL_EN + txdiv */
    out[1] = INREG32(&DSI_PHY_REG->MIPITX_DSI_PLL_CON2); /* PCW */
    out[2] = INREG32(&DSI_PHY_REG->MIPITX_DSI_PLL_PWR);  /* PWR_ON/ISO/ACK */
    out[3] = INREG32(DSI_REG_COM_CTRL);                  /* COM_CTRL 0x010: bit0 RESET, bit1 EN */
    out[4] = INREG32(&DSI_REG->DSI_MODE_CTRL);           /* MODE */
    out[5] = INREG32(&DSI_REG->DSI_PSCTRL);              /* PS_SEL/PS_WC */
    out[6] = INREG32(&DSI_REG->DSI_TXRX_CTRL);           /* LANE_NUM */
    out[7] = INREG32(&DSI_REG->DSI_INTSTA);              /* BUSY etc. */
    out[8] = INREG32(&DSI_REG->DSI_START);               /* START bits */
    out[9] = INREG32(&DSI_REG->DSI_STA);                 /* live status */
    out[10] = INREG32(&DSI_REG->DSI_PHY_LCCON);          /* clock lane */
    out[11] = INREG32(&DSI_REG->DSI_VM_CMD_CON);         /* video-mode cmd */
}

/* ============================ DSI self-test (BIST) ============================ */

/* Standard MT6592 DSI built-in self-test registers (base 0x1400c000). These
 * offsets are NOT used by stock LK; they are provided (per the
 * Reference/J36-ULTRA/src/lcd.c port) as a datapath-independent "is the panel
 * link alive" smoke test that drives a solid colour straight out of the link. */
#define DSI_BIST_PATTERN  (DSI_BASE + 0x0f0u)
#define DSI_BIST_CON      (DSI_BASE + 0x0b0u)

/* 1:1 port of Reference/J36-ULTRA/src/lcd.c dsi_selftest_colour().
 *   DSI_BIST_PATTERN <- colour (0x00RRGGBB)
 *   DSI_BIST_CON     <- (1<<14)|(1<<2)|(1<<0)  : enable fixed-pattern BIST so it
 *                                                 drives the video timing gen
 *   DSI_START        |= (1<<1)                 : start the video stream (bit1)
 * Identical bit values to the reference; the START register is accessed through
 * the typed struct (offset 0x0 here vs the reference's raw label) but the bit
 * set is the same 0x2 the reference writes. */
void mt6592_dsi_bist_solid_color(uint32_t colour)
{
    OUTREG32(DSI_BIST_PATTERN, colour);
    /* enable fixed-pattern BIST and let it drive the video timing generator */
    OUTREG32(DSI_BIST_CON, (1u << 14) | (1u << 2) | (1u << 0));
    /* Reference: setbits(DSI_START, 0x2) -- start the video stream (bit1). */
    OUTREG32(&DSI_REG->DSI_START, INREG32(&DSI_REG->DSI_START) | 0x2u);
}

/* ── Generic MIPI LCD 30 initialization (fallback path) ──
 * Minimal DCS sequence that works on a wide variety of 3.5" 640x480 / similar
 * MIPI DSI panels (Sitronix, Ilitek, JD, ST7701-class, etc). Uses only the
 * data lane in LP mode (the DSI host is left in the state after video_setup
 * which keeps the clock lane in LP for DCS).
 */
int mt6592_dsi_generic_lcd30_init(void)
{
    /* Some panels are sensitive to the order and need an explicit reset here
     * even if MMSYS already pulsed it. We do a SWRESET too. */
    (void)DSI_dcs_write_short(0x01, 0, 0);   /* SWRESET */
    mdelay(20);

    (void)DSI_dcs_write_short(0x11, 0, 0);   /* SLPOUT */
    mdelay(120);

    (void)DSI_dcs_write_short(0x3A, 1, 0x77); /* COLMOD 24bpp */
    mdelay(5);

    (void)DSI_dcs_write_short(0x36, 1, 0x00); /* MADCTL normal */
    mdelay(5);

    (void)DSI_dcs_write_short(0x29, 0, 0);    /* DISPON */
    mdelay(20);

    return 0;
}

/* Blast a white block directly on the data lane using RAMWR (0x2C).
 * This exercises the data lane with payload bytes (white pixels) after the
 * DCS header. Useful as a "is the data lane talking to the panel controller?"
 * test even before full video scanout is running. We send a modest number of
 * white pixels (one long packet) so it doesn't take forever in LP.
 */
int mt6592_dsi_blast_white_via_data_lane(uint32_t pixel_count)
{
    /* Build a long DCS packet: 0x2C + pixel_count*3 bytes of 0xFF (white RGB888) */
    const uint32_t max_pixels_per_pkt = 40u; /* DSI CMDQ is 32 words: <= 120 payload bytes is safe */
    uint32_t n = pixel_count;
    if (n > max_pixels_per_pkt) n = max_pixels_per_pkt;

    uint8_t buf[3 + (40 * 3)];
    buf[0] = 0x2C; /* RAMWR */
    for (uint32_t i = 0; i < n; ++i) {
        buf[1 + i*3 + 0] = 0xFF;
        buf[1 + i*3 + 1] = 0xFF;
        buf[1 + i*3 + 2] = 0xFF;
    }
    return DSI_dcs_write_long(0x2C, &buf[1], n * 3u);
}

int mt6592_dsi_fill_rect_rgb888(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t pixels;
    uint32_t sent = 0u;
    uint8_t pkt[40u * 3u];

    if (x >= MT6592_J36_PANEL_WIDTH || y >= MT6592_J36_PANEL_HEIGHT || w == 0u || h == 0u) return 0;
    if (w > MT6592_J36_PANEL_WIDTH - x) w = MT6592_J36_PANEL_WIDTH - x;
    if (h > MT6592_J36_PANEL_HEIGHT - y) h = MT6592_J36_PANEL_HEIGHT - y;
    if (w > 0xffffffffu / h) return -1;

    DSI_SetMode(DSI_CMD_MODE);
    DSI_clk_HS_mode(0);

    if (dsi_set_window(x, y, x + w - 1u, y + h - 1u) != 0) return -1;

    pixels = w * h;
    while (sent < pixels) {
        uint32_t n = pixels - sent;
        uint8_t cmd = (sent == 0u) ? 0x2Cu : 0x3Cu; /* RAMWR, then RAMWRC */
        if (n > 40u) n = 40u;

        for (uint32_t i = 0; i < n; ++i) {
            pkt[i * 3u + 0u] = r;
            pkt[i * 3u + 1u] = g;
            pkt[i * 3u + 2u] = b;
        }
        if (DSI_dcs_write_long(cmd, pkt, n * 3u) != 0) return -1;
        sent += n;
    }
    return 0;
}

int mt6592_dsi_fill_rect_rgb565(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                uint16_t color)
{
    uint32_t pixels;
    uint32_t sent = 0u;
    uint8_t pkt[60u * 2u];
    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)color;

    if (x >= MT6592_J36_PANEL_WIDTH || y >= MT6592_J36_PANEL_HEIGHT || w == 0u || h == 0u) return 0;
    if (w > MT6592_J36_PANEL_WIDTH - x) w = MT6592_J36_PANEL_WIDTH - x;
    if (h > MT6592_J36_PANEL_HEIGHT - y) h = MT6592_J36_PANEL_HEIGHT - y;
    if (w > 0xffffffffu / h) return -1;

    DSI_SetMode(DSI_CMD_MODE);
    DSI_clk_HS_mode(0);

    if (DSI_dcs_write_short(0x3Au, 1, 0x55u) != 0) return -1; /* RGB565 for RAMWR payloads */
    if (dsi_set_window(x, y, x + w - 1u, y + h - 1u) != 0) return -1;

    pixels = w * h;
    while (sent < pixels) {
        uint32_t n = pixels - sent;
        uint8_t cmd = (sent == 0u) ? 0x2Cu : 0x3Cu; /* RAMWR, then RAMWRC */
        if (n > 60u) n = 60u;

        for (uint32_t i = 0; i < n; ++i) {
            pkt[i * 2u + 0u] = hi;
            pkt[i * 2u + 1u] = lo;
        }
        if (DSI_dcs_write_long(cmd, pkt, n * 2u) != 0) return -1;
        sent += n;
    }
    return 0;
}
