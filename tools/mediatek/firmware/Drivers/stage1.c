/*
 * Virtua stage1 - stock-LK boot.img loader + OS hand-off.
 *
 * This is the boot.img "kernel" the STOCK MediaTek Little Kernel loads and
 * jumps to.  The stock LK has already powered the panel and is scanning out
 * its boot-logo framebuffer via OVL0.
 *
 * Stage1 stays small. The Android boot image carries MVII.bin separately as
 * the raw ramdisk payload, loaded where MVII is linked to run.
 */
#include <stdint.h>

#include "mt6592_battery_curve.h"
#include "mt6592_bootinfo.h"
#include "mt6592_delay.h"
#include "mt6592_pwrap.h"
#include "external_power.h"
#include "mvii_assets.h"
#include "stage1.h"

/*
 * Persistent stage1 progress markers (boot.img flow only): three 512-byte
 * status-sector writes so `flash -mtk-read-boot-status` can localise a boot
 * failure to "LK never ran us" / "died in splash or ramdisk locate" /
 * "handed off, crash is in the OS". Off for the LK-slot and flash-payload
 * builds, which have their own status flows / must not touch eMMC.
 */
#ifndef MVII_MT6592_STAGE1_STATUS_MARKS
#define MVII_MT6592_STAGE1_STATUS_MARKS 0
#endif
#if MVII_MT6592_STAGE1_STATUS_MARKS
#include "mt6592_bootstatus.h"
#define STAGE1_MARK(stage, msg)                    \
    do {                                           \
        mt6592_bootstatus_set_stage((stage));      \
        mt6592_bootstatus_set_message((msg));      \
        (void)mt6592_bootstatus_flush();           \
    } while (0)
#else
#define STAGE1_MARK(stage, msg) do { } while (0)
#endif

/* Freestanding copy into the live framebuffer (no libc in the loader).
 *
 * Word-at-a-time wherever alignment allows, and that is not a micro-optimisation
 * here. Stage1 runs with the D-cache off, so every store goes straight out to
 * DRAM with nothing to coalesce it, and `dst` is volatile, so the compiler is
 * forbidden from widening the accesses itself. The byte loop this replaces
 * therefore issued 1,228,800 separate uncached byte stores for one 640x480 BGRA
 * frame -- into a buffer the display controller is scanning out the entire time.
 *
 * That is what the logo cascading in from the top actually was. Not a slow
 * decode and not a slow panel: the copy simply lost the race with the refresh,
 * so the screen showed the boundary travelling down. Four bytes per store cuts
 * it to 307,200 and the frame lands quickly enough to appear at once.
 */
static void s1_memcpy(volatile uint8_t *dst, const uint8_t *src, uint32_t n)
{
    uint32_t i = 0u;

    if ((((uintptr_t)dst | (uintptr_t)src) & 3u) == 0u) {
        volatile uint32_t *d32 = (volatile uint32_t *)dst;
        const uint32_t *s32 = (const uint32_t *)src;
        const uint32_t words = n >> 2;
        for (; i < words; ++i) d32[i] = s32[i];
        i <<= 2;
    }
    for (; i < n; ++i) dst[i] = src[i];
}

/* ------------------------------------------------------------------ */
/* Exception-handshake globals (unchanged)                            */
/* ------------------------------------------------------------------ */
extern volatile uint32_t mt6592_stage1_abort_flag;
extern volatile uint32_t mt6592_stage1_abort_resume;

/* ------------------------------------------------------------------ */
/* MT6592 registers (unchanged)                                       */
/* ------------------------------------------------------------------ */
#define OVL0_BASE        0x14007000u
#define OVL_EN           0x000cu
#define OVL_ROI_SIZE     0x0020u
#define OVL_SRC_CON      0x002cu
#define OVL_L0_ADDR      0x0040u
#define OVL_L0_PITCH     0x0044u
#define OVL_LAYER_STRIDE 0x0020u

#define WDT_BASE         0x10007000u
#define WDT_MODE         0x0000u
#define WDT_RESTART      0x0008u
#define WDT_MODE_KEY     0x22000000u
#define WDT_RESTART_KEY  0x00001971u
#define TOPRGU_BASE      0x10000500u

#define DRAM_BASE        0x80000000u
#define DRAM_END         0xc0000000ull

#ifndef FALLBACK_FB_ADDR
#define FALLBACK_FB_ADDR 0x82700000u
#endif
#ifndef PANEL_W
#define PANEL_W          640u
#endif
#ifndef PANEL_H
#define PANEL_H          480u
#endif

#define BIT(n) (1u << (n))

enum {
    /* Kept in step with mt6592_pmic.c by hand -- stage1 links against almost
     * nothing, so it cannot include that header. See the long provenance note
     * there: 0x0758 bit 4 starts a conversion, 0x076e bit 7 strobes it, and the
     * result is a 12-bit field, not the 15-bit one this file used to read. */
    PMIC_AUXADC_RQST0 = 0x0758u,
    PMIC_AUXADC_RQST0_START = BIT(4),
    PMIC_AUXADC_CON = 0x076eu,
    PMIC_AUXADC_CON_FIELD = 0x01ffu,
    PMIC_AUXADC_CON_STROBE = BIT(7),
    PMIC_AUXADC_ADC0 = 0x0714u,
    /* The other half of the shunt. BATSNS (channel 7, data 0x0714) sits on the
     * cell side of CUST_R_SENSE and I_SENSE (channel 6, data 0x0716) on the
     * charger side; their difference over 68 mOhm is the charge current, and the
     * gauge needs it to turn a terminal voltage into an open-circuit one. Same
     * registers and same channel numbering mt6592_pmic.c uses -- kept in step by
     * hand, as everything in this enum is, because stage1 cannot include it. */
    PMIC_AUXADC_BATSNS_CHANNEL = 7u,
    PMIC_AUXADC_ISENSE_CHANNEL = 6u,
    PMIC_AUXADC_ISENSE_DATA = 0x0716u,
    /* cust +0x140, CUST_R_SENSE, `mov r0,#68' at 0xc060fc60. */
    STAGE1_R_SENSE_MOHM = 68,
    /* Stock's g_Get_I_Charging @ 0xC061203C: twenty pairs, sorted, two trimmed
     * from each end, the middle sixteen meaned by `asr #4'. */
    STAGE1_SENSE_SAMPLES = 20,
    STAGE1_SENSE_TRIM = 2,
    PMIC_AUXADC_READY = BIT(15),
    /* 15-bit field, 2^15 scale -- what this file had originally. See the note in
     * mt6592_pmic.c: hardware reads 0x0714=0xca41, so bit 14 of the value is set
     * and the 12-bit narrowing was wrong. */
    PMIC_AUXADC_VALUE_MASK = 0x7fffu,
    PMIC_AUXADC_VALUE_BITS = 15u,
    PMIC_AUXADC_FULL_SCALE_MV = 7200u,
    /* One conversion, waited out unconditionally after the strobe, and the
     * bounded backstop poll after that. Both are mt6592_pmic.c's numbers
     * (AUXADC_CONVERSION_US / AUXADC_POLL_LIMIT) and must stay its numbers: the
     * two files sample the same converter and hand their answers to the same
     * gauge. */
    PMIC_AUXADC_CONVERSION_US = 150u,
    PMIC_AUXADC_POLL_LIMIT = 256u,
    PMIC_CHR_CON0 = 0x0000u,
    PMIC_CHR_CON0_CSDAC_EN = BIT(3),
    PMIC_CHR_CON0_CHR_EN = BIT(4),
    PMIC_CHR_CON0_CHRDET = BIT(5),
    PMIC_CHR_CON2 = 0x0004u,
    PMIC_CHR_CON2_VBAT_CV_EN = BIT(1),
    PMIC_CHR_CON2_CS_EN = BIT(3),
    PMIC_CHR_CON4 = 0x0008u,
    PMIC_CHR_CON13 = 0x001au,
    PMIC_CHR_CON23 = 0x002eu,
    PMIC_CHR_CON23_CSDAC_MODE = BIT(2),
    PMIC_CHR_CON23_HWCV_EN = BIT(6),

    /* Status aggregate.
     *   bit 1  PWRKEY_DEB  -- 1 while the key is RELEASED, 0 while it is held.
     *                         This is how the charge park below notices someone
     *                         wants the OS, and it is the only field here this
     *                         file still decides anything on.
     *   bit 5  was read as RO_BATON_UNDET and is no longer believed. That name
     *                         came from a vendor header, not from the image on
     *                         disk: the stock kernel carries exactly one "baton"
     *                         string in the whole binary and it belongs to the
     *                         interrupt handler, while the one routine whose job
     *                         is this question -- CHARGING_CMD_GET_BATTERY_STATUS,
     *                         see CHR_CON7 below -- reads 0x0e bit 12 and never
     *                         touches 0x142. 0x142 is also where this port reads
     *                         the power and home keys, which is not the company a
     *                         connector comparator keeps. Still logged raw every
     *                         charger boot, because a boot line is free and a
     *                         disagreement with bit 12 would be worth seeing. */
    PMIC_CHRSTATUS = 0x0142u,
    PMIC_CHRSTATUS_PWRKEY_DEB = BIT(1),
    PMIC_CHRSTATUS_BATON_UNDET = BIT(5),

    /* BATON: the battery-connector comparator, and the only presence signal on
     * this board that looks at the connector instead of at VBAT. There is no
     * power-path FET here -- the charger output IS the VBAT/VSYS node -- so any
     * presence test built on the battery voltage answers "fitted" off the
     * charger alone, which is a structural failure, not a tuning problem.
     *
     * BATON_EN gates the comparator. Nothing in this tree had ever set it, which
     * is the most likely reason the earlier RGS_BATON_UNDET attempt read
     * "fitted" on an empty connector: an un-enabled comparator has no verdict to
     * give. It is enabled and allowed to settle before either bit is believed,
     * and both bits are published raw on every charger boot so the readback
     * settles which of the two the MT6322 actually drives.
     *
     * AND BATON_EN WAS NOT THE WHOLE ENABLE. The vendor kernel's
     * CHARGING_CMD_GET_BATTERY_STATUS -- charging_func[11] at 0xc05141b8, the one
     * function on the part whose entire job is this question -- writes bit 2
     * BEFORE bit 0 and then reads bit 12:
     *
     *   pmic_config_interface(0x0e, 1, 1, 2)   <- never done here until now
     *   pmic_config_interface(0x0e, 1, 1, 0)   <- BATON_EN, what this file did
     *   pmic_read_interface  (0x0e, &v, 1, 12) <- RGS_BATON_UNDET
     *
     * So the reasoning above was right about the mechanism and one write short of
     * acting on it: enabling half an enable leaves the comparator's front end off,
     * and an off comparator answers stably, which is exactly what "reads fitted on
     * an empty connector" looked like. Bit 2's NAME is inferred from MT6323's
     * field order (RG_BATON_TDET_EN); the write order is transcribed instruction
     * by instruction from the vendor image and does not depend on the name. */
    PMIC_CHR_CON7 = 0x000eu,
    PMIC_CHR_CON7_BATON_EN = BIT(0),
    PMIC_CHR_CON7_BATON_HT_EN = BIT(1),
    PMIC_CHR_CON7_BATON_TDET_EN = BIT(2),
    PMIC_CHR_CON7_RGS_BATON_UNDET = BIT(12),
    /* pwrap round trips are slow; this is comfortably past the comparator's
     * settling time and still invisible next to the two-second splash pause. */
    STAGE1_BATON_SETTLE_CYCLES = 200000u,

    STAGE1_CHARGE_IDLE = 0u,
    STAGE1_CHARGE_ARMED = 1u,
    STAGE1_CHARGE_PWRAP_NOT_READY = 2u,
    STAGE1_CHARGE_READ_FAILED = 3u,
    STAGE1_CHARGE_NO_VBUS = 4u,
    STAGE1_CHARGE_WRITE_FAILED = 5u,
};

/*
 * SoC AUXADC — the analog-stick converter. Unrelated to the PMIC AUXADC above,
 * which is reached over pwrap and only carries BATSNS. Register layout and the
 * stop/settle/start/read sequence are the vendor ones, mirrored from
 * the OS keypad driver so both samplers see the same converter in the same state.
 */
enum {
    SOC_AUXADC_BASE = 0x11001000u,
    SOC_AUXADC_CON1_SET = 0x0008u,
    SOC_AUXADC_CON1_CLR = 0x000cu,
    SOC_AUXADC_CON2 = 0x0010u,
    SOC_AUXADC_CON2_BUSY = BIT(0),
    SOC_AUXADC_DAT0 = 0x0014u,
    SOC_AUXADC_DAT_STRIDE = 0x0004u,
    SOC_AUXADC_DAT_READY = BIT(12),
    SOC_AUXADC_DAT_MASK = 0x0fffu,
    SOC_AUXADC_SETTLE_CYCLES = 850u,
    SOC_AUXADC_POLL_LIMIT = 96u,

    STAGE1_PERICFG_BASE = 0x10003000u,
    STAGE1_PERI_PDN0_CLR = 0x0010u,
    STAGE1_PERI_PDN0_AUXADC_BITS = 0x0ff00000u, /* bits 20..27 cover AUXADC + neighbours */

    /* The first conversions after a clock ungate read low. Throw them away:
     * that cold bias is exactly what the OS-side lazy seeding used to lock in
     * as "center", and it is the whole reason the stick rested off-axis. */
    STAGE1_JOY_DISCARD_SAMPLES = 6u,
    STAGE1_JOY_AVERAGE_SAMPLES = 12u,
    /* Sanity window on a 12-bit result, wide on purpose. This rejects "the ADC
     * is not powered / not answering" (rails at 0 or 0xfff), not "the stick is
     * deflected" — a deflected stick is still the best center estimate we have,
     * and stage1 has no way to tell deflection from a mechanically off-centre
     * rest position. */
    STAGE1_JOY_CENTER_MIN = 400u,
    STAGE1_JOY_CENTER_MAX = 3800u,
};

/* Captured before the splash, published into the handoff record at the very
 * end (mt6592_bootinfo_init() zeroes that record, so it cannot be written
 * directly until build_os_handoff() has run). */
static uint32_t g_stage1_joy_center[MT6592_BOOT_JOY_CHANNEL_COUNT];
static int      g_stage1_joy_center_valid;


/*
 * Survival hold: VSYS on this PMIC IS the battery rail (no power path), so a
 * deeply discharged cell browns the box out the moment the dashboard loads
 * up — the PMIC hard-resets and the boot loops forever. The only workable
 * moment to fix that is here, at the splash, where the load is minimal.
 *
 * VBAT is sampled WHILE CHARGING (the charger is never paused to measure:
 * with no battery fitted it may be the only thing holding the rail), so the
 * floor is set against the under-charge terminal voltage. The J36 3000 mAh
 * pack on this unit tops out around 3.8 V and cuts off around 3.2 V; 20% of
 * that usable cell window is about 3.32 V, so the hold only covers the bottom
 * instead of waiting for a near-nominal/full pack. A failed/absurd read means
 * "no battery": boot immediately on USB power. Not a battery dependency —
 * batteryless and no-VBUS boots are never held.
 */
#ifndef MVII_MT6592_STAGE1_CHARGE_HOLD
#define MVII_MT6592_STAGE1_CHARGE_HOLD 1
#endif
#ifndef MVII_MT6592_STAGE1_VBAT_SURVIVAL_MV
#define MVII_MT6592_STAGE1_VBAT_SURVIVAL_MV 3320
#endif
#ifndef MVII_MT6592_STAGE1_VBAT_SURVIVAL_EXIT_MV
#define MVII_MT6592_STAGE1_VBAT_SURVIVAL_EXIT_MV 3350
#endif
#ifndef MVII_MT6592_STAGE1_CHARGE_HOLD_MAX_SECONDS
#define MVII_MT6592_STAGE1_CHARGE_HOLD_MAX_SECONDS 180
#endif

/*
 * Charger park: do not auto-start the OS when the box was only woken to charge.
 *
 * Plugging the cable into a powered-off device makes the PMIC power the SoC up
 * so LK can draw its battery symbol; the stock flow paints that symbol and
 * stops. MVII, being the kernel LK loads, was booting the whole OS instead, so
 * "plug in to charge" turned into "the device switched itself on".
 *
 * LK tells us which of the two it is: it passes MediaTek's private boot-mode
 * ATAG (see ATAG_MTK_BOOT below) in the tag list. When the mode is one of the
 * charging modes AND VBUS is actually present, stage1 stays where it is —
 * charger armed, watchdog held off, LK's battery symbol left untouched on the
 * panel — instead of painting a splash and handing off.
 *
 * Deliberately conservative in three ways, because the numeric mode values are
 * the standard MediaTek enum and could not be re-confirmed against this exact
 * LK build:
 *   - the raw mode is logged on EVERY boot before it is acted on, so one
 *     `flash -mtk-read-boot-status` readback settles the enum;
 *   - CHRDET must corroborate the mode, so a misread cannot park a normal boot
 *     (a power-button boot has no cable, or has one and behaves as before);
 *   - parking never powers the device off, so a misdetection is always
 *     recoverable by pulling the cable and pressing power. The PMIC's hardware
 *     long-press still works throughout.
 */
#ifndef MVII_MT6592_STAGE1_CHARGE_PARK
#define MVII_MT6592_STAGE1_CHARGE_PARK 1
#endif

#ifndef MVII_ARM_SECOND_STAGE_ADDR
#define MVII_ARM_SECOND_STAGE_ADDR 0x84000000u
#endif
#ifndef MVII_ARM_OS_HANDOFF_ADDR
#define MVII_ARM_OS_HANDOFF_ADDR MVII_ARM_SECOND_STAGE_ADDR
#endif

typedef struct {
    uint32_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp_bytes;
} stage1_framebuffer_t;

static inline uint32_t rd32(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline void     wr32(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }

static stage1_sink_fn g_sink = 0;
static void          *g_sink_ctx = 0;

void mvii_arm_stage1_set_sink(stage1_sink_fn sink, void *ctx)
{
    g_sink = sink;
    g_sink_ctx = ctx;
}

/* Watchdog disable (unchanged) */
static void wdt_off(void)
{
    uint32_t mode   = rd32(WDT_BASE + WDT_MODE);
    uint32_t toprgu = rd32(TOPRGU_BASE);
    wr32(WDT_BASE + WDT_MODE, (mode & ~0x4fu) | WDT_MODE_KEY);
    wr32(TOPRGU_BASE, (toprgu & ~1u) | WDT_MODE_KEY);
    wr32(WDT_BASE + WDT_RESTART, WDT_RESTART_KEY);
}

static int stage1_pwrap_ready(void)
{
    /* Run (or confirm) pwrap init exactly once; mt6592_pwrap_init() early-outs
     * in the warm/LK context, and the per-kick path below must stay cheap. */
    static int init_attempted;
    if (!init_attempted) {
        init_attempted = 1;
        (void)mt6592_pwrap_init();
    }
    return mt6592_pwrap_is_ready();
}

static uint32_t g_stage1_charge_last_result = STAGE1_CHARGE_IDLE;

/*
 * Latent USB charger arm. Battery-agnostic by design: the only gate is VBUS
 * presence (CHRDET) — with a battery fitted the PMIC charges it, without one
 * the box simply runs from USB power. Never blocks and never decides boot.
 *
 * Re-issued continuously (once per delay_seconds() tick and from the park
 * loop) because the PMIC charger watchdog the stock preloader leaves armed
 * drops CHR_EN if the FSM goes unattended; a periodic full re-arm bounds any
 * watchdog trip to a ~1 s charging pause instead of a permanent stop.
 */
static int stage1_arm_usb_charger(void)
{
    uint32_t con0 = 0u;
    uint32_t con2 = 0u;
    uint32_t con23 = 0u;

    /* Rewriting CS_VTH / CV-enable / CHR_EN is the sequence that drops a
     * cell-less rail. Preserve the charger mode and disable its watchdog
     * before a transfer or kernel handoff can outlast the service loop. */
    if (MVII_WITHOUT_BATTERY) {
        if (!stage1_pwrap_ready())
            g_stage1_charge_last_result = STAGE1_CHARGE_PWRAP_NOT_READY;
        else if (external_power_hold() != 0)
            g_stage1_charge_last_result = STAGE1_CHARGE_WRITE_FAILED;
        else
            g_stage1_charge_last_result = STAGE1_CHARGE_IDLE;
        return 0;
    }

    if (!stage1_pwrap_ready()) {
        g_stage1_charge_last_result = STAGE1_CHARGE_PWRAP_NOT_READY;
        return 0;
    }
    if (mt6592_pwrap_read(PMIC_CHR_CON0, &con0) != 0) {
        g_stage1_charge_last_result = STAGE1_CHARGE_READ_FAILED;
        return 0;
    }
    if ((con0 & PMIC_CHR_CON0_CHRDET) == 0u) {
        g_stage1_charge_last_result = STAGE1_CHARGE_NO_VBUS;
        return 0;
    }

    if (mt6592_pwrap_write(PMIC_CHR_CON13, 0x0000u) != 0 ||   /* charger WDT off/kick */
        mt6592_pwrap_write(PMIC_CHR_CON4, 0x000cu) != 0 ||    /* CS_VTH ~450 mA       */
        mt6592_pwrap_read(PMIC_CHR_CON2, &con2) != 0 ||
        mt6592_pwrap_write(PMIC_CHR_CON2, con2 | PMIC_CHR_CON2_VBAT_CV_EN | PMIC_CHR_CON2_CS_EN) != 0 ||
        mt6592_pwrap_read(PMIC_CHR_CON23, &con23) != 0 ||
        mt6592_pwrap_write(PMIC_CHR_CON23, con23 | PMIC_CHR_CON23_CSDAC_MODE | PMIC_CHR_CON23_HWCV_EN) != 0 ||
        mt6592_pwrap_write(PMIC_CHR_CON0, con0 | PMIC_CHR_CON0_CSDAC_EN | PMIC_CHR_CON0_CHR_EN) != 0) {
        g_stage1_charge_last_result = STAGE1_CHARGE_WRITE_FAILED;
        return 0;
    }
    g_stage1_charge_last_result = STAGE1_CHARGE_ARMED;
    return 1;
}

/* BATSNS via PMIC AUXADC; VBAT[mV] = raw / 4096 * 1800 mV * 4. Reads the
 * under-charge terminal voltage when the charger is armed.
 *
 * Same stop -> wait-for-clear -> settle -> start -> wait-for-new-ready sequence
 * as mt6592_pmic.c (and as stage1_soc_auxadc_read below); the long note lives
 * there. Short version: READY is still set from the previous conversion when a
 * new one is requested, so without the stop half the poll returns a stale
 * sample immediately.
 *
 * Unconditional (it used to sit inside the survival-hold switch) because the
 * battery percentage published into the handoff record needs it on every boot,
 * charge hold or not. */
static int stage1_auxadc_read_raw(uint32_t channel, uint32_t data_reg, uint32_t *out_raw)
{
    uint32_t raw = 0u;
    uint32_t con = 0u;

    if (!out_raw) return -1;

    /*
     * ── ASK FOR A CONVERSION FIRST. ALWAYS. ──
     *
     * This used to open by reading the data register and returning immediately
     * if READY was set, on the reading that the converter free-runs and READY
     * means "this channel has a current value". The reading was right and the
     * shortcut built on it was wrong, and mt6592_pmic.c proved it on hardware:
     * a 64-conversion window reported a peak-to-peak spread of EXACTLY ZERO
     * counts while the same value moved thirty counts between two reads a
     * second apart. READY here is STICKY -- set once and never observed clear
     * -- so the early-out was taken on every call after the first and nothing
     * ever requested another conversion. Every average built on it was an
     * average of one stale number.
     *
     * That mattered little while this file took a single BATSNS reading. It
     * matters now: stage1_charge_current_ma() below takes twenty pairs and
     * trims them, and with the shortcut in place those twenty pairs were twenty
     * copies of one latched value on each channel -- a trimmed mean of a
     * constant, and on I_SENSE a constant left there by whoever last kicked the
     * hardware, which may well have been the LK a boot.img load ago.
     *
     * So the request and the strobe are unconditional, exactly as in
     * mt6592_pmic.c's adc_step(), and the conversion is waited out before the
     * register is believed. Both halves are read-modify-write so no other
     * channel's request bit is disturbed. The difference is only in who waits:
     * the driver's version is a state machine that returns between the strobe and
     * the collect, because it runs on the frame path; this one blocks, because
     * stage1 has nothing else to do.
     */
    wdt_off();
    if (mt6592_pwrap_read(PMIC_AUXADC_RQST0, &con) != 0) return -1;
    if (mt6592_pwrap_write(PMIC_AUXADC_RQST0, con | PMIC_AUXADC_RQST0_START) != 0) return -1;
    if (mt6592_pwrap_read(PMIC_AUXADC_CON, &con) != 0) return -1;
    con &= PMIC_AUXADC_CON_FIELD;
    if (mt6592_pwrap_write(PMIC_AUXADC_CON, con & ~(uint32_t)BIT(channel)) != 0) return -1;
    if (mt6592_pwrap_read(PMIC_AUXADC_CON, &con) != 0) return -1;
    con &= PMIC_AUXADC_CON_FIELD;
    if (mt6592_pwrap_write(PMIC_AUXADC_CON, con | (uint32_t)BIT(channel)) != 0) return -1;

    /* Let the conversion happen before looking. With READY sticky the poll below
     * cannot tell a finished conversion from the value that was already sitting
     * there, so this wait is the only thing standing between the strobe and a
     * stale read -- the difference between sampling the converter and re-reading
     * its output latch. The poll after it is the bounded backstop for a channel
     * that has never converted at all, which is why it is 256 reads and not the
     * 2000 it used to be: 2000 pwrap round trips is tens of milliseconds with
     * nothing servicing the charger, and on a cell-less board that rail has
     * nothing else holding it up. */
    mt6592_delay_cycles(PMIC_AUXADC_CONVERSION_US * 33u);

    for (uint32_t i = 0; i < PMIC_AUXADC_POLL_LIMIT; ++i) {
        if ((i & 0x3fu) == 0u) wdt_off();
        if (mt6592_pwrap_read(data_reg, &raw) != 0) return -1;
        if ((raw & PMIC_AUXADC_READY) != 0u) break;
    }
    if ((raw & PMIC_AUXADC_READY) == 0u) return -1;
    *out_raw = raw & PMIC_AUXADC_VALUE_MASK;
    return 0;
}

static int stage1_auxadc_read_batsns_mv(int *out_mv)
{
    uint32_t raw = 0u;

    if (!out_mv) return -1;
    if (stage1_auxadc_read_raw(PMIC_AUXADC_BATSNS_CHANNEL, PMIC_AUXADC_ADC0, &raw) != 0)
        return -1;

    *out_mv = (int)(((uint64_t)raw * (uint64_t)PMIC_AUXADC_FULL_SCALE_MV) >>
                    PMIC_AUXADC_VALUE_BITS);
    return 0;
}

static void stage1_sense_insert(int *a, int n, int v)
{
    int i = n;
    while (i > 0 && a[i - 1] > v) {
        a[i] = a[i - 1];
        --i;
    }
    a[i] = v;
}

/*
 * THE CHARGE CURRENT, IN MILLIAMPS, THE SAME WAY `bat' MEASURES IT.
 *
 * I_SENSE and BATSNS straddle the 68 mOhm shunt, so the difference between them
 * is the drop the charge current develops across it. Twenty pairs taken back to
 * back -- BATSNS then I_SENSE, one immediately after the other, never all of one
 * channel and then all of the other, because the two conversions have to straddle
 * the same ripple or the phase difference lands in the subtraction and the
 * subtraction IS the measurement. Sorted, two trimmed from each end, the middle
 * sixteen meaned. That is stock's g_Get_I_Charging and it is what mt6592_pmic.c's
 * mt6592_pmic_sense_pair() does; the two must not drift, because LK and the OS
 * both feed the answer into the same gauge.
 *
 * Stock's zero test is kept verbatim: I_SENSE at or below BATSNS is no current.
 * The shunt cannot develop a negative drop in the charge direction, so a
 * non-positive difference is the pair's own offset showing through, and zero is
 * the honest report -- which the gauge reads as "apply no correction".
 *
 * ── AND IT RE-ARMS THE CHARGER AS IT GOES ──
 *
 * Forty real conversions is about six milliseconds, and six milliseconds is a
 * long time to leave the PMIC charger FSM unattended on THIS board: VBAT is
 * VSYS, there is no power path, and with no cell fitted the charger block is
 * the only thing holding the rail up at all. Every other stretch in this file
 * that takes time re-arms while it waits -- delay_seconds() does it once a
 * second, both park loops do it every 64k spins -- and this one did not, which
 * made it the one unattended gap in the boot path and put it between the LK's
 * last service call and stage1's first.
 *
 * So it re-arms every fourth pair. The cost is four extra pwrap writes per pair
 * group and it cannot change the measurement: the arm touches the charger
 * control registers, never the converter.
 *
 * Returns 0 and a current, or -1 if either channel would not convert.
 */
enum { STAGE1_SENSE_REARM_EVERY = 4 };

static int stage1_charge_current_ma(int *out_ma)
{
    int bat_mv[STAGE1_SENSE_SAMPLES];
    int isen_mv[STAGE1_SENSE_SAMPLES];
    int bat_sum = 0;
    int isen_sum = 0;
    int i;

    if (!out_ma) return -1;

    for (i = 0; i < STAGE1_SENSE_SAMPLES; ++i) {
        uint32_t bat = 0u;
        uint32_t isen = 0u;

        if ((i % STAGE1_SENSE_REARM_EVERY) == 0) (void)stage1_arm_usb_charger();

        if (stage1_auxadc_read_raw(PMIC_AUXADC_BATSNS_CHANNEL, PMIC_AUXADC_ADC0, &bat) != 0)
            return -1;
        if (stage1_auxadc_read_raw(PMIC_AUXADC_ISENSE_CHANNEL, PMIC_AUXADC_ISENSE_DATA,
                                   &isen) != 0)
            return -1;

        stage1_sense_insert(bat_mv, i,
                            (int)(((uint64_t)bat * (uint64_t)PMIC_AUXADC_FULL_SCALE_MV) >>
                                  PMIC_AUXADC_VALUE_BITS));
        stage1_sense_insert(isen_mv, i,
                            (int)(((uint64_t)isen * (uint64_t)PMIC_AUXADC_FULL_SCALE_MV) >>
                                  PMIC_AUXADC_VALUE_BITS));
    }

    for (i = STAGE1_SENSE_TRIM; i < STAGE1_SENSE_SAMPLES - STAGE1_SENSE_TRIM; ++i) {
        bat_sum += bat_mv[i];
        isen_sum += isen_mv[i];
    }
    bat_sum >>= 4;
    isen_sum >>= 4;

    *out_ma = (isen_sum > bat_sum) ? (1000 * (isen_sum - bat_sum)) / STAGE1_R_SENSE_MOHM : 0;
    (void)stage1_arm_usb_charger();
    return 0;
}

#if MVII_MT6592_STAGE1_STATUS_MARKS
/* STAGE1_MARK with the live VBAT appended, so `flash -mtk-read-boot-status`
 * shows the charge climbing during a survival hold. */
static void stage1_mark_vbat(uint32_t stage, const char *prefix, int mv)
{
    static char buf[96];
    uint32_t n = 0;
    char digits[8];
    uint32_t d = 0;
    uint32_t v = (mv < 0) ? 0u : (uint32_t)mv;

    while (prefix[n] != '\0' && n < sizeof(buf) - 12u) {
        buf[n] = prefix[n];
        ++n;
    }
    do {
        digits[d++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && d < sizeof(digits));
    while (d != 0u) buf[n++] = digits[--d];
    buf[n++] = 'm';
    buf[n++] = 'V';
    buf[n] = '\0';
    mt6592_bootstatus_set_stage(stage);
    mt6592_bootstatus_set_message(buf);
    (void)mt6592_bootstatus_flush();
}
#else
/* Args deliberately discarded unevaluated: the bootstatus stage enums do not
 * exist in builds without STATUS_MARKS (header not included). */
#define stage1_mark_vbat(stage, prefix, mv) do { } while (0)
#endif

#if MVII_MT6592_STAGE1_STATUS_MARKS
/* STAGE1_MARK with an unsigned decimal appended. General-purpose; whether it has
 * a caller depends on which of the switches above are on, hence maybe_unused. */
__attribute__((unused))
static void stage1_mark_u32(uint32_t stage, const char *prefix, uint32_t value)
{
    static char buf[96];
    uint32_t n = 0;
    char digits[12];
    uint32_t d = 0;

    while (prefix[n] != '\0' && n < sizeof(buf) - 14u) {
        buf[n] = prefix[n];
        ++n;
    }
    do {
        digits[d++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && d < sizeof(digits));
    while (d != 0u) buf[n++] = digits[--d];
    buf[n] = '\0';
    mt6592_bootstatus_set_stage(stage);
    mt6592_bootstatus_set_message(buf);
    (void)mt6592_bootstatus_flush();
}
#else
#define stage1_mark_u32(stage, prefix, value) do { } while (0)
#endif

/* ------------------------------------------------------------------ */
/* Battery presence and charge, measured once, before the OS exists    */
/* ------------------------------------------------------------------ */

static uint32_t g_stage1_batt_state = MT6592_BOOT_BATTERY_UNKNOWN;
static uint32_t g_stage1_batt_mv;
static uint32_t g_stage1_batt_pct;
static uint32_t g_stage1_batt_raw;
static int      g_stage1_batt_measured;

/*
 * Is a cell actually fitted?
 *
 * Arm the BATON comparator the way the vendor's own CHARGING_CMD_GET_BATTERY_STATUS
 * arms it -- CHR_CON7 bit 2, then bit 0 -- let it settle, and read bit 12.
 *
 * This is the fourth presence candidate this board has been given, and the
 * previous three failed for two distinct reasons worth keeping straight. BATSNS
 * in the Li-ion band fails structurally: there is no power-path FET here, so the
 * charger output IS the node being measured and it reads "fitted" off the cable
 * alone. The two BATON attempts failed procedurally: one never enabled the
 * comparator at all, the other enabled half of it. Both then read a detector that
 * was switched off, and an off detector answers stably -- which is why "constant 0
 * on an empty connector" was mistaken for a pin that is not wired.
 *
 * The verdict therefore comes from CHR_CON7[12] now, not CHRSTATUS[5]; see the
 * register block above for why the latter was dropped.
 *
 * So the raw registers are published alongside the verdict on every charger
 * boot, and the verdict is only ever allowed to make the boot *shorter*: it can
 * skip the charge park, never cause one, and the power key breaks the park
 * regardless. A wrong answer here costs an unnecessary boot, not a brick.
 */
static uint32_t stage1_battery_presence(uint32_t *raw_out)
{
    uint32_t con7 = 0u;
    uint32_t status = 0u;

    if (raw_out) *raw_out = 0u;
    if (!stage1_pwrap_ready()) return MT6592_BOOT_BATTERY_UNKNOWN;
    if (mt6592_pwrap_read(PMIC_CHR_CON7, &con7) != 0) return MT6592_BOOT_BATTERY_UNKNOWN;

    if ((con7 & (PMIC_CHR_CON7_BATON_TDET_EN | PMIC_CHR_CON7_BATON_EN)) !=
        (PMIC_CHR_CON7_BATON_TDET_EN | PMIC_CHR_CON7_BATON_EN)) {
        /* Two writes, in stock's order, not one folded read-modify-write: bit 2
         * before bit 0 is the correction being made here, and merging them would
         * throw away the only thing that changed. */
        if (mt6592_pwrap_write(PMIC_CHR_CON7, con7 | PMIC_CHR_CON7_BATON_TDET_EN) != 0) {
            return MT6592_BOOT_BATTERY_UNKNOWN;
        }
        if (mt6592_pwrap_write(PMIC_CHR_CON7,
                               con7 | PMIC_CHR_CON7_BATON_TDET_EN |
                                   PMIC_CHR_CON7_BATON_EN) != 0) {
            return MT6592_BOOT_BATTERY_UNKNOWN;
        }
        mt6592_delay_cycles(STAGE1_BATON_SETTLE_CYCLES);
        if (mt6592_pwrap_read(PMIC_CHR_CON7, &con7) != 0) return MT6592_BOOT_BATTERY_UNKNOWN;
        if ((con7 & PMIC_CHR_CON7_BATON_EN) == 0u) {
            /* The enable did not stick, so the comparator is not running and
             * whatever the status bit reads is not a measurement. */
            stage1_mark_u32(MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY,
                            "stage1: BATON_EN would not set, con7=", con7);
            return MT6592_BOOT_BATTERY_UNKNOWN;
        }
    }

    if (mt6592_pwrap_read(PMIC_CHRSTATUS, &status) != 0) status = 0u;
    if (raw_out) *raw_out = status;

    /* Both candidate bits, raw, every boot -- but the VERDICT now comes from
     * CHR_CON7 bit 12, not from CHRSTATUS bit 5, and that is a deliberate swap.
     *
     * CHR_CON7[12] is the bit stock's own CHARGING_CMD_GET_BATTERY_STATUS reads;
     * whatever 0x142[5] is, it is not what the vendor consults when asked this
     * question, and 0x142 has since been identified as the register this port
     * already reads for the power key (bit 1 PWRKEY_DEB, bit 2 HOMEKEY_DEB) --
     * i.e. a keypad status word, which is why it never moved with the cell.
     * It stays published because a boot log costs nothing and a disagreement
     * between the two would be worth seeing. */
    stage1_mark_u32(MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY,
                    "stage1: BATON chrstatus=", status);
    stage1_mark_u32(MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY,
                    "stage1: BATON con7=", con7);

    return (con7 & PMIC_CHR_CON7_RGS_BATON_UNDET) != 0u ? MT6592_BOOT_BATTERY_ABSENT
                                                        : MT6592_BOOT_BATTERY_PRESENT;
}

/*
 * One measurement for the whole of stage1: presence, voltage, percentage.
 *
 * Taken here rather than in the OS because this is the only moment on the boot
 * where the load is low enough for VBAT to mean anything -- mt6592_pmic.c holds
 * the AUXADC off for the first three seconds of runtime for exactly that
 * reason, and the splash that wants to print the number is drawn inside that
 * window. Cached, then published into the handoff record for stage2.
 */
static void stage1_measure_battery(void)
{
    int mv = 0;
    int charge_ma = 0;

    if (g_stage1_batt_measured) return;
    g_stage1_batt_measured = 1;

    g_stage1_batt_state = stage1_battery_presence(&g_stage1_batt_raw);

    /* Same "unreadable or absurd means no cell" window the survival hold uses.
     * It cannot prove presence (with no power path the charger holds this node
     * up on its own), but it does refute it: a reading outside the window means
     * nothing sane is on BATSNS, and a percentage from it would be fiction. */
    if (stage1_auxadc_read_batsns_mv(&mv) != 0 || mv < 2500 || mv > 5000) {
        g_stage1_batt_mv = 0u;
        g_stage1_batt_pct = 0u;
        if (g_stage1_batt_state == MT6592_BOOT_BATTERY_PRESENT) {
            /* BATON says fitted, BATSNS says there is nothing there. Do not
             * pick a winner -- say so and drop to UNKNOWN, which no caller
             * acts on. */
            stage1_mark_u32(MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY,
                            "stage1: BATON says fitted but BATSNS is unreadable; "
                            "battery UNKNOWN, chrstatus=", g_stage1_batt_raw);
            g_stage1_batt_state = MT6592_BOOT_BATTERY_UNKNOWN;
        }
        return;
    }

    /* THE CURRENT, THEN THE PERCENTAGE -- in that order, because the second is
     * not computable without the first. mv is a TERMINAL voltage and the vendor's
     * table is indexed by OPEN-CIRCUIT voltage; on a plugged-in board those differ
     * by the drop the charge current develops across the pack's own internal
     * resistance, which at the currents this charger delivers is over a hundred
     * millivolts and, at the top of this pack's curve, most of the last twenty
     * points. LK reads the same two channels the OS's `bat' reads and hands the
     * answer to the same header, so the splash and the shell cannot disagree.
     *
     * A pair that will not convert leaves charge_ma at 0, which disables the
     * correction rather than guessing at one. */
    if (stage1_charge_current_ma(&charge_ma) != 0) charge_ma = 0;

    g_stage1_batt_mv = (uint32_t)mv;
    g_stage1_batt_pct = (uint32_t)mt6592_battery_percent_from_mv(mv, charge_ma);
    stage1_mark_vbat(MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY,
                     "stage1: battery measured, VBAT=", mv);
    stage1_mark_u32(MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY,
                    "stage1: battery charge current mA=", (uint32_t)charge_ma);
    stage1_mark_vbat(MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY, "stage1: battery OCV=",
                     mt6592_battery_ocv_mv(mv, charge_ma));
    stage1_mark_u32(MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY,
                    "stage1: battery percent=", g_stage1_batt_pct);
}

/* bpp / pixel helpers (unchanged) */
static uint32_t bpp_from_pitch(uint32_t pitch, uint32_t width)
{
    if (width == 0u) return 0u;
    if (pitch >= width * 4u && (pitch & 3u) == 0u) return 4u;
    if (pitch >= width * 3u) return 3u;
    if (pitch >= width * 2u && (pitch & 1u) == 0u) return 2u;
    return 0u;
}

static uint16_t to565(uint32_t argb)
{
    uint32_t r = (argb >> 16) & 0xffu, g = (argb >> 8) & 0xffu, b = argb & 0xffu;
    return (uint16_t)(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
}

static void put(volatile uint8_t *row, uint32_t x, uint32_t bpp, uint32_t argb)
{
    if (bpp == 4u) {
        ((volatile uint32_t *)row)[x] = argb;
    } else if (bpp == 3u) {
        volatile uint8_t *p = row + x * 3u;
        p[0] = (uint8_t)argb; p[1] = (uint8_t)(argb >> 8); p[2] = (uint8_t)(argb >> 16);
    } else if (bpp == 2u) {
        ((volatile uint16_t *)row)[x] = to565(argb);
    }
}

/* ================================================================== */
/* Optional raw BGRA8888 splash hook                                  */
/* ================================================================== */

/* Normal boot.img builds provide these from microsoft_logo_stub.S so stage1
 * stays small and skips painting. */
extern const uint8_t microsoft_logo_bgra_bin_start[];
extern const uint8_t microsoft_logo_bgra_bin_end[];

static int find_lk_framebuffer(stage1_framebuffer_t *fb)
{
    uint32_t roi, W, H, en, src;

    if (!fb) return 0;
    fb->addr = 0u;
    fb->pitch = 0u;
    fb->width = 0u;
    fb->height = 0u;
    fb->bpp_bytes = 0u;

    wdt_off();

    roi = rd32(OVL0_BASE + OVL_ROI_SIZE);
    W = roi & 0xffffu;
    H = (roi >> 16) & 0xffffu;
    if (W == 0u || W > PANEL_W) W = PANEL_W;
    if (H == 0u || H > PANEL_H) H = PANEL_H;

    en  = rd32(OVL0_BASE + OVL_EN);
    src = rd32(OVL0_BASE + OVL_SRC_CON);

    if ((en & 1u) != 0u && src != 0u) {
        for (int layer = 3; layer >= 0; --layer) {
            uint32_t addr, pitch, bpp;
            if ((src & (1u << layer)) == 0u) continue;
            addr  = rd32(OVL0_BASE + OVL_L0_ADDR  + (uint32_t)layer * OVL_LAYER_STRIDE);
            pitch = rd32(OVL0_BASE + OVL_L0_PITCH + (uint32_t)layer * OVL_LAYER_STRIDE) & 0xffffu;
            bpp   = bpp_from_pitch(pitch, W);
            if (bpp == 0u || addr < DRAM_BASE) continue;
            if ((uint64_t)addr + (uint64_t)pitch * H > DRAM_END) continue;

            fb->addr = addr;
            fb->pitch = pitch;
            fb->width = W;
            fb->height = H;
            fb->bpp_bytes = bpp;
            return 1;
        }
    }

    fb->addr = FALLBACK_FB_ADDR;
    fb->pitch = PANEL_W * 4u;
    fb->width = PANEL_W;
    fb->height = PANEL_H;
    fb->bpp_bytes = 4u;
    return 1;
}

/* Paint a raw BGRA8888 buffer (B,G,R,A per pixel) into the live framebuffer.
 * Fast path for the common 32-bpp full-screen case; safe fallback otherwise.
 */
static void paint_raw_bgra(uint32_t fb_addr, uint32_t pitch, uint32_t W, uint32_t H, uint32_t bpp,
                           const uint8_t *bgra_data, uint32_t data_len)
{
    if (data_len < W * H * 4u) return;

    volatile uint8_t *base = (volatile uint8_t *)(uintptr_t)fb_addr;

    if (bpp == 4u && pitch == W * 4u) {
        /* Fast path: the BGRA layout already matches and, with pitch == W*4,
         * the whole image is one contiguous run -- so copy it in a single pass
         * instead of H separate row calls. Splitting it per row bought nothing
         * except the chance to watch the boundary march down the panel. The
         * watchdog is disarmed once up front for the same reason: petting it
         * every 32 rows meant stopping the copy 15 times mid-frame. */
        wdt_off();
        s1_memcpy(base, bgra_data, W * H * 4u);
        __asm__ volatile("dsb sy" ::: "memory");
        wdt_off();
        return;
    }

    /* General path: convert BGRA bytes to argb and use existing put() */
    for (uint32_t y = 0; y < H; ++y) {
        volatile uint8_t *row = base + (uintptr_t)y * pitch;
        if ((y & 0x1fu) == 0u) wdt_off();
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t *p = bgra_data + (uintptr_t)(y * W + x) * 4u;
            uint32_t b = p[0], g = p[1], r = p[2], a = p[3];
            uint32_t argb = (a << 24) | (r << 16) | (g << 8) | b;
            put(row, x, bpp, argb);
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Discover live layers and paint the boot logo into them. This is the stage1
 * splash paint in the boot path.
 *
 * ── THE LOGO IS NO LONGER PART OF THIS BINARY, WHEN IT DOES NOT HAVE TO BE ──
 *
 * It used to be, unconditionally: a full-frame logo object `.incbin`'d a 1.2 MB
 * BGRA frame straight into MVIIS1.elf, so changing the splash meant rebuilding
 * and reflashing a boot image. The picture now lives in the asset slot on the
 * LOGO partition, which the MVII LK stages in DRAM before it hands over here --
 * so this looks there first and the splash becomes a one-partition flash.
 *
 * The embedded blob stays as the fallback, and it is not vestigial: booted under
 * the STOCK MediaTek LK nobody stages anything, mvii_assets_adopt() fails, and
 * the .incbin is the only picture there is. Same for a board whose LOGO
 * partition has never been written with a container.
 *
 * mvii_asset_raw() rather than a blit because the stored bytes ARE the frame --
 * the generator keeps the logo uncompressed for exactly this -- so the fast
 * memcpy path above still applies and the splash does not get slower to gain a
 * flag. A pixel-by-pixel composite here would double the DRAM traffic on a
 * surface with no transparency in it.
 */
static uint32_t paint_microsoft_logo(void)
{
    stage1_framebuffer_t fb;
    const uint8_t *logo = 0;
    uint32_t logo_size = 0u;

    if (!find_lk_framebuffer(&fb)) return 0u;

    if (mvii_assets_adopt() == 0) {
        const mvii_asset_entry_t *e = mvii_asset_find(MVII_ASSET_LOGO);
        if (e && e->format == MVII_ASSET_FMT_ARGB8888 && e->width == fb.width &&
            e->height == fb.height) {
            logo = mvii_asset_raw(MVII_ASSET_LOGO, &logo_size);
        }
    }

    if (!logo) {
        logo = microsoft_logo_bgra_bin_start;
        logo_size = (uint32_t)(microsoft_logo_bgra_bin_end - microsoft_logo_bgra_bin_start);
    }

    paint_raw_bgra(fb.addr, fb.pitch, fb.width, fb.height, fb.bpp_bytes, logo, logo_size);
    return 1u;
}

void mvii_arm_stage1_run(uint32_t entry_r0, uint32_t entry_r1,
                         uint32_t entry_r2, uint32_t entry_r3)
{
    (void)entry_r0; (void)entry_r1; (void)entry_r2; (void)entry_r3;
    (void)paint_microsoft_logo();   /* MVIIFlash diagnostic shows the logo */
}

/*
 * Optional embedded second stage. Normal boot.img builds link the stub so
 * MVIIS1 stays small; MVII.bin is carried once by the Android ramdisk field and
 * LK loads it at MVII_ARM_SECOND_STAGE_ADDR.
 */
extern const uint8_t mvii_stage2_blob_start[];
extern const uint8_t mvii_stage2_blob_end[];

/* Signature check & jump helpers (unchanged) */
static int os_second_stage_present(uint32_t addr)
{
    static const char sig[] = "MVIIARM2";
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)(addr + 4u);
    if (addr < DRAM_BASE) return 0;
    for (uint32_t i = 0; i < 8u; ++i)
        if (p[i] != (uint8_t)sig[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Ramdisk-carried OS locate + relocate                               */
/*                                                                    */
/* The stock LK does NOT load the boot.img ramdisk to the header's    */
/* ramdisk_addr. Its loader places the ramdisk contiguously after the */
/* page-padded kernel (kernel_addr + kernel_pages * page_size, per    */
/* the lk.full-decompile boot path) and only advertises the real      */
/* location through ATAG_INITRD2. So stage1 must find MVII.bin and    */
/* move it to its 0x84000000 link address itself.                     */
/* ------------------------------------------------------------------ */

#define ATAG_CORE     0x54410001u
#define ATAG_NONE     0x00000000u
#define ATAG_INITRD2  0x54420005u
/* MediaTek's private boot-mode tag: {size=3, 0x41000802, mode}. Confirmed in
 * this device's own LK (Reference/j36-lk-reverse, FUN_81e02ae8 emits size 3 and
 * tag 0x41000802 followed by one word read from LK's boot-mode global). */
#define ATAG_MTK_BOOT 0x41000802u
/* Payload values. These are MediaTek's stock BOOTMODE enum; the tag itself is
 * confirmed against this LK, the numbers are not (the kernel image would not
 * decompress and LK's Thumb-2 literal pools defeated the string xref). Hence
 * stage1_mark_u32() logs the observed value on every boot, and nothing acts on
 * it without CHRDET agreeing. */
#define MTK_BOOT_MODE_KERNEL_POWER_OFF_CHARGING 8u
#define MTK_BOOT_MODE_LOW_POWER_OFF_CHARGING    9u
#define MTK_IMG_MAGIC 0x58881688u
#define RAMDISK_SCAN_END 0x82600000u   /* stay below the 0x82700000 LK canvas */
/* Cap for a copy whose length we could not determine exactly (bare-image scan
 * fallback with no ATAG size). Generous over MVII.bin (~1.4 MiB) with room to
 * grow, but small enough that the worst-case byte copy stays quick. The ATAG
 * path supplies the exact size and never hits this cap. */
#define RAMDISK_LEN_CAP  0x00800000u   /* 8 MiB */

/* Overlap-safe copy (dst above src copies backwards). */
static void s1_memmove(uint32_t dst, uint32_t src, uint32_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)dst;
    const uint8_t *s = (const uint8_t *)(uintptr_t)src;
    if (dst == src || n == 0u) return;
    if (dst < src) {
        for (uint32_t i = 0; i < n; ++i) {
            if ((i & 0xfffffu) == 0u) wdt_off();
            d[i] = s[i];
        }
    } else {
        for (uint32_t i = n; i != 0u; --i) {
            if ((i & 0xfffffu) == 0u) wdt_off();
            d[i - 1u] = s[i - 1u];
        }
    }
}

/* r2 at the boot.img kernel entry is the ATAG list (or a DTB, or garbage —
 * both rejected by the ATAG_CORE check). ATAG_INITRD2 = {addr, size} of the
 * ramdisk exactly where LK loaded it. */
static int find_ramdisk_from_atags(uint32_t tags, uint32_t *addr_out, uint32_t *len_out)
{
    const uint32_t *p;
    uint32_t guard = 4096u;

    if (tags < DRAM_BASE || tags >= (uint32_t)(DRAM_END - 0x10000ull) || (tags & 3u) != 0u) return 0;
    p = (const uint32_t *)(uintptr_t)tags;
    if (p[1] != ATAG_CORE) return 0;
    while (guard--) {
        uint32_t size = p[0];
        uint32_t tag = p[1];
        if (tag == ATAG_NONE || size < 2u || size > 1024u) break;
        if (tag == ATAG_INITRD2 && size >= 4u) {
            *addr_out = p[2];
            *len_out = p[3];
            return 1;
        }
        p += size;
    }
    return 0;
}

#if MVII_MT6592_STAGE1_CHARGE_PARK
/* Same walk, for MediaTek's boot-mode tag. Kept separate rather than folded
 * into the ramdisk walk because the two run at opposite ends of stage1: the
 * boot mode decides whether there is going to be a hand-off at all. */
static int find_boot_mode_from_atags(uint32_t tags, uint32_t *mode_out)
{
    const uint32_t *p;
    uint32_t guard = 4096u;

    if (!mode_out) return 0;
    if (tags < DRAM_BASE || tags >= (uint32_t)(DRAM_END - 0x10000ull) || (tags & 3u) != 0u) return 0;
    p = (const uint32_t *)(uintptr_t)tags;
    if (p[1] != ATAG_CORE) return 0;
    while (guard--) {
        uint32_t size = p[0];
        uint32_t tag = p[1];
        if (tag == ATAG_NONE || size < 2u || size > 1024u) break;
        if (tag == ATAG_MTK_BOOT && size >= 3u) {
            *mode_out = p[2];
            return 1;
        }
        p += size;
    }
    return 0;
}
#endif /* MVII_MT6592_STAGE1_CHARGE_PARK */

/* End of stage1's own file footprint (linker symbol): the LK-placed ramdisk
 * sits at or beyond this, and starting here also keeps the scan from matching
 * the "MVIIARM2" signature string inside stage1's own .rodata. */
extern uint8_t __image_end[];

/* Fallback when no usable ATAGs: page-scan DRAM after the stage1 image for
 * the MTK "ROOTFS" wrapper or a bare MVII image. */
static int find_ramdisk_by_scan(uint32_t *addr_out, uint32_t *len_out)
{
    uint32_t scan_start =
        ((uint32_t)(uintptr_t)__image_end + 0x7ffu) & ~0x7ffu;
    /* Builds linked outside DRAM (SRAM flash payload) must never let the scan
     * wander below DRAM. */
    if (scan_start < DRAM_BASE + 0x8000u) scan_start = DRAM_BASE + 0x8000u;
    for (uint32_t a = scan_start; a < RAMDISK_SCAN_END; a += 0x800u) {
        const uint32_t *w = (const uint32_t *)(uintptr_t)a;
        if ((a & 0x3fffffu) == 0u) wdt_off();
        if (w[0] == MTK_IMG_MAGIC) {
            const uint8_t *n = (const uint8_t *)(uintptr_t)(a + 8u);
            if (n[0] == 'R' && n[1] == 'O' && n[2] == 'O' && n[3] == 'T' &&
                n[4] == 'F' && n[5] == 'S') {
                *addr_out = a;
                *len_out = w[1] + 512u;
                return 1;
            }
        }
        if (os_second_stage_present(a)) {
            *addr_out = a;
            *len_out = 0u; /* bare image: size unknown, caller caps the copy */
            return 1;
        }
    }
    return 0;
}

/* Normalise a candidate source (strip the 512-byte MTK ROOTFS wrapper when
 * present), verify the MVII signature, and move the image to dst. */
static int try_ramdisk_source(uint32_t src, uint32_t len, uint32_t dst)
{
    if (src < DRAM_BASE || src >= (uint32_t)DRAM_END) return 0;
    if (*(const volatile uint32_t *)(uintptr_t)src == MTK_IMG_MAGIC) {
        uint32_t wrapped = *(const volatile uint32_t *)(uintptr_t)(src + 4u);
        src += 512u;
        len = wrapped != 0u ? wrapped : (len > 512u ? len - 512u : 0u);
    }
    if (!os_second_stage_present(src)) return 0;
    if (src == dst) return 1;
    if (len == 0u || len > RAMDISK_LEN_CAP) len = RAMDISK_LEN_CAP;
    s1_memmove(dst, src, len);
    __asm__ volatile("dsb sy\n\tisb" ::: "memory");
    return os_second_stage_present(dst);
}

/* Diagnostics for the hand-off status marker: which locate path fired and
 * what source/length were used, so a truncated or mis-sized copy is visible
 * in one `-mtk-read-boot-status` readback. */
static char g_ramdisk_note[96];
__attribute__((unused)) static const char* ramdisk_note(void) { return g_ramdisk_note; }
static char* note_append(char* out, const char* end, const char* text)
{
    while (out + 1 < end && text && *text) *out++ = *text++;
    *out = 0;
    return out;
}
static char* note_append_hex(char* out, const char* end, uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    out = note_append(out, end, "0x");
    for (int shift = 28; shift >= 0 && out + 1 < end; shift -= 4) {
        *out++ = hex[(v >> (uint32_t)shift) & 0xfu];
    }
    *out = 0;
    return out;
}
static void note_ramdisk(const char* how, uint32_t src, uint32_t len)
{
    char* out = g_ramdisk_note;
    const char* end = g_ramdisk_note + sizeof(g_ramdisk_note);
    out = note_append(out, end, "stage1: OS via ");
    out = note_append(out, end, how);
    out = note_append(out, end, " src=");
    out = note_append_hex(out, end, src);
    out = note_append(out, end, " len=");
    out = note_append_hex(out, end, len);
    (void)note_append(out, end, " -> 0x84000000");
}

static int place_ramdisk_second_stage(uint32_t dst, uint32_t tags)
{
    uint32_t src = 0u;
    uint32_t len = 0u;

    /* Always prefer the FRESH ramdisk LK re-loads every boot (ATAG, then a
     * DRAM scan for its lower-memory landing spot) over whatever is already at
     * the 0x84000000 destination. The board warm-watchdog-resets with DRAM
     * preserved, so a previous boot's relocated-and-then-executed image is
     * still sitting at dst with a valid signature but dirtied .data/.bss/stack
     * — jumping straight into it faults before stage2 can run. A fresh copy
     * (from a source that is NOT dst) overwrites that stale image.
     *
     * try_ramdisk_source() is a no-op copy when src == dst, which is the
     * legitimate case where LK honoured ramdisk_addr and loaded the fresh
     * ramdisk to dst itself. */
    if (find_ramdisk_from_atags(tags, &src, &len) && try_ramdisk_source(src, len, dst)) {
        note_ramdisk(src == dst ? "atag-in-place" : "atag", src, len);
        return 1;
    }
    if (find_ramdisk_by_scan(&src, &len) && src != dst && try_ramdisk_source(src, len, dst)) {
        note_ramdisk("scan", src, len);
        return 1;
    }
    /* Last resort only: no fresh source located, but a valid image is present
     * at dst (may be stale — this is a best-effort boot rather than parking). */
    if (os_second_stage_present(dst)) {
        note_ramdisk("in-place-fallback", dst, 0u);
        return 1;
    }
    return 0;
}

/*
 * Copy an optional embedded second stage to its link/run address. Returns 1 on
 * success (blob present and signature verified at the destination), 0 otherwise.
 */
static int place_embedded_second_stage(uint32_t dst)
{
    const uint8_t *src = mvii_stage2_blob_start;
    uint32_t len = (uint32_t)(mvii_stage2_blob_end - mvii_stage2_blob_start);
    if (len < 16u) return 0;                 /* stub/placeholder blob — nothing to place */
    if (dst < DRAM_BASE) return 0;
    s1_memcpy((volatile uint8_t *)(uintptr_t)dst, src, len);
    __asm__ volatile("dsb sy\n\tisb" ::: "memory");
    return os_second_stage_present(dst);
}

static void jump_to_second_stage(uint32_t addr, uint32_t r0, uint32_t r1,
                                 uint32_t r2, uint32_t r3)
{
    void (*entry)(uint32_t, uint32_t, uint32_t, uint32_t) =
        (void (*)(uint32_t, uint32_t, uint32_t, uint32_t))(uintptr_t)addr;
    __asm__ volatile("dsb sy\n\tisb" ::: "memory");
    entry(r0, r1, r2, r3);
}

static const mt6592_boot_handoff_t *build_os_handoff(uint32_t entry_r0, uint32_t entry_r1,
                                                     uint32_t entry_r2, uint32_t entry_r3)
{
    stage1_framebuffer_t fb;
    const mt6592_boot_handoff_t *handoff = mt6592_bootinfo_init(entry_r0, entry_r1, entry_r2, entry_r3);
    if (find_lk_framebuffer(&fb)) {
        mt6592_bootinfo_set_framebuffer(fb.addr, fb.width, fb.height, fb.pitch, fb.bpp_bytes * 8u);
    }
    if (g_stage1_joy_center_valid) {
        mt6592_bootinfo_set_joystick_centers(g_stage1_joy_center);
    }
    /* Unconditional: an ABSENT or UNKNOWN verdict is a result stage2 needs as
     * much as a percentage, because it is what stops the splash printing 0% on
     * a board that is running off the cable with no cell in it. */
    mt6592_bootinfo_set_battery(g_stage1_batt_mv, g_stage1_batt_pct,
                                g_stage1_batt_state, g_stage1_batt_raw);
    return handoff;
}

static void park_forever(void)
{
    uint32_t spin = 0u;
    for (;;) {
        wdt_off();
        /* Keep the latent charger armed while parked (~every second). */
        if ((spin++ & 0xffffu) == 0u) (void)stage1_arm_usb_charger();
        for (volatile uint32_t d = 0; d < 8000u; ++d) { }
    }
}

#if MVII_MT6592_STAGE1_CHARGE_PARK
/*
 * Power key, straight off the PMIC. stage1 has no keypad driver and does not
 * need one: pwrap is already up for the charger, and CHRSTATUS carries the
 * debounced key state (bit clear = held).
 */
static int stage1_pwrkey_held(void)
{
    uint32_t v = 0u;
    if (!stage1_pwrap_ready()) return 0;
    if (mt6592_pwrap_read(PMIC_CHRSTATUS, &v) != 0) return 0;
    return (v & PMIC_CHRSTATUS_PWRKEY_DEB) == 0u;
}

/*
 * Park in charge mode. Returns only when the power key says to boot anyway.
 *
 * Same loop as park_forever(), split out so the intent reads differently at the
 * call site and so the status marker says why we stopped.
 *
 * The power-key exit is the whole difference between "parked charging" and
 * "bricked while plugged in". Before it, this loop polled nothing and never
 * left, so plugging the cable in and then pressing power did nothing at all --
 * the device sat on LK's battery symbol with a live CPU and ignored the only
 * control the user has. Pressing power on a charging device means "turn on",
 * and that is now what it does: fall out of the park and continue the ordinary
 * boot, splash and all, with no reset in between. It is also the recovery path
 * for a wrong charge-mode or battery-presence verdict, which is why neither of
 * those decisions is allowed to be load-bearing.
 *
 * What this still deliberately does NOT do:
 *
 *   - It does not power the PMIC off. mt6592_pmic.c is not linked into stage1,
 *     and even if it were, powering off on a mode we could not fully verify
 *     would turn a misdetection into "the device refuses to boot". Parking is
 *     recoverable from the outside; powering off on a wrong guess is not.
 *   - It does not paint anything. LK's battery symbol is already on the panel
 *     and OVL keeps compositing it with no CPU involvement, which is exactly
 *     the behaviour wanted here. (The same OVL persistence that had to be
 *     broken in the OS boot UI for the OS path is the feature on this path.)
 *   - It does not exit when the cable is pulled. Removing power from a device
 *     that only came up because of that power drops the rail on its own; there
 *     is nothing sensible to boot into.
 */
static void stage1_charge_park(void)
{
    /* One spin is ~15 us of busy-wait, so the charger re-arm below lands about
     * once a second and the key is sampled about once a millisecond. The key
     * has to read held across every one of PARK_PWRKEY_POLLS consecutive
     * samples, which is a deliberate press rather than a contact bounce or a
     * single bad pwrap round trip. */
    enum { PARK_PWRKEY_POLL_SPINS = 64u, PARK_PWRKEY_POLLS = 64u };
    uint32_t spin = 0u;
    uint32_t held = 0u;

    STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_HOLD,
                "stage1: charger boot mode; parked charging, press power to boot");
    for (;;) {
        wdt_off();
        /* Re-arm roughly once a second: the same cadence park_forever() uses,
         * and the charger enable is the only thing that has to stay true. */
        if ((spin & 0xffffu) == 0u) (void)stage1_arm_usb_charger();
        if ((spin & (PARK_PWRKEY_POLL_SPINS - 1u)) == 0u) {
            if (stage1_pwrkey_held()) {
                if (++held >= PARK_PWRKEY_POLLS) {
                    STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_ARMED,
                                "stage1: power key pressed at charge park; booting");
                    return;
                }
            } else {
                held = 0u;
            }
        }
        ++spin;
        for (volatile uint32_t d = 0; d < 8000u; ++d) { }
    }
}

/*
 * Decide whether this boot exists only to charge.
 *
 * Three conditions, and the third is new: LK said so via its private boot-mode
 * tag, the PMIC agrees there is VBUS, and there is a battery to charge. The
 * first two alone are not enough — the tag value is the unverified half, and
 * CHRDET alone is just "someone plugged in a running device". The raw mode is
 * published before the decision either way, so a single boot-status readback
 * confirms or corrects the enum above.
 *
 * The battery condition exists because "parked charging" is meaningless with no
 * cell fitted: there is nothing to charge, the box is simply running off USB,
 * and LK's battery symbol sitting on the panel forever is the device refusing
 * to start for no reason. It only ever shortens the boot — an ABSENT verdict
 * skips the park, and UNKNOWN (an unreadable or un-enabled BATON comparator)
 * falls back to the previous behaviour rather than guessing, because the power
 * key now breaks the park either way.
 */
static int stage1_should_park_for_charge(uint32_t atags)
{
    uint32_t mode = 0u;

    if (!find_boot_mode_from_atags(atags, &mode)) {
        STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY,
                    "stage1: no MTK boot-mode ATAG; treating as normal boot");
        return 0;
    }
    stage1_mark_u32(MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY, "stage1: LK boot mode=", mode);

    if (mode != MTK_BOOT_MODE_KERNEL_POWER_OFF_CHARGING &&
        mode != MTK_BOOT_MODE_LOW_POWER_OFF_CHARGING) {
        return 0;
    }
    if (!stage1_arm_usb_charger()) {
        /* Charging mode without VBUS is a contradiction; believe the hardware. */
        STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY,
                    "stage1: charger boot mode but no VBUS; booting normally");
        return 0;
    }
    /* After the charger is armed, so the comparator is read with the rail in the
     * state it will spend the park in. */
    stage1_measure_battery();
    if (g_stage1_batt_state == MT6592_BOOT_BATTERY_ABSENT) {
        STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY,
                    "stage1: charger boot mode with no battery fitted; "
                    "nothing to charge, booting normally");
        return 0;
    }
    return 1;
}
#endif /* MVII_MT6592_STAGE1_CHARGE_PARK */

/* Simple seconds delay; doubles as the stage1 charger heartbeat. */
static void delay_seconds(uint32_t secs)
{
    while (secs--) {
        wdt_off();
        (void)stage1_arm_usb_charger();
        for (volatile uint32_t i = 0; i < 1200000u; ++i) { }
    }
}

/* Non-blocking: report whether stage1 saw USB power and armed the charger.
 * Boot proceeds unconditionally either way — the battery is optional. */
static void stage1_charge_latent_start(void)
{
    if (stage1_arm_usb_charger()) {
        STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_ARMED,
                    "stage1: USB charger armed (CHR_CON0.CHR_EN, battery optional)");
    } else {
        switch (g_stage1_charge_last_result) {
        case STAGE1_CHARGE_PWRAP_NOT_READY:
            STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_IDLE,
                        "stage1: charger idle; pwrap not ready");
            break;
        case STAGE1_CHARGE_READ_FAILED:
            STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_IDLE,
                        "stage1: charger idle; CHR_CON0 read failed");
            break;
        case STAGE1_CHARGE_NO_VBUS:
            STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_IDLE,
                        "stage1: charger idle; CHR_CON0.CHRDET is clear");
            break;
        case STAGE1_CHARGE_WRITE_FAILED:
            STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_IDLE,
                        "stage1: charger idle; PMIC charger write failed");
            break;
        default:
            STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_IDLE,
                        "stage1: charger idle");
            break;
        }
    }
}

/*
 * Hold at the splash while a deeply discharged battery charges to the
 * survival floor (see the knob block up top for why this exists and why the
 * floor is an under-charge voltage). Holds ONLY when all three are true:
 * VBUS present, VBAT readable, VBAT below the floor. Batteryless / no-VBUS /
 * unreadable-VBAT boots continue immediately; pulling the cable mid-hold
 * releases the boot.
 */
static void stage1_charge_survival_hold(void)
{
#if MVII_MT6592_STAGE1_CHARGE_HOLD
    uint32_t secs = 0u;
    int mv = 0;
    int best_mv = 0;

    if (!stage1_arm_usb_charger()) {
        return;                          /* no VBUS or PMIC writes failed */
    }
    if (stage1_auxadc_read_batsns_mv(&mv) != 0 || mv < 2500 || mv > 5000) {
        STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_ARMED,
                    "stage1: VBAT unreadable; assuming no battery, USB power only");
        return;
    }
    if (mv >= MVII_MT6592_STAGE1_VBAT_SURVIVAL_MV) return;

    best_mv = mv;
    stage1_mark_vbat(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_HOLD,
                     "stage1: battery below survival floor; charging at splash, VBAT=", mv);
    for (;;) {
        delay_seconds(1);                /* also re-arms the charger each tick */
        if (!stage1_arm_usb_charger()) {
            STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_ARMED,
                        "stage1: charger unavailable during hold; continuing boot");
            break;                       /* cable pulled or charger writes failed */
        }
        if (stage1_auxadc_read_batsns_mv(&mv) != 0 || mv < 2500 || mv > 5000) {
            STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_ARMED,
                        "stage1: VBAT became unreadable during hold; continuing boot");
            break;
        }
        if (mv > best_mv) best_mv = mv;
        if (mv >= MVII_MT6592_STAGE1_VBAT_SURVIVAL_EXIT_MV) {
            break;
        }
        ++secs;
        if (secs >= MVII_MT6592_STAGE1_CHARGE_HOLD_MAX_SECONDS) {
            stage1_mark_vbat(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_ARMED,
                             "stage1: survival hold timeout; best VBAT=", best_mv);
            break;
        }
        if ((secs % 30u) == 0u) {
            stage1_mark_vbat(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_HOLD,
                             "stage1: charging at splash, VBAT=", mv);
        }
    }
    stage1_mark_vbat(MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_ARMED,
                     "stage1: survival hold released, VBAT=", mv);
#endif
}

/*
 * One fresh AUXADC conversion, vendor style (IMM_GetOneChannelValue): stop the
 * channel, wait for its ready bit to CLEAR, let the input mux settle, restart
 * it, wait for a NEW ready, then read. Skipping the stop half returns whatever
 * conversion was already in flight, which is a value sampled before the mux
 * moved.
 */
static int stage1_soc_auxadc_read(uint32_t channel, uint32_t *out_raw)
{
    uint32_t dat_addr;
    uint32_t data = 0u;

    if (!out_raw || channel > 15u) return -1;
    dat_addr = SOC_AUXADC_BASE + SOC_AUXADC_DAT0 + channel * SOC_AUXADC_DAT_STRIDE;

    for (uint32_t i = 0; i < SOC_AUXADC_POLL_LIMIT; ++i) {
        if ((rd32(SOC_AUXADC_BASE + SOC_AUXADC_CON2) & SOC_AUXADC_CON2_BUSY) == 0u) break;
    }

    wr32(SOC_AUXADC_BASE + SOC_AUXADC_CON1_CLR, 1u << channel);
    for (uint32_t i = 0; i < SOC_AUXADC_POLL_LIMIT; ++i) {
        if ((rd32(dat_addr) & SOC_AUXADC_DAT_READY) == 0u) break;
    }
    mt6592_delay_cycles(SOC_AUXADC_SETTLE_CYCLES);

    wr32(SOC_AUXADC_BASE + SOC_AUXADC_CON1_SET, 1u << channel);
    mt6592_delay_cycles(SOC_AUXADC_SETTLE_CYCLES);
    for (uint32_t i = 0; i < SOC_AUXADC_POLL_LIMIT; ++i) {
        data = rd32(dat_addr);
        if ((data & SOC_AUXADC_DAT_READY) != 0u) {
            wr32(SOC_AUXADC_BASE + SOC_AUXADC_CON1_CLR, 1u << channel);
            *out_raw = data & SOC_AUXADC_DAT_MASK;
            return 0;
        }
    }
    wr32(SOC_AUXADC_BASE + SOC_AUXADC_CON1_CLR, 1u << channel);
    return -1;
}

/*
 * Capture the analog-stick rest positions, as early in stage1 as the AUXADC can
 * be made to answer.
 *
 * The OS cannot do this for itself. The OS keypad driver seeds its centers lazily, on
 * the first mt6592_keys_read_axes() call, which does not happen until the frame
 * pump is running — several seconds of boot later, with the device already in
 * the user's hands. A thumb resting on a stick at that moment becomes the
 * locked center, and every reading afterwards is biased by however far it was
 * pushed. Sampling here does not make that impossible (nothing can), but it
 * moves the capture to before the splash even paints, which is about as close
 * to "power-on, hands still on the power button" as the boot flow allows.
 *
 * All four channels must answer plausibly or nothing is published: a partial
 * seed is worse than none, because the OS fallback at least knows it is
 * guessing.
 */
static void stage1_capture_joystick_centers(void)
{
    uint32_t centers[MT6592_BOOT_JOY_CHANNEL_COUNT];

    /* Ungate the AUXADC peripheral clock. PDN0_CLR is write-1-to-clear, so this
     * touches only the bits named and leaves every other peripheral alone. */
    wr32(STAGE1_PERICFG_BASE + STAGE1_PERI_PDN0_CLR, STAGE1_PERI_PDN0_AUXADC_BITS);
    mt6592_delay_cycles(SOC_AUXADC_SETTLE_CYCLES * 8u);

    for (uint32_t i = 0; i < MT6592_BOOT_JOY_CHANNEL_COUNT; ++i) {
        const uint32_t channel = MT6592_BOOT_JOY_FIRST_CHANNEL + i;
        uint32_t accum = 0u;
        uint32_t taken = 0u;
        uint32_t raw = 0u;
        uint32_t center;

        for (uint32_t d = 0; d < STAGE1_JOY_DISCARD_SAMPLES; ++d) {
            (void)stage1_soc_auxadc_read(channel, &raw);
        }
        for (uint32_t s = 0; s < STAGE1_JOY_AVERAGE_SAMPLES; ++s) {
            if (stage1_soc_auxadc_read(channel, &raw) != 0) continue;
            accum += raw;
            ++taken;
        }
        if (taken == 0u) return;                    /* channel never went ready */
        center = accum / taken;
        if (center < (uint32_t)STAGE1_JOY_CENTER_MIN ||
            center > (uint32_t)STAGE1_JOY_CENTER_MAX) {
            return;                                 /* railed: ADC not really up */
        }
        centers[i] = center;
    }

    for (uint32_t i = 0; i < MT6592_BOOT_JOY_CHANNEL_COUNT; ++i) {
        g_stage1_joy_center[i] = centers[i];
    }
    g_stage1_joy_center_valid = 1;
}

/*
 * stage1 entry point
 *
 * 0. Capture the analog-stick rest positions, and — if LK says this boot only
 *    happened because a charger was plugged into a powered-off device — park
 *    there charging instead of starting the OS (stage1_should_park_for_charge).
 * 1. Arm the USB charger (latent — never blocks, never gates boot), measure the
 *    battery, and — only for a deeply discharged pack on USB power — hold at the
 *    splash until it can survive the boot (see stage1_charge_survival_hold).
 * 2. Paint the splash object, for boots under a loader that did not already put
 *    it on the panel. No pause on either side of it; see the note there.
 * 3. Hand off to OS second stage (or park if none present).
 */
void mvii_arm_stage1_main(uint32_t entry_r0, uint32_t entry_r1,
                          uint32_t entry_r2, uint32_t entry_r3)
{
    const mt6592_boot_handoff_t *handoff;
    static const uint32_t candidates[] = {
        MVII_ARM_SECOND_STAGE_ADDR,
        MVII_ARM_OS_HANDOFF_ADDR,
        0x84000000u,
        0x80f00000u,
    };

    wdt_off();

#if MVII_MT6592_STAGE1_STATUS_MARKS
    mt6592_bootstatus_init();
#endif
    STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY, "stage1: entered from LK");

    /* Before anything else that takes time. The charge survival hold below can
     * park here for minutes on a flat pack — every one of those is a second in
     * which someone can pick the device up and rest a thumb on a stick, which is
     * precisely the miscalibration this exists to avoid. */
    stage1_capture_joystick_centers();

#if MVII_MT6592_STAGE1_CHARGE_PARK
    /* Before the splash and before the survival hold: if this boot only happened
     * because a cable was plugged into a powered-off device AND there is a cell
     * in it to charge, there is no OS to start yet. Leaves LK's battery symbol
     * on the panel and charges until the power key says otherwise, at which
     * point it returns and the ordinary boot below carries on -- splash, stage2,
     * dashboard -- with no reset in between. */
    if (stage1_should_park_for_charge(entry_r2)) {
        stage1_charge_park();
    }
#endif

    /* THE CHARGER FIRST, THEN THE MEASUREMENT -- and that order is load-bearing
     * on this board, not tidiness. VBAT is VSYS here: there is no power path, so
     * with no cell fitted the charger block is the only thing holding the rail
     * up, and the PMIC's charger watchdog drops CHR_EN if the FSM goes
     * unattended. stage1_measure_battery() below is now the longest unattended
     * stretch in stage1 -- it takes the twenty-pair BATSNS/I_SENSE reading the
     * gauge needs -- and it used to run BEFORE anything here re-armed the
     * charger, on a rail that had been coasting since the LK's last service call
     * several seconds and one boot.img load earlier. It re-arms inside its own
     * loop as well; this is the arm it starts from. */
    stage1_charge_latent_start();

    /* Idempotent; the charge-park path above has usually already run it. This is
     * the call that covers every other boot, so the percentage reaches stage2's
     * splash whether the device was woken by the cable or by the power key. */
    stage1_measure_battery();

    stage1_charge_survival_hold();

    /*
     * THE SPLASH, WITH NO DEAD TIME AROUND IT.
     *
     * This used to be `delay_seconds(2); paint; delay_seconds(3)' -- five
     * seconds in which the only thing that happened was two charger heartbeats.
     * The two seconds were there so the LK's own picture stayed up before this
     * one replaced it, and the three so this one could be read before stage2
     * drew over it. Neither survives the change above it: the MVII LK now paints
     * the logo into the framebuffer BEFORE it commits the scanout, so the same
     * picture has been on the panel since the instant the backlight came up --
     * through the whole boot.img load. Waiting two seconds to replace an image
     * with itself, and three more to admire it, is five seconds of boot spent on
     * nothing.
     *
     * The paint stays, and it is not vestigial: booted under the STOCK MediaTek
     * LK nobody staged an asset slot and nobody painted anything, so this is the
     * first and only time the logo reaches the panel on that path.
     *
     * The heartbeat the deleted delays carried does not: the re-arm is explicit
     * now, immediately before the handoff, which is also where the largest load
     * step of the boot is about to happen.
     */
    (void)paint_microsoft_logo();
    (void)stage1_arm_usb_charger();
    STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_STAGE2_WRAPPED, "stage1: splash up, locating OS");

    /* 5. Hand off to OS. Stage2 expects r0 to be an MVII handoff record.
     *
     * Order: (a) diagnostic embedded blob, (b) the ramdisk-carried image —
     * located via ATAG_INITRD2 or a DRAM scan and moved to its 0x84000000
     * link address (the stock LK does NOT load the ramdisk to the header's
     * ramdisk_addr; it lands right after the padded kernel), (c) legacy
     * candidate addresses. */
    handoff = build_os_handoff(entry_r0, entry_r1, entry_r2, entry_r3);
    if (place_embedded_second_stage(MVII_ARM_SECOND_STAGE_ADDR)) {
        STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_HANDOFF, "stage1: jumping to embedded OS");
        jump_to_second_stage(MVII_ARM_SECOND_STAGE_ADDR,
                             (uint32_t)(uintptr_t)handoff, 0u, 0u, 0u);
    }
    if (place_ramdisk_second_stage(MVII_ARM_SECOND_STAGE_ADDR, entry_r2)) {
        STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_HANDOFF, ramdisk_note());
        jump_to_second_stage(MVII_ARM_SECOND_STAGE_ADDR,
                             (uint32_t)(uintptr_t)handoff, 0u, 0u, 0u);
    }
    for (uint32_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (os_second_stage_present(candidates[i])) {
            STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_HANDOFF, "stage1: jumping to legacy candidate address");
            jump_to_second_stage(candidates[i], (uint32_t)(uintptr_t)handoff, 0u, 0u, 0u);
            break;
        }
    }

    STAGE1_MARK(MT6592_BOOT_STATUS_STAGE_STAGE1_STAGE2_MISSING, "stage1: no OS image found; parked");
    park_forever();
}
