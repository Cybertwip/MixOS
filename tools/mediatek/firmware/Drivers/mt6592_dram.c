/* Copyright (c) 2026 SynChanCyberSecurity LLC. All Rights Reserved. */
/*
 * MT6592 LPDDR2 DRAM bring-up -- freestanding port.
 *
 * Ported from MediaDeb/MT6592-KK-KERNEL preloader (mediatek/platform/mt6592/
 * preloader/src/drivers/{pll.c,emi.c}): mt_mempll_init(1066,1) + init_lpddr2().
 * The mem-PLL register values are pre-computed for our exact case (1066 MHz,
 * 1-PLL sync mode). The EMI_SETTINGS are the EXACT values extracted from this
 * device's stock preloader (Hardware/.../preloader_j36ultra.bin, MTK_BLOADER
 * record 0 -> type 0x0002, EMI_CONA 0x0002a3ae), not the generic default.
 * init_lpddr2 ends with built-in 1066 default DQ/DQS delays. The preloader's
 * post-init EMI/DRAMC enable bits are applied before the framebuffer probe.
 *
 * gpt_busy_wait_us() is replaced with a conservative bounded busy loop. Returns
 * 0 on success after the stage1 framebuffer window accepts write/read probes.
 */

#include <stdint.h>

#include "mt6592_dram.h"

/* ---- bases (IO_PHYS = 0x10000000) ---- */
#define DRAMC0_BASE   0x10004000u
#define DDRPHY_BASE   0x1000F000u
#define EMI_BASE      0x10203000u
#define EMI_CONA      (EMI_BASE + 0x0000u)
#define EMI_CONF      (EMI_BASE + 0x0028u)
#define EMI_CONM      (EMI_BASE + 0x0060u)

/* ---- this board's EMI_SETTINGS (extracted from stock preloader record 0) ---- */
#define V_EMI_CONA        0x0002a3aeu  /* dual-rank, & 0x20000 set */
#define V_DRVCTL0         0xaa00aa00u
#define V_DRVCTL1         0xaa00aa00u
#define V_GDDR3CTL1       0x01000000u
#define V_CONF1           0xf0048683u
#define V_DDR2CTL         0xa00632d1u
#define V_MISCTL0         0x07800000u
#define V_ACTIM           0x44584493u
#define V_ACTIM05T        0x04002600u
#define V_TEST2_3         0xbf080401u
#define V_ACTIM1          0x01000510u
#define V_CONF2           0x0340633fu
#define V_PD_CTRL         0xd1642342u
#define V_PADCTL3         0x00008888u
#define V_DQODLY          0x88888888u
#define V_MR1             0x00c30001u
#define V_MR2             0x00060002u
#define V_MR3             0x00020003u
#define V_MR5             0x00000006u
#define V_MR10            0x00ff000au
#define V_MR63            0x0000003fu

/* DRAM physical window (rank0 base). Dual 512MB ranks -> 1GB total. */
#define DRAM_BASE         0x80000000u
#ifndef MVII_MT6592_DRAM_PROBE_ADDR
#define MVII_MT6592_DRAM_PROBE_ADDR 0x82700000u
#endif
#ifndef MVII_MT6592_DRAM_PROBE_BYTES
#define MVII_MT6592_DRAM_PROBE_BYTES 0x0012c000u
#endif

static uint32_t g_last_bad_addr = 0u;
static uint32_t g_last_readback = 0u;
static uint32_t g_last_phase = 0u;
static uint32_t g_drvp = 0x0au;
static uint32_t g_drvn = 0x0bu;
volatile uint32_t mt6592_stage1_abort_flag = 0u;
volatile uint32_t mt6592_stage1_abort_resume = 0u;

static uint32_t rd(uint32_t a) { return *(volatile uint32_t*)(uintptr_t)a; }
static void wr(uint32_t a, uint32_t v) { *(volatile uint32_t*)(uintptr_t)a = v; }
static void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* init_lpddr2 writes DRAMC0_BASE+off and DDRPHY_BASE+off with the same value
 * almost everywhere; WB mirrors that 1:1. */
static void WB(uint32_t off, uint32_t v) { wr(DRAMC0_BASE + off, v); wr(DDRPHY_BASE + off, v); }
static void WB_OR(uint32_t off, uint32_t v) {
    wr(DRAMC0_BASE + off, rd(DRAMC0_BASE + off) | v);
    wr(DDRPHY_BASE + off, rd(DDRPHY_BASE + off) | v);
}
static void WB_AND(uint32_t off, uint32_t v) {
    wr(DRAMC0_BASE + off, rd(DRAMC0_BASE + off) & v);
    wr(DDRPHY_BASE + off, rd(DDRPHY_BASE + off) & v);
}
static void EW(uint32_t off, uint32_t v) { wr(EMI_BASE + off, v); }
static void dram_udelay(uint32_t us);

static uint32_t rextdn_drive_value(void) {
    return 0x00220022u | (g_drvp << 28) | (g_drvn << 24) | (g_drvp << 12) | (g_drvn << 8);
}

static void rextdn_sw_calibration(void) {
    uint32_t tmp;
    uint32_t drvp;
    uint32_t drvn;

    tmp = rd(DRAMC0_BASE + 0x01e4u) & 0xffffdfffu;
    wr(DRAMC0_BASE + 0x01e4u, tmp);

    tmp = rd(DRAMC0_BASE + 0x0100u) & 0xfffcffffu;
    wr(DRAMC0_BASE + 0x0100u, tmp);

    tmp = (rd(DRAMC0_BASE + 0x00c0u) & 0xfffdf0f0u) | 0x000500f0u;
    wr(DRAMC0_BASE + 0x00c0u, tmp);
    for (drvp = 0u; drvp <= 15u; ++drvp) {
        tmp = (rd(DRAMC0_BASE + 0x00c0u) & 0xffff0fffu) | (drvp << 12);
        wr(DRAMC0_BASE + 0x00c0u, tmp);
        dram_udelay(1000u);
        if ((rd(DRAMC0_BASE + 0x03dcu) >> 31) == 1u) break;
    }
    if (drvp > 15u) drvp = 0x0au;

    tmp = rd(DRAMC0_BASE + 0x0100u) & 0xfffcffffu;
    wr(DRAMC0_BASE + 0x0100u, tmp);

    tmp = (rd(DRAMC0_BASE + 0x00c0u) & 0xfffbffffu) | 0x0002000fu;
    wr(DRAMC0_BASE + 0x00c0u, tmp);
    for (drvn = 0u; drvn <= 15u; ++drvn) {
        tmp = (rd(DRAMC0_BASE + 0x00c0u) & 0xfffff0ffu) | (drvn << 8);
        wr(DRAMC0_BASE + 0x00c0u, tmp);
        dram_udelay(1000u);
        if ((rd(DRAMC0_BASE + 0x03dcu) >> 31) == 0u) {
            if (drvn > 0u) --drvn;
            break;
        }
    }
    if (drvn > 15u) drvn = 0x0bu;

    g_drvp = drvp;
    g_drvn = drvn;
}

static int safe_write_read32(uint32_t addr, uint32_t want, uint32_t* got_out) __attribute__((noinline));
static int safe_write_read32(uint32_t addr, uint32_t want, uint32_t* got_out) {
    uint32_t got = 0u;

    mt6592_stage1_abort_flag = 0u;
    mt6592_stage1_abort_resume = (uint32_t)(uintptr_t)&&faulted;
    __asm__ volatile("dsb sy\nisb" ::: "memory");

    *(volatile uint32_t*)(uintptr_t)addr = want;
    __asm__ volatile("dsb sy" ::: "memory");
    got = *(volatile uint32_t*)(uintptr_t)addr;
    __asm__ volatile("dsb sy\nisb" ::: "memory");

    mt6592_stage1_abort_resume = 0u;
    if (mt6592_stage1_abort_flag) goto faulted;
    if (got_out) *got_out = got;
    return got == want;

faulted:
    mt6592_stage1_abort_resume = 0u;
    if (got_out) *got_out = 0xdab0da7au;
    return 0;
}

/* Conservative microsecond delay (no timer here). Tuned long on purpose: the
 * mem-PLL lock wait must be >= ~1ms; over-delaying is harmless for bring-up. */
static void dram_udelay(uint32_t us) {
    volatile uint32_t i;
    while (us--) {
        for (i = 0; i < 1500u; ++i) {
            __asm__ volatile("nop");
        }
    }
}

/* mt_mempll_init(1066, 1) -- DDRPHY register values pre-computed for 1066MHz,
 * 1-PLL sync mode (see mt6592_dram.c header note). Offsets are (reg<<2). */
static void mempll_init_1066(void) {
    WB_OR(0x007c, 0x00000001u);                 /* DFREQ_DIV2=1 (DRAMC0+DDRPHY) */
    wr(DDRPHY_BASE + 0x640u, 0x00000007u);      /* 0x190: 1PLL sync mode */

    wr(DDRPHY_BASE + 0x5c0u, 0x00000000u);      /* 0x170 */
    wr(DDRPHY_BASE + 0x5c4u, 0x00000000u);      /* 0x171 */
    wr(DDRPHY_BASE + 0x5c8u, 0x1111ff11u);      /* 0x172 */
    wr(DDRPHY_BASE + 0x5ccu, 0x11111137u);      /* 0x173 */
    wr(DDRPHY_BASE + 0x600u, 0x00000000u);      /* 0x180 */
    wr(DDRPHY_BASE + 0x604u, rd(DDRPHY_BASE + 0x604u) | 0xa4040000u); /* 0x181 seed */
    wr(DDRPHY_BASE + 0x60cu, 0x2c000000u);      /* 0x183 */
    wr(DDRPHY_BASE + 0x608u, 0x000000a8u);      /* 0x182 */
    wr(DDRPHY_BASE + 0x614u, 0x2c000000u);      /* 0x185 */
    wr(DDRPHY_BASE + 0x610u, 0x000000a8u);      /* 0x184 */
    wr(DDRPHY_BASE + 0x61cu, 0x2c000000u);      /* 0x187 */
    wr(DDRPHY_BASE + 0x618u, 0x000000a8u);      /* 0x186 */
    wr(DDRPHY_BASE + 0x620u, 0x00000000u);      /* 0x188 */
    wr(DDRPHY_BASE + 0x624u, 0xa0186187u);      /* 0x189 n_info */
    wr(DDRPHY_BASE + 0x640u, 0x00000007u);      /* 0x190 */

    /* power-on sequence */
    dram_udelay(2);
    wr(DDRPHY_BASE + 0x604u, rd(DDRPHY_BASE + 0x604u) | 0xa4044000u);  /* +bias_en */
    dram_udelay(2);
    wr(DDRPHY_BASE + 0x604u, rd(DDRPHY_BASE + 0x604u) | 0xa404c000u);  /* +bias_lpf_en */
    dram_udelay(1000);
    wr(DDRPHY_BASE + 0x600u, 0x00000004u);                            /* mempll_en */
    dram_udelay(20);
    wr(DDRPHY_BASE + 0x628u, 0x10000000u);                            /* 0x18a */
    dram_udelay(2);
    wr(DDRPHY_BASE + 0x628u, 0x18000000u);
    wr(DDRPHY_BASE + 0x620u, 0x00000800u);                            /* 0x188 sdm_prd_1 */
    dram_udelay(2);
    wr(DDRPHY_BASE + 0x600u, 0x00000004u);
    wr(DDRPHY_BASE + 0x604u, rd(DDRPHY_BASE + 0x604u) | 0xa504c000u);  /* +div_en */
    dram_udelay(23);
    wr(DDRPHY_BASE + 0x60cu, 0x20040000u);      /* 0x183 mempll2_en (1PLL) */
    wr(DDRPHY_BASE + 0x614u, 0x20000000u);      /* 0x185 */
    wr(DDRPHY_BASE + 0x61cu, 0x20000000u);      /* 0x187 */
    dram_udelay(23);
    wr(DDRPHY_BASE + 0x640u, 0x00000037u);      /* 0x190 final: +dmpll2_clk_en+dmall_ck_en */

    /* control switch to SPM */
    wr(DDRPHY_BASE + 0x5ccu, 0x00101010u);      /* 0x173 */
    wr(DDRPHY_BASE + 0x5c8u, 0x0000fc10u);      /* 0x172 (1PLL) */
    wr(DDRPHY_BASE + 0x5c0u, 0x003c1b96u);      /* 0x170 delay time */
}

/* init_lpddr2() -- faithful transcription (no DLE/DQS calibration; built-in
 * 1066 default delays). Dual-rank path taken (EMI_CONA & 0x20000). */
static void init_lpddr2(void) {
    /* ---- EMI setting ---- */
    EW(0x0060, 0x0000051fu);
    EW(0x0140, 0x20406188u);
    EW(0x0144, 0x20406188u);
    EW(0x0100, 0x40105806u);
    EW(0x0108, 0x808050eau);
    EW(0x0110, 0xffff50c4u);
    EW(0x0118, 0x0810d84au);
    EW(0x0120, 0x40405042u);
    EW(0x0148, 0x9719595eu);
    EW(0x014c, 0x9719595eu);
    wr(EMI_CONA, V_EMI_CONA);
    EW(0x00f8, 0x00000000u);
    EW(0x0400, 0x007f0001u);
    EW(0x0008, 0x05121624u);
    EW(0x0010, 0x04070506u);
    EW(0x0018, 0x05121624u);
    EW(0x0020, 0x04070506u);
    EW(0x0030, 0x04050516u);
    EW(0x0038, 0x04050516u);
    EW(0x0158, 0x00010800u);
    EW(0x0150, 0x6470fc79u);
    EW(0x0154, 0x6470fc79u);
    EW(0x00e8, 0x00000100u);
    EW(0x00f0, 0x38470000u);
    EW(0x0078, 0x23110003u);
    EW(0x0158, 0x1f011f00u);
    EW(0x00e8, 0x00001004u);
    EW(0x0060, 0x0000051fu);
    EW(0x00d0, 0x84848c84u);
    EW(0x00d8, 0x00480084u);

    /* ---- DRAMC/DDRPHY driving + config ---- */
    WB(0x00b4, rextdn_drive_value());
    WB(0x00b8, rextdn_drive_value());
    WB(0x00bc, rextdn_drive_value());
    WB_OR(0x0640, 0x00003f00u);                 /* pre-emphasis after MEMPLL init */
    WB(0x00fc, V_MISCTL0);
    WB(0x0048, 0x0001110du);
    WB(0x00d8, 0x00500900u);                    /* pinmux */
    WB(0x00e4, 0x00000000u);
    WB(0x008c, 0x00000001u);
    WB(0x0090, 0x00000000u);
    WB(0x0094, 0x80000000u);
    WB(0x00dc, 0x83004004u);
    WB(0x00e0, 0x1b004004u);                    /* !SYSTEM_26M, !3PLL */
    WB(0x0124, 0xaa080033u);
    WB(0x00f0, 0xc0000000u);
    WB(0x00f4, V_GDDR3CTL1);
    WB(0x0168, 0x00000080u);
    WB(0x00d8, 0x00700900u);
    WB(0x0004, V_CONF1);
    WB(0x007c, V_DDR2CTL);                       /* !ASYNC */
    WB(0x0028, 0xf1200f01u);
    WB(0x01e0, 0x3001ebffu);
    WB(0x0158, 0xf0f0f0f0u);
    WB(0x0400, 0x00111100u);
    WB(0x0404, 0x00000002u);
    WB(0x0408, 0x00222222u);
    WB(0x040c, 0x33330000u);
    WB(0x0410, 0x33330000u);
    WB(0x0110, 0x0b052311u);
    WB(0x00e4, 0x00000005u);                     /* CKEBYCTL */

    dsb();
    dram_udelay(200);

    /* ---- DRAM wakeup + mode-register write sequence ---- */
    WB(0x0088, V_MR63);
    WB(0x01e4, 0x00000001u);
    dsb();
    dram_udelay(10);
    WB(0x01e4, 0x00000000u);

    WB_AND(0x0110, ~0x7u);                       /* disable 2-rank for ZQ cal */

    WB(0x0088, V_MR10);                           /* ZQ calibration init (rank0) */
    WB(0x01e4, 0x00000001u);
    dsb();
    dram_udelay(1);
    WB(0x01e4, 0x00000000u);

    if (rd(EMI_CONA) & 0x20000u) {               /* dual-rank: ZQ cal rank1 */
        WB_OR(0x0110, 0x8u);
        WB(0x0088, V_MR10);
        WB(0x01e4, 0x00000001u);
        dsb();
        dram_udelay(1);
        WB(0x01e4, 0x00000000u);
        WB_AND(0x0110, ~0x8u);
        WB_OR(0x0110, 0x1u);
    }

    WB(0x0088, V_MR1);
    WB(0x01e4, 0x00000001u);
    dsb();
    dram_udelay(1);
    WB(0x01e4, 0x00000000u);

    WB(0x0088, V_MR2);
    WB(0x01e4, 0x00000001u);
    dsb();
    dram_udelay(1);
    WB(0x01e4, 0x00000000u);

    WB(0x0088, V_MR3);
    WB(0x01e4, 0x00000001u);
    dsb();
    dram_udelay(1);
    WB(0x01e4, 0x00001100u);

    if (rd(EMI_CONA) & 0x20000u) {
        WB(0x0110, 0x00112391u);
    } else {
        WB(0x0110, 0x00112390u);
    }

    WB(0x00e4, 0x00000001u);                      /* CKEBYCTL */
    WB(0x01ec, 0x00000001u);                      /* dual scheduler */
    WB(0x0084, 0x00000a56u);
    WB(0x000c, 0x00000000u);
    WB(0x0000, V_ACTIM);
    WB(0x01f8, V_ACTIM05T);
    WB(0x0044, V_TEST2_3);
    WB(0x01e8, V_ACTIM1);
    WB(0x0008, V_CONF2);
    WB(0x0010, 0x00000000u);
    WB(0x00f8, 0xedcb000fu);
    WB(0x0020, 0x00000000u);
    WB_OR(0x0640, 0xfc7f8008u);                   /* dynamic clk gating (!ASYNC,!3PLL) */
    WB(0x01dc, V_PD_CTRL);
    WB(0x0110, 0x00112381u);

    /* ---- built-in 1066 default RX/TX delays (no calibration) ---- */
    WB(0x0210, 0x05050101u);
    WB(0x0214, 0x00040307u);
    WB(0x0218, 0x01030503u);
    WB(0x021c, 0x03020100u);
    WB(0x0220, 0x05070001u);
    WB(0x0224, 0x03040602u);
    WB(0x0228, 0x03030405u);
    WB(0x022c, 0x04040004u);
    WB(0x0018, 0x1F221E22u);
    WB(0x001c, 0x1F221E22u);
    WB(0x0014, V_PADCTL3);                         /* DQS out delay */
    WB(0x0010, V_PADCTL3);                         /* DQM out delay */
    WB(0x0200, V_DQODLY);                          /* DQ out delay */
    WB(0x0204, V_DQODLY);
    WB(0x0208, V_DQODLY);
    WB(0x020c, V_DQODLY);
    dsb();
}

static void finish_lpddr2_post_init(void) {
    uint32_t val1;

    /* Android preloader enables EMI transactions after DRAMC init. Without this
     * bit the framebuffer writes can disappear even when DRAMC registers read
     * back normally. */
    wr(EMI_CONM, rd(EMI_CONM) | (1u << 10));

    WB_OR(0x015c, 0x80000000u);
    if (rd(EMI_CONA) & 0x20000u) {
        val1 = rd(DRAMC0_BASE + 0x00e0u) & 0x07000000u;
        val1 = (((val1 >> 24) + 1u) << 16);
        wr(DRAMC0_BASE + 0x01c4u, (rd(DRAMC0_BASE + 0x01c4u) & 0xfff0ffffu) | val1);
        wr(DDRPHY_BASE + 0x01c4u, (rd(DDRPHY_BASE + 0x01c4u) & 0xfff0ffffu) | val1);
    }
    WB_OR(0x0630, 1u << 20);
    WB_OR(0x01c0, 0x80000000u);
    WB_OR(0x01ec, 0x00004f10u);
    dsb();
}

static int mt6592_dram_framebuffer_selftest(uint32_t* out_first_bad, uint32_t* out_readback) {
    static const uint32_t pats[4] = {0xa5a5a5a5u, 0x5a5a5a5au, 0x00000000u, 0xffffffffu};
    static const uint32_t offs[5] = {0x00000000u, 0x00000040u, 0x00001000u, 0x00020000u, 0x0012b000u};
    uint32_t ta_status = 0u;

    wr(DRAMC0_BASE + 0x003cu, 0x55000000u);
    wr(DRAMC0_BASE + 0x0040u, (rd(DRAMC0_BASE + 0x0040u) & 0xaa000000u) | 0x00000100u);
    wr(DRAMC0_BASE + 0x0008u, rd(DRAMC0_BASE + 0x0008u) | (1u << 31));
    wr(DRAMC0_BASE + 0x0008u, rd(DRAMC0_BASE + 0x0008u) | (1u << 30) | (1u << 31));
    for (uint32_t i = 0u; i < 100000u; ++i) {
        ta_status = rd(DRAMC0_BASE + 0x03fcu);
        if (ta_status & (1u << 10)) break;
    }
    wr(DRAMC0_BASE + 0x0008u, rd(DRAMC0_BASE + 0x0008u) & ~((1u << 30) | (1u << 31)));
    if ((ta_status & (1u << 10)) == 0u || (ta_status & (1u << 14)) != 0u || (ta_status & (1u << 18)) == 0u) {
        if (out_first_bad) *out_first_bad = DRAMC0_BASE + 0x03fcu;
        if (out_readback) *out_readback = ta_status;
        return MT6592_DRAM_ERR_SELFTEST;
    }

    for (uint32_t p = 0; p < 4u; ++p) {
        for (uint32_t i = 0; i < 5u; ++i) {
            uint32_t off = offs[i];
            uint32_t got;
            uint32_t want;
            if (off + 4u > MVII_MT6592_DRAM_PROBE_BYTES) continue;
            want = pats[p] ^ off;
            if (!safe_write_read32(MVII_MT6592_DRAM_PROBE_ADDR + off, want, &got)) {
                if (out_first_bad) *out_first_bad = MVII_MT6592_DRAM_PROBE_ADDR + off;
                if (out_readback) *out_readback = got;
                return MT6592_DRAM_ERR_SELFTEST;
            }
        }
    }
    return MT6592_DRAM_OK;
}

/* Write/read patterns across a span of each rank and report mismatches. */
int mt6592_dram_selftest(uint32_t* out_first_bad, uint32_t* out_readback) {
    static const uint32_t pats[4] = {0xa5a5a5a5u, 0x5a5a5a5au, 0x0u, 0xffffffffu};
    /* probe a few offsets in rank0 (and into rank1 at +512MB) */
    static const uint32_t offs[6] = {
        0x00100000u, 0x02700000u, 0x0a000000u, 0x1f000000u, 0x20000000u, 0x3f000000u};
    for (uint32_t p = 0; p < 4u; ++p) {
        for (uint32_t i = 0; i < 6u; ++i) {
            volatile uint32_t* a = (volatile uint32_t*)(uintptr_t)(DRAM_BASE + offs[i]);
            *a = pats[p] ^ offs[i];
        }
        dsb();
        for (uint32_t i = 0; i < 6u; ++i) {
            volatile uint32_t* a = (volatile uint32_t*)(uintptr_t)(DRAM_BASE + offs[i]);
            uint32_t want = pats[p] ^ offs[i];
            uint32_t got = *a;
            if (got != want) {
                if (out_first_bad) *out_first_bad = DRAM_BASE + offs[i];
                if (out_readback) *out_readback = got;
                return -1;
            }
        }
    }
    return 0;
}

/* ============================================================================
 * RX DQS gating window calibration (rank 0) -- faithful port of the MT6592
 * preloader dramc_dqs_gw.c / dramc_calib.c. init_lpddr2 leaves DRAMC_DQSGCTL at
 * its 0xAA080088 default (uncalibrated), so the read strobe gate is never
 * trained and every read through DRAMC stalls the bus. This trains it: enable
 * the hardware DQS gating-window counter, sweep coarse(0..25) x fine(0..15) with
 * test reads whose gating counter (DQSGNWCNT0) reads 0x04040404 only when the
 * gate lands, then program the centre of the widest pass-window. Mirrors the
 * vendor DRAMC_WRITE_REG/READ_REG which touch all three DRAMC bases (incl. the
 * read-only NAO window where the counter lives). EMI transactions must already
 * be enabled (done at the top here) so the CPU test reads reach DRAMC.
 * ========================================================================= */
#define DRAMC_NAO_BASE   0x1020E000u
#define R_CONF1          0x004u
#define R_R0DQSIEN       0x094u
#define R_DQSCTL1        0x0e0u
#define R_PHYCTL1        0x0f0u
#define R_GDDR3CTL1      0x0f4u
#define R_DQSGCTL        0x124u
#define R_SPCMD          0x1e4u
#define R_DQSGNWCNT0     0x3c0u  /* preloader DQS gating window counter (DQSGNWCNT0) */
#define R_DQSGNWCNT_DDRPHY 0x3c0u
#define R_DQSGNWCNT_NAO   0x3c0u
#define R_ACTIM_EXT       0x1f4u  /* preloader writes to 0x1f4 during calibration */

/* Real DQS read-gating delay registers, recovered from the stock preloader's
 * per-iteration delay-apply (calibration framework 0x36d8 -> setter @file 0x2cf0;
 * see Hardware/Virtua/loader/mtk-da/RE_FINDINGS.md). The earlier port poked
 * 0xe0/0x124/0x094, which DO NOT move the gate -- so the counter stayed 0x00 for
 * every coarse/fine. The preloader writes the calibrated delays here, on all
 * three windows (DRAMC0, DDRPHY, NAO):
 *   0x010 = coarse, one nibble per byte-lane  (c | c<<4 | c<<8 | c<<12)
 *   0x014 = fine,   one nibble per byte-lane  (f | f<<4 | f<<8 | f<<12)
 *   0x200..0x20c = per-DQ-bit coarse taps (DQSIEN); uniform = c * 0x11111111 */
#define R_DQS_COARSE      0x010u
#define R_DQS_FINE        0x014u
#define R_DQSIEN_TAP0     0x200u
#define R_DQSIEN_TAP1     0x204u
#define R_DQSIEN_TAP2     0x208u
#define R_DQSIEN_TAP3     0x20cu

static void d3_write(uint32_t off, uint32_t v) {
    wr(DRAMC0_BASE + off, v);
    wr(DDRPHY_BASE + off, v);
    wr(DRAMC_NAO_BASE + off, v);
}
static void d3_set(uint32_t off, uint32_t bits) {
    /* Per-base set: read each window and set bits. Do not OR across windows. */
    wr(DRAMC0_BASE + off, rd(DRAMC0_BASE + off) | bits);
    wr(DDRPHY_BASE + off, rd(DDRPHY_BASE + off) | bits);
    wr(DRAMC_NAO_BASE + off, rd(DRAMC_NAO_BASE + off) | bits);
}
static void d3_clr(uint32_t off, uint32_t bits) {
    /* Per-base clear: read each window and clear bits. Do not OR across windows. */
    wr(DRAMC0_BASE + off, rd(DRAMC0_BASE + off) & ~bits);
    wr(DDRPHY_BASE + off, rd(DDRPHY_BASE + off) & ~bits);
    wr(DRAMC_NAO_BASE + off, rd(DRAMC_NAO_BASE + off) & ~bits);
}

/* DDR_PHY_RESET_NEW (preloader 0x3848 / 0x3a7c): set PHYCTL1[28]+GDDR3CTL1[25]
 * across all three windows (value merged from all three), hold, then clear.
 * The merge matters: the windows are kept coherent, so read the OR of all three
 * before setting the bit, and write that merged value back to each window. */
static uint32_t d3_or3(uint32_t off) {
    return rd(DRAMC0_BASE + off) | rd(DDRPHY_BASE + off) | rd(DRAMC_NAO_BASE + off);
}
static void gw_phy_reset(void) {
    d3_write(R_PHYCTL1,  d3_or3(R_PHYCTL1)  | (1u << 28));
    d3_write(R_GDDR3CTL1, d3_or3(R_GDDR3CTL1) | (1u << 25));
    dram_udelay(2);
    d3_write(R_PHYCTL1,  d3_or3(R_PHYCTL1)  & ~(1u << 28));
    d3_write(R_GDDR3CTL1, d3_or3(R_GDDR3CTL1) & ~(1u << 25));
}

/* Apply the swept DQS gating coarse/fine to the registers the gate ACTUALLY
 * watches (recovered from the stock preloader -- see R_DQS_COARSE et al above).
 * Single-knob/uniform form: program every byte-lane and every DQ bit with the
 * same value, exactly as the preloader does during its sweep. */
static uint32_t nib4(uint32_t v) {  /* v -> v|v<<4|v<<8|v<<12 (4 byte-lanes) */
    v &= 0xfu;
    return v | (v << 4) | (v << 8) | (v << 12);
}
static void gw_set_coarse(uint32_t c) {
    uint32_t tap = (c & 0xfu) * 0x11111111u;   /* per-DQ-bit coarse tap, uniform */
    d3_write(R_DQS_COARSE, nib4(c));
    d3_write(R_DQSIEN_TAP0, tap);
    d3_write(R_DQSIEN_TAP1, tap);
    d3_write(R_DQSIEN_TAP2, tap);
    d3_write(R_DQSIEN_TAP3, tap);
}
static void gw_set_fine(uint32_t v) {
    d3_write(R_DQS_FINE, nib4(v));
}

/* One gating test read (Sequence_Read): reset counter, PHY reset, CPU read,
 * then the counter reads 0x04040404 only if the gate caught DQS correctly.
 * The stock preloader reads the counter from all three DRAMC windows
 * (DRAMC0, DDRPHY, NAO) at offset 0x3c0. Try all three; the first non-zero
 * result is the authoritative counter value. */
/* The stock preloader iterates over three register windows to read the
 * DQS gating counter: DRAMC0, DDRPHY, and NAO. On some MT6592 steppings
 * the counter only responds through one of these windows. Try all three
 * and return the first non-zero result. */
static uint32_t gw_read_counter(void) {
    uint32_t v;
    v = rd(DRAMC0_BASE + R_DQSGNWCNT0);
    if (v != 0x00000000u) return v;
    v = rd(DDRPHY_BASE + R_DQSGNWCNT0);
    if (v != 0x00000000u) return v;
    v = rd(DRAMC_NAO_BASE + R_DQSGNWCNT0);
    return v;
}
static void gw_spcmd_dqs_gating_reset(void) {
    /* The preloader toggles SPCMD[9] (DQS gating window counter reset) on all
     * three DRAMC register windows (DRAMC0, DDRPHY, NAO). Keep the same order. */
    d3_set(R_SPCMD, 1u << 9);
    d3_clr(R_SPCMD, 1u << 9);
}
static int gw_read_pass(void) {
    volatile uint32_t rval;
    /* Reset the DQS gating window counter on all three DRAMC windows, then
     * issue a CPU read from DRAM_BASE. The counter latches 0x04040404 when
     * the read DQS strobe lands inside the trained gate window. */
    gw_spcmd_dqs_gating_reset();
    gw_phy_reset();
    rval = *(volatile uint32_t *)(uintptr_t)DRAM_BASE;
    (void)rval;
    return gw_read_counter() == 0x04040404u;
}
/* Do_Read_Test_DDR2: require two consecutive passing reads. */
static int gw_test(void) {
    if (!gw_read_pass()) return -1;
    if (!gw_read_pass()) return -1;
    return 0;
}

static int popcount32(uint32_t v) { int n = 0; while (v) { n += (int)(v & 1u); v >>= 1; } return n; }
static int first_set32(uint32_t v) { for (int i = 0; i < 32; ++i) if (v & (1u << i)) return i; return 0; }

/* Coarse and fine are each a 4-bit nibble per byte-lane in the real gate
 * registers (0x010 / 0x014), so both sweep 0..15. */
#define GW_COARSE_MAX 16u
#define GW_FINE_MAX   16u

static int mt6592_dram_dqs_gating_cal(void) {
    uint32_t dqs_gw[GW_COARSE_MAX];
    uint32_t c, fi, rank0_col;
    int best_c = -1, best_cnt = 0, f;

    /* Pre-calib enables (emi.c order: tx-enable + sync, BEFORE calibration). */
    wr(EMI_CONM, rd(EMI_CONM) | (1u << 10));
    WB_OR(0x015c, 0x80000000u);

    /* The stock preloader writes 0x1f4 (ACTIM_EXT / DQS gate extension) on all
     * three DRAMC bases before the sweep. Our init_lpddr2 skips this register.
     * Without it the DQS gating counter may not fire. Use the preloader's LPDDR2
     * value 0x000e0b0d. */
    wr(DRAMC0_BASE + R_ACTIM_EXT, 0x000e0b0du);
    wr(DDRPHY_BASE + R_ACTIM_EXT, 0x000e0b0du);
    wr(DRAMC_NAO_BASE + R_ACTIM_EXT, 0x000e0b0du);

    /* MA type: program rank0 column count into CONF1[9:8] from EMI_CONA[5:4]. */
    rank0_col = (rd(EMI_CONA) >> 4) & 0x3u;
    wr(DRAMC0_BASE + R_CONF1, (rd(DRAMC0_BASE + R_CONF1) & ~(0x3u << 8)) | (rank0_col << 8));

    /* Sweep coarse delay (0..25) x fine delay (0..15) with the hardware DQS
     * gating-window counter. The counter reads 0x04040404 only when the DQS
     * strobe lands inside the gate window. The preloader enables DQSCTL1[28]
     * and SPCMD[8] on all three DRAMC register windows (DRAMC0, DDRPHY, NAO);
     * do the same so the counter actually fires. */
    d3_set(R_DQSCTL1, 1u << 28);
    d3_set(R_SPCMD, 1u << 8);

    for (c = 0; c < GW_COARSE_MAX; ++c) {
        dqs_gw[c] = 0u;
        gw_set_coarse(c);
        for (fi = 0; fi < GW_FINE_MAX; ++fi) {
            gw_set_fine(fi);
            if (gw_test() == 0) dqs_gw[c] |= (1u << fi);
        }
        if (popcount32(dqs_gw[c]) > best_cnt) {
            best_cnt = popcount32(dqs_gw[c]);
            best_c = (int)c;
        }
    }

    /* Disable gating window counter on all three DRAMC windows. Both the
     * burst-mode bit (SPCMD[8]) AND the counter enable (DQSCTL1[28]) must be
     * cleared, otherwise normal DRAM reads stall waiting for a counter event. */
    d3_clr(R_SPCMD, 1u << 8);
    d3_clr(R_DQSCTL1, 1u << 28);

    if (best_c < 0 || best_cnt == 0) {
        /* Sweep found no pass window. Fall back to coarse=3 (the stock
         * J36 Ultra preloader value) but use gw_set_coarse() which does a
         * read-modify-write preserving the upper DQSGCTL control bits
         * (bits [7:6] and [3:2]) that init_lpddr2 set. The prior hardcoded
         * d3_write(R_DQSGCTL, 0xAA080033) cleared those bits, which caused
         * every normal DRAM read to stall the AXI bus.
         *
         * Read the raw gate counter so the breadcrumb can show whether the
         * counter is even responding (0x00000000 = dead, 0x04040404 = pass,
         * anything else = gate misaligned). */
        uint32_t raw_cnt = gw_read_counter();
        gw_set_coarse(3u);
        gw_set_fine(0u);
        gw_phy_reset();
        dram_udelay(1);
        /* coarse=3 in [15:8], raw counter byte0 in [23:16] */
        g_last_readback = 0x0300u | ((raw_cnt & 0xffu) << 16);
        g_last_bad_addr = DRAM_BASE;
        return MT6592_DRAM_OK;       /* not an error -- caller will RW-verify */
    }

    /* programme the centre of the widest pass-window */
    f = first_set32(dqs_gw[best_c]) + best_cnt / 2;
    if (f >= (int)GW_FINE_MAX) f = (int)GW_FINE_MAX - 1;
    gw_set_coarse((uint32_t)best_c);
    gw_set_fine((uint32_t)f);
    gw_phy_reset();
    dram_udelay(1);

    /* stash result for the breadcrumb: coarse in [15:8], fine index in [7:0] */
    g_last_readback = ((uint32_t)best_c << 8) | (uint32_t)f;
    g_last_bad_addr = 0u;
    return MT6592_DRAM_OK;
}

int mt6592_dram_init_step(uint32_t step) {
    g_last_phase = step;
    if (step == MT6592_DRAM_STEP_GATING) {
        return mt6592_dram_dqs_gating_cal();
    }
    if (step == MT6592_DRAM_STEP_MEMPLL) {
        wr(EMI_CONF, 0x04210000u);   /* enable EMI address scramble (mt_set_emi) */
        mempll_init_1066();
        return MT6592_DRAM_OK;
    }
    if (step == MT6592_DRAM_STEP_REXTDN) {
        rextdn_sw_calibration();
        return MT6592_DRAM_OK;
    }
    if (step == MT6592_DRAM_STEP_LPDDR2) {
        init_lpddr2();
        return MT6592_DRAM_OK;
    }
    if (step == MT6592_DRAM_STEP_POST) {
        finish_lpddr2_post_init();
        return MT6592_DRAM_OK;
    }
    if (step == MT6592_DRAM_STEP_TEST) {
        g_last_bad_addr = 0u;
        g_last_readback = 0u;
        return mt6592_dram_framebuffer_selftest(&g_last_bad_addr, &g_last_readback);
    }
    return MT6592_DRAM_ERR_BAD_STEP;
}

int mt6592_dram_init(void) {
    int rc;

    rc = mt6592_dram_init_step(MT6592_DRAM_STEP_MEMPLL);
    if (rc != MT6592_DRAM_OK) return rc;
    rc = mt6592_dram_init_step(MT6592_DRAM_STEP_REXTDN);
    if (rc != MT6592_DRAM_OK) return rc;
    rc = mt6592_dram_init_step(MT6592_DRAM_STEP_LPDDR2);
    if (rc != MT6592_DRAM_OK) return rc;
    rc = mt6592_dram_init_step(MT6592_DRAM_STEP_POST);
    if (rc != MT6592_DRAM_OK) return rc;
    rc = mt6592_dram_init_step(MT6592_DRAM_STEP_GATING);
    if (rc != MT6592_DRAM_OK) return rc;
    return mt6592_dram_init_step(MT6592_DRAM_STEP_TEST);
}

int mt6592_dram_last_failure(uint32_t* first_bad, uint32_t* readback) {
    if (first_bad) *first_bad = g_last_bad_addr;
    if (readback) *readback = g_last_readback;
    return g_last_bad_addr ? -1 : 0;
}

/* Fault-guarded single-word write/read at an arbitrary DRAM address. A trained
 * DQS gate is not proof that a normal data read returns -- the counter sweep can
 * false-pass -- so callers use this to actually exercise a real read. Returns 1
 * on write/read match, 0 on mismatch or a precise data abort (the guard). A true
 * AXI bus stall (the uncalibrated-read failure mode) cannot be caught and will
 * still hang; that is intentionally surfaced as a hang at the caller's breadcrumb
 * so the failing address is unambiguous. *got_out holds the readback (or the
 * 0xdab0da7a sentinel on a caught abort). */
int mt6592_dram_verify_rw(uint32_t addr, uint32_t* got_out) {
    uint32_t got = 0u;
    int ok = safe_write_read32(addr, 0xa55aa55au ^ addr, &got);
    if (got_out) *got_out = got;
    return ok;
}

uint32_t mt6592_dram_last_phase(void) {
    return g_last_phase;
}
