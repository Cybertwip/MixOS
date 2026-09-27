/*
 * dsi_reg.h — MT6592 DSI + MIPITX register map (J36 Ultra)
 *
 * Faithful port of the vendor kernel header
 *   Reference/android/mediatek/platform/mt6592/kernel/drivers/video/dsi_reg.h
 * The bitfield struct layouts are reproduced 1:1 so the read-modify-write
 * OUTREGBIT() accesses behave exactly as the stock dsi_drv.c. Only the Linux
 * type names (UINT32) are swapped for <stdint.h> types, and the register access
 * helpers are reimplemented freestanding (no mach/sync_write.h).
 *
 * Physical bases (bare metal, MMU flat) per the kernel io map
 *   memory.h: IO_VIRT_TO_PHYS(v) = 0x10000000 | (v & 0x0fffffff)
 *     DSI host  DSI_BASE        VA 0xF400C000 -> PA 0x1400C000
 *     MIPITX    MIPI_CONFIG_BASE VA 0xF0010000 -> PA 0x10010000
 *     efuse     0xF0206180        -> PA 0x10206180  (per-lane RT_CODE)
 */
#ifndef MT6592_DSI_REG_H
#define MT6592_DSI_REG_H

#include <stdint.h>

/* ── Register access helpers (replace mach/sync_write.h OUTREG/OUTREGBIT) ── */
#define INREG32(addr)        (*(volatile uint32_t *)(uintptr_t)(addr))
#define OUTREG32(addr, val)  (*(volatile uint32_t *)(uintptr_t)(addr) = (uint32_t)(val))
#define AS_UINT32(x)         (*(uint32_t *)(void *)(x))

/* Read 32 bits, set one bitfield, write 32 bits back — identical to the stock
 * OUTREGBIT (read-modify-write on the typed register struct). */
#define OUTREGBIT(TYPE, REG, bit, value)                 \
    do {                                                 \
        uint32_t _v = INREG32(&(REG));                   \
        TYPE _r = *(TYPE *)(void *)&_v;                  \
        _r.bit = (value);                                \
        OUTREG32(&(REG), AS_UINT32(&_r));                \
    } while (0)

/* ── MIPITX D-PHY (DSI_PHY_REGS @ 0x10010000) ── */
typedef struct {
    unsigned RG_DSI0_LDOCORE_EN      : 1;
    unsigned RG_DSI0_CKG_LDOOUT_EN   : 1;
    unsigned RG_DSI0_BCLK_SEL        : 2;
    unsigned RG_DSI0_LD_IDX_SEL      : 3;
    unsigned rsv_7                   : 1;
    unsigned RG_DSI0_PHYCLK_SEL      : 2;
    unsigned RG_DSI0_DSICLK_FREQ_SEL : 1;
    unsigned RG_DSI0_LPTX_CLMP_EN    : 1;
    unsigned rsv_12                  : 20;
} MIPITX_DSI0_CON_REG;

typedef struct {
    unsigned RG_DSI0_LNTC_LDOOUT_EN  : 1;
    unsigned RG_DSI0_LNTC_LOOPBACK_EN: 1;
    unsigned RG_DSI0_LNTC_LPTX_IPLUS1: 1;
    unsigned RG_DSI0_LNTC_LPTX_IPLUS2: 1;
    unsigned RG_DSI0_LNTC_LPTX_IMINUS: 1;
    unsigned RG_DSI0_LNTC_PHI_SEL    : 1;
    unsigned rsv_6                   : 2;
    unsigned RG_DSI0_LNTC_RT_CODE    : 4;
    unsigned rsv_12                  : 20;
} MIPITX_DSI0_CLOCK_LANE_REG;

typedef struct {
    unsigned RG_DSI0_LNT0_LDOOUT_EN  : 1;
    unsigned RG_DSI0_LNT0_LOOPBACK_EN: 1;
    unsigned RG_DSI0_LNT0_LPTX_IPLUS1: 1;
    unsigned RG_DSI0_LNT0_LPTX_IPLUS2: 1;
    unsigned RG_DSI0_LNT0_LPTX_IMINUS: 1;
    unsigned RG_DSI0_LNT0_LPCD_IPLUS : 1;
    unsigned RG_DSI0_LNT0_LPCD_IMINUS: 1;
    unsigned RG_DSI0_LNT0_RT_CODE    : 4;
    unsigned rsv_11                  : 21;
} MIPITX_DSI0_DATA_LANE0_REG;

typedef struct {
    unsigned RG_DSI0_LNT1_LDOOUT_EN  : 1;
    unsigned RG_DSI0_LNT1_LOOPBACK_EN: 1;
    unsigned RG_DSI0_LNT1_LPTX_IPLUS1: 1;
    unsigned RG_DSI0_LNT1_LPTX_IPLUS2: 1;
    unsigned RG_DSI0_LNT1_LPTX_IMINUS: 1;
    unsigned RG_DSI0_LNT1_RT_CODE    : 4;
    unsigned rsv_9                   : 23;
} MIPITX_DSI0_DATA_LANE1_REG;

typedef struct {
    unsigned RG_DSI0_LNT2_LDOOUT_EN  : 1;
    unsigned RG_DSI0_LNT2_LOOPBACK_EN: 1;
    unsigned RG_DSI0_LNT2_LPTX_IPLUS1: 1;
    unsigned RG_DSI0_LNT2_LPTX_IPLUS2: 1;
    unsigned RG_DSI0_LNT2_LPTX_IMINUS: 1;
    unsigned RG_DSI0_LNT2_RT_CODE    : 4;
    unsigned rsv_9                   : 23;
} MIPITX_DSI0_DATA_LANE2_REG;

typedef struct {
    unsigned RG_DSI0_LNT3_LDOOUT_EN  : 1;
    unsigned RG_DSI0_LNT3_LOOPBACK_EN: 1;
    unsigned RG_DSI0_LNT3_LPTX_IPLUS1: 1;
    unsigned RG_DSI0_LNT3_LPTX_IPLUS2: 1;
    unsigned RG_DSI0_LNT3_LPTX_IMINUS: 1;
    unsigned RG_DSI0_LNT3_RT_CODE    : 4;
    unsigned rsv_9                   : 23;
} MIPITX_DSI0_DATA_LANE3_REG;

typedef struct {
    unsigned RG_DSI_LNT_INTR_EN      : 1;
    unsigned RG_DSI_LNT_HS_BIAS_EN   : 1;
    unsigned RG_DSI_LNT_IMP_CAL_EN   : 1;
    unsigned RG_DSI_LNT_TESTMODE_EN  : 1;
    unsigned RG_DSI_LNT_IMP_CAL_CODE : 4;
    unsigned RG_DSI_LNT_AIO_SEL      : 3;
    unsigned RG_DSI_PAD_TIE_LOW_EN   : 1;
    unsigned RG_DSI_DEBUG_INPUT_EN   : 1;
    unsigned RG_DSI_PRESERVE         : 3;
    unsigned rsv_16                  : 16;
} MIPITX_DSI_TOP_CON_REG;

typedef struct {
    unsigned RG_DSI_BG_CORE_EN       : 1;
    unsigned RG_DSI_BG_CKEN          : 1;
    unsigned RG_DSI_BG_DIV           : 2;
    unsigned RG_DSI_BG_FAST_CHARGE   : 1;
    unsigned RG_DSI_V12_SEL          : 3;
    unsigned RG_DSI_V10_SEL          : 3;
    unsigned RG_DSI_V072_SEL         : 3;
    unsigned RG_DSI_V04_SEL          : 3;
    unsigned RG_DSI_V032_SEL         : 3;
    unsigned RG_DSI_V02_SEL          : 3;
    unsigned rsv_23                  : 1;
    unsigned RG_DSI_BG_R1_TRIM       : 4;
    unsigned RG_DSI_BG_R2_TRIM       : 4;
} MIPITX_DSI_BG_CON_REG;

typedef struct {
    unsigned RG_DSI0_MPPLL_PLL_EN    : 1;
    unsigned RG_DSI0_MPPLL_PREDIV    : 2;
    unsigned RG_DSI0_MPPLL_TXDIV0    : 2;
    unsigned RG_DSI0_MPPLL_TXDIV1    : 2;
    unsigned RG_DSI0_MPPLL_POSDIV    : 3;
    unsigned RG_DSI0_MPPLL_MONVC_EN  : 1;
    unsigned RG_DSI0_MPPLL_MONREF_EN : 1;
    unsigned RG_DSI0_MPPLL_VDO_EN    : 1;
    unsigned rsv_13                  : 19;
} MIPITX_DSI_PLL_CON0_REG;

typedef struct {
    unsigned RG_DSI0_MPPLL_SDM_FRA_EN      : 1;
    unsigned RG_DSI0_MPPLL_SDM_SSC_PH_INIT : 1;
    unsigned RG_DSI0_MPPLL_SDM_SSC_EN      : 1;
    unsigned rsv_3                         : 13;
    unsigned RG_DSI0_MPPLL_SDM_SSC_PRD     : 16;
} MIPITX_DSI_PLL_CON1_REG;

typedef struct {
    unsigned RG_DSI0_MPPLL_SDM_PCW_0_7   : 8;
    unsigned RG_DSI0_MPPLL_SDM_PCW_8_15  : 8;
    unsigned RG_DSI0_MPPLL_SDM_PCW_16_23 : 8;
    unsigned RG_DSI0_MPPLL_SDM_PCW_H     : 7;
    unsigned rsv_31                      : 1;
} MIPITX_DSI_PLL_CON2_REG;

typedef struct {
    unsigned RG_DSI0_MPPLL_SDM_SSC_DELTA1 : 16;
    unsigned RG_DSI0_MPPLL_SDM_SSC_DELTA  : 16;
} MIPITX_DSI_PLL_CON3_REG;

typedef struct {
    unsigned RG_DSI0_MPPLL_SDM_PCW_CHG : 1;
    unsigned rsv_1                     : 31;
} MIPITX_DSI_PLL_CHG_REG;

typedef struct {
    unsigned RG_MPPLL_TST_EN      : 1;
    unsigned RG_MPPLL_TSTCK_EN    : 1;
    unsigned RG_MPPLL_TSTSEL      : 2;
    unsigned rsv_4                : 4;
    unsigned RG_MPPLL_PRESERVE_L  : 2;
    unsigned RG_MPPLL_PRESERVE_H  : 6;
    unsigned rsv_16               : 16;
} MIPITX_DSI_PLL_TOP_REG;

typedef struct {
    unsigned DA_DSI0_MPPLL_SDM_PWR_ON  : 1;
    unsigned DA_DSI0_MPPLL_SDM_ISO_EN  : 1;
    unsigned rsv_2                     : 6;
    unsigned AD_DSI0_MPPLL_SDM_PWR_ACK : 1;
    unsigned rsv_9                     : 23;
} MIPITX_DSI_PLL_PWR_REG;

typedef struct {
    MIPITX_DSI0_CON_REG        MIPITX_DSI0_CON;        // 0000
    MIPITX_DSI0_CLOCK_LANE_REG MIPITX_DSI0_CLOCK_LANE; // 0004
    MIPITX_DSI0_DATA_LANE0_REG MIPITX_DSI0_DATA_LANE0; // 0008
    MIPITX_DSI0_DATA_LANE1_REG MIPITX_DSI0_DATA_LANE1; // 000C
    MIPITX_DSI0_DATA_LANE2_REG MIPITX_DSI0_DATA_LANE2; // 0010
    MIPITX_DSI0_DATA_LANE3_REG MIPITX_DSI0_DATA_LANE3; // 0014
    uint32_t                   rsv_18[10];             // 0018..003C
    MIPITX_DSI_TOP_CON_REG     MIPITX_DSI_TOP_CON;     // 0040
    MIPITX_DSI_BG_CON_REG      MIPITX_DSI_BG_CON;      // 0044
    uint32_t                   rsv_48[2];              // 0048..004C
    MIPITX_DSI_PLL_CON0_REG    MIPITX_DSI_PLL_CON0;    // 0050
    MIPITX_DSI_PLL_CON1_REG    MIPITX_DSI_PLL_CON1;    // 0054
    MIPITX_DSI_PLL_CON2_REG    MIPITX_DSI_PLL_CON2;    // 0058
    MIPITX_DSI_PLL_CON3_REG    MIPITX_DSI_PLL_CON3;    // 005C
    MIPITX_DSI_PLL_CHG_REG     MIPITX_DSI_PLL_CHG;     // 0060
    MIPITX_DSI_PLL_TOP_REG     MIPITX_DSI_PLL_TOP;     // 0064
    MIPITX_DSI_PLL_PWR_REG     MIPITX_DSI_PLL_PWR;     // 0068
} volatile DSI_PHY_REGS, *PDSI_PHY_REGS;

/* ── DSI host controller (DSI_REGS @ 0x1400C000) ── */
typedef struct {
    unsigned DSI_START      : 1;
    unsigned rsv_1          : 1;
    unsigned SLEEPOUT_START : 1;
    unsigned rsv_3          : 13;
    unsigned VM_CMD_START   : 1;
    unsigned rsv_17         : 15;
} DSI_START_REG;

typedef struct {
    unsigned RD_RDY        : 1;
    unsigned CMD_DONE      : 1;
    unsigned TE_RDY        : 1;
    unsigned VM_DONE       : 1;
    unsigned EXT_TE        : 1;
    unsigned VM_CMD_DONE   : 1;
    unsigned SLEEPOUT_DONE : 1;
    unsigned rsv_7         : 24;
    unsigned BUSY          : 1;
} DSI_INT_STATUS_REG;

typedef struct {
    unsigned DSI_RESET : 1;
    unsigned DSI_EN    : 1;
    unsigned rsv_2     : 30;
} DSI_COM_CTRL_REG;

typedef struct {
    unsigned MODE          : 2;
    unsigned rsv_2         : 14;
    unsigned FRM_MODE      : 1;
    unsigned MIX_MODE      : 1;
    unsigned V2C_SWITCH_ON : 1;
    unsigned C2V_SWITCH_ON : 1;
    unsigned SLEEP_MODE    : 1;
    unsigned rsv_21        : 11;
} DSI_MODE_CTRL_REG;

typedef struct {
    unsigned VC_NUM      : 2;
    unsigned LANE_NUM    : 4;
    unsigned DIS_EOT     : 1;
    unsigned NULL_EN     : 1;
    unsigned TE_FREERUN  : 1;
    unsigned EXT_TE_EN   : 1;
    unsigned EXT_TE_EDGE : 1;
    unsigned TE_AUTO_SYNC: 1;
    unsigned MAX_RTN_SIZE: 4;
    unsigned HSTX_CKLP_EN: 1;
    unsigned rsv_17      : 15;
} DSI_TXRX_CTRL_REG;

typedef struct {
    unsigned DSI_PS_WC  : 14;
    unsigned rsv_14     : 2;
    unsigned DSI_PS_SEL : 2;
    unsigned rsv_18     : 14;
} DSI_PSCTRL_REG;

typedef struct {
    unsigned PHY_RST  : 1;
    unsigned rsv1     : 4;
    unsigned HTXTO_RST: 1;
    unsigned LRXTO_RST: 1;
    unsigned BTATO_RST: 1;
    unsigned rsv8     : 24;
} DSI_PHY_CON_REG;

typedef struct {
    unsigned LC_HS_TX_EN : 1;
    unsigned LC_ULPM_EN  : 1;
    unsigned LC_WAKEUP_EN: 1;
    unsigned rsv3        : 29;
} DSI_PHY_LCCON_REG;

typedef struct {
    unsigned L0_HS_TX_EN : 1;
    unsigned L0_ULPM_EN  : 1;
    unsigned L0_WAKEUP_EN: 1;
    unsigned rsv3        : 29;
} DSI_PHY_LD0CON_REG;

typedef struct {
    unsigned char LPX;
    unsigned char HS_PRPR;
    unsigned char HS_ZERO;
    unsigned char HS_TRAIL;
} DSI_PHY_TIMCON0_REG;

typedef struct {
    unsigned char TA_GO;
    unsigned char TA_SURE;
    unsigned char TA_GET;
    unsigned char DA_HS_EXIT;
} DSI_PHY_TIMCON1_REG;

typedef struct {
    unsigned char CONT_DET;
    unsigned char rsv8;
    unsigned char CLK_ZERO;
    unsigned char CLK_TRAIL;
} DSI_PHY_TIMCON2_REG;

typedef struct {
    unsigned char CLK_HS_PRPR;
    unsigned char CLK_HS_POST;
    unsigned char CLK_HS_EXIT;
    unsigned      rsv24 : 8;
} DSI_PHY_TIMCON3_REG;

typedef struct {
    unsigned VM_CMD_EN : 1;
    unsigned LONG_PKT  : 1;
    unsigned TIME_SEL  : 1;
    unsigned TS_VSA_EN : 1;
    unsigned TS_VBP_EN : 1;
    unsigned TS_VFP_EN : 1;
    unsigned rsv6      : 2;
    unsigned CM_DATA_ID: 8;
    unsigned CM_DATA_0 : 8;
    unsigned CM_DATA_1 : 8;
} DSI_VM_CMD_CON_REG;

typedef struct {
    DSI_START_REG       DSI_START;       // 0000
    DSI_INT_STATUS_REG  DSI_STA;         // 0004
    uint32_t            DSI_INTEN;       // 0008
    DSI_INT_STATUS_REG  DSI_INTSTA;      // 000C
    DSI_COM_CTRL_REG    DSI_COM_CTRL;    // 0010
    DSI_MODE_CTRL_REG   DSI_MODE_CTRL;   // 0014
    DSI_TXRX_CTRL_REG   DSI_TXRX_CTRL;   // 0018
    DSI_PSCTRL_REG      DSI_PSCTRL;      // 001C
    uint32_t            DSI_VSA_NL;      // 0020
    uint32_t            DSI_VBP_NL;      // 0024
    uint32_t            DSI_VFP_NL;      // 0028
    uint32_t            DSI_VACT_NL;     // 002C
    uint32_t            rsv_30[8];       // 0030..004C
    uint32_t            DSI_HSA_WC;      // 0050
    uint32_t            DSI_HBP_WC;      // 0054
    uint32_t            DSI_HFP_WC;      // 0058
    uint32_t            DSI_BLLP_WC;     // 005C
    uint32_t            DSI_CMDQ_SIZE;   // 0060
    uint32_t            DSI_HSTX_CKL_WC; // 0064
    uint32_t            rsv_0068[39];    // 0068..0100
    DSI_PHY_LCCON_REG   DSI_PHY_LCCON;   // 0104
    DSI_PHY_LD0CON_REG  DSI_PHY_LD0CON;  // 0108
    uint32_t            rsv_010C;        // 010C
    DSI_PHY_TIMCON0_REG DSI_PHY_TIMECON0;// 0110
    DSI_PHY_TIMCON1_REG DSI_PHY_TIMECON1;// 0114
    DSI_PHY_TIMCON2_REG DSI_PHY_TIMECON2;// 0118
    DSI_PHY_TIMCON3_REG DSI_PHY_TIMECON3;// 011C
    uint32_t            DSI_PHY_TIMECON4;// 0120
    uint32_t            rsv_0124[3];     // 0124..012C
    DSI_VM_CMD_CON_REG  DSI_VM_CMD_CON;  // 0130
} volatile DSI_REGS, *PDSI_REGS;

#endif /* MT6592_DSI_REG_H */
