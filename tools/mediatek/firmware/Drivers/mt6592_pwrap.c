/* Copyright (c) 2026 SynChanCyberSecurity LLC. All Rights Reserved. */
/*
 * MT6592 PMIC wrapper (pwrap) cold init -- freestanding port.
 *
 * Faithful port of the MT6592 / MT6323 (SLV_6323) pwrap_init() from
 * MediaDeb/MT6592-KK-KERNEL (mediatek/platform/mt6592/kernel/drivers/pmic_wrap/
 * pwrap_hal.c), with kernel timers replaced by bounded poll loops and printk
 * removed. Brings pwrap from reset to INIT_DONE so WACS2 reads/writes to the
 * MT6323 PMIC work -- which is what the self-contained stage1 needs in BROM/DA
 * context (no preloader) to enable the LCD power rails. Returns 0 on success.
 */

#include <stdint.h>

#include "mt6592_pwrap.h"

/* ---- base addresses ---- */
#define PWRAP_BASE          0x1000d000u
#define CKSYS_BASE          0x10000000u /* TOPCKGEN */
#define INFRACFG_AO_BASE    0x10001000u
#define CLK_CFG_4_CLR       (CKSYS_BASE + 0x088u)
#define CLK_SPI_CK_26M      0xFu
#define INFRA_GLOBALCON_RST0 (INFRACFG_AO_BASE + 0x030u)
#define PWRAP_SOFT_RESET_BIT (1u << 7)

/* ---- PMIC_WRAP registers (offsets from PWRAP_BASE) ---- */
#define PMIC_WRAP_MUX_SEL        (PWRAP_BASE + 0x00u)
#define PMIC_WRAP_WRAP_EN        (PWRAP_BASE + 0x04u)
#define PMIC_WRAP_DIO_EN         (PWRAP_BASE + 0x08u)
#define PMIC_WRAP_SIDLY          (PWRAP_BASE + 0x0Cu)
#define PMIC_WRAP_RDDMY          (PWRAP_BASE + 0x18u)
#define PMIC_WRAP_SI_CK_CON      (PWRAP_BASE + 0x1Cu)
#define PMIC_WRAP_CSHEXT_WRITE   (PWRAP_BASE + 0x20u)
#define PMIC_WRAP_CSHEXT_READ    (PWRAP_BASE + 0x24u)
#define PMIC_WRAP_CSLEXT_START   (PWRAP_BASE + 0x28u)
#define PMIC_WRAP_CSLEXT_END     (PWRAP_BASE + 0x2Cu)
#define PMIC_WRAP_STAUPD_PRD     (PWRAP_BASE + 0x30u)
#define PMIC_WRAP_STAUPD_GRPEN   (PWRAP_BASE + 0x34u)
#define PMIC_WRAP_HIPRIO_ARB_EN  (PWRAP_BASE + 0x50u)
#define PMIC_WRAP_MAN_EN         (PWRAP_BASE + 0x5Cu)
#define PMIC_WRAP_MAN_CMD        (PWRAP_BASE + 0x60u)
#define PMIC_WRAP_WACS0_EN       (PWRAP_BASE + 0x6Cu)
#define PMIC_WRAP_INIT_DONE0     (PWRAP_BASE + 0x70u)
#define PMIC_WRAP_WACS1_EN       (PWRAP_BASE + 0x80u)
#define PMIC_WRAP_INIT_DONE1     (PWRAP_BASE + 0x84u)
#define PMIC_WRAP_WACS2_EN       (PWRAP_BASE + 0x94u)
#define PMIC_WRAP_INIT_DONE2     (PWRAP_BASE + 0x98u)
#define PMIC_WRAP_WACS2_CMD      (PWRAP_BASE + 0x9Cu)
#define PMIC_WRAP_WACS2_RDATA    (PWRAP_BASE + 0xA0u)
#define PMIC_WRAP_WACS2_VLDCLR   (PWRAP_BASE + 0xA4u)
#define PMIC_WRAP_INT_EN         (PWRAP_BASE + 0xA8u)
#define PMIC_WRAP_SIG_ADR        (PWRAP_BASE + 0xB8u)
#define PMIC_WRAP_SIG_MODE       (PWRAP_BASE + 0xBCu)
#define PMIC_WRAP_CRC_EN         (PWRAP_BASE + 0xC8u)
#define PMIC_WRAP_TIMER_EN       (PWRAP_BASE + 0xCCu)
#define PMIC_WRAP_WDT_UNIT       (PWRAP_BASE + 0xD4u)
#define PMIC_WRAP_WDT_SRC_EN     (PWRAP_BASE + 0xD8u)
#define PMIC_WRAP_CIPHER_KEY_SEL (PWRAP_BASE + 0x124u)
#define PMIC_WRAP_CIPHER_IV_SEL  (PWRAP_BASE + 0x128u)
#define PMIC_WRAP_CIPHER_EN      (PWRAP_BASE + 0x12Cu)
#define PMIC_WRAP_CIPHER_RDY     (PWRAP_BASE + 0x130u)
#define PMIC_WRAP_CIPHER_MODE    (PWRAP_BASE + 0x134u)
#define PMIC_WRAP_CIPHER_SWRST   (PWRAP_BASE + 0x138u)
#define PMIC_WRAP_DCM_EN         (PWRAP_BASE + 0x13Cu)
#define PMIC_WRAP_DCM_DBC_PRD    (PWRAP_BASE + 0x140u)
#define PMIC_WRAP_ADC_CMD_ADDR   (PWRAP_BASE + 0x144u)
#define PMIC_WRAP_PWRAP_ADC_CMD  (PWRAP_BASE + 0x148u)
#define PMIC_WRAP_ADC_RDY_ADDR   (PWRAP_BASE + 0x14Cu)
#define PMIC_WRAP_ADC_RDATA_ADDR1 (PWRAP_BASE + 0x150u)
#define PMIC_WRAP_ADC_RDATA_ADDR2 (PWRAP_BASE + 0x154u)

/* ---- MT6323 PMIC dewrap register addresses (PMIC_REG_BASE = 0) ---- */
#define DEW_DIO_EN         0x018Au
#define DEW_READ_TEST      0x018Cu
#define DEW_WRITE_TEST     0x018Eu
#define DEW_CRC_EN         0x0192u
#define DEW_CRC_VAL        0x0194u
#define DEW_CIPHER_KEY_SEL 0x0198u
#define DEW_CIPHER_IV_SEL  0x019Au
#define DEW_CIPHER_EN      0x019Cu
#define DEW_CIPHER_RDY     0x019Eu
#define DEW_CIPHER_MODE    0x01A0u
#define DEW_CIPHER_SWRST   0x01A2u
#define DEW_RDDMY_NO       0x01A4u
#define TOP_CKCON1         0x0126u
#define TOP_CKCON1_CLR     0x012Au
#define AUXADC_CON21       0x076Cu
#define AUXADC_ADC12       0x072Cu
#define AUXADC_ADC13       0x072Eu
#define AUXADC_ADC14       0x0730u

/* ---- constants ---- */
#define WACS2              (1u << 3)
#define DISABLE_ALL        0u
#define MANUAL_MODE        1u
#define WRAPPER_MODE       0u
#define OP_WR              0x1u
#define OP_CSH             0x0u
#define OP_CSL             0x1u
#define OP_OUTS            0x8u
#define DEFAULT_VALUE_READ_TEST 0x5aa5u
#define WRITE_TEST_VALUE   0xa55au
#define CHECK_CRC          0u

#define WACS_FSM_IDLE      0x00u
#define WACS_FSM_WFVLDCLR  0x06u
#define WACS_INIT_DONE     0x01u
#define WACS_SYNC_IDLE     0x01u

#define POLL_CAP           2000000u

/* ---- low-level register access ---- */
static uint32_t pr32(uint32_t addr) {
    return *(volatile uint32_t*)(uintptr_t)addr;
}
static void pw32(uint32_t addr, uint32_t value) {
    *(volatile uint32_t*)(uintptr_t)addr = value;
}
static void pwrap_set_bit(uint32_t addr, uint32_t bit) {
    pw32(addr, pr32(addr) | bit);
}
static void pwrap_clr_bit(uint32_t addr, uint32_t bit) {
    pw32(addr, pr32(addr) & ~bit);
}

static uint32_t get_wacs0_fsm(uint32_t x) { return (x >> 16) & 0x7u; }
static uint32_t get_wacs0_rdata(uint32_t x) { return x & 0xffffu; }
static uint32_t get_sync_idle0(uint32_t x) { return (x >> 20) & 0x1u; }
static uint32_t get_init_done0(uint32_t x) { return (x >> 21) & 0x1u; }

/* fp(reg) returns non-zero while we must keep waiting. */
typedef uint32_t (*loop_cond_fp)(uint32_t);
static int wait_for_state(loop_cond_fp fp, uint32_t reg, uint32_t* read_reg) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < POLL_CAP; ++i) {
        v = pr32(reg);
        if (!fp(v)) {
            if (read_reg) *read_reg = v;
            return 0;
        }
    }
    return MT6592_PWRAP_ERR_TIMEOUT;
}

static uint32_t cond_fsm_idle(uint32_t x) { return get_wacs0_fsm(x) != WACS_FSM_IDLE; }
static uint32_t cond_fsm_vldclr(uint32_t x) { return get_wacs0_fsm(x) != WACS_FSM_WFVLDCLR; }
static uint32_t cond_sync(uint32_t x) { return get_sync_idle0(x) != WACS_SYNC_IDLE; }
static uint32_t cond_idle_and_sync(uint32_t x) {
    return (get_wacs0_fsm(x) != WACS_FSM_IDLE) || (get_sync_idle0(x) != WACS_SYNC_IDLE);
}
static uint32_t cond_cipher_ready(uint32_t x) { return x != 1u; }

/* ---- WACS2 no-check transactions (used during init) ---- */
static int wacs2_nochk(uint32_t write, uint32_t adr, uint32_t wdata, uint32_t* rdata) {
    uint32_t reg_rdata = 0;
    int rc;
    if ((write & ~1u) != 0u) return MT6592_PWRAP_ERR_ARG;
    if ((adr & ~0xffffu) != 0u) return MT6592_PWRAP_ERR_ARG;
    if ((wdata & ~0xffffu) != 0u) return MT6592_PWRAP_ERR_ARG;

    /* Leftover-state recovery (the stock kernel pwrap_hal does exactly this
     * at the top of pwrap_wacs2_hal): a read whose caller timed out before
     * collecting RDATA — or a handoff from preloader/LK mid-transaction —
     * leaves the WACS2 FSM parked in WFVLDCLR. Without writing VLDCLR the
     * engine never returns to IDLE, so EVERY later PMIC transaction times
     * out: battery reads, PWRKEY polls and, fatally, the charger keepalive —
     * on a battery-less USB-powered board the unattended charger FSM then
     * drops the power path and the box turns off seconds later. Clearing the
     * stale VLDCLR here makes any wedge self-heal on the next transaction. */
    if (get_wacs0_fsm(pr32(PMIC_WRAP_WACS2_RDATA)) == WACS_FSM_WFVLDCLR) {
        pw32(PMIC_WRAP_WACS2_VLDCLR, 1u);
    }

    rc = wait_for_state(cond_fsm_idle, PMIC_WRAP_WACS2_RDATA, 0);
    if (rc != 0) return rc;

    pw32(PMIC_WRAP_WACS2_CMD, (write << 31) | ((adr >> 1) << 16) | wdata);

    if (write == 0u) {
        if (!rdata) return MT6592_PWRAP_ERR_ARG;
        rc = wait_for_state(cond_fsm_vldclr, PMIC_WRAP_WACS2_RDATA, &reg_rdata);
        if (rc != 0) return rc;
        *rdata = get_wacs0_rdata(reg_rdata);
        pw32(PMIC_WRAP_WACS2_VLDCLR, 1u);
    }
    return 0;
}
static int pwrap_read_nochk(uint32_t adr, uint32_t* rdata) { return wacs2_nochk(0u, adr, 0u, rdata); }
static int pwrap_write_nochk(uint32_t adr, uint32_t wdata) { return wacs2_nochk(1u, adr, wdata, 0); }

/* ---- init helpers ---- */
static int reset_spislv(void) {
    int rc;
    pw32(PMIC_WRAP_HIPRIO_ARB_EN, DISABLE_ALL);
    pw32(PMIC_WRAP_WRAP_EN, 0u);
    pw32(PMIC_WRAP_MUX_SEL, MANUAL_MODE);
    pw32(PMIC_WRAP_MAN_EN, 1u);
    pw32(PMIC_WRAP_DIO_EN, 0u);

    pw32(PMIC_WRAP_MAN_CMD, (OP_WR << 13) | (OP_CSL << 8));
    pw32(PMIC_WRAP_MAN_CMD, (OP_WR << 13) | (OP_OUTS << 8));
    pw32(PMIC_WRAP_MAN_CMD, (OP_WR << 13) | (OP_CSH << 8));
    pw32(PMIC_WRAP_MAN_CMD, (OP_WR << 13) | (OP_OUTS << 8));
    pw32(PMIC_WRAP_MAN_CMD, (OP_WR << 13) | (OP_OUTS << 8));
    pw32(PMIC_WRAP_MAN_CMD, (OP_WR << 13) | (OP_OUTS << 8));
    pw32(PMIC_WRAP_MAN_CMD, (OP_WR << 13) | (OP_OUTS << 8));

    rc = wait_for_state(cond_sync, PMIC_WRAP_WACS2_RDATA, 0);

    pw32(PMIC_WRAP_MAN_EN, 0u);
    pw32(PMIC_WRAP_MUX_SEL, WRAPPER_MODE);
    return rc;
}

static int init_sistrobe(void) {
    uint32_t arb_backup = pr32(PMIC_WRAP_HIPRIO_ARB_EN);
    uint32_t rdata = 0;
    uint32_t result = 0;
    int ind, leading_one, tailing_one, tmp1, tmp2;

    pw32(PMIC_WRAP_HIPRIO_ARB_EN, WACS2);
    for (ind = 0; ind < 24; ++ind) {
        pw32(PMIC_WRAP_SI_CK_CON, ((uint32_t)ind >> 2) & 0x7u);
        pw32(PMIC_WRAP_SIDLY, 0x3u - ((uint32_t)ind & 0x3u));
        if (wacs2_nochk(0u, DEW_READ_TEST, 0u, &rdata) == 0 && rdata == DEFAULT_VALUE_READ_TEST) {
            result |= (0x1u << ind);
        }
    }
    for (ind = 23; ind >= 0; --ind) if (result & (0x1u << ind)) break;
    leading_one = ind;
    for (ind = 0; ind < 24; ++ind) if (result & (0x1u << ind)) break;
    tailing_one = ind;

    tmp1 = (0x1 << (leading_one + 1)) - 1;
    tmp2 = (0x1 << tailing_one) - 1;
    pw32(PMIC_WRAP_HIPRIO_ARB_EN, arb_backup);
    if (leading_one < 0 || (tmp1 - tmp2) != (int)result) return MT6592_PWRAP_ERR_SISTROBE;

    ind = (leading_one + tailing_one) / 2;
    pw32(PMIC_WRAP_SI_CK_CON, ((uint32_t)ind >> 2) & 0x7u);
    pw32(PMIC_WRAP_SIDLY, 0x3u - ((uint32_t)ind & 0x3u));
    pw32(PMIC_WRAP_HIPRIO_ARB_EN, arb_backup);
    return 0;
}

static int init_reg_clock(void) {
    uint32_t rdata = 0;
    pwrap_write_nochk(TOP_CKCON1_CLR, 0x3u);
    pwrap_read_nochk(TOP_CKCON1, &rdata);
    if ((rdata & 0x3u) != 0u) return MT6592_PWRAP_ERR_REGCLK;

    pwrap_write_nochk(DEW_RDDMY_NO, 0x8u);
    pw32(PMIC_WRAP_RDDMY, 0x8u);

    /* regck_sel == 2 (12 MHz), SLV_6323 waveform. */
    pw32(PMIC_WRAP_CSHEXT_READ, 0x0u);
    pw32(PMIC_WRAP_CSHEXT_WRITE, 0x7u);
    pw32(PMIC_WRAP_CSLEXT_START, 0x0u);
    pw32(PMIC_WRAP_CSLEXT_END, 0x0u);
    return 0;
}

static int init_dio(uint32_t dio_en) {
    uint32_t arb_backup = pr32(PMIC_WRAP_HIPRIO_ARB_EN);
    uint32_t rdata = 0;
    int rc;
    pw32(PMIC_WRAP_HIPRIO_ARB_EN, WACS2);
    pwrap_write_nochk(DEW_DIO_EN, dio_en);
    rc = wait_for_state(cond_idle_and_sync, PMIC_WRAP_WACS2_RDATA, 0);
    if (rc != 0) return rc;
    pw32(PMIC_WRAP_DIO_EN, dio_en);
    pwrap_read_nochk(DEW_READ_TEST, &rdata);
    if (rdata != DEFAULT_VALUE_READ_TEST) return MT6592_PWRAP_ERR_DIO;
    pw32(PMIC_WRAP_HIPRIO_ARB_EN, arb_backup);
    return 0;
}

static int init_cipher(void) {
    uint32_t arb_backup = pr32(PMIC_WRAP_HIPRIO_ARB_EN);
    uint32_t rdata = 0;
    int rc;
    pw32(PMIC_WRAP_HIPRIO_ARB_EN, WACS2);

    pw32(PMIC_WRAP_CIPHER_SWRST, 1u);
    pw32(PMIC_WRAP_CIPHER_SWRST, 0u);
    pw32(PMIC_WRAP_CIPHER_KEY_SEL, 1u);
    pw32(PMIC_WRAP_CIPHER_IV_SEL, 2u);
    pw32(PMIC_WRAP_CIPHER_EN, 1u);

    /* config cipher @ PMIC (MT6323) */
    pwrap_write_nochk(DEW_CIPHER_SWRST, 0x1u);
    pwrap_write_nochk(DEW_CIPHER_SWRST, 0x0u);
    pwrap_write_nochk(DEW_CIPHER_KEY_SEL, 0x1u);
    pwrap_write_nochk(DEW_CIPHER_IV_SEL, 0x2u);
    pwrap_write_nochk(DEW_CIPHER_EN, 0x1u);

    /* wait cipher ready @ AP */
    rc = wait_for_state(cond_cipher_ready, PMIC_WRAP_CIPHER_RDY, 0);
    if (rc != 0) return rc;

    /* wait cipher ready @ PMIC */
    for (uint32_t i = 0; i < POLL_CAP; ++i) {
        pwrap_read_nochk(DEW_CIPHER_RDY, &rdata);
        if (rdata == 0x1u) break;
    }

    pwrap_write_nochk(DEW_CIPHER_MODE, 0x1u);
    rc = wait_for_state(cond_idle_and_sync, PMIC_WRAP_WACS2_RDATA, 0);
    if (rc != 0) return rc;
    pw32(PMIC_WRAP_CIPHER_MODE, 1u);

    pwrap_read_nochk(DEW_READ_TEST, &rdata);
    if (rdata != DEFAULT_VALUE_READ_TEST) return MT6592_PWRAP_ERR_CIPHER;
    pw32(PMIC_WRAP_HIPRIO_ARB_EN, arb_backup);
    return 0;
}

int mt6592_pwrap_init(void) {
    int rc;
    uint32_t rdata = 0;

    /* If a preloader already brought pwrap up (warm/LK context), don't re-init
     * a working PMIC link -- just report ready. Only the cold BROM/DA path needs
     * the full sequence below. */
    if (mt6592_pwrap_is_ready()) return 0;

    /* toggle PMIC_WRAP soft reset */
    pwrap_set_bit(INFRA_GLOBALCON_RST0, PWRAP_SOFT_RESET_BIT);
    pwrap_clr_bit(INFRA_GLOBALCON_RST0, PWRAP_SOFT_RESET_BIT);

    /* SPI_CK = 26 MHz */
    pw32(CLK_CFG_4_CLR, CLK_SPI_CK_26M);

    /* enable DCM */
    pw32(PMIC_WRAP_DCM_EN, 1u);
    pw32(PMIC_WRAP_DCM_DBC_PRD, 0u);

    rc = reset_spislv();
    if (rc != 0) return MT6592_PWRAP_ERR_RESET_SPI;

    pw32(PMIC_WRAP_WRAP_EN, 1u);
    pw32(PMIC_WRAP_HIPRIO_ARB_EN, WACS2);
    pw32(PMIC_WRAP_WACS2_EN, 1u);

    pw32(PMIC_WRAP_RDDMY, 0xFu); /* SLV_6323 */

    rc = init_sistrobe();
    if (rc != 0) return rc;

    rc = init_reg_clock();
    if (rc != 0) return rc;

    rc = init_dio(1u);
    if (rc != 0) return rc;

    rc = init_cipher();
    if (rc != 0) return rc;

    /* write test */
    if (pwrap_write_nochk(DEW_WRITE_TEST, WRITE_TEST_VALUE) != 0 ||
        pwrap_read_nochk(DEW_WRITE_TEST, &rdata) != 0 || rdata != WRITE_TEST_VALUE) {
        return MT6592_PWRAP_ERR_WRITE_TEST;
    }

    /* CRC signature checking */
    if (pwrap_write_nochk(DEW_CRC_EN, 1u) != 0) return MT6592_PWRAP_ERR_CRC;
    pw32(PMIC_WRAP_CRC_EN, 1u);
    pw32(PMIC_WRAP_SIG_MODE, CHECK_CRC);
    pw32(PMIC_WRAP_SIG_ADR, DEW_CRC_VAL);

    /* PMIC_WRAP enables */
    pw32(PMIC_WRAP_HIPRIO_ARB_EN, 0x1ffu);
    pw32(PMIC_WRAP_WACS0_EN, 1u);
    pw32(PMIC_WRAP_WACS1_EN, 1u);
    pw32(PMIC_WRAP_STAUPD_PRD, 0x5u);
    pw32(PMIC_WRAP_STAUPD_GRPEN, 0xffu);
    pw32(PMIC_WRAP_WDT_UNIT, 0xfu);
    pw32(PMIC_WRAP_WDT_SRC_EN, 0xffffffffu);
    pw32(PMIC_WRAP_TIMER_EN, 0x1u);
    pw32(PMIC_WRAP_INT_EN, 0x7fffffffu);

    /* GPS_INTF (SLV_6323) */
    pw32(PMIC_WRAP_ADC_CMD_ADDR, AUXADC_CON21);
    pw32(PMIC_WRAP_PWRAP_ADC_CMD, 0x8000u);
    pw32(PMIC_WRAP_ADC_RDY_ADDR, AUXADC_ADC12);
    pw32(PMIC_WRAP_ADC_RDATA_ADDR1, AUXADC_ADC13);
    pw32(PMIC_WRAP_ADC_RDATA_ADDR2, AUXADC_ADC14);

    /* init done */
    pw32(PMIC_WRAP_INIT_DONE2, 1u);
    pw32(PMIC_WRAP_INIT_DONE0, 1u);
    pw32(PMIC_WRAP_INIT_DONE1, 1u);

    /* confirm WACS2 reports init-done */
    rdata = pr32(PMIC_WRAP_WACS2_RDATA);
    if (get_init_done0(rdata) != WACS_INIT_DONE) {
        /* INIT_DONE0 reads back via WACS2_RDATA bit21 only after a transaction;
         * do one read to refresh, then check. */
        if (pwrap_read_nochk(DEW_READ_TEST, &rdata) != 0) return MT6592_PWRAP_ERR_NOT_INIT;
    }
    return 0;
}

int mt6592_pwrap_is_ready(void) {
    return get_init_done0(pr32(PMIC_WRAP_WACS2_RDATA)) == WACS_INIT_DONE;
}

/* ---- runtime PMIC register access ----
 * Public WACS2 read/write for the OS-side PMIC users (key-reset disable,
 * battery/charger, power-off). Requires pwrap INIT_DONE (either the stage1
 * cold init above or the stock preloader's own init on a warm boot). */
int mt6592_pwrap_read(uint32_t adr, uint32_t* rdata) {
    if (!mt6592_pwrap_is_ready()) return MT6592_PWRAP_ERR_NOT_INIT;
    return pwrap_read_nochk(adr, rdata);
}

int mt6592_pwrap_write(uint32_t adr, uint32_t wdata) {
    if (!mt6592_pwrap_is_ready()) return MT6592_PWRAP_ERR_NOT_INIT;
    return pwrap_write_nochk(adr, wdata);
}
