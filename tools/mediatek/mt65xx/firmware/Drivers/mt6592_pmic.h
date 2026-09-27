/*
 * MT6322 PMIC on the J36 Ultra — power_supply shaped, and non-blocking.
 *
 * ══ WHAT THIS REPLACED, AND WHY ══
 *
 * The previous driver grew a battery-presence subsystem: a BATON pin watcher, a
 * 64-sample sliding window over the CS_DET charge-current comparator with duty
 * and edge-rate thresholds, a "rung" probe that walked the CV setpoint down and
 * recorded on the SD card whether the board survived, a 96-conversion voltage
 * window compared against a card-stored baseline, an operator override, a
 * one-way latch, and three verdict enums to carry the results. Around a thousand
 * lines, and every one of them was trying to answer a question this hardware
 * cannot answer.
 *
 * IT CANNOT BE ANSWERED. There is no power-path FET on this PMIC family, so the
 * charger's output IS the system node: VBAT is VSYS. With a cable in, a FULL
 * pack and an EMPTY HOLDER are the same node, measured at 4177 mV and 4178 mV
 * back to back. The one asymmetric signal — charge current — is zero in both
 * cases, because a full cell accepts nothing and a holder has nothing to accept.
 * The BATON pin never moved on this board across either state. The CS_DET duty
 * read 500, 484 and 421 per mille in three reads of the SAME state, which is
 * noise at two sigma of a comparator toggling on half of all samples.
 *
 * So this driver reports what it measures and does not report what it cannot.
 * There is no PRESENT property and no presence API. That is not a regression; it
 * is the removal of a guess that had been dressed up as a measurement, and which
 * cost two separate boot-blocking bugs while it was believed.
 *
 * ══ WHAT WAS WRONG WITH THE MEASUREMENTS THEMSELVES ══
 *
 * "It measures the power rail instead of the battery." Half true, and the half
 * that is true is hardware: BATSNS watches VBAT, VBAT is VSYS, and no choice of
 * ADC channel separates them — there is no battery-only tap on this part. What
 * IS available is the vendor's own separation, and the old driver buried it under
 * three filters:
 *
 *   BATSNS (ch 7) is the CELL side of the 68 mOhm sense resistor. VSEN (ch 6) is
 *   the SYSTEM side, measured 35..41 mV above it while charging. The difference
 *   over R_SENSE is the current; the current times the pack's own internal
 *   resistance is the IR drop; the terminal voltage minus that drop is the
 *   OPEN-CIRCUIT voltage, and the vendor's 77-row curve is indexed by exactly
 *   that. mt6592_battery_curve.h has been carrying the whole conversion, from
 *   the stock kernel, the entire time.
 *
 * Gone with it: the 1/8 EMA, the 8 mV publish deadband, the warm-up hold, the "was
 * the window below the port's baseline" trust bit, and the second gauge writer that
 * disagreed with the first. Two filters remain, both stock's: a median of five
 * conversions per channel (PMIC_AUXADC_STOCK_TIMES) and a median of five shunt
 * DELTAS for the current.
 *
 * But the curve lookup is NOT by itself the reported percentage, and it cannot be
 * on this board. With no power-path FET, VBAT is VSYS: attach a charger and the
 * terminal reading becomes the CV setpoint, so the lookup says full whatever the
 * cell holds — and a full pack and an empty holder differ by seven millivolts. The
 * reported percentage is therefore stock's oam_d_5: seeded from the PMIC's WAKEUP
 * OCV LATCH, which is a VBAT conversion the hardware took before the charger path
 * existed, and rate-limited toward the live lookup at one point per thirty seconds
 * in whichever direction the measured shunt current says charge is moving. The full
 * argument is above gauge_percent() in the .c; the consequences for a caller are
 * next to MT6592_PSY_CAPACITY below.
 *
 * ══ NOTHING HERE BLOCKS ══
 *
 * This is the other half of the complaint: "the update rate for the screen is
 * super slow, and draws the panel images in cascade." The old driver busy-waited
 * on the frame path. mt6592_pmic_battery_presence_service() was called ungated on
 * every engine turn and, once every two seconds, spent ~100 ms inside a spin loop
 * taking 96 conversions across six display frames. At 60 Hz that is six frames
 * lost in a row, twice a second — which is what a repaint looks like when you can
 * watch it arrive in bands.
 *
 * The AUXADC is a state machine now. mt6592_pmic_service() performs AT MOST ONE
 * pwrap transaction per call and returns; the 1 ms a conversion needs — the
 * vendor's own mandatory settle, not an estimate — is spent by the caller drawing,
 * not by this driver spinning. A conversion is three calls and it publishes
 * immediately; the five-sample median refines what is already on screen.
 *
 * The corollary binds the CALLER: because throughput is the call rate, a loop that
 * services the PMIC once per 30 ms sleep clocks the converter at 90 ms per
 * conversion and the gauge crawls. That was the second half of the "slow battery
 * sampling" report and it was fixed in the loop, not here — see lk_park_hold_ms()
 * in mvii_lk_main.c, which spends its wait in 1 ms slices with a service call in
 * each. Service it as often as you can; every call that has nothing due is one
 * timer read and a compare.
 *
 * The console still needs "read it now", and the console is allowed to block, so
 * that is a separate, explicitly named entry point: mt6592_pmic_sample_blocking().
 * The rule is the whole point of the split — the frame path never waits, and
 * anything that does wait says so in its name.
 */

#ifndef MT6592_PMIC_H
#define MT6592_PMIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * PROPERTIES — Linux power_supply names, Linux power_supply units.
 *
 * Microvolts and microamps at this boundary, because that is what
 * power_supply_get_property() means by VOLTAGE_NOW and CURRENT_NOW and there is
 * no reason for a second convention. The convenience struct below is in mV/mA,
 * which is what every consumer in this tree prints; mt6592_pmic_battery_read()
 * is where the division happens, in one place.
 * ══════════════════════════════════════════════════════════════════════════ */
typedef enum {
    MT6592_PSY_ONLINE = 0,              /* 1/0   supply on CHRIN                */
    MT6592_PSY_STATUS,                  /* mt6592_pmic_status_t                 */
    MT6592_PSY_VOLTAGE_NOW,             /* uV    BATSNS, the cell terminal      */
    MT6592_PSY_VOLTAGE_OCV,             /* uV    terminal with the IR drop out  */
    MT6592_PSY_CURRENT_NOW,             /* uA    + into the cell, - out of it    */
    MT6592_PSY_CAPACITY,                /* %     see the note below this enum   */
    MT6592_PSY_INPUT_VOLTAGE_NOW,       /* uV    VCHR, the cable's own voltage  */
    MT6592_PSY_CONSTANT_CHARGE_VOLTAGE, /* uV    CV setpoint, read back         */
    /* The USB side. See "THE CABLE, AND WHAT KIND OF CABLE" below. */
    MT6592_PSY_USB_ONLINE,              /* 1/0   a USB port is the supply       */
    MT6592_PSY_MAINS_ONLINE,            /* 1/0   a wall adapter is the supply   */
    MT6592_PSY_USB_TYPE,                /* mt6592_pmic_usb_type_t               */
    MT6592_PSY_CURRENT_MAX              /* uA    what the port licenses         */
} mt6592_psy_prop_t;

/*
 * ── WHAT MT6592_PSY_CAPACITY IS, AND WHAT IT DELIBERATELY IS NOT ──
 *
 * It is NOT the vendor curve looked up on the current voltage. It cannot be, on
 * this board: there is no power-path FET, so VBAT *is* VSYS, and with a cable in
 * every live channel reads the charger's CV setpoint rather than the cell. A
 * lookup therefore returns "full" at any real state of charge, which is the 99%
 * that used to be displayed for the whole time a charger was attached.  A pack
 * sitting on the 4.20 V CV rail with the integrator at 100% is published as
 * 100 -- the 99% UI hold is only for a pack that has not yet reached CV.
 *
 * It is stock's oam_d_5 (battery_meter.c, oam_run()): a percentage SEEDED from the
 * PMIC's wakeup OCV latch — a VBAT conversion the hardware took before the charger
 * path existed, and the only reading on this board that is about the cell — and
 * then moved toward the live IR-compensated curve lookup by at most one point per
 * thirty seconds, in the direction the measured shunt current says charge is
 * actually flowing. Four consequences worth knowing at the call site:
 *
 *   - It is SLOW ON PURPOSE. Plugging a cable in does not move it. Neither does
 *     a load step, a card read, or a Wi-Fi scan. If it moved quickly it would be
 *     reporting the rail, which is the bug.
 *   - It is MONOTONE in the direction of current: rising while charging, falling
 *     while discharging, never both within one interval. The one exception is a
 *     TERMINAL voltage at or under 3450 mV (stock's V_0PERCENT_TRACKING), where it
 *     walks down one percent per second toward zero whatever the current says --
 *     stock's mt_battery_0Percent_tracking_check(), ported as a ramp because it is
 *     one in the reference. It still cannot jump: a supply dip as a cable parts
 *     costs one percent, not the whole level.
 *   - IT NEVER READS 0% ON A GUESS. Nothing assigns zero to a level that was not
 *     already approaching it, so 0% means a hundred consecutive seconds of
 *     under-threshold readings and not one bad sample.
 *   - The absolute level is only as good as the wakeup latch. On a warm reboot the
 *     latch can be a boot old; the loader logs which seed it used, and there is
 *     nothing better available without interrupting the charge, which on a
 *     cell-less board costs a power cycle.
 *
 * MT6592_PSY_VOLTAGE_NOW / _OCV / CURRENT_NOW are unfiltered by comparison — they
 * are the medians of the last five conversions and they move immediately. Use
 * those to watch the hardware; use CAPACITY to show a human a charge level.
 */

/* POWER_SUPPLY_STATUS_*, same set and same meanings. */
typedef enum {
    MT6592_PMIC_STATUS_UNKNOWN = 0,
    MT6592_PMIC_STATUS_CHARGING,
    MT6592_PMIC_STATUS_DISCHARGING,
    MT6592_PMIC_STATUS_NOT_CHARGING,
    MT6592_PMIC_STATUS_FULL
} mt6592_pmic_status_t;

/* ══════════════════════════════════════════════════════════════════════════
 * THE CABLE, AND WHAT KIND OF CABLE
 *
 * MT6592_PSY_ONLINE answers "is something on CHRIN", which is a comparator on a
 * pin and is true of a 500 mA laptop port and a 2 A wall brick alike. What tells
 * those apart is BC1.2, and on this part the entire classifier -- the D+/D-
 * current source, the sink, the reference ladder and the comparator -- lives in
 * the PMIC at CHR_CON18/19. The USB PHY contributes exactly one mux bit
 * (RG_USB20_BC11_SW_EN). So this is a PMIC service, it needs no USB stack, and it
 * works in a loader that has none.
 *
 * POWER_SUPPLY_USB_TYPE_* names, because they are the names for these results.
 * NONSTANDARD has no Linux name and is not folded into DCP: it is a distinct
 * hardware outcome (DCD saw a charger, the D- probe did not answer where the spec
 * says it should) and the reference gives it its own current limit, so discarding
 * it would be discarding a measurement.
 * ══════════════════════════════════════════════════════════════════════════ */
typedef enum {
    MT6592_PMIC_USB_TYPE_UNKNOWN = 0,    /* no cable, or not classified yet     */
    MT6592_PMIC_USB_TYPE_SDP,            /* standard downstream port -- a host  */
    MT6592_PMIC_USB_TYPE_CDP,            /* charging downstream port            */
    MT6592_PMIC_USB_TYPE_DCP,            /* dedicated charging port -- a brick  */
    MT6592_PMIC_USB_TYPE_NONSTANDARD,    /* a charger, off-spec on D-           */
    MT6592_PMIC_USB_TYPE_APPLE_BRICK_ID  /* the divergent D+/D- resistor codes  */
} mt6592_pmic_usb_type_t;

/*
 * The classification, and 1/0 for the two supplies Linux would register as
 * separate power_supply devices ("usb" and "ac"). SDP and CDP are usb; DCP,
 * NONSTANDARD and Apple are ac. UNKNOWN with a cable in is neither yet -- the
 * classifier takes a moment, see below -- and is not an error.
 */
mt6592_pmic_usb_type_t mt6592_pmic_usb_type(void);
int mt6592_pmic_usb_online(void);
int mt6592_pmic_mains_online(void);

/* What the classified port licenses on its input, in mA. From this board's own
 * cust_charging.h (mediatek/custom/tinno92_wet_kk): 500 for a host, 650 for a
 * charging host, 950 for a brick, 500 for an off-spec one. 0 when unclassified,
 * which a caller must read as "do not raise the limit yet" and not as "zero".
 *
 * This is a separate accessor and not derivable from the type, because
 * APPLE_BRICK_ID is one type covering three distinguishable bricks -- 0.5 A, 1 A
 * and 2.1 A. The classifier can tell them apart and the enum cannot, so the
 * measurement is reported where it survives. */
int mt6592_pmic_input_current_limit_ma(void);

/*
 * ── THE PLUG EVENT ──
 *
 * A counter, bumped once for every cable arrival, every cable departure, and
 * every time the classification of an attached cable settles. It never resets and
 * it is wrap-safe to compare with !=.
 *
 * This is what a Linux driver's CHRDET interrupt would reach for
 * power_supply_changed() to say. There is no interrupt here -- nothing in this
 * driver blocks and nothing takes a handler -- so the edge is LATCHED instead of
 * delivered, and a consumer keeps the last value it saw and compares:
 *
 *     static uint32_t seen;
 *     uint32_t now = mt6592_pmic_charger_generation();
 *     if (now != seen) { seen = now; ...repaint, relight, re-limit... }
 *
 * A counter rather than a consume-once flag because there is more than one
 * consumer -- the park, the shell, the LED -- and a flag the first one clears is
 * an event the second one never sees. It also cannot MISS a plug: a poll slower
 * than someone waggling a cable still observes that the count moved, where a
 * level compare would have seen the same level twice and reported nothing.
 */
uint32_t mt6592_pmic_charger_generation(void);

/*
 * Re-run the classifier on the cable that is already in.
 *
 * Normally unnecessary: attach runs it once, which is what stock does. It is here
 * for the case where something else took D+/D- while a detection was in flight --
 * a gadget enumerating, a host driver probing -- because BC1.2 SOURCES CURRENT ON
 * THOSE TWO LINES and a run that overlapped one is a run whose answer is void.
 * Costs no time to ask; the work happens in mt6592_pmic_service() as usual.
 */
void mt6592_pmic_charger_retype(void);

/*
 * Read one property. 0 on success, -1 when nothing has been measured yet.
 *
 * -1 is Linux's -ENODATA, not an error: the ADC state machine needs a few
 * service calls to land its first median, and until then there is no number.
 * *val is left alone on -1, so a caller that seeds it with a default and ignores
 * the return code gets its default rather than a stale reading or a hole.
 */
int mt6592_pmic_get_property(mt6592_psy_prop_t prop, int* val);

/*
 * The same property, but pump the converter until it answers.
 *
 * BLOCKS. Up to a few milliseconds per channel, and it says so in the name
 * because the split between this and get_property() is the reason the display
 * stopped stuttering. Console commands and one-shot bring-up probes only —
 * NEVER the frame path, never a power tick, never anything a repaint waits on.
 */
int mt6592_pmic_sample_blocking(mt6592_psy_prop_t prop, int* val);

/* ══════════════════════════════════════════════════════════════════════════
 * THE CONVENIENCE SAMPLE
 *
 * One call for the handful of consumers that want the whole picture, in the
 * units they print. Every field is filled on every path, including failure —
 * a caller that ignores the return value gets defined, conservative values
 * rather than whatever was on its stack. That contract has been broken twice
 * in this driver's history and both times it put the loader into a charge park
 * it could not leave, so it is stated here and enforced in one place.
 * ══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    int charger_online;  /* 1 cable, 0 none, -1 not determined yet             */
    int status;          /* mt6592_pmic_status_t                               */
    int battery_mv;      /* BATSNS terminal voltage;   -1 nothing measured yet  */
    int ocv_mv;          /* terminal with IR removed;  -1 nothing measured yet  */
    int battery_percent; /* 0..100, MT6592_PSY_CAPACITY; -1 unknown            */
    /*
     * The same level in TENTHS of a percent, 0..1000; -1 whenever
     * battery_percent is. Always battery_percent*10 + a fraction in 0..9, so a
     * caller that shows one decimal and a caller that shows none cannot disagree.
     *
     * The fraction is only ever non-zero while a charge is being counted --- it
     * comes out of the coulomb accumulator's remainder, which is real
     * sub-percent progress. The voltage path that runs on discharge has no
     * fraction to give and reports .0, which is honest: it moves in whole
     * percents.
     *
     * This exists because a percent of this pack is ~97 s of charging. Without a
     * decimal, a screen watched for a minute looks stuck even when it is not.
     */
    int battery_permille;
    int current_ma;      /* + into the cell, - out of it. See current_valid.   */
    int current_valid;   /* 1 when current_ma is a measurement, 0 when a guess */
    int charger_mv;      /* VCHR, the cable's own voltage; -1 unknown          */
    int usb_type;        /* mt6592_pmic_usb_type_t; UNKNOWN until BC1.2 lands  */
    int input_limit_ma;  /* what usb_type licenses; 0 while UNKNOWN            */
} mt6592_pmic_battery_t;

int mt6592_pmic_battery_read(mt6592_pmic_battery_t* out);

/*
 * WHICH SYMBOL TO DRAW.
 *
 * Two states, because two states is what this board can tell apart: a cable, or
 * no cable. There used to be a third — a bare cable meaning "powered, no cell" —
 * and it is gone with the presence machinery that fed it. Nothing here can
 * distinguish a full pack from an empty holder (see the note at the top of this
 * file), so an icon claiming to is an icon that will be wrong half the time, and
 * it was: it drew "no battery" on a board with a good cell in the holder for the
 * whole time a cable was in.
 */
typedef enum {
    MT6592_PMIC_ICON_BATTERY = 0,   /* on the cell                             */
    MT6592_PMIC_ICON_BATTERY_CABLE  /* cable in                                */
} mt6592_pmic_supply_icon_t;

mt6592_pmic_supply_icon_t mt6592_pmic_supply_icon(const mt6592_pmic_battery_t* bat);

/* ══════════════════════════════════════════════════════════════════════════
 * THE SERVICE — the one thing the frame path calls.
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * Advance everything by one step. Bounded, non-blocking, safe to call as often
 * as anybody likes; each internal job has its own interval and returns
 * immediately when nothing is due.
 *
 * It does three things, in this order and for this reason:
 *
 *   1. Retry the charger-watchdog disarm until the register proves it took.
 *      FIRST, unconditionally, because this is the write that keeps a cell-less
 *      board alive and it must not be behind a converter that has wedged.
 *   2. Advance the AUXADC by one transaction on one channel, round robin.
 *   3. Every 500 ms, re-arm the charger's input path and CV target.
 */
void mt6592_pmic_service(void);

/* The old name for the same thing. Kept because thirty call sites use it and
 * they all mean "service the PMIC"; it is a straight alias, not a subset. */
void mt6592_pmic_charger_service(void);

/*
 * Called by the heavy paths on the way IN — a card read, an app launch, a Wi-Fi
 * scan — each of which steps the board's draw up sharply, with nothing between
 * that step and the rail when no cell is fitted. Runs the service and skips its
 * 500 ms gate, so the input path is open before the draw arrives rather than up
 * to half a second after it.
 */
void mt6592_pmic_power_hold(void) __attribute__((weak));

/* RTC BBPU power-off. Does not return when it works. */
void mt6592_pmic_power_off(void);

/* ══════════════════════════════════════════════════════════════════════════
 * CHARGER — the comparators, and the CV setpoint.
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * The charger block's analogue comparators, as a bitmask. Every one is a live
 * comparator on silicon, and every register/mask/shift is literal in the stock
 * kernel's own accessor — each upmu_get_rgs_* is a single
 * pmic_read_interface(reg, &v, mask, shift) and nothing else:
 *
 *   upmu_get_rgs_chr_ldo_det    (0x00, 1, 1)
 *   upmu_get_rgs_chrdet         (0x00, 1, 5)
 *   upmu_get_rgs_vcdt_lv_det    (0x00, 1, 6)
 *   upmu_get_rgs_vcdt_hv_det    (0x00, 1, 7)
 *   upmu_get_rgs_cs_det         (0x04, 1, 5)
 *   upmu_get_rgs_vbat_cv_det    (0x04, 1, 6)
 *   upmu_get_rgs_vbat_cc_det    (0x04, 1, 7)
 *
 * CS_DET is the charge-current-source comparator. It was once read as a
 * battery-presence bit, under the name CHR_CON2_CELL, and that is not what the
 * part has. It is exported here as a diagnostic and decides nothing.
 */
enum {
    MT6592_PMIC_CHG_LDO_DET     = 1u << 0, /* CHR_CON0[1]                      */
    MT6592_PMIC_CHG_CHRDET      = 1u << 1, /* CHR_CON0[5]  supply on CHRIN     */
    MT6592_PMIC_CHG_VCDT_LV_DET = 1u << 2, /* CHR_CON0[6]  input above LV vth  */
    MT6592_PMIC_CHG_VCDT_HV_DET = 1u << 3, /* CHR_CON0[7]  input above HV vth  */
    MT6592_PMIC_CHG_CS_DET      = 1u << 4, /* CHR_CON2[5]  charge current      */
    MT6592_PMIC_CHG_VBAT_CV_DET = 1u << 5, /* CHR_CON2[6]  node at CV setpoint */
    MT6592_PMIC_CHG_VBAT_CC_DET = 1u << 6, /* CHR_CON2[7]  node above CC vth   */
    MT6592_PMIC_CHG_VALID       = 1u << 7  /* the read itself succeeded        */
};

/* One read of both comparator registers, as the mask above. Cheap: two pwrap
 * transactions, no conversion. */
uint32_t mt6592_pmic_charger_flags(void);

/*
 * The CV setpoint, as CHR_CON3[4:0], and as millivolts through stock's own
 * table at 0xc0895a7c.
 *
 * Code 0 is 4200 mV and is what the service writes. The register powers on at
 * code 29 = 4162 mV on this board, which is TWENTY-ONE MILLIVOLTS BELOW the
 * node's measured 4183 mV — a CV loop asked to regulate to a voltage the pack
 * has already passed does nothing at all, because a charger cannot sink. That
 * one unwritten register was the whole of "it never charges".
 */
int  mt6592_pmic_charger_cv_code(void);
int  mt6592_pmic_charger_cv_mv(void);

/*
 * Override the setpoint by hand, for a console sweep, and hand it back.
 *
 * While a hold is in force the service leaves CHR_CON3 alone; without that the
 * next 500 ms tick would undo the sweep mid-measurement. RAISING is the safe
 * direction and LOWERING IS NOT: dropping the setpoint below the node kills a
 * cell-less board in under a millisecond, measured, because the charger output
 * is the system rail. The hold expires on its own so a console command that dies
 * cannot leave the board on a low rung.
 */
int  mt6592_pmic_charger_cv_set(uint32_t code);
void mt6592_pmic_charger_cv_release(void);

/* ══════════════════════════════════════════════════════════════════════════
 * KEYS
 * ══════════════════════════════════════════════════════════════════════════ */

/* 1 while the power key is held. One pwrap read of CHRSTATUS[1], which reads 1
 * when the key is RELEASED. */
int mt6592_pmic_pwrkey_pressed(void);

/*
 * Disarm the PMIC's own long-press key-combo reset, and dump the recon that
 * would let it be done properly.
 *
 * READ-ONLY unless built with -DMVII_MT6592_DISARM_KEY_RESET=1. TOP_RST_MISC's
 * address on the MT6322 is unconfirmed and a mis-aimed PMIC write can cut a
 * rail, so the default is to log the candidate registers and touch nothing.
 * Also the earliest point the runtime owns the PMIC, so it takes the first
 * charger-watchdog disarm attempt with it.
 */
void mt6592_pmic_disable_key_reset(void);

/* ══════════════════════════════════════════════════════════════════════════
 * DIAGNOSTICS — for the console. None of this decides anything.
 * ══════════════════════════════════════════════════════════════════════════ */

/* One PMIC register, by address. */
int mt6592_pmic_read16_pub(uint32_t addr, uint32_t* out);

/*
 * The two sense taps, raw, and the difference between them.
 *
 * BATSNS is the CELL side of the 68 mOhm shunt and VSEN is the SYSTEM side, so
 * out_delta = VSEN - BATSNS is positive when current flows INTO the cell. Both
 * channels carry the same 4x divider (cust +0x1a8 and +0x1ac), so counts are
 * directly comparable and the difference needs no scaling before the divide.
 *
 * Returns 0 when both landed. Reads the state machine's latest medians — it
 * does not force a conversion, so two calls in the same millisecond return the
 * same numbers.
 */
int mt6592_pmic_sense_pair(int* out_batsns_raw, int* out_vsen_raw,
                           int* out_delta_counts, int* out_ma);

/* Raw BATSNS counts and the counts-per-millivolt scale, so a console can show
 * the conversion rather than just its result. */
int mt6592_pmic_vbat_sample_raw(void);
int mt6592_pmic_vbat_counts_per_mv_x100(void);

/*
 * The two numbers behind MT6592_PSY_CAPACITY, for a console that has to explain a
 * percentage the operator disagrees with.
 *
 * mt6592_pmic_hw_ocv_mv()          the PMIC's wakeup OCV latch in mV, which is what
 *                                  the level was SEEDED from; -1 if the latch could
 *                                  not be believed and the seed came from the rail.
 * mt6592_pmic_soc_target_percent() the live IR-compensated curve lookup, i.e. where
 *                                  the published level is slewing TO; -1 if there is
 *                                  no sample yet. Publishing this directly is the bug
 *                                  the slew exists to prevent -- it is diagnostic
 *                                  only, and with a cable in it is a fact about the
 *                                  rail, not about the cell.
 */
int mt6592_pmic_hw_ocv_mv(void);
int mt6592_pmic_soc_target_percent(void);

/*
 * The charge-current step CHR_CON4 is programmed to, in mA, or -1 before any
 * write of it has been confirmed. One of the sixteen CS_VTH codes, so it is
 * 1600/1500/.../450/300/200/70 and nothing in between.
 *
 * NOT A MEASUREMENT -- it is what the charger has been ASKED for. There is no
 * input-side shunt on this board, so what the port actually delivers is not
 * observable; the only measured current is the pack's, in
 * mt6592_pmic_battery_t::current_ma. Both are worth showing next to each other,
 * which is what the loader's detail screen does, but they are not the same fact
 * and a caller must not present this one as one.
 */
int mt6592_pmic_charge_step_ma(void);

/* The IR correction, shown as its parts: what R the curve used at this voltage,
 * and what open-circuit voltage came out. 0 on success. */
int mt6592_pmic_gauge_ocv(int terminal_mv, int* out_ocv_mv, int* out_r_mohm,
                          int* out_charge_ma);

#ifdef __cplusplus
}
#endif

#endif /* MT6592_PMIC_H */
