/*
 * mt6592_pmic.c — MT6322/MT6323-class PMIC runtime services for the J36 Ultra.
 *
 * The contract, the units and the reasons live in mt6592_pmic.h. This file is
 * the implementation of exactly that and nothing more: a power_supply-shaped
 * read side, a non-blocking AUXADC state machine behind it, the charger writes
 * that keep a cell-less board alive, keys, and RTC power-off.
 *
 * ══ THE SHAPE OF IT ══
 *
 * Three AUXADC channels are converted, round robin, ONE PWRAP TRANSACTION PER
 * mt6592_pmic_service() CALL:
 *
 *   BATSNS  ch 7, 0x0714   the CELL side of the 68 mOhm shunt
 *   VSEN    ch 6, 0x0716   the SYSTEM side of it
 *   VCHR    ch 4, 0x0718   VCDT, the cable, through a 369/39 board divider
 *
 * Each keeps a ring of five raw samples and publishes their MEDIAN — stock's own
 * PMIC_AUXADC_STOCK_TIMES — from the FIRST sample onward, improving as the ring
 * fills rather than withholding an answer until it is full.
 *
 * A FOURTH filtered quantity is kept, and it is not a channel: the shunt DELTA,
 * one signed sample per pair of ADJACENT BATSNS/ISENSE conversions, median of
 * five. The current is read from that and never from the difference of the two
 * channels' own medians — on a board where VBAT is VSYS, those two medians can
 * straddle a plug event and their difference is then hundreds of fabricated
 * milliamps. Everything else is arithmetic:
 *
 *   the shunt delta over 68 mOhm              -> the cell current, signed
 *   BATSNS minus that current's IR drop       -> the open-circuit voltage
 *   the vendor's 77-row OCV curve             -> a TARGET percentage
 *   the wakeup OCV latch + a 1%/30 s slew     -> the PUBLISHED percentage
 *
 * That last line is the part that is easy to get wrong and this driver got wrong
 * once: see gauge_percent(). With a charger attached, every live channel here
 * measures the rail, so the published level is anchored to the PMIC's own
 * pre-charge VBAT latch and only creeps toward the live lookup.
 *
 * A conversion costs THREE service calls (write the request bit low, write it
 * high, read the data) plus the 1 ms the hardware needs in between, so a channel
 * lands a fresh sample every ~9 calls and the very first one publishes. Nothing
 * waits: the millisecond is spent as elapsed wall time by whatever the caller does
 * next, and COLLECT is a timestamp comparison before it is a transaction.
 *
 * The one exception is mt6592_pmic_sample_blocking(), which parks the round robin
 * and drives the SAME state machine in a loop with real delays — alternating the
 * two shunt channels when the property needs a current, because stock's
 * battery_meter_get_charging_current() interleaves them and an adjacent pair is
 * the only kind that means anything. There is one copy of the register recipe, so
 * the console's "read it now" cannot drift from the frame path's "read it
 * eventually".
 *
 * ══ WHAT IS NOT HERE ══
 *
 * Battery presence. It is not decidable on this board and the whole subsystem
 * that used to guess at it — BATON watcher, CS_DET duty window, CV rung probe,
 * voltage window, operator override, three verdict enums — is gone. See the note
 * at the top of the header for the measurements that retired it.
 *
 * Also gone: the SOC EMA and its 8 mV publish deadband, the warm-up hold, the
 * twenty-pair trimmed mean (a median of five deltas is the same rejection for a
 * quarter of the traffic), and the second gauge writer that disagreed with the
 * first.
 *
 * The EMA's replacement is NOT another smoother bolted onto a lookup. It is
 * stock's oam_d_5 — a seeded, direction-gated, rate-limited state variable — which
 * is a different thing with a different justification, written out in full above
 * gauge_percent().
 */

#include "mt6592_pmic.h"

#include "external_power.h"
#include "mt6592_battery_curve.h"
#include "mt6592_bootstatus.h"
#include "mt6592_delay.h"
#include "mt6592_pwrap.h"
#include "mt6592_timer.h"
#include "mt6592_uart.h"

#define BIT(n) (1u << (n))

enum {
    /* ---- Keys / reset ---------------------------------------------------- */
    PMIC_TOP_RST_MISC = 0x011au,       /* long-press reset config             */
    PMIC_RST_MISC_PWRKEY_RST_EN = BIT(6),
    PMIC_RST_MISC_HOMEKEY_RST_EN = BIT(5),
    PMIC_CHRSTATUS = 0x0142u,          /* key debounce status                 */
    PMIC_CHRSTATUS_PWRKEY_DEB = BIT(1),/* 1 = released, 0 = held              */

    /* ---- AUXADC ----------------------------------------------------------
     *
     * Every number below is stock's, from PMIC_IMM_GetOneChannelValue
     * (0xc04f18e4 in the kernel, 0x81e07e34 in the loader) which is:
     *
     *   0x0758 bit 4 <- 1                     RG_VBUF_EN, the input buffer
     *   read 0x076e mask 0x1ff, bic 1<<ch, write      the request field, LOW
     *   read 0x076e mask 0x1ff, orr 1<<ch, write      and HIGH: one edge, one
     *                                                 conversion, on the bit
     *                                                 belonging to the channel
     *   poll the channel's data register: [15] READY, [14:0] value
     *   mV = value * 1800 * 4 >> 15           channels 6 and 7
     *   mV = value * 1800     >> 15           channels 1,3,4,5,8
     *
     * 0x0758 is NOT a channel select — bit 4 is set for every channel, which is
     * why a bit walk of it found nothing — and READY IS STICKY: it is set once
     * and never observed clear, proved by 64 conversions across 67 ms with a
     * peak-to-peak spread of exactly zero counts. So the poll cannot serve as
     * the wait; the unconditional AUXADC_CONVERSION_US after the strobe is the
     * only thing standing between a strobe and a re-read of the output latch.
     */
    /*
     * ── THE CHANNEL/REGISTER MAP IS NOT GUESSABLE AND WAS GUESSED WRONG ──
     *
     * The request bit index and the data register index are two DIFFERENT
     * orderings of the same nine channels, and this file previously assumed they
     * were the same ordering. From MediaTek's own tables --
     * mediatek/platform/mt6592/lk/upmu_common.c (every upmu_get_rg_adc_out_*
     * names its AUXADC_ADCn) and the channel comment in PMIC_IMM_GetOneChannelValue
     * in the same directory's mt_pmic.c -- the two orderings are:
     *
     *   request bit (0x076e)        data register
     *   0  BATON2                   ADC0  0x0714  BATSNS
     *   1  CH6                      ADC1  0x0716  ISENSE
     *   2  THR_SENSE2               ADC2  0x0718  VCDT      <- the charger pin
     *   3  THR_SENSE1               ADC3  0x071a  BATON1
     *   4  VCDT                     ADC4  0x071c  THR_SENSE1
     *   5  BATON1                   ADC5  0x071e  THR_SENSE2
     *   6  ISENSE                   ADC6  0x0720  BATON2
     *   7  BATSNS                   ADC7  0x0722  CH5 (ACCDET)
     *   8  CH5 (ACCDET)             ADC11 0x072a  CH6
     *
     * BATSNS (7/0x0714) and ISENSE (6/0x0716) happen to coincide in both, which
     * is why they worked. VCDT does not: it is request bit 4, data register
     * 0x0718. This file asked for bit 5 and read 0x071a -- which is BATON1 in
     * both columns, so the "charger voltage" was a battery-detect pin converted
     * on a battery-detect request. Every VCHR-derived number in this driver, and
     * every conclusion drawn from one, was that pin.
     *
     * VCHARGER_CHANNEL_NUMBER is 4 in the board's own
     * custom/tinno92_wet_kk/kernel/battery/battery/cust_battery_meter.h, next to
     * VBAT 7 and ISENSE 6, which is the third independent statement of it.
     */
    PMIC_AUXADC_RQST0 = 0x0758u,       /* AUXADC_CON11                        */
    PMIC_AUXADC_RQST0_START = BIT(4),  /* RG_VBUF_EN                          */
    PMIC_AUXADC_CON = 0x076eu,         /* AUXADC_CON22, RG_AP_RQST_LIST       */
    PMIC_AUXADC_CON_FIELD = 0x01ffu,
    PMIC_AUXADC_ADC0 = 0x0714u,        /* BATSNS data                         */
    PMIC_AUXADC_BATSNS_CHANNEL = 7u,
    PMIC_AUXADC_VSEN_CHANNEL = 6u,     /* ISENSE, the system side of the shunt */
    PMIC_AUXADC_VSEN_DATA = 0x0716u,
    PMIC_AUXADC_VCHR_CHANNEL = 4u,     /* VCDT                                */
    PMIC_AUXADC_VCHR_DATA = 0x0718u,
    PMIC_AUXADC_VCHR_FULL_SCALE_MV = 1800u, /* channel 4 takes the x1 arm     */

    /*
     * ── THE ONE READING ON THIS BOARD THAT IS THE CELL AND NOT THE RAIL ──
     *
     * AUXADC_ADC8, RG_ADC_OUT_WAKEUP_PCHR: a VBAT conversion the PMIC latches AT
     * WAKEUP, before the pre-charger path is enabled. Not a live channel — there is
     * no request bit for it and nothing to strobe. One read, once, and it answers
     * the question this port could not otherwise ask: what was the battery at,
     * with nothing driving it?
     *
     * This is the whole fix for "when charging it reads 99% always". Every live
     * channel on this board measures VSYS, because there is no power-path FET and
     * VBAT *is* VSYS, so with a cable in they all read the charger's CV setpoint
     * whatever the cell is doing. The kernel's answer is not to keep looking for a
     * cleaner live channel — it is get_hw_ocv()
     * (platform/mt6592/kernel/drivers/power/battery_meter_hal.c:127), whose result
     * seeds oam_d0 and therefore the entire published SOC. It reads AUXADC_ADC8,
     * scales it with r_val_temp = 4 exactly like channels 6 and 7, and adds
     * g_hw_ocv_tune_value:
     *
     *     adc_result = (adc_result_reg * r_val_temp * VOLTAGE_FULL_RANGE) / ADC_PRECISE;
     *     adc_result += g_hw_ocv_tune_value;   // 8
     *
     * and the #if'd SWCHR_POWER_PATH arm reads ADC9 instead. This board is not
     * SWCHR (the charger is the PMIC's own linear path, CHR_CON0/CON3), so PCHR is
     * the right latch.
     */
    PMIC_AUXADC_HW_OCV_DATA = 0x0724u,     /* AUXADC_ADC8                      */
    PMIC_AUXADC_HW_OCV_TUNE_MV = 8u,       /* g_hw_ocv_tune_value              */
    PMIC_AUXADC_READY = BIT(15),
    PMIC_AUXADC_VALUE_MASK = 0x7fffu,  /* the WHOLE 15-bit field; 0x0714 has
                                        * been read with bit 14 set, so any
                                        * narrowing to 12 bits is wrong        */
    PMIC_AUXADC_VALUE_BITS = 15u,
    /*
     * VOLTAGE_FULL_RANGE. The cust-data initialiser writes it, and it is 1800 --
     * verified instruction by instruction in the stock kernel this board shipped
     * with (Reference/J36-ULTRA/kernel, gunzipped, text base 0xc0008000):
     *
     *   0xc060fc14  movw r1,#1800
     *   0xc060fc20  str  r1,[r3,#-0x1a4]     VOLTAGE_FULL_RANGE = 1800
     *   0xc060fc68  mov  r0,#4
     *   0xc060fc78  str  r0,[r3,#-0x1a8]     R_BAT_SENSE        = 4
     *   0xc060fc84  str  r0,[r3,#-0x1ac]     R_I_SENSE          = 4
     *
     * The same run of stores is where CUST_R_SENSE 68 (-0x140) and the two
     * resistances mt6592_battery_curve.h keeps at zero (-0x110, -0x16c) come
     * from, so this is the right struct and the right board.
     *
     * 1800 x 4 = 7200 mV full scale on channels 6 and 7, which puts 19181 counts
     * at 4214 mV -- a cell at its CV setpoint. (3800 was TALKING_RECHARGE_VOLTAGE
     * and never a scale.)
     *
     * THIS WAS 3600, AND THAT IS THE "READS 100% ON A FLAT PACK" BUG. It doubles
     * the full scale to 14400 mV, so every millivolt this driver reports is twice
     * what the pin measured: a cell resting at 3500 mV is published as ~7000, and
     * mt6592_battery_percent_from_ocv() returns a flat 100 for anything above the
     * top of the curve (4190 mV). That is 100% at ANY terminal voltage over
     * ~2100 mV -- which is to say always, right down to the point where the board
     * stops running. Three other things fall out of the same doubling and all of
     * them come back with it:
     *
     *   - hw_ocv_read_mv() rejects the wakeup latch every time, because 2x puts it
     *     outside PMIC_HW_OCV_MIN/MAX_MV. The one reading on this board that is
     *     the cell rather than the rail was being thrown away on every boot, and
     *     the log said so: "wakeup OCV latch unreadable".
     *   - PMIC_V_0PERCENT_TRACKING_MV (3450) is compared against the terminal
     *     reading, so the walk-down-to-empty ramp could never arm.
     *   - sense_ma_from_counts() reports twice the current, which the coulomb
     *     counter in gauge_percent() bills straight into the charging level.
     *
     * The rest of this file was always written for 7200 -- see the arithmetic
     * spelled out at sense_mv_from_counts(), sense_ma_from_counts() and
     * mt6592_pmic_vbat_counts_per_mv_x100(), and PMIC_AUXADC_VCHR_FULL_SCALE_MV
     * below, which is the SAME reference on its x1 arm and stayed at 1800. The
     * constant was the only thing that disagreed with the driver it belongs to.
     */
    PMIC_AUXADC_VOLTAGE_FULL_RANGE_MV = 1800u,
    PMIC_AUXADC_R_BAT_SENSE = 4u,
    PMIC_AUXADC_FULL_SCALE_MV =
        PMIC_AUXADC_VOLTAGE_FULL_RANGE_MV * PMIC_AUXADC_R_BAT_SENSE,
    /* `times'. Both callers of the loader's channel-7 wrapper pass 5 (0x81e178c4
     * and 0x81e1b91c), so five samples per answer is the vendor's own count. */
    PMIC_AUXADC_STOCK_TIMES = 5u,

    /* ---- Charger block --------------------------------------------------- */
    PMIC_CHR_CON0 = 0x0000u,
    PMIC_CHR_CON0_VCDT_HV_EN = BIT(0), /* upmu_set_rg_vcdt_hv_en              */
    PMIC_CHR_CON0_CHR_LDO_DET = BIT(1),
    PMIC_CHR_CON0_CSDAC_EN = BIT(3),
    PMIC_CHR_CON0_CHR_EN = BIT(4),
    PMIC_CHR_CON0_CHRDET = BIT(5),
    PMIC_CHR_CON0_VCDT_LV_DET = BIT(6),
    PMIC_CHR_CON0_VCDT_HV_DET = BIT(7),
    PMIC_CHR_CON1 = 0x0002u,
    PMIC_CHR_CON2 = 0x0004u,
    PMIC_CHR_CON2_VBAT_CV_EN = BIT(1),
    PMIC_CHR_CON2_CS_EN = BIT(3),
    /* RGS_CS_DET, the charge-current-source comparator. It was once read as a
     * cell-presence bit under the name CHR_CON2_CELL; the part has no such bit.
     * Exported as a diagnostic flag and decides nothing. */
    PMIC_CHR_CON2_CS_DET = BIT(5),
    PMIC_CHR_CON2_VBAT_CV_DET = BIT(6),
    PMIC_CHR_CON2_VBAT_CC_DET = BIT(7),
    /* [4:0] indexes stock's CV table (0xc0895a7c, transcribed as k_cv_mv below).
     * Code 0 is 4200 mV. The register powers on at 29 = 4162 mV, twenty-one
     * millivolts BELOW this pack's measured 4183 — and a CV loop asked to
     * regulate to a voltage the node has already passed does nothing at all,
     * because a charger cannot sink. That one unwritten register was the whole
     * of "it never charges". */
    PMIC_CHR_CON3 = 0x0006u,
    PMIC_CHR_CON3_CV_MASK = 0x001fu,
    PMIC_CHR_CON3_CV_4200MV = 0u,
    PMIC_CHR_CON4 = 0x0008u,           /* CS_VTH charge-current select        */
    PMIC_CHR_CON4_CS_VTH_MASK = 0x000fu,
    /* ~450 mA. Do not raise it: CS_VTH sets how hard the CSDAC drives VBAT, and
     * VBAT is VSYS here, so with no cell to clamp the node the arm sequence
     * drove it into OVP and latched the PMIC off. Measured, on this board. */
    PMIC_CHR_CON4_CS_VTH_DEFAULT = 0x000cu,
    /* Battery over-voltage protection: upmu_set_rg_vbat_ov_en (0x0c, 0x1, 0) and
     * upmu_set_rg_vbat_ov_vth (0x0c, 0x7, 1). Stock's charging_hw_init arms both
     * before it enables anything, at code 1 = 4.3 V for a non-HIGH_BATTERY board.
     * This is the comparator that stops the charger if the node runs away, which
     * on a board where VBAT is VSYS is the only thing standing between a stuck
     * CSDAC and the rail. */
    PMIC_CHR_CON6 = 0x000cu,
    PMIC_CHR_CON6_VBAT_OV_EN = BIT(0),
    PMIC_CHR_CON6_VBAT_OV_VTH_MASK = 0x0007u << 1,
    PMIC_CHR_CON6_VBAT_OV_VTH_4300MV = 0x0001u << 1,
    /* upmu_set_rg_baton_en (0x0e, 0x1, 0), upmu_set_rg_baton_ht_en (0x0e, 0x1, 1).
     * BATON is the battery-detect comparator. It decides nothing here — presence
     * stays undecidable on this board — but stock enables it in charging_hw_init
     * ahead of CHR_EN, and the charger's own state machine is documented against
     * that configuration, so it is armed the same way. HT is stock's 0. */
    PMIC_CHR_CON7 = 0x000eu,
    PMIC_CHR_CON7_BATON_EN = BIT(0),
    PMIC_CHR_CON7_BATON_HT_EN = BIT(1),
    /* The watchdog, from the stock kernel's own accessors:
     *   upmu_set_rg_chrwdt_td      (0x1a, 0xf, 0)
     *   upmu_set_rg_chrwdt_en      (0x1a, 0x1, 4)
     *   upmu_set_rg_chrwdt_wr      (0x1a, 0x1, 8)
     *   upmu_set_rg_chrwdt_int_en  (0x1e, 0x1, 0)
     *   upmu_set_rg_chrwdt_flag_wr (0x1e, 0x1, 1)
     *   upmu_get_rgs_chrwdt_out    (0x1e, 0x1, 2) */
    PMIC_CHR_CON13 = 0x001au,
    PMIC_CHR_CON13_CHRWDT_TD_MASK = 0x000fu,
    /* Code 0 is the 4 s window — "CHRWDT_TD, 4s" in stock's own comment, at every
     * one of the four sites that writes it. Named rather than a bare 0 because
     * this one is a value in a field, not a bit to clear. */
    PMIC_CHR_CON13_CHRWDT_TD_4S = 0x0000u,
    PMIC_CHR_CON13_CHRWDT_EN = BIT(4),
    PMIC_CHR_CON13_CHRWDT_WR = BIT(8),
    PMIC_CHR_CON15 = 0x001eu,
    PMIC_CHR_CON15_CHRWDT_INT_EN = BIT(0),
    PMIC_CHR_CON15_CHRWDT_FLAG_WR = BIT(1),
    PMIC_CHR_CON15_CHRWDT_OUT = BIT(2),/* 1 = the timer has already expired   */
    PMIC_CHR_CON20 = 0x0028u,
    PMIC_CHR_CON20_CSDAC_STP_INC_1 = 0x0001u,
    PMIC_CHR_CON20_CSDAC_STP_DEC_2 = 0x0020u,
    PMIC_CHR_CON21 = 0x002au,
    PMIC_CHR_CON21_CSDAC_DLY_4 = 0x0004u,
    PMIC_CHR_CON21_CSDAC_STP_1 = 0x0010u,
    /* upmu_set_rg_low_ich_db (0x2c, 0x3f, 0). Debounce on the low-charge-current
     * comparator that terminates the charge. Stock writes 1. */
    PMIC_CHR_CON22 = 0x002cu,
    PMIC_CHR_CON22_LOW_ICH_DB_MASK = 0x003fu,
    PMIC_CHR_CON22_LOW_ICH_DB_DEFAULT = 0x0001u,
    PMIC_CHR_CON23 = 0x002eu,
    PMIC_CHR_CON23_CSDAC_MODE = BIT(2),
    PMIC_CHR_CON23_HWCV_EN = BIT(6),
    /* upmu_set_rg_vcdt_mode (0x2e, 0x1, 1) and upmu_set_rg_ulc_det_en
     * (0x2e, 0x1, 7). VCDT_MODE 0 is stock's; ULC_DET_EN arms the under-load
     * current detector the termination check reads. */
    PMIC_CHR_CON23_VCDT_MODE = BIT(1),
    PMIC_CHR_CON23_ULC_DET_EN = BIT(7),
    /* VSYS under-voltage lockout: upmu_set_rg_uvlo_vthl (0x20, 0x3, 0). Below
     * the threshold the PMIC latches off, and VBAT is VSYS here, so a dip is a
     * power cut that presents as a restart. 0 is the widest dip it will ride. */
    PMIC_CHR_CON16 = 0x0020u,
    PMIC_CHR_CON16_UVLO_VTHL_MASK = 0x0003u,
    PMIC_CHR_CON16_UVLO_VTHL_LOWEST = 0x0000u,
    /*
     * ══ USB DOWNLOAD MODE, AND WHY NOTHING CHARGED ══
     *
     * upmu_set_rg_usbdl_rst (0x20, 0x1, 2), upmu_set_rg_usbdl_set (0x20, 0x1, 3).
     *
     * USBDL is a HARDWARE charging mode. The preloader's pl_charging(1) is exactly
     * hw_set_cc(450) followed by USBDL_SET = 1, and from that point the PMIC runs
     * the charge itself on a hardware state machine with its own current setting
     * and its own watchdog — CHR_CON13's, which pl_kick_chr_wdt() then has to kick
     * every four seconds. Software CHR_EN, CSDAC_EN, HWCV_EN and the CV target do
     * not drive that state machine; they are the path it displaces.
     *
     * So a loader entered from the preloader inherits a charger that is not
     * listening to it. Every register this file arms reads back exactly as
     * intended, the ladder sees VBUS, CHRDET is set, and the pack still drains
     * under the panel — because the block is in hardware mode, and its watchdog,
     * which nothing here kicks, has long since expired.
     *
     * Every charging_hw_init() in the vendor tree opens by force-leaving USBDL,
     * the PMIC HAL with usbdl_rst(1) alone and the switching-charger HAL with
     * usbdl_set(0) first. The preloader's own exit, usbdl_wo_battery_forced(0),
     * clears SET and then pulses RST, which is the order used below: dropping the
     * request before asking the latch to release cannot re-arm behind the reset.
     */
    PMIC_CHR_CON16_USBDL_RST = BIT(2),
    PMIC_CHR_CON16_USBDL_SET = BIT(3),

    /* ---- BC1.2, the USB charger classifier ------------------------------
     *
     * THE BATTERY-CHARGING COMPARATOR IS IN THE PMIC, NOT IN THE USB PHY. That is
     * the fact that puts this whole state machine in this file: on MT6592 the
     * D+/D- source, sink, reference and comparator are CHR_CON18/CHR_CON19 behind
     * pwrap (upmu_set_rg_bc11_* in the reference's pmic.c), and the only USB-side
     * involvement is one mux bit. So classifying a cable needs no USB stack, no
     * controller and no gadget -- which is what makes it available to a loader
     * whose whole job is to show a charge screen.
     *
     * Field geometry from mediatek/.../include/mach/upmu_hw.h:524-541. The 2-bit
     * fields take 0/1/2 as distinct settings and not as a bitmask, so they are
     * written as shifted values under a mask rather than OR'd bits.
     */
    PMIC_CHR_CON18 = 0x0024u,
    PMIC_CHR_CON18_BC11_BB_CTRL = BIT(0),
    PMIC_CHR_CON18_BC11_RST = BIT(1),
    PMIC_CHR_CON18_BC11_VSRC_EN_MASK = 0x3u << 2,
    PMIC_CHR_CON18_BC11_VSRC_EN_SHIFT = 2u,
    PMIC_CHR_CON18_BC11_CMP_OUT = BIT(7), /* RGS_, read-only comparator result  */
    PMIC_CHR_CON19 = 0x0026u,
    PMIC_CHR_CON19_BC11_VREF_VTH_MASK = 0x3u << 0,
    PMIC_CHR_CON19_BC11_VREF_VTH_SHIFT = 0u,
    PMIC_CHR_CON19_BC11_CMP_EN_MASK = 0x3u << 2,
    PMIC_CHR_CON19_BC11_CMP_EN_SHIFT = 2u,
    PMIC_CHR_CON19_BC11_IPD_EN_MASK = 0x3u << 4,
    PMIC_CHR_CON19_BC11_IPD_EN_SHIFT = 4u,
    PMIC_CHR_CON19_BC11_IPU_EN_MASK = 0x3u << 6,
    PMIC_CHR_CON19_BC11_IPU_EN_SHIFT = 6u,
    PMIC_CHR_CON19_BC11_BIAS_EN = BIT(8),

    /* ---- RTC (power-off lives here on this family) -----------------------
     *
     * Three things gate a power-off and this file once had one of them. From the
     * stock loader: FUN_81e08c6c writes WRTGR and polls BBPU bit 6 (CBUSY) —
     * the RTC is on a 32 kHz domain behind pwrap, so a write is not a write
     * until the bridge retires it. FUN_81e08c98 unlocks the write interface with
     * PROT = 0x586a, trigger, PROT = 0x9136, trigger; WITHOUT IT EVERY RTC WRITE
     * IS DISCARDED, which is what "power off did not latch" was reporting.
     * FUN_81e08cc0 then writes BBPU = 0x4309 — not 0x4300, whose low bits carry
     * no command — and triggers. */
    PMIC_RTC_BBPU = 0x8000u,
    PMIC_RTC_BBPU_KEY = 0x43u << 8,
    PMIC_RTC_BBPU_PWREN = BIT(0),
    PMIC_RTC_BBPU_AUTO = BIT(3),
    PMIC_RTC_BBPU_CBUSY = BIT(6),
    PMIC_RTC_WRTGR = 0x803cu,
    PMIC_RTC_PROT = 0x8036u,
    PMIC_RTC_PROT_KEY1 = 0x586au,
    PMIC_RTC_PROT_KEY2 = 0x9136u,
    /* Stock's CBUSY loop is unbounded; a wedged pwrap must not take the console
     * with it, and retiring one RTC write costs a few 32 kHz ticks. */
    PMIC_RTC_CBUSY_TRIES = 4000u,

    /* ---- SoC keypad window (recon only; see mt6592_pmic_disable_key_reset) -
     * 0x10011000, not 0x10010000: the latter is the MIPITX DSI PHY page. */
    KPD_BASE = 0x10011000u,
    KPD_DUMP_WORDS = 12u,

    /* ---- Cadences -------------------------------------------------------- */
    /* CHRDET is a live comparator on the CHRIN pin: no arming, no settling, one
     * pwrap read. It gets its own short interval because the cheap half of the
     * picture has no reason to wait on the expensive half. */
    CHARGER_DETECT_POLL_US = 100000u,
    /* Re-arm cadence for the input path. Short because the things that most need
     * it open — a card read, a Wi-Fi scan, an app launch — are also the things
     * that stop the frame pump for hundreds of milliseconds; those paths force a
     * re-arm through mt6592_pmic_power_hold() rather than trusting this. */
    CHARGER_KICK_INTERVAL_US = 500000u,
    /* And the cadence after a kick that could not be attempted at all. Must stay
     * strictly under the interval above or the retry is not a retry; the static
     * assert below enforces it, because that arithmetic was once silently a
     * no-op for exactly this reason. */
    CHARGER_KICK_RETRY_US = 50000u
};

/* C89-compatible compile-time check: a zero-width array is a hard error, and
 * this file is built without <assert.h>. */
typedef char mt6592_pmic_kick_retry_shorter_than_interval
    [(CHARGER_KICK_RETRY_US < CHARGER_KICK_INTERVAL_US) ? 1 : -1];

/*
 * The x4 arm is 7200 mV and nothing else, pinned here because it has been wrong
 * once and because it is wrong SILENTLY: every millivolt, milliamp and
 * percentage this driver publishes is scaled by it, none of them is checked
 * against anything, and the first symptom is a gauge that reads full on a flat
 * pack. stage1.c writes the same 7200 out longhand (PMIC_AUXADC_FULL_SCALE_MV,
 * stage1.c:141) and the console's `bat' and `batscan' print on it too; they
 * cannot include this header, so the number has to be defended where it is
 * defined. See the provenance on PMIC_AUXADC_VOLTAGE_FULL_RANGE_MV.
 */
typedef char mt6592_pmic_auxadc_x4_arm_is_7200mv
    [(PMIC_AUXADC_FULL_SCALE_MV == 7200) ? 1 : -1];
/* And the x1 arm is the same reference undivided, which is what makes the x4 a
 * x4. If these two ever stop agreeing, one of them has been edited alone. */
typedef char mt6592_pmic_auxadc_arms_share_one_reference
    [(PMIC_AUXADC_FULL_SCALE_MV ==
      PMIC_AUXADC_VCHR_FULL_SCALE_MV * PMIC_AUXADC_R_BAT_SENSE)
         ? 1
         : -1];

/* Key-combo reset disarm: READ-ONLY by default. Until the MT6322's TOP_RST_MISC
 * address is confirmed from the logged dump, no PMIC or KPD register is written
 * — a mis-aimed PMIC write can cut a rail. Build with
 * -DMVII_MT6592_DISARM_KEY_RESET=1 to arm the clearing (the A+B reboot fix). */
#ifndef MVII_MT6592_DISARM_KEY_RESET
#define MVII_MT6592_DISARM_KEY_RESET 0
#endif

/*
 * ── THE SHUNT AND THE ZERO, BOTH FROM THE STOCK IMAGE ──
 *
 * g_Get_I_Charging finishes with
 *   I(mA) = 1000 * ((I_SENSE_mV - BAT_SENSE_mV) + [cust+0x7b8]) / [cust+0x140]
 * and both operands are in the image even though the struct is in .bss:
 * `mov r0,#68 ; str r0,[r3,#0x140]' at 0xc060fc60 is CUST_R_SENSE in milliohms,
 * and +0x7b8's only writer (0xc0612710) is called twice, both times with 0.
 *
 * The board divider in front of CHRIN is the same story: 330 at cust +0x1b0 and
 * 39 at +0x1b4 are R_CHARGER_1/2, and battery_meter_get_charger_voltage()
 * returns pin_mV * (R1 + R2) / R2. 369/39 is 9.46, so the pin's 1800 mV full
 * scale is 17046 mV at the cable — which is why a saturated conversion there now
 * means a fault rather than a units error.
 */
enum {
    MT6592_PMIC_R_SENSE_MOHM = 68,      /* cust +0x140                        */
    MT6592_PMIC_I_SENSE_OFFSET_MV = 0,  /* cust +0x7b8, set to 0 by stock     */
    MT6592_PMIC_R_CHARGER_1 = 330u,     /* cust +0x1b0                        */
    MT6592_PMIC_R_CHARGER_2 = 39u,      /* cust +0x1b4                        */
    MT6592_PMIC_VCHR_DIVIDER_NUM = MT6592_PMIC_R_CHARGER_1 + MT6592_PMIC_R_CHARGER_2,
    MT6592_PMIC_VCHR_DIVIDER_DEN = MT6592_PMIC_R_CHARGER_2
};

/*
 * ── THE GAUGE'S POLICY CONSTANTS, ALL OF THEM THE VENDOR'S ──
 *
 * 3500 mV is the stock LK's only battery number (0xdac, twice: `mv < 0xdac' at
 * 0x81e178c4 and `0xdab < mv' at 0x81e1b91c). The rest is the kernel's cust
 * block and the state machine that consumes it:
 *
 *   cust +0x90 = 4050  V_CC2TOPOFF_THRES    constant current -> top off
 *   cust +0x94 = 4110  RECHARGING_VOLTAGE   full -> charge again
 *   cust +0x98 =  150  CHARGING_FULL_CURRENT
 *
 * and in BAT_TopOffModeAction at 0xc0618f08:
 *   0xc0618fb8  if (ICharging > charging_full_current) count = 0;
 *   0xc0618fcc  else if (++count == 6)  -> bat_full
 * SIX CONSECUTIVE SAMPLES. This driver once called a pack full on one, of a
 * quantity stock's own divide can only resolve in 14.7 mA steps.
 */
enum {
    /*
     * V_0PERCENT_TRACKING, cust_charging.h:52. The voltage at which stock starts
     * WALKING the displayed level down to zero -- not the voltage at which it
     * displays zero. See the ramp in gauge_percent(); the distinction is the
     * whole of the "reads 0% the moment the cable comes out" bug.
     *
     * Compared against the TERMINAL reading, as stock compares BMT_status.bat_vol,
     * and not against the IR-compensated OCV. Compensating first would let a
     * current that reads positive drag the comparison under the threshold, which
     * is the one direction this must not be sensitive to.
     */
    PMIC_V_0PERCENT_TRACKING_MV = 3450,
    /* SYSTEM_OFF_VOLTAGE, battery_common.h:14. Stock powers off at this MINUS
     * 100 mV, and only after low_power_count > 6 consecutive polls with the level
     * already at zero (battery_common.c:1616). Recorded for the provenance of the
     * threshold above; nothing in this loader powers the board off. */
    PMIC_SYSTEM_OFF_MV = 3400,
    PMIC_CHARGING_FULL_CURRENT_MA = 150,
    PMIC_V_CC2TOPOFF_MV = 4050,
    PMIC_RECHARGING_MV = 4110,
    /*
     * The linear charger's CV rail.  A pack still in constant-current sits
     * well below this even with IR; 4246 mV on the detail screen IS this
     * rail.  The 150 mA termination never completes here because the shunt
     * includes the system's own draw (hundreds of mA while the cell takes
     * none), so a full pack was held at the 99% UI cap forever.  Sitting
     * on this rail with the integrator at 100% is full.
     */
    PMIC_CV_HOLD_MV = 4200,
    PMIC_FULL_CHECK_TIMES = 6,

    /*
     * d5_count_time, from oam_run(): the published SOC moves one percent per
     * interval and no faster. Stock adds 10 to d5_count per call and steps when it
     * reaches 30, on a 10 s poll — thirty seconds per percent. Kept exactly,
     * because the rate is not a display preference: it is what keeps the number
     * from following the charger's rail instead of the cell.
     */
    PMIC_SOC_SLEW_INTERVAL_US = 30000000,

    /*
     * gFG_BATT_CAPACITY_aging, in mAh. Stock sets it to Q_MAX_POS_25
     * (battery_meter.c:1430), which this board's cust profile puts at 1499
     * (custom/mt6592/kernel/battery/battery/cust_battery_meter.h:39). The same
     * file's CUST_R_SENSE is 68, matching the shunt this driver already divides
     * by, so this is the right pack profile and not a sibling board's.
     *
     * It exists for the charging branch of gauge_percent(), which counts coulombs
     * the way oam_run() does rather than chasing the curve.
     */
    PMIC_BATT_CAPACITY_MAH = 1499,

    /*
     * HOW LONG A CHARGE RUN SURVIVES A GAP IN THE MEASUREMENT.
     *
     * VBUS on this board drops out on its own. The panel's inrush pulls the input
     * under the VCDT threshold during bring-up, and the console shows CHRDET
     * blinking off and back mid-park with the pack current never leaving 470-563
     * mA on either side of it. Every one of those blinks used to end the run: the
     * accumulator was zeroed and re-based, so the partial percent it had built up
     * was thrown away, and the voltage slew got a sample to ratchet DEPLETION up
     * on. A percent takes ~97 s to earn at 550 mA and a dropout arrives sooner
     * than that, so the level could never finish one and lost ground on each --
     * which is exactly "69% and going down while the trend goes up".
     *
     * Inside this window a gap is treated as a gap: the run's clock is advanced so
     * the missing time is not billed as coulombs, nothing is discarded, and the
     * voltage path is not allowed to touch the level. Past it, the cable really is
     * out and the voltage path takes over from wherever the count had reached.
     */
    PMIC_SOC_CAR_GRACE_US = 10000000,

    /*
     * How far apart two rungs of the full-detect ladder must be.
     *
     * "SIX CONSECUTIVE SAMPLES" is only a claim about elapsed time if the samples
     * are spaced, and stock's are: BAT_TopOffModeAction runs from BAT_thread at
     * BAT_TASK_PERIOD, which is 10 s in battery_common.h — six samples is a
     * MINUTE of sustained low current. This driver's converter lands a fresh
     * BATSNS median every ~9 ms when a park is pumping it hard, so a
     * generation-counted ladder would call a pack full in sixty MILLISECONDS, off
     * a transient, and unlock the 100% display on a board that is nowhere near it.
     *
     * 1 s, which is the LK's OWN BAT_TASK_PERIOD (mt_bat_bq24196.c:101) rather
     * than the kernel's 10 s — this is a loader, a park may only live for half a
     * minute, and six seconds of steady sub-150 mA is a real observation where
     * sixty milliseconds is noise.
     */
    PMIC_LADDER_INTERVAL_US = 1000000,

    /* The wakeup latch is only believed inside a window no cell on this board can
     * be outside of. Below 2500 mV the part would not be running; above 4500 mV no
     * CV code in k_cv_mv can even be selected. Outside it, the register is stale,
     * unlatched, or not what this driver thinks it is. */
    PMIC_HW_OCV_MIN_MV = 2500,
    PMIC_HW_OCV_MAX_MV = 4500
};

/* Stock's CV table at 0xc0895a7c, indexed by CHR_CON3[4:0]. Index 0 is the
 * 4200 mV that CHARGING_CMD_SET_CV_VOLTAGE resolves 4200000 uV to. */
static const int16_t k_cv_mv[32] = {
    4200, 4212, 4225, 4237, 4250, 4262, 4275, 4300,
    4325, 4350, 4375, 4400, 4425, 4162, 4175, 2200,
    4050, 4100, 4125, 3775, 3800, 3850, 3900, 4000,
    4050, 4100, 4125, 4137, 4150, 4162, 4175, 4187
};

static int g_key_reset_disabled;
static int g_chrwdt_expiry_logged;
static int g_chrwdt_armed_logged;
static uint64_t g_last_charger_kick_us;
static uint64_t g_last_chrdet_poll_us;
/* -1 until one CHRDET read has succeeded. Everything downstream treats -1 as
 * "not determined yet" and never as "no cable". */
static int g_chrdet_cached = -1;

/* The wakeup OCV latch, read once at init; -1 if it could not be believed. */
static int g_hw_ocv_mv = -1;
/* oam_d_5: published DEPLETION in percent, -1 until seeded. The percentage every
 * caller sees is 100 minus this, and it moves one step per
 * PMIC_SOC_SLEW_INTERVAL_US. See gauge_percent(). */
static int g_soc_dep = -1;
static uint64_t g_soc_slew_us;
/* The BATSNS generation the integrator last consumed, so a hundred readers
 * between two samples do not each get a step. */
static uint32_t g_soc_gen;

/*
 * oam_car / oam_d0, for the charging branch.
 *
 * g_soc_car_base is the depletion the pack was at when the current charge run
 * started (oam_d0) and is -1 whenever no run is in progress. g_soc_car is the
 * charge delivered since then in mA*us, which is the product this driver already
 * has to hand: a current in whole mA and a timer in whole us. int64_t because a
 * single hour at 500 mA is 1.8e12 of them.
 *
 * A run ends the moment a sample comes back not-charging, which re-bases the
 * next one at wherever the level had got to. Nothing carries across an unplug.
 */
static int g_soc_car_base = -1;
static int64_t g_soc_car;
static uint64_t g_soc_car_us;
/* The last sample that actually had current going into the pack, which is what
 * PMIC_SOC_CAR_GRACE_US is measured against. */
static uint64_t g_soc_charge_us;

/* mA*us that move the level one percent: a percent of the pack is
 * PMIC_BATT_CAPACITY_MAH/100 mAh, and an mAh is 3.6e9 mA*us. */
#define PMIC_SOC_CAR_PER_PCT ((int64_t)36000000 * (int64_t)PMIC_BATT_CAPACITY_MAH)

/*
 * Two log sinks, deliberately. mt6592_uart_puts() is the live serial log;
 * mt6592_bootstatus_log_text() reaches the eMMC ring that
 * `flash -mtk-read-boot-status` reads back afterwards. Power faults are
 * diagnosed after the fact, so anything about the supply, the charger or its
 * watchdog goes to both — with no cell fitted, "the box turned itself off" is
 * precisely the case where UART-only reaches nobody. Periodic register dumps
 * stay UART-only; the ring is small and boot evidence is worth more in it.
 */
static void plog(const char* s) {
    mt6592_uart_puts(s);
    mt6592_bootstatus_log_text(s);
}

static void plog_hex32(uint32_t value) {
    static const char hex[] = "0123456789abcdef";
    char out[11];
    out[0] = '0';
    out[1] = 'x';
    for (uint32_t i = 0; i < 8u; ++i) {
        out[2u + i] = hex[(value >> ((7u - i) * 4u)) & 0x0fu];
    }
    out[10] = 0;
    plog(out);
}

static void plog_dec(uint32_t value) {
    char out[11];
    uint32_t i = 10u;
    out[10] = 0;
    if (value == 0u) {
        plog("0");
        return;
    }
    while (value != 0u && i != 0u) {
        out[--i] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    plog(&out[i]);
}

/*
 * The one-shot charger recon, and it goes to BOTH sinks per the note above.
 *
 * There was a UART-only twin of this for a while, on the rule that register dumps
 * stay off the ring. The rule is about PERIODIC dumps -- this one fires once per
 * cable insertion, and it is the single most useful thing in the ring when a
 * board with no cell fitted turns itself off: the charger's state at the moment
 * the input path was armed, readable after the fact with
 * `flash -mtk-read-boot-status'. UART-only reaches nobody in exactly that case.
 */
static void plog_reg(const char* name, uint32_t addr) {
    uint32_t v = 0;
    const int rc = mt6592_pwrap_read(addr, &v);
    plog("  pmic: ");
    plog(name);
    plog("[");
    plog_hex32(addr);
    plog("]=");
    if (rc == 0) {
        plog_hex32(v);
    } else {
        plog("<read-fail>");
    }
    plog("\n");
}

static int pwrap_up(void) {
    /* Read-only readiness check. Never run the pwrap COLD init from the runtime:
     * it soft-resets the wrapper the live display rails depend on. stage1 and the
     * preloader own cold init; if INIT_DONE is not set here, PMIC services simply
     * stay unavailable. */
    static int logged;
    if (mt6592_pwrap_is_ready()) return 1;
    if (!logged) {
        logged = 1;
        plog("pmic: pwrap not INIT_DONE; PMIC services disabled\n");
    }
    return 0;
}

/*
 * ── THE CHARGER WATCHDOG ──
 *
 * This function used to disarm the timer, once per boot, before the charger was
 * ever enabled. It was written to stop a board with no cell fitted rebooting
 * itself a few seconds after the boot logo, and it did stop that. It also stopped
 * the board charging, and that took much longer to see, because a disarmed
 * watchdog leaves every charger register reading back exactly as it should.
 *
 * CHRWDT is a hardware timer inside the charger block, and on this PMIC the block
 * gates on it. Every stock path that turns this charger on arms and kicks the
 * watchdog in the same breath, immediately before CSDAC_EN and CHR_EN, and the
 * one path that turns the charger off turns the watchdog off with it:
 *
 *   preloader  hw_set_cc()            TD=0, WR=1, INT_EN=1, EN=1, FLAG_WR=1,
 *                                     then CSDAC_EN=1, HWCV_EN=1, CHR_EN=1
 *   LK         pchr_turn_on_charging() kick_charger_wdt(), then CS_VTH,
 *                                     CSDAC_EN=1, CHR_EN=1  -- and that IS the
 *                                     whole of stock LK's charge enable
 *   kernel     charging_enable(TRUE)   ... EN=1, FLAG_WR=1 ... CSDAC_EN, CHR_EN
 *   kernel     charging_enable(FALSE)  INT_EN=0, EN=0, FLAG_WR=0, CSDAC_EN=0,
 *                                     CHR_EN=0, HWCV_EN=0
 *
 * Watchdog armed if and only if charging, in all three, with no exception. So the
 * timer gets armed here, and the deal it represents gets honoured: this is called
 * from charger_arm(), which runs every CHARGER_KICK_INTERVAL_US (500 ms) while a
 * cable is in, well inside the 4 s the TD field below selects. That is strictly
 * safer than what the disarm replaced, which was a timer the preloader had ALREADY
 * armed and that nothing then kicked -- the expiry, not the arming, is what took
 * VBAT down on a cell-less board.
 *
 * Two deliberate departures from stock, both about the expiry rather than the
 * timer:
 *
 *   INT_EN stays 0. Stock sets it and has a handler; this firmware has neither a
 *   handler nor anything to do with the news. Masked, an expiry is a charge that
 *   stops, not an interrupt into nowhere.
 *
 *   FLAG_WR is written 1, not 0. This is the bit the disarm got backwards. Stock
 *   writes 1 to it in all three enable paths and 0 only in charging_enable(FALSE):
 *   1 clears the latched expiry, 0 is part of shutting the timer down. Writing 0
 *   to clear a latch leaves the latch exactly where it was.
 *
 * CON13 carries both the kick (WR, bit 8) and the enable (EN, bit 4), so both go
 * in one write; CON15's latch clear goes first, so the kick lands on a clean flag.
 */
/*
 * The other half of the deal, and stock's charging_enable(FALSE) in the same order:
 * mask, stop, release the flag. Called when the cable is out, i.e. when the kick
 * above is no longer going to happen — an armed timer that nothing kicks is exactly
 * the arrangement that took a cell-less board's own rail down, and leaving one
 * behind on unplug would be that bug with a cable-shaped trigger.
 *
 * FLAG_WR = 0 here, not 1: shutting the timer down, not clearing a latch.
 */
static void charger_watchdog_disarm(void) {
    uint32_t reg = 0;

    if (!pwrap_up()) return;

    if (mt6592_pwrap_read(PMIC_CHR_CON15, &reg) == 0) {
        (void)mt6592_pwrap_write(PMIC_CHR_CON15,
                                 reg & ~(uint32_t)PMIC_CHR_CON15_CHRWDT_INT_EN);
    }
    if (mt6592_pwrap_read(PMIC_CHR_CON13, &reg) == 0) {
        (void)mt6592_pwrap_write(PMIC_CHR_CON13,
                                 reg & ~(uint32_t)PMIC_CHR_CON13_CHRWDT_EN);
    }
    if (mt6592_pwrap_read(PMIC_CHR_CON15, &reg) == 0) {
        (void)mt6592_pwrap_write(PMIC_CHR_CON15,
                                 reg & ~(uint32_t)PMIC_CHR_CON15_CHRWDT_FLAG_WR);
    }

    /* Re-armed from scratch on the next insertion, so both log lines are worth
     * having again — a second plug that fails to arm is a different fault from a
     * first one that did. */
    g_chrwdt_armed_logged = 0;
    g_chrwdt_expiry_logged = 0;
}

static void charger_watchdog_kick(void) {
    uint32_t con13 = 0;
    uint32_t con15 = 0;

    if (!pwrap_up()) return;

    if (mt6592_pwrap_read(PMIC_CHR_CON15, &con15) == 0) {
        /* Worth one line the first time: OUT set means the timer really had run
         * out, which is the direct confirmation that this was the thing holding
         * the charger off rather than a plausible story about it. */
        if (!g_chrwdt_expiry_logged && (con15 & PMIC_CHR_CON15_CHRWDT_OUT) != 0u) {
            g_chrwdt_expiry_logged = 1;
            plog("pmic: CHRWDT had expired (CON15 OUT set); clearing the latch\n");
        }
        (void)mt6592_pwrap_write(PMIC_CHR_CON15,
                                 (con15 & ~(uint32_t)PMIC_CHR_CON15_CHRWDT_INT_EN) |
                                     (uint32_t)PMIC_CHR_CON15_CHRWDT_FLAG_WR);
    }

    if (mt6592_pwrap_read(PMIC_CHR_CON13, &con13) != 0) return;
    (void)mt6592_pwrap_write(PMIC_CHR_CON13,
                             (con13 & ~(uint32_t)PMIC_CHR_CON13_CHRWDT_TD_MASK) |
                                 (uint32_t)PMIC_CHR_CON13_CHRWDT_TD_4S |
                                 (uint32_t)PMIC_CHR_CON13_CHRWDT_EN |
                                 (uint32_t)PMIC_CHR_CON13_CHRWDT_WR);

    /* Readback, not "the write returned 0": pwrap acknowledges a transaction it
     * delivered and cannot promise the charger block honoured it. A watchdog that
     * silently refused to arm is the no-charge bug again, and this is the only
     * place it would be visible. */
    if (!g_chrwdt_armed_logged && mt6592_pwrap_read(PMIC_CHR_CON13, &con13) == 0) {
        g_chrwdt_armed_logged = 1;
        plog((con13 & PMIC_CHR_CON13_CHRWDT_EN) != 0u
                 ? "pmic: charger watchdog armed and kicked (CON13="
                 : "pmic: charger watchdog REFUSED TO ARM (CON13=");
        plog_hex32(con13);
        plog(")\n");
    }
}

/*
 * ── KEY-COMBO RESET ──
 *
 * Read-only recon unless MVII_MT6592_DISARM_KEY_RESET is defined. The MT6592's
 * A and B buttons are keypad matrix pins, and something — the PMIC's long-press
 * reset, or the SoC keypad block's own — reboots the board when both are held.
 * The dump identifies which before this file writes anything, because a
 * mis-addressed PMIC write can drop a rail the display is running on.
 */
void mt6592_pmic_disable_key_reset(void) {
    uint32_t misc = 0;
    uint32_t i;

    if (g_key_reset_disabled) return;
    g_key_reset_disabled = 1;

    plog("pmic: key-reset recon\n");
    for (i = 0u; i < KPD_DUMP_WORDS; ++i) {
        const uint32_t addr = KPD_BASE + i * 4u;
        const uint32_t v = *(volatile uint32_t*)(uintptr_t)addr;
        plog("  kpd[");
        plog_hex32(addr);
        plog("]=");
        plog_hex32(v);
        plog("\n");
    }

    if (!pwrap_up()) return;
    if (mt6592_pwrap_read(PMIC_TOP_RST_MISC, &misc) != 0) {
        plog("pmic: TOP_RST_MISC read failed\n");
        return;
    }
    plog("  pmic: TOP_RST_MISC=");
    plog_hex32(misc);
    plog("\n");

#if MVII_MT6592_DISARM_KEY_RESET
    {
        const uint32_t want =
            misc & ~(uint32_t)(PMIC_RST_MISC_PWRKEY_RST_EN | PMIC_RST_MISC_HOMEKEY_RST_EN);
        if (want != misc && mt6592_pwrap_write(PMIC_TOP_RST_MISC, want) == 0) {
            plog("  pmic: TOP_RST_MISC <- ");
            plog_hex32(want);
            plog("\n");
        }
    }
#else
    plog("  pmic: disarm not built in (MVII_MT6592_DISARM_KEY_RESET=0)\n");
#endif
}

int mt6592_pmic_pwrkey_pressed(void) {
    uint32_t status = 0;

    if (!pwrap_up()) return -1;
    if (mt6592_pwrap_read(PMIC_CHRSTATUS, &status) != 0) return -1;

    /* PWRKEY_DEB is the debounced level and reads 1 when the key is RELEASED. */
    return (status & PMIC_CHRSTATUS_PWRKEY_DEB) ? 0 : 1;
}

/*
 * ── OPERATOR CV HOLD ──
 *
 * `batcal` writes CHR_CON3 by hand. The periodic re-arm would otherwise stamp
 * 4200 mV back over it within half a second and the operator would watch their
 * setpoint evaporate. A hold with a linger keeps the driver's hands off the
 * register for a few seconds after each manual write, which is long enough to
 * read a current back and short enough that a forgotten override cannot leave
 * the board on a wrong setpoint indefinitely.
 */
enum { CV_HOLD_LINGER_US = 3000000u };

static uint64_t g_cv_hold_us;

static int cv_operator_holds(uint64_t now) {
    if (g_cv_hold_us == 0u) return 0;
    if (now < g_cv_hold_us) return 1; /* clock went backwards; assume held */
    return (now - g_cv_hold_us) < (uint64_t)CV_HOLD_LINGER_US;
}

int mt6592_pmic_charger_cv_code(void) {
    uint32_t con3 = 0;

    if (!pwrap_up()) return -1;
    if (mt6592_pwrap_read(PMIC_CHR_CON3, &con3) != 0) return -1;
    return (int)(con3 & PMIC_CHR_CON3_CV_MASK);
}

int mt6592_pmic_charger_cv_mv(void) {
    const int code = mt6592_pmic_charger_cv_code();
    if (code < 0) return -1;
    return (int)k_cv_mv[(uint32_t)code & 0x1fu];
}

int mt6592_pmic_charger_cv_set(uint32_t code) {
    uint32_t con3 = 0;
    uint32_t want;

    if (!pwrap_up()) return -1;
    if (mt6592_pwrap_read(PMIC_CHR_CON3, &con3) != 0) return -1;

    want = (con3 & ~(uint32_t)PMIC_CHR_CON3_CV_MASK) | (code & PMIC_CHR_CON3_CV_MASK);
    if (mt6592_pwrap_write(PMIC_CHR_CON3, want) != 0) return -1;

    /* Refresh the hold on every accepted write, including a no-op one: the
     * operator asked for this value, so keep the re-arm out of it either way. */
    g_cv_hold_us = mt6592_timer_microseconds();

    if (mt6592_pwrap_read(PMIC_CHR_CON3, &con3) != 0) return -1;
    return ((con3 & PMIC_CHR_CON3_CV_MASK) == (code & PMIC_CHR_CON3_CV_MASK)) ? 0 : -1;
}

void mt6592_pmic_charger_cv_release(void) {
    g_cv_hold_us = 0u;
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE AUXADC
 * ══════════════════════════════════════════════════════════════════════════
 *
 * A conversion is THREE pwrap transactions and one wait, and this file spends
 * exactly ONE transaction per mt6592_pmic_service() call. That is the whole
 * reason this is a state machine and not a function with a loop in it: the old
 * driver's ninety-six-conversion sweep held the CPU for something like a hundred
 * milliseconds inside a frame, so the compositor missed its window and repainted
 * in bands — the "cascading" the operator reported. Nothing here waits on
 * hardware while a caller is trying to draw.
 *
 * The wait is spent as elapsed wall time, not as a delay: COLLECT compares a
 * timestamp and returns if the millisecond is not up, so it belongs to whatever
 * the caller does next. Only the blocking pump converts it into a real
 * mt6592_delay_cycles(), because there its caller has nothing else to do.
 *
 * ── WHY THREE AND NOT EIGHT ──
 *
 * It was eight, and eight is why the gauge was unusable. The tick that drives
 * this runs at POWER_TICK_INTERVAL_US = 20 ms (the OS runtime compat layer) and the
 * charge park at ~30 ms, so eight steps was 160 ms per conversion; five
 * conversions before anything was published was 800 ms per channel and, round
 * robin over three channels, 2.4 SECONDS before the status bar could draw a
 * number at all. That is the "extremely slow battery sampling" -- it was not the
 * drawing, it was the driver refusing to answer for two and a half seconds.
 *
 * Five of those eight steps were read-modify-write ceremony. Stock re-reads
 * AUXADC_CON22 before each of its two writes because in the kernel the request
 * list is shared with accdet, the thermal driver and the audio path; in this
 * loader nothing else touches it, and the field is register-governed -- hardware
 * never writes it back. So the value is read ONCE into a shadow, and after that a
 * conversion is: write the bit low, write it high, read the data. RG_VBUF_EN gets
 * the same treatment: it is not a channel select, so it is enabled once and
 * latched. Any pwrap failure drops both caches and the next conversion re-reads,
 * which is the only case where the shadow could have gone stale.
 */
enum {
    /*
     * ONE MILLISECOND, AND IT IS NOT NEGOTIABLE.
     *
     * MediaTek's kernel-side PMIC_IMM_GetOneChannelValue
     * (platform/mt6592/kernel/drivers/power/pmic.c) strobes the request bit and
     * then does, in full:
     *
     *     //Duo to HW limitation
     *     if(dwChannel!=8)
     *     msleep(1);
     *
     * before it looks at READY at all -- and then polls READY in further 1 ms
     * steps. That blind millisecond is the vendor's own statement that READY does
     * not go clear on the request edge, so an early read returns the previous
     * conversion and returns it with READY set.
     *
     * This file used to wait 150 us on the strength of a bench measurement that a
     * 150 us read "was not stale". It cannot distinguish those two cases: the
     * quantity it compared was the value, and the value barely moves between two
     * conversions of the same slow rail. Under a step -- exactly the plug and
     * unplug the operator reported as a flaky window -- one conversion of lag is
     * one whole wrong reading, and on the current pair it is a wrong DIFFERENCE,
     * which is worse: BATSNS from after the step minus ISENSE from before it is
     * hundreds of milliamps of pure fiction.
     *
     * It costs nothing to obey. On the frame path the wait is elapsed wall time
     * (see ADC_STEP_COLLECT), and the service is called at 20 ms, so a 1 ms settle
     * and a 150 us settle are the same single call.
     */
    AUXADC_CONVERSION_US = 1000u,
    /* Bounded because a wedged wrapper must not become a spin. On the frame path
     * each poll is a separate service call, so this is 256 frames, not 256 reads
     * back to back. */
    AUXADC_POLL_LIMIT = 256u,
    /* The blocking pump gets a much tighter one — it is holding the CPU. */
    AUXADC_BLOCKING_POLL_LIMIT = 64u
};

enum { ADC_BATSNS = 0u, ADC_VSEN = 1u, ADC_VCHR = 2u, ADC_CHANNELS = 3u };

/* Five, from stock: both callers of the loader's channel-7 wrapper pass 5, and
 * the kernel's battery_meter_get_battery_voltage() also asks for 5. A median of
 * five rejects a single bad conversion completely, which is the only failure this
 * ADC actually shows, and costs nothing to compute.
 *
 * Worth knowing what stock's `times' actually buys, because it is not five
 * conversions: the LK's PMIC_IMM_GetOneChannelValue strobes ONCE, outside its
 * sampling loop, and then reads the same data register `deCount' times and
 * averages. With READY sticky and the latch unchanged until the next strobe, all
 * five reads are the same number and the average is that number. Five samples is
 * therefore this driver's own noise handling, not the vendor's -- which is why it
 * is a median (throws out an outlier) rather than a mean (spreads it over five). */
enum { ADC_MEDIAN = PMIC_AUXADC_STOCK_TIMES };

typedef struct {
    uint32_t channel;  /* AUXADC channel number, = the bit in CON            */
    uint32_t data_reg; /* where its result latches                           */
    int ring[ADC_MEDIAN];
    uint32_t n;        /* samples held, saturating at ADC_MEDIAN             */
    uint32_t pos;      /* next ring slot                                     */
    int median;        /* published raw counts; -1 before the first sample   */
    uint32_t updates;  /* generation counter, bumped on every commit         */
} adc_chan_t;

static adc_chan_t g_adc[ADC_CHANNELS] = {
    {PMIC_AUXADC_BATSNS_CHANNEL, PMIC_AUXADC_ADC0, {0, 0, 0, 0, 0}, 0u, 0u, -1, 0u},
    {PMIC_AUXADC_VSEN_CHANNEL, PMIC_AUXADC_VSEN_DATA, {0, 0, 0, 0, 0}, 0u, 0u, -1, 0u},
    {PMIC_AUXADC_VCHR_CHANNEL, PMIC_AUXADC_VCHR_DATA, {0, 0, 0, 0, 0}, 0u, 0u, -1, 0u}};

enum {
    ADC_STEP_ARM = 0u,   /* the entry step: whatever cache is cold, or LOW    */
    ADC_STEP_LOW_WRITE,  /* this channel's request bit <- 0                   */
    ADC_STEP_HIGH_WRITE, /* <- 1: the edge that launches the conversion       */
    ADC_STEP_COLLECT     /* settle, then read the data register               */
};

static uint32_t g_adc_cur;      /* channel index being converted             */
static uint32_t g_adc_step;     /* ADC_STEP_*                               */
static uint64_t g_adc_strobe_us;
static uint32_t g_adc_polls;
static int g_adc_pin = -1;      /* >= 0: round robin parked, blocking pump   */
static int g_adc_blocking;      /* spend real delays instead of returning    */

/* The two caches. `con_shadow' is the low nine bits of AUXADC_CON22 as this
 * driver last left them; `vbuf_on' records that RG_VBUF_EN has been set. Both are
 * invalidated together on any pwrap failure — a write that may or may not have
 * landed leaves the register in a state this driver cannot claim to know. */
static uint32_t g_adc_con_shadow;
static int g_adc_con_valid;
static int g_adc_vbuf_on;

/* Median of the first @p n entries. n is 1..ADC_MEDIAN; for an even n this takes
 * the upper of the two middle values, which for counts is a rounding choice and
 * nothing more. */
static int adc_median_of(const int* ring, uint32_t n) {
    int sorted[ADC_MEDIAN];
    uint32_t i;
    uint32_t j;

    if (n > (uint32_t)ADC_MEDIAN) n = (uint32_t)ADC_MEDIAN;
    for (i = 0u; i < n; ++i) sorted[i] = ring[i];
    for (i = 1u; i < n; ++i) {
        const int key = sorted[i];
        j = i;
        while (j > 0u && sorted[j - 1u] > key) {
            sorted[j] = sorted[j - 1u];
            --j;
        }
        sorted[j] = key;
    }
    return sorted[n / 2u];
}

/*
 * ── THE SHUNT DELTA IS ITS OWN FILTERED QUANTITY ──
 *
 * The kernel's battery_meter_get_charging_current() takes twenty BATSNS/ISENSE
 * pairs back to back, sorts each channel, drops the two lowest and two highest of
 * each, averages the rest, and subtracts the two averages. Two of those three
 * steps are load-bearing here and one is not portable:
 *
 *   - filter, yes: at 68 mOhm one ADC count is 3.2 mA, and the difference of two
 *     15-bit readings of a ~4 V rail is a handful of counts riding on ~19000.
 *   - PAIRS TAKEN ADJACENTLY, yes: that is what makes the subtraction mean
 *     anything.
 *   - subtract-the-filtered-channels, no. That is only equal to
 *     filter-the-differences when the rail is stationary, and on this board the
 *     rail is exactly what moves: no power-path FET, so VBAT *is* VSYS, and a
 *     cable going in shifts both channels by hundreds of counts at once. Stock
 *     gets away with it because it collects all forty conversions inside a few
 *     milliseconds; this driver spreads them over a second, so it must difference
 *     first and filter after. Common mode then cancels inside each pair instead of
 *     being averaged into both operands.
 *
 * So: each ISENSE conversion that lands IMMEDIATELY AFTER a BATSNS conversion
 * (adjacency is checked against a global conversion counter, so no pair is ever
 * formed across a third channel or across a plug event) contributes one signed
 * delta, and the deltas get the median.
 */
static int g_delta_ring[ADC_MEDIAN];
static uint32_t g_delta_n;
static uint32_t g_delta_pos;
static int g_delta_median;  /* counts, signed — hence a separate valid flag    */
static int g_delta_valid;
static uint32_t g_delta_updates;

static uint32_t g_adc_conv_seq;         /* every commit, any channel           */
static int g_pair_raw[2];               /* [ADC_BATSNS], [ADC_VSEN]            */
static uint32_t g_pair_seq[2];          /* the conv_seq each was taken at      */
static int g_pair_have[2];

static void delta_forget(void) {
    g_delta_n = 0u;
    g_delta_pos = 0u;
    g_delta_valid = 0;
    g_pair_have[0] = 0;
    g_pair_have[1] = 0;
}

static void delta_commit(int counts) {
    g_delta_ring[g_delta_pos] = counts;
    g_delta_pos = (g_delta_pos + 1u) % (uint32_t)ADC_MEDIAN;
    if (g_delta_n < (uint32_t)ADC_MEDIAN) ++g_delta_n;
    g_delta_median = adc_median_of(g_delta_ring, g_delta_n);
    g_delta_valid = 1;
    ++g_delta_updates;
}

/* Called after every commit on BATSNS or VSEN. Forms a pair only from two
 * conversions that were consecutive in the whole machine's history. */
static void pair_note(uint32_t idx, int raw) {
    const uint32_t other = (idx == (uint32_t)ADC_BATSNS) ? (uint32_t)ADC_VSEN
                                                        : (uint32_t)ADC_BATSNS;

    g_pair_raw[idx] = raw;
    g_pair_seq[idx] = g_adc_conv_seq;
    g_pair_have[idx] = 1;

    if (g_pair_have[other] && g_pair_seq[other] + 1u == g_adc_conv_seq) {
        /* VSEN is the system side of the shunt and BATSNS the cell side, so
         * ISENSE minus BATSNS is positive for current INTO the cell. */
        const int b = (idx == (uint32_t)ADC_BATSNS) ? raw : g_pair_raw[other];
        const int s = (idx == (uint32_t)ADC_BATSNS) ? g_pair_raw[other] : raw;
        delta_commit(s - b);
        /* Each conversion belongs to at most one pair. */
        g_pair_have[0] = 0;
        g_pair_have[1] = 0;
    }
}

/*
 * Publish on EVERY sample, from however many the ring holds.
 *
 * This used to withhold the median until all five slots were full, on the
 * reasoning that a median of two is a coin toss. True, and irrelevant: the
 * alternative on offer was not a better number, it was NO number for 800 ms per
 * channel — and the callers, which all read -1 as "nothing measured yet", drew a
 * blank status bar for two and a half seconds after boot and again after every
 * plug event. A median of one is the sample; a median of three is already
 * outlier-proof; a median of five arrives 240 ms later and quietly replaces it.
 * Monotonically improving beats absent.
 */
static void adc_commit(adc_chan_t* c, int raw) {
    c->ring[c->pos] = raw;
    c->pos = (c->pos + 1u) % (uint32_t)ADC_MEDIAN;
    if (c->n < (uint32_t)ADC_MEDIAN) ++c->n;
    c->median = adc_median_of(c->ring, c->n);
    ++c->updates;
    ++g_adc_conv_seq;
    if (c == &g_adc[ADC_BATSNS]) pair_note((uint32_t)ADC_BATSNS, raw);
    if (c == &g_adc[ADC_VSEN]) pair_note((uint32_t)ADC_VSEN, raw);
}

static void adc_forget(adc_chan_t* c) {
    c->n = 0u;
    c->pos = 0u;
    c->median = -1;
    if (c == &g_adc[ADC_BATSNS] || c == &g_adc[ADC_VSEN]) delta_forget();
}

/* Next channel in the round robin. VCHR is skipped while there is no cable: its
 * conversion would be a guaranteed zero and would cost a third of the ADC's
 * throughput for it. Pinned means the blocking pump owns the machine. */
static uint32_t adc_next_index(uint32_t idx) {
    uint32_t i;

    if (g_adc_pin >= 0) return (uint32_t)g_adc_pin;
    for (i = 1u; i <= (uint32_t)ADC_CHANNELS; ++i) {
        const uint32_t cand = (idx + i) % (uint32_t)ADC_CHANNELS;
        if (cand == (uint32_t)ADC_VCHR && g_chrdet_cached != 1) continue;
        return cand;
    }
    return (uint32_t)ADC_BATSNS;
}

/* Abandon this conversion and move on. Deliberately does NOT commit anything:
 * a channel with a broken transaction keeps its old median and its callers keep
 * getting the last good answer rather than a zero. */
static void adc_abort(void) {
    g_adc_cur = adc_next_index(g_adc_cur);
    g_adc_step = ADC_STEP_ARM;
    g_adc_polls = 0u;
}

static void adc_fail(const char* what) {
    static int logged;
    if (!logged) {
        logged = 1;
        plog("pmic: auxadc pwrap failure (");
        plog(what);
        plog("); retrying quietly from here\n");
    }
    /* A failed transaction may or may not have landed, so both caches are now
     * claims this driver cannot back up. Drop them and re-read next time. */
    g_adc_con_valid = 0;
    g_adc_vbuf_on = 0;
    adc_abort();
}

/*
 * Make the two latched registers true, one transaction at a time.
 *
 * Returns 0 when nothing had to be done — the caller then proceeds into the
 * conversion in the SAME call, because a step that spends no transaction has no
 * business costing a 20 ms tick. Returns non-zero when a transaction was spent
 * (or failed), and the caller returns.
 */
static int adc_arm(void) {
    uint32_t v = 0;

    if (!g_adc_vbuf_on) {
        if (mt6592_pwrap_read(PMIC_AUXADC_RQST0, &v) != 0) {
            adc_fail("RQST0 read");
            return -1;
        }
        if ((v & PMIC_AUXADC_RQST0_START) == 0u) {
            if (mt6592_pwrap_write(PMIC_AUXADC_RQST0,
                                   v | PMIC_AUXADC_RQST0_START) != 0) {
                adc_fail("RQST0 write");
                return -1;
            }
        }
        /* RG_VBUF_EN is not a channel select. Every channel needs it, nothing in
         * this loader clears it, so it is set once per boot and remembered. */
        g_adc_vbuf_on = 1;
        return 1;
    }

    if (!g_adc_con_valid) {
        if (mt6592_pwrap_read(PMIC_AUXADC_CON, &v) != 0) {
            adc_fail("CON read");
            return -1;
        }
        /* The whole register is kept, not just PMIC_AUXADC_CON_FIELD: the request
         * bits are the only ones this driver may move, and preserving the rest
         * costs nothing now that the value is cached. */
        g_adc_con_shadow = v;
        g_adc_con_valid = 1;
        return 1;
    }

    return 0;
}

/* One step, at most one pwrap transaction. A whole conversion is three of them
 * (bit low, bit high, read data) plus the 1 ms the hardware needs in between. */
static void adc_step(void) {
    adc_chan_t* c;
    uint32_t v = 0;

    if (!pwrap_up()) return;

    if (g_adc_step == ADC_STEP_ARM) {
        if (adc_arm() != 0) return;
        g_adc_step = ADC_STEP_LOW_WRITE;
    }
    c = &g_adc[g_adc_cur];

    switch (g_adc_step) {
        case ADC_STEP_LOW_WRITE: {
            const uint32_t low = g_adc_con_shadow & ~BIT(c->channel);

            if (mt6592_pwrap_write(PMIC_AUXADC_CON, low) != 0) {
                adc_fail("CON write (low)");
                return;
            }
            g_adc_con_shadow = low;
            g_adc_step = ADC_STEP_HIGH_WRITE;
            return;
        }

        case ADC_STEP_HIGH_WRITE: {
            const uint32_t high = g_adc_con_shadow | BIT(c->channel);

            if (mt6592_pwrap_write(PMIC_AUXADC_CON, high) != 0) {
                adc_fail("CON write (high)");
                return;
            }
            g_adc_con_shadow = high;
            /* The low-to-high edge on this channel's bit IS the start of
             * conversion. Everything after this is waiting for it. */
            g_adc_strobe_us = mt6592_timer_microseconds();
            g_adc_step = ADC_STEP_COLLECT;
            return;
        }

        case ADC_STEP_COLLECT:
        default: {
            const uint64_t now = mt6592_timer_microseconds();
            const uint64_t waited = (now >= g_adc_strobe_us) ? (now - g_adc_strobe_us)
                                                             : 0u;

            /* The settle is not a state of its own, because it costs no
             * transaction and READY is sticky: checking the clock here is free,
             * and on the frame path the caller's own 20 ms tick has already paid
             * the 1 ms twenty times over by the time we are called again. */
            if (waited < (uint64_t)AUXADC_CONVERSION_US) {
                if (!g_adc_blocking) return;
                mt6592_delay_cycles((uint32_t)((uint64_t)AUXADC_CONVERSION_US - waited) *
                                    MT6592_DELAY_LEGACY_CYCLES_PER_US);
            }

            if (mt6592_pwrap_read(c->data_reg, &v) != 0) {
                adc_fail("data read");
                return;
            }
            if ((v & PMIC_AUXADC_READY) != 0u) {
                adc_commit(c, (int)(v & PMIC_AUXADC_VALUE_MASK));
                adc_abort(); /* not a failure here — just "next channel, step 0" */
                return;
            }
            /* READY is sticky on this part, so reaching here at all means the
             * latch really is empty; give it a bounded chance in 1 ms steps, the
             * same shape as the kernel's own poll loop. */
            ++g_adc_polls;
            if (g_adc_polls >= (g_adc_blocking ? (uint32_t)AUXADC_BLOCKING_POLL_LIMIT
                                               : (uint32_t)AUXADC_POLL_LIMIT)) {
                adc_fail("data never READY");
                return;
            }
            if (g_adc_blocking) {
                mt6592_delay_cycles((uint32_t)AUXADC_CONVERSION_US *
                                    MT6592_DELAY_LEGACY_CYCLES_PER_US);
            }
            return;
        }
    }
}

/*
 * The blocking pump: the same state machine, driven hard, on one channel.
 *
 * Only mt6592_pmic_sample_blocking() and the console reach this. It parks the
 * round robin, so a conversion already in flight on another channel is dropped
 * (its ring keeps its old contents), and restores it afterwards. `samples` is
 * counted in COMMITS, not iterations, so a caller asking for five gets five real
 * conversions or the guard, never a stale median.
 */
static int adc_pump_blocking(uint32_t idx, uint32_t samples) {
    adc_chan_t* c = &g_adc[idx];
    const uint32_t before = c->updates;
    /* Three steps per conversion, plus one for a cold cache, plus poll slack. */
    uint32_t guard = (samples + 1u) * (4u + (uint32_t)AUXADC_BLOCKING_POLL_LIMIT);
    const int prev_pin = g_adc_pin;

    if (!pwrap_up()) return -1;

    g_adc_pin = (int)idx;
    g_adc_blocking = 1;
    if (g_adc_cur != idx) {
        g_adc_cur = idx;
        g_adc_step = ADC_STEP_ARM;
        g_adc_polls = 0u;
    }

    while ((c->updates - before) < samples && guard-- != 0u) {
        adc_step();
    }

    g_adc_blocking = 0;
    g_adc_pin = prev_pin;
    /* Leave g_adc_cur where the pump left it; the round robin picks up from
     * there and adc_next_index() will step off a pinned channel on its own. */
    return ((c->updates - before) >= samples) ? 0 : -1;
}

/*
 * Pump BATSNS and ISENSE ALTERNATELY, @p rounds times.
 *
 * This is the shape of battery_meter_get_charging_current():
 *
 *     for (i = 0; i < 20; i++) {
 *             ADC_BAT_SENSE_tmp[i] = PMIC_IMM_GetOneChannelValue(VBAT_CHANNEL, 1);
 *             ADC_I_SENSE_tmp[i]   = PMIC_IMM_GetOneChannelValue(ISENSE_CHANNEL, 1);
 *     }
 *
 * one immediately after the other, so the pair straddles the shunt at one instant
 * rather than at two instants a hundred milliseconds apart. Pumping five of one
 * and then five of the other — which is what two adc_pump_blocking() calls do —
 * yields exactly ONE adjacent pair out of ten conversions, and that is why the
 * console's one-shot current read was noisier than the frame path's.
 */
static int adc_pump_pair(uint32_t rounds) {
    const uint32_t before = g_delta_updates;
    uint32_t i;

    for (i = 0u; i < rounds; ++i) {
        if (adc_pump_blocking((uint32_t)ADC_BATSNS, 1u) != 0) return -1;
        if (adc_pump_blocking((uint32_t)ADC_VSEN, 1u) != 0) return -1;
    }
    return ((g_delta_updates - before) > 0u) ? 0 : -1;
}

/*
 * ── COUNTS TO MILLIVOLTS ──
 *
 * Channels 6 and 7 take the x4 arm: 1800 mV VOLTAGE_FULL_RANGE times R_BAT_SENSE
 * 4, over 15 bits. Channel 5 takes the x1 arm and then the board's 369/39
 * divider. Both are stock's arithmetic, in stock's order, so the rounding
 * matches what the vendor's own numbers would have been.
 */
static int sense_mv_from_counts(int raw) {
    if (raw < 0) return -1;
    return (int)(((uint32_t)raw * PMIC_AUXADC_FULL_SCALE_MV) >> PMIC_AUXADC_VALUE_BITS);
}

static int vchr_mv_from_counts(int raw) {
    if (raw < 0) return -1;
    /*
     * Divider applied before the shift, so the 9.46x multiply does not lose the low
     * bits of a small pin reading -- IN 64 BITS, BECAUSE THAT ORDER OVERFLOWS 32.
     *
     * A 5 V cable puts ~528 mV on the pin, which is ~9600 of 32768 counts, and
     * 9600 * 1800 * 369 is 6.4e9 against a uint32_t ceiling of 4.29e9. Everything
     * above ~6465 counts, i.e. every cable voltage above 3.4 V, wrapped. The wrap
     * is not random either: it lands a real 5.17 V cable on 1809 mV, just above the
     * AUXADC's own 1800 mV full scale, which reads exactly like a divider that was
     * never applied and is in fact one that was applied and then truncated. Both
     * halves of the previous comment were right about the intent and wrong about
     * the width.
     */
    return (int)(((uint64_t)(uint32_t)raw * (uint64_t)PMIC_AUXADC_VCHR_FULL_SCALE_MV *
                  (uint64_t)MT6592_PMIC_VCHR_DIVIDER_NUM) /
                 ((uint64_t)MT6592_PMIC_VCHR_DIVIDER_DEN << PMIC_AUXADC_VALUE_BITS));
}

/*
 * ── COUNTS TO MILLIAMPS ──
 *
 * Stock computes I = 1000 * (VSEN_mV - BATSNS_mV + offset) / R_SENSE_mOhm with
 * both voltages already truncated to whole millivolts, which quantises the answer
 * to 1000/68 = 14.7 mA whatever the ADC resolves. That coarseness is an artefact
 * of the order of operations, not a calibration, so this does the same division
 * in COUNTS and gets ~3.2 mA per count:
 *
 *   mA = delta_counts * 7200 * 1000 / (68 << 15)
 *
 * Sign: VSEN is the system side of the shunt and BATSNS the cell side, so current
 * INTO the cell makes VSEN the higher of the two and the result positive. The
 * offset is stock's, and stock's is zero.
 */
static int sense_ma_from_counts(int delta_counts) {
    const int32_t num = (int32_t)delta_counts * (int32_t)PMIC_AUXADC_FULL_SCALE_MV * 1000;
    const int32_t den = (int32_t)MT6592_PMIC_R_SENSE_MOHM << PMIC_AUXADC_VALUE_BITS;
    return (int)(num / den);
}

int mt6592_pmic_hw_ocv_mv(void) {
    return g_hw_ocv_mv;
}

int mt6592_pmic_vbat_counts_per_mv_x100(void) {
    /* 7200 mV over 32768 counts: 4.55 counts/mV, reported x100 so a caller can
     * turn a raw delta into millivolts without repeating the scale. */
    return (int)((100u << PMIC_AUXADC_VALUE_BITS) / PMIC_AUXADC_FULL_SCALE_MV);
}

int mt6592_pmic_vbat_sample_raw(void) {
    return g_adc[ADC_BATSNS].median;
}

int mt6592_pmic_sense_pair(int* out_batsns_raw, int* out_vsen_raw, int* out_delta_counts,
                           int* out_ma) {
    const int b = g_adc[ADC_BATSNS].median;
    const int s = g_adc[ADC_VSEN].median;

    /* Reads the published medians; forces nothing. A caller that wants a fresh
     * pair asks mt6592_pmic_sample_blocking() for CURRENT_NOW first. */
    if (b < 0 || s < 0 || !g_delta_valid) return -1;

    if (out_batsns_raw) *out_batsns_raw = b;
    if (out_vsen_raw) *out_vsen_raw = s;
    /* The two rails are reported as they were filtered, for the console's benefit,
     * but the delta is the FILTERED DELTA and is deliberately not s - b: those two
     * medians can be minutes apart in composition, and their difference is the
     * quantity this driver refuses to trust. */
    if (out_delta_counts) *out_delta_counts = g_delta_median;
    if (out_ma) *out_ma = sense_ma_from_counts(g_delta_median);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE CHARGER, READ SIDE
 * ══════════════════════════════════════════════════════════════════════════ */

/* CHRDET is a live comparator on the CHRIN pin — one read, no arming, no
 * settling — so it gets its own short cadence and is the one thing this driver
 * will assert about the outside world. -1 until the first read succeeds; nothing
 * downstream may read -1 as "no cable". */
static int charger_detect_poll(uint64_t now) {
    uint32_t con0 = 0;

    if (g_chrdet_cached >= 0 && g_last_chrdet_poll_us != 0u && now >= g_last_chrdet_poll_us &&
        (now - g_last_chrdet_poll_us) < (uint64_t)CHARGER_DETECT_POLL_US) {
        return g_chrdet_cached;
    }
    if (!pwrap_up()) return g_chrdet_cached;
    if (mt6592_pwrap_read(PMIC_CHR_CON0, &con0) != 0) return g_chrdet_cached;

    g_last_chrdet_poll_us = now;
    g_chrdet_cached = (con0 & PMIC_CHR_CON0_CHRDET) ? 1 : 0;
    return g_chrdet_cached;
}

uint32_t mt6592_pmic_charger_flags(void) {
    uint32_t con0 = 0;
    uint32_t con2 = 0;
    uint32_t flags = 0;

    /* No VALID bit means "could not read"; a caller must not read the absence of
     * CHRDET out of an all-zero word. */
    if (!pwrap_up()) return 0u;
    if (mt6592_pwrap_read(PMIC_CHR_CON0, &con0) != 0) return 0u;
    if (mt6592_pwrap_read(PMIC_CHR_CON2, &con2) != 0) return 0u;

    flags = MT6592_PMIC_CHG_VALID;
    if (con0 & PMIC_CHR_CON0_CHR_LDO_DET) flags |= MT6592_PMIC_CHG_LDO_DET;
    if (con0 & PMIC_CHR_CON0_CHRDET) flags |= MT6592_PMIC_CHG_CHRDET;
    if (con0 & PMIC_CHR_CON0_VCDT_LV_DET) flags |= MT6592_PMIC_CHG_VCDT_LV_DET;
    if (con0 & PMIC_CHR_CON0_VCDT_HV_DET) flags |= MT6592_PMIC_CHG_VCDT_HV_DET;
    if (con2 & PMIC_CHR_CON2_CS_DET) flags |= MT6592_PMIC_CHG_CS_DET;
    if (con2 & PMIC_CHR_CON2_VBAT_CV_DET) flags |= MT6592_PMIC_CHG_VBAT_CV_DET;
    if (con2 & PMIC_CHR_CON2_VBAT_CC_DET) flags |= MT6592_PMIC_CHG_VBAT_CC_DET;
    return flags;
}

/* ══════════════════════════════════════════════════════════════════════════
 * BC1.2 — WHAT KIND OF CABLE
 *
 * A transcription of hw_charger_type_detection() (reference pmic.c:427) TURNED
 * INSIDE OUT. Stock's version is eight functions of straight-line register writes
 * separated by mdelay(): 100 + 400 + up to two 80s, so 660 ms of a CPU doing
 * nothing. Nothing in this driver is allowed to block for 660 ms -- the charge
 * park repaints out of the same loop that services this -- so the delays become
 * dwells measured against the GPT and each service call does at most the register
 * writes for one transition. Same sequence, same thresholds, same decision tree,
 * spread over about forty service ticks instead of held in one.
 *
 * The fields are written ONE AT A TIME, in the reference's order, rather than
 * folded into one write per register. They could be folded -- vref, cmp, ipd, ipu
 * and bias all live in CHR_CON19 -- but the order is load-bearing (the comparator
 * is enabled LAST, after the source, the sink and the reference are settled) and
 * an audit of this against pmic.c should be a line-for-line read, not an argument
 * about whether a coalesced write preserved an ordering.
 *
 * The quirks are deliberate and are stock's: step A1 leaves VREF_VTH at 2 instead
 * of clearing it, and step B1 sets IPD where its comment says IPU. Both are that
 * way in the reference, which is the code that shipped on this board.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Defined with the charger's write side, ~500 lines down, because that is where
 * every other register write in this file is argued. Declared here because BC1.2
 * is a read that has to drive the pins first. */
static int charger_rmw(uint32_t addr, uint32_t clear_mask, uint32_t set_mask);

enum {
    /* RG_USB20_BC11_SW_EN, the one bit of this that is not in the PMIC: it hands
     * D+/D- from the USB PHY to the PMIC's comparator. Same window and same
     * byte-wide access as the MUSB host driver and mt6592_usb_gadget.c, which both
     * clear this bit as part of bringing the PHY up -- so a detection in flight
     * when either of those probes is a detection whose answer is void, and that
     * is what mt6592_pmic_charger_retype() is for.
     *
     * ── AND IN THE LOADER THIS BIT PROBABLY DOES NOTHING ──
     *
     * Setting it is the whole of stock's Charger_Detect_Init() (preloader
     * usbphy.c:492). But it is a switch inside the U2 PHY, and nothing in the LK
     * link powers that PHY -- see the MUSB host driver, "the U2 PHY window
     * (0x11210800) are clocked at runtime after the LK handoff". A switch in an
     * unpowered block does not close, and an unrouted D+ floats, and a floating D+
     * walks stock's tree DCD-high -> A1-high -> B1-low, which is the NONSTANDARD
     * leaf. Every charger, wall or host, classifies the same -- which is exactly
     * what this board does on the bench.
     *
     * NOT PROVEN, and the test that would prove it is to bring the PHY up and see
     * whether a wall charger starts reading STANDARD. Left as it is because stock
     * LK does not classify at all: mt_battery.c:138 hardcodes CS_VTH=0xC, 450 mA,
     * for every charger, and there is no bc11 anywhere in its PMIC-charger path.
     * So the number this arrives at is stock LK's number, by a longer route. */
    BC11_PHY_BASE = 0x11210800u,
    BC11_PHY_SW_CTRL = 0x1au,
    BC11_PHY_SW_EN = 0x80u,

    /* Stock's mdelay()s, as dwells. */
    BC11_DWELL_INIT_US = 100000u,
    BC11_DWELL_DCD_US = 400000u,
    BC11_DWELL_STEP_US = 80000u,

    /*
     * This board's own cust_charging.h. Every one of these is now the number stock
     * uses for that type, taken from custom/mt6592/kernel/battery/battery/
     * cust_charging.h:25-32 and mapped through linear_charging.c:320-380:
     *
     *   SDP    USB_CHARGER_CURRENT             500 mA
     *   CDP    CHARGING_HOST_CHARGER_CURRENT   650 mA
     *   DCP    AC_CHARGER_CURRENT              650 mA
     *   NONSTD NON_STD_AC_CHARGER_CURRENT      500 mA
     *
     * DCP WAS 950. That was this file's own number, not stock's, reasoned from the
     * CS_VTH cap rather than read off the profile -- and 950 walks the table to the
     * 900 mA step, 250 mA past anything stock programs on a board whose VBAT is its
     * VSYS. Nothing here has measured 900 mA into this hardware. Stock's 650 mA is
     * the measured-and-shipped figure, so it is the one used.
     *
     * The Apple limits stay as the kernel's per-type numbers: stock has no single
     * collapsed value for them and they are not reachable on a compliant port.
     */
    BC11_LIMIT_SDP_MA = 500,
    BC11_LIMIT_CDP_MA = 650,
    BC11_LIMIT_DCP_MA = 650,
    BC11_LIMIT_NONSTD_MA = 500,
    BC11_LIMIT_APPLE_0_5A_MA = 500,
    BC11_LIMIT_APPLE_1_0A_MA = 1000,
    BC11_LIMIT_APPLE_2_1A_MA = 2100
};

typedef enum {
    BC11_IDLE = 0, /* no cable, or the answer is in                             */
    BC11_INIT,
    BC11_DCD,
    BC11_A1,
    BC11_B1,
    BC11_C1,
    BC11_A2,
    BC11_B2,
    BC11_TEARDOWN
} bc11_step_t;

static bc11_step_t g_bc11_step;
static int g_bc11_armed;      /* the current step's drive writes have been done  */
static uint64_t g_bc11_us;    /* when they were                                  */
static int g_bc11_type = (int)MT6592_PMIC_USB_TYPE_UNKNOWN;
static int g_bc11_limit_ma;   /* 0 while UNKNOWN; see the note on Apple below    */
static uint32_t g_chg_gen;    /* the plug event, see mt6592_pmic_charger_generation */

static void bc11_phy_sw_en(int on) {
    volatile uint8_t* r = (volatile uint8_t*)(uintptr_t)(BC11_PHY_BASE + BC11_PHY_SW_CTRL);
    const uint8_t cur = *r;
    *r = on ? (uint8_t)(cur | BC11_PHY_SW_EN) : (uint8_t)(cur & (uint8_t)~BC11_PHY_SW_EN);
}

/* One BC11 field. `val' is the reference's argument verbatim -- 0/1/2 for the
 * two-bit fields, which take those as settings and not as bitmasks. */
static void bc11_field(uint32_t addr, uint32_t mask, uint32_t shift, uint32_t val) {
    (void)charger_rmw(addr, mask, (val << shift) & mask);
}

/* Named for the reference's accessors so the steps below read as its code does. */
static void bc11_bb_ctrl(uint32_t v) {
    bc11_field(PMIC_CHR_CON18, PMIC_CHR_CON18_BC11_BB_CTRL, 0u, v);
}
static void bc11_rst(uint32_t v) {
    bc11_field(PMIC_CHR_CON18, PMIC_CHR_CON18_BC11_RST, 1u, v);
}
static void bc11_vsrc_en(uint32_t v) {
    bc11_field(PMIC_CHR_CON18, PMIC_CHR_CON18_BC11_VSRC_EN_MASK,
               PMIC_CHR_CON18_BC11_VSRC_EN_SHIFT, v);
}
static void bc11_vref_vth(uint32_t v) {
    bc11_field(PMIC_CHR_CON19, PMIC_CHR_CON19_BC11_VREF_VTH_MASK,
               PMIC_CHR_CON19_BC11_VREF_VTH_SHIFT, v);
}
static void bc11_cmp_en(uint32_t v) {
    bc11_field(PMIC_CHR_CON19, PMIC_CHR_CON19_BC11_CMP_EN_MASK,
               PMIC_CHR_CON19_BC11_CMP_EN_SHIFT, v);
}
static void bc11_ipd_en(uint32_t v) {
    bc11_field(PMIC_CHR_CON19, PMIC_CHR_CON19_BC11_IPD_EN_MASK,
               PMIC_CHR_CON19_BC11_IPD_EN_SHIFT, v);
}
static void bc11_ipu_en(uint32_t v) {
    bc11_field(PMIC_CHR_CON19, PMIC_CHR_CON19_BC11_IPU_EN_MASK,
               PMIC_CHR_CON19_BC11_IPU_EN_SHIFT, v);
}
static void bc11_bias_en(uint32_t v) {
    bc11_field(PMIC_CHR_CON19, PMIC_CHR_CON19_BC11_BIAS_EN, 8u, v);
}

/* RGS_BC11_CMP_OUT. A failed read is reported as 0, which every branch below
 * treats as "the probe did not answer" -- the same conclusion a real 0 carries,
 * so a wedged wrapper degrades the classification instead of corrupting it. */
static int bc11_cmp_out(void) {
    uint32_t con18 = 0;
    if (mt6592_pwrap_read(PMIC_CHR_CON18, &con18) != 0) return 0;
    return (con18 & PMIC_CHR_CON18_BC11_CMP_OUT) ? 1 : 0;
}

static void bc11_settle(int type, int limit_ma, const char* what) {
    g_bc11_type = type;
    g_bc11_limit_ma = limit_ma;
    g_bc11_step = BC11_TEARDOWN;
    g_bc11_armed = 0;
    ++g_chg_gen; /* the classification is part of the plug event */
    plog("pmic: BC1.2 says ");
    plog(what);
    plog(" (");
    plog_dec((uint32_t)limit_ma);
    plog(" mA input)\n");
}

/* The drive writes for one step. Stock's order, stock's values. */
static void bc11_arm(bc11_step_t step) {
    switch (step) {
        case BC11_INIT: /* hw_bc11_init */
            bc11_phy_sw_en(1);
            bc11_bias_en(0x1u);
            bc11_vsrc_en(0x0u);
            bc11_vref_vth(0x0u);
            bc11_cmp_en(0x0u);
            bc11_ipu_en(0x0u);
            bc11_ipd_en(0x0u);
            bc11_rst(0x1u);
            bc11_bb_ctrl(0x1u);
            break;
        case BC11_DCD: /* hw_bc11_DCD */
            bc11_ipu_en(0x2u);
            bc11_ipd_en(0x1u);
            bc11_vref_vth(0x1u);
            bc11_cmp_en(0x2u);
            break;
        case BC11_A1: /* hw_bc11_stepA1 */
            bc11_ipu_en(0x2u);
            bc11_vref_vth(0x2u);
            bc11_cmp_en(0x2u);
            break;
        case BC11_B1: /* hw_bc11_stepB1 -- IPD where the comment says IPU */
            bc11_ipd_en(0x1u);
            bc11_vref_vth(0x0u);
            bc11_cmp_en(0x1u);
            break;
        case BC11_C1: /* hw_bc11_stepC1 */
            bc11_ipu_en(0x1u);
            bc11_vref_vth(0x2u);
            bc11_cmp_en(0x1u);
            break;
        case BC11_A2: /* hw_bc11_stepA2 */
            bc11_vsrc_en(0x2u);
            bc11_ipd_en(0x1u);
            bc11_vref_vth(0x0u);
            bc11_cmp_en(0x1u);
            break;
        case BC11_B2: /* hw_bc11_stepB2 */
            bc11_ipu_en(0x2u);
            bc11_vref_vth(0x1u);
            bc11_cmp_en(0x1u);
            break;
        case BC11_TEARDOWN: /* hw_bc11_done */
            bc11_vsrc_en(0x0u);
            bc11_vref_vth(0x0u);
            bc11_cmp_en(0x0u);
            bc11_ipu_en(0x0u);
            bc11_ipd_en(0x0u);
            bc11_bias_en(0x0u);
            bc11_phy_sw_en(0);
            break;
        case BC11_IDLE:
        default: break;
    }
}

static uint32_t bc11_dwell_us(bc11_step_t step) {
    switch (step) {
        case BC11_INIT: return (uint32_t)BC11_DWELL_INIT_US;
        case BC11_DCD: return (uint32_t)BC11_DWELL_DCD_US;
        case BC11_TEARDOWN: return 0u; /* nothing to settle; nothing to read */
        default: return (uint32_t)BC11_DWELL_STEP_US;
    }
}

/* The step's own teardown writes, then the branch. Split from bc11_arm() because
 * every step reads its comparator BEFORE it stops driving, and folding the two
 * would read a comparator that had already been switched off. */
static void bc11_retire(bc11_step_t step) {
    const int avail = (step == BC11_INIT || step == BC11_TEARDOWN) ? 0 : bc11_cmp_out();

    switch (step) {
        case BC11_INIT:
            g_bc11_step = BC11_DCD;
            g_bc11_armed = 0;
            return;

        case BC11_DCD:
            bc11_ipu_en(0x0u);
            bc11_ipd_en(0x0u);
            bc11_cmp_en(0x0u);
            bc11_vref_vth(0x0u);
            /* A charger answered the data-contact probe: the divergent, non-BC1.2
             * branch. Everything down here is a proprietary D+/D- resistor code. */
            g_bc11_step = avail ? BC11_A1 : BC11_A2;
            g_bc11_armed = 0;
            return;

        case BC11_A1:
            bc11_ipu_en(0x0u);
            bc11_cmp_en(0x0u); /* VREF_VTH deliberately left at 2, as in stock */
            g_bc11_step = avail ? BC11_B1 : BC11_C1;
            g_bc11_armed = 0;
            return;

        case BC11_B1:
            bc11_ipu_en(0x0u);
            bc11_cmp_en(0x0u);
            bc11_vref_vth(0x0u);
            if (avail) {
                bc11_settle((int)MT6592_PMIC_USB_TYPE_APPLE_BRICK_ID,
                            BC11_LIMIT_APPLE_2_1A_MA, "Apple 2.1 A");
            } else {
                bc11_settle((int)MT6592_PMIC_USB_TYPE_NONSTANDARD, BC11_LIMIT_NONSTD_MA,
                            "a non-standard charger");
            }
            return;

        case BC11_C1:
            bc11_ipu_en(0x0u);
            bc11_cmp_en(0x0u);
            bc11_vref_vth(0x0u);
            bc11_settle((int)MT6592_PMIC_USB_TYPE_APPLE_BRICK_ID,
                        avail ? BC11_LIMIT_APPLE_1_0A_MA : BC11_LIMIT_APPLE_0_5A_MA,
                        avail ? "Apple 1.0 A" : "Apple 0.5 A");
            return;

        case BC11_A2:
            bc11_vsrc_en(0x0u);
            bc11_ipd_en(0x0u);
            bc11_cmp_en(0x0u);
            if (avail) {
                g_bc11_step = BC11_B2;
                g_bc11_armed = 0;
            } else {
                bc11_settle((int)MT6592_PMIC_USB_TYPE_SDP, BC11_LIMIT_SDP_MA,
                            "a standard USB host");
            }
            return;

        case BC11_B2:
            bc11_ipu_en(0x0u);
            bc11_cmp_en(0x0u);
            bc11_vref_vth(0x0u);
            if (avail) {
                bc11_settle((int)MT6592_PMIC_USB_TYPE_DCP, BC11_LIMIT_DCP_MA,
                            "a dedicated charger");
            } else {
                bc11_settle((int)MT6592_PMIC_USB_TYPE_CDP, BC11_LIMIT_CDP_MA,
                            "a charging host");
            }
            return;

        case BC11_TEARDOWN:
        case BC11_IDLE:
        default:
            g_bc11_step = BC11_IDLE;
            g_bc11_armed = 0;
            return;
    }
}

/* Put the pins back whatever state the run was in, and forget the answer. Called
 * on unplug, where the classification is not merely stale but meaningless: the
 * next cable need not be the same cable. */
static void bc11_abandon(void) {
    if (g_bc11_step != BC11_IDLE && pwrap_up()) bc11_arm(BC11_TEARDOWN);
    else bc11_phy_sw_en(0);
    g_bc11_step = BC11_IDLE;
    g_bc11_armed = 0;
    g_bc11_type = (int)MT6592_PMIC_USB_TYPE_UNKNOWN;
    g_bc11_limit_ma = 0;
}

/* One transition per call, at most. Costs a compare when there is nothing in
 * flight, which is every call but the forty after a cable arrives. */
static void bc11_service(uint64_t now) {
    if (g_bc11_step == BC11_IDLE) return;
    if (!pwrap_up()) return;

    if (!g_bc11_armed) {
        bc11_arm(g_bc11_step);
        g_bc11_armed = 1;
        g_bc11_us = (now != 0u) ? now : 1u;
        /* The teardown has nothing to settle, so it retires in the same call it
         * was armed in rather than costing a whole extra tick. */
        if (bc11_dwell_us(g_bc11_step) == 0u) bc11_retire(g_bc11_step);
        return;
    }

    if (now < g_bc11_us) return; /* the GPT wrapped; wait for it to catch up */
    if ((now - g_bc11_us) < (uint64_t)bc11_dwell_us(g_bc11_step)) return;
    bc11_retire(g_bc11_step);
}

mt6592_pmic_usb_type_t mt6592_pmic_usb_type(void) {
    return (mt6592_pmic_usb_type_t)g_bc11_type;
}

int mt6592_pmic_usb_online(void) {
    if (g_chrdet_cached != 1) return 0;
    return (g_bc11_type == (int)MT6592_PMIC_USB_TYPE_SDP ||
            g_bc11_type == (int)MT6592_PMIC_USB_TYPE_CDP)
               ? 1
               : 0;
}

int mt6592_pmic_mains_online(void) {
    if (g_chrdet_cached != 1) return 0;
    return (g_bc11_type == (int)MT6592_PMIC_USB_TYPE_DCP ||
            g_bc11_type == (int)MT6592_PMIC_USB_TYPE_NONSTANDARD ||
            g_bc11_type == (int)MT6592_PMIC_USB_TYPE_APPLE_BRICK_ID)
               ? 1
               : 0;
}

int mt6592_pmic_input_current_limit_ma(void) { return g_bc11_limit_ma; }

uint32_t mt6592_pmic_charger_generation(void) { return g_chg_gen; }

/* Arm a run from the top. No generation bump: the caller owns that, because the
 * plug edge bumps once for the whole event and the public entry point below bumps
 * for itself. */
static void bc11_start(void) {
    g_bc11_type = (int)MT6592_PMIC_USB_TYPE_UNKNOWN;
    g_bc11_limit_ma = 0;
    g_bc11_step = BC11_INIT;
    g_bc11_armed = 0;
}

void mt6592_pmic_charger_retype(void) {
    if (g_chrdet_cached != 1) return; /* nothing to classify */
    bc11_start();
    ++g_chg_gen;
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE GAUGE
 * ══════════════════════════════════════════════════════════════════════════ */

static int g_charge_full;        /* the ladder's verdict, latched               */
static uint32_t g_full_count;    /* consecutive low-current samples             */
static uint32_t g_recharge_count;/* consecutive samples under the recharge line */
static int g_topoff_seen;      /* the node has reached V_CC2TOPOFF this plug  */
static uint32_t g_ladder_gen;  /* g_adc[BATSNS].updates at the last advance   */
static uint64_t g_ladder_us;   /* when that advance happened                   */
static int g_online_last = -1; /* for the plug/unplug edge                    */

/*
 * Stock's BAT_TopOffModeAction, kept intact:
 *
 *   the node must first reach V_CC2TOPOFF (4050 mV) under charge — a pack that
 *   never got there is not near full whatever its current looks like — and then
 *   SIX CONSECUTIVE samples must show charge current at or under 150 mA. Any
 *   sample above that resets the count. Once full, only dropping back below
 *   RECHARGING (4110 mV) clears it.
 *
 * Advanced at most once per PMIC_LADDER_INTERVAL_US, and only on a newly published
 * BATSNS median, so "six samples" is six seconds of steady low current rather than
 * six frames or — since a conversion now lands every few milliseconds — six
 * consecutive readings of the same instant.
 */
static void ladder_advance(int online, int bat_mv, int ma, int ma_valid) {
    int saturated;
    int at_cv;

    /*
     * ── AN UNPLUG ENDS THE CHARGE RUN. IT DOES NOT EMPTY THE PACK ──
     *
     * This branch used to clear g_charge_full as well, on the reading that
     * "nothing about full is meaningful once the cable is out". That reading is
     * wrong on its own terms: full is a statement about the PACK, and a pack that
     * was full one second ago is still full the next. What un-fulls it is the cell
     * relaxing under the recharge line, which the block below already tests, and
     * which is just as true unplugged.
     *
     * And it is wrong about `online' ON THIS BOARD IN PARTICULAR. CHRDET is a
     * comparator on a pin this board can itself drive, so `online' goes false for
     * reasons that have nothing to do with the cell. Every one of those used to
     * throw the full latch away — and since g_charge_full is what PINS the level
     * in gauge_percent(), losing it hands a full pack to the two paths that can
     * only walk downward.
     *
     * What an unplug DOES end is the run: g_full_count and g_topoff_seen are the
     * run's state, so the next cable in has to earn both from scratch.
     */
    if (online != 1) {
        g_full_count = 0u;
        g_topoff_seen = 0;
    } else if (bat_mv >= PMIC_V_CC2TOPOFF_MV) {
        g_topoff_seen = 1;
    }

    if (g_charge_full) {
        /*
         * ── LEAVING FULL IS ALSO A SEQUENCE ──
         *
         * Earning full takes six samples in a row; losing it took ONE, and the one
         * it took was a comparison against 4110 mV of a node that sits only a
         * little above that at the charger's setpoint. A single ADC sample dipping
         * a few counts under the line threw the latch away, the level fell back to
         * the 99% cap, the ladder spent another six seconds re-earning it, and the
         * display walked 99 - 100 - 99 forever. A real recharge is not a sample, it
         * is a condition: the pack has genuinely relaxed away from the setpoint and
         * stayed there.
         */
        if (bat_mv > 0 && bat_mv < PMIC_RECHARGING_MV) {
            if (++g_recharge_count >= (uint32_t)PMIC_FULL_CHECK_TIMES) {
                g_charge_full = 0;
                g_full_count = 0u;
                g_recharge_count = 0u;
                plog("pmic: battery no longer full (six samples under the recharge line)\n");
            }
        } else {
            g_recharge_count = 0u;
        }
        return;
    }
    g_recharge_count = 0u;

    /* Earning full needs a charger. Losing it, above, does not. */
    if (online != 1 || !g_topoff_seen) return;

    /*
     * WHAT COUNTS AS A FULL-CONDITION SAMPLE. Two things do.
     *
     * (1) A SMALL CURRENT, BY MAGNITUDE. Stock compares ICharging, which is a
     *     magnitude, so its `else' branch takes every reading at or under 150 mA --
     *     zero included. This driver's delta is signed, and a pack whose charger has
     *     terminated is exactly the case that matters here: the cell sits at a few
     *     tens of milliamps with the median straddling zero, so one negative sample
     *     in six was enough to zero the count. A real discharge is hundreds of
     *     milliamps and still resets. An unknown current is not a small one.
     *
     * (2) THE COULOMB INTEGRATOR HAVING SATURATED. This is the rung the board was
     *     missing, and it is what stock spends MAX_CV_CHARGING_TIME waiting for: a
     *     charger that keeps sourcing more than 150 mA at top-off voltage never
     *     completes the six, but the integrator underneath keeps counting, drives
     *     the level to 100, and gets held at 99 by mt_battery_100Percent_tracking_
     *     check(). The remainder then walks 99.0 -> 99.9, crosses a whole percent,
     *     and snaps back to 99.0 -- forever. Having delivered a full pack's worth of
     *     charge with the node past V_CC2TOPOFF IS the full condition; there is no
     *     more capacity to give and nothing above 100% to show.
     *
     * Either way it is still SIX CONSECUTIVE rungs, so neither a brief current dip
     * nor one noisy saturated sample latches full on its own.
     *
     * `> 0' AND NOT `>= 0'. A run that began at 100% has delivered nothing, so its
     * g_soc_dep == 0 is where it started and not where the coulombs put it — and
     * this driver can start there without evidence: boot with a cable in and an
     * unreadable OCV latch and the seed is taken off the rail, which with no
     * power-path FET is the charger's setpoint whatever the pack holds. Top-off is
     * then "seen" off that same rail, and the two together would latch full on a
     * flat pack.
     */
    saturated = (g_soc_car_base > 0 && g_soc_dep == 0)
             || (g_soc_dep == 0 && bat_mv >= PMIC_CV_HOLD_MV);
    /* The shunt is net current (charger minus the board).  At CV it still
     * reads hundreds of milliamps while the cell takes nothing, so the
     * 150 mA test never succeeds.  Six samples on the CV rail is the
     * same statement: the charger is holding the node, not filling it. */
    at_cv = (bat_mv >= PMIC_CV_HOLD_MV);

    if (!saturated && !at_cv &&
        (!ma_valid || ma > PMIC_CHARGING_FULL_CURRENT_MA ||
         ma < -PMIC_CHARGING_FULL_CURRENT_MA)) {
        g_full_count = 0u;
        return;
    }
    if (++g_full_count >= (uint32_t)PMIC_FULL_CHECK_TIMES) {
        g_full_count = 0u;
        g_charge_full = 1;
        plog(saturated
                 ? "pmic: battery full (six samples past top-off, a pack's worth delivered)\n"
                 : "pmic: battery full (six samples within 150 mA of zero past top-off)\n");
    }
}

/* Signed cell current, from the filtered shunt DELTA — never from the difference
 * of the two channels' own medians (see the delta-ring note above). Invalid until
 * one adjacent BATSNS/ISENSE pair has landed, which takes two conversions. */
static int gauge_current_ma(int* out_valid) {
    if (!g_delta_valid) {
        if (out_valid) *out_valid = 0;
        return 0;
    }
    if (out_valid) *out_valid = 1;
    return sense_ma_from_counts(g_delta_median);
}

/*
 * get_hw_ocv(), battery_meter_hal.c:127. The wakeup VBAT latch, in millivolts.
 *
 * Returns -1 if the wrapper is down, if READY is clear (the latch never happened
 * — a warm path that never dropped VBAT), or if the value is outside a window no
 * lithium cell on this board can be in. -1 means "no seed available", never a
 * number: seeding an integrator from a bad reading is worse than not seeding it.
 */
static int hw_ocv_read_mv(void) {
    uint32_t v = 0;
    int mv;

    if (!pwrap_up()) return -1;
    if (mt6592_pwrap_read(PMIC_AUXADC_HW_OCV_DATA, &v) != 0) return -1;
    if ((v & PMIC_AUXADC_READY) == 0u) return -1;

    /* r_val_temp is 4 here, the same x4 arm as channels 6 and 7, so the scale is
     * PMIC_AUXADC_FULL_SCALE_MV — then stock's flat +8 mV trim. */
    mv = (int)(((v & PMIC_AUXADC_VALUE_MASK) * PMIC_AUXADC_FULL_SCALE_MV) >>
               PMIC_AUXADC_VALUE_BITS) +
         (int)PMIC_AUXADC_HW_OCV_TUNE_MV;

    if (mv < PMIC_HW_OCV_MIN_MV || mv > PMIC_HW_OCV_MAX_MV) return -1;
    return mv;
}

/*
 * Read the latch once, as early as the wrapper allows, and say so out loud.
 *
 * There is no mt6592_pmic_init() on this port — the service call is the entry
 * point — so this hangs off the first service call that finds pwrap up. Reading it
 * early matters: nothing clears the latch, but the sooner it is in hand the sooner
 * the gauge can be seeded from a cell rather than from a rail.
 *
 * The log line is not decoration. When the number the park shows looks wrong, the
 * first question is which seed it came from, and this is the only place that can
 * answer it.
 */
static void hw_ocv_prime(void) {
    static int tried;

    if (tried) return;
    if (!pwrap_up()) return; /* not yet; try again on the next service call */
    tried = 1;

    g_hw_ocv_mv = hw_ocv_read_mv();
    if (g_hw_ocv_mv > 0) {
        const int pct = mt6592_battery_percent_from_ocv(g_hw_ocv_mv);
        plog("pmic: wakeup OCV latch ");
        plog_dec((uint32_t)g_hw_ocv_mv);
        plog(" mV (");
        plog_dec((uint32_t)((pct < 0) ? 0 : pct));
        plog("%); seeding the gauge from the cell\n");
    } else {
        plog("pmic: wakeup OCV latch unreadable; gauge will seed from the live "
             "rail\n");
    }
}

/*
 * ── THE PUBLISHED PERCENT IS AN INTEGRATOR SEEDED FROM THE CELL, NOT A LOOKUP ──
 *
 * This function used to be a lookup: IR-compensate BATSNS, index the curve, clamp,
 * done. That is wrong on this board in a way no amount of care inside the lookup
 * can fix, and it is the "reads 99% always while charging" the operator reported.
 *
 * There is no power-path FET here, so VBAT *is* VSYS: with a cable in, BATSNS
 * reads the charger's CV setpoint, not the cell. IR compensation takes a little of
 * that back — 145 mV at 926 mA — but the charger holds the rail wherever it likes,
 * so the compensated number is still a fact about the POWER RAIL. Hand that to the
 * curve and it says full, every time, at any real state of charge, and the 99% cap
 * below files the corner off so it looks almost plausible.
 *
 * The kernel does not do a lookup either, and this is stock's structure, from
 * oam_run() (battery_meter.c:1471) and battery_meter_initial() (:1391):
 *
 *   - oam_d0, the SEED, comes from get_hw_ocv() — the wakeup latch, taken before
 *     the charger existed. That single reading is what anchors the absolute level,
 *     and it is the only number on this board that is about the cell.
 *   - oam_d_3 is the live compensated lookup. It is a TARGET, not the answer.
 *   - oam_d_5, the published depletion, moves toward oam_d_3 by ONE PERCENT per
 *     d5_count_time, and the direction is gated on gFG_Is_Charging:
 *
 *         if (gFG_Is_Charging == KAL_FALSE) { if (oam_d_3 > oam_d_5) oam_d_5++; }
 *         else                             { if (oam_d_5 > oam_d_3) oam_d_5--; }
 *
 *     Depletion, so charging can only ever raise the percentage and discharging
 *     can only ever lower it. A cable going in cannot make the number jump,
 *     because nothing here is allowed to jump.
 *
 * That last property is also the answer to the flaky plug/unplug window from the
 * display's side: even with a perfectly filtered pair, the compensated lookup
 * genuinely moves several points the instant a charger engages. Stock never shows
 * that transient, and neither does this now.
 *
 * Percent = 100 - oam_d_5, exactly as battery_meter_get_battery_percentage().
 */
static int gauge_percent(int online, int bat_mv, int ma, int ma_valid, int* out_ocv_mv) {
    const uint64_t now = mt6592_timer_microseconds();
    const uint32_t gen = g_adc[ADC_BATSNS].updates;
    int target_dep; /* oam_d_3, as depletion */
    int ocv;
    int pct;

    if (out_ocv_mv) *out_ocv_mv = -1;

    /* A missing sample does not un-publish a level that has already been
     * established; the integrator holds. -1 only before the first seed. */
    if (bat_mv <= 0) return (g_soc_dep >= 0) ? (100 - g_soc_dep) : -1;

    ocv = mt6592_battery_ocv_mv(bat_mv, ma_valid ? ma : 0);
    if (out_ocv_mv) *out_ocv_mv = ocv;

    /*
     * THE STATE ADVANCES ONCE PER SAMPLE, NOT ONCE PER READER.
     *
     * In the kernel this integrator lives in oam_run(), driven by the fuel-gauge
     * timer, and battery_meter_get_battery_percentage() is a pure `100 - oam_d_5'.
     * Here the two are one function, so it is tied to the BATSNS generation
     * counter instead: the first reader after a new median advances it, every
     * reader after that gets the same answer for free. Otherwise the status bar
     * and the charge park -- which both read every frame -- would drive the slew at
     * the frame rate, which is the opposite of what a slew is for.
     *
     * The cached answer goes through the same 99% cap as the computed one. It used
     * to be a bare `100 - g_soc_dep', so a saturated integrator that the ladder had
     * not yet called full published 100 to every reader inside the median and 99 to
     * the one that advanced it — the 99-100-99 flicker, at frame rate rather than
     * at the ladder's.
     */
    if (gen == g_soc_gen && g_soc_dep >= 0) {
        const int held = 100 - g_soc_dep;
        return (online == 1 && !g_charge_full && held >= 100) ? 99 : held;
    }
    g_soc_gen = gen;

    pct = mt6592_battery_percent_from_ocv(ocv);
    if (pct < 0) return (g_soc_dep >= 0) ? (100 - g_soc_dep) : -1;
    if (pct > 100) pct = 100;
    target_dep = 100 - pct;

    /*
     * ── EMPTY IS A RAMP, NOT A SNAP ──
     *
     * This used to be a hard `if (ocv <= 3500) { g_soc_dep = 100; return 0; }'
     * ahead of the lookup, on the reasoning that a cell under the floor is the
     * one state where a smooth number is the wrong answer. That reasoning is
     * wrong, and stock says so: mt_battery_0Percent_tracking_check()
     * (battery_common.c:1517) does nothing but `BMT_status.UI_SOC--', one percent
     * per BAT_TASK_PERIOD, in BOTH of its branches. Even at or below
     * SYSTEM_OFF_VOLTAGE it decrements by one. Nothing in stock ever assigns zero
     * to a level that was not already there, and the actual power-off needs the
     * level at zero AND the voltage 100 mV under SYSTEM_OFF_VOLTAGE for more than
     * six consecutive polls.
     *
     * THE SNAP IS WHY THE LEVEL READ 0% WHEN THE CABLE CAME OUT. Unplugging
     * forgets every ring (see mt6592_pmic_service()), so the next conversion is a
     * median of ONE -- and with no power-path FET the supply is genuinely
     * interrupted for a few milliseconds as the cable parts. One unfiltered
     * sample of that dip was enough to latch g_soc_dep = 100, and from there the
     * ordinary slew crawls back at one percent per THIRTY SECONDS, so what the
     * operator sees is not a transient. It is 0%, and it stays.
     *
     * As a ramp the same dip costs at most one percent, which is the property
     * that was missing: sub-threshold voltage aims the integrator at empty, it
     * does not teleport it there. The rate is the battery task's own period
     * rather than the 30 s slew, because that is the period stock's ramp runs on
     * -- a genuinely flat cell still reaches 0% in a minute and a half.
     *
     * Only once there IS a level to walk down. A first-ever sample this low is not
     * a ramp, it is a seed, and the seed below already reads ~0% off the curve for
     * any OCV under the table's bottom row.
     *
     * ── AND ONLY WHILE THE PACK IS LOSING CHARGE, WHICH IS THE BUG ──
     *
     * "THE BATTERY DOES NOT CHARGE." This branch used to run on the voltage alone,
     * and below the floor it ends the charge run and returns BEFORE the coulomb
     * integrator further down can be reached. So a pack sitting at 3400 mV WITH A
     * CHARGER IN walked to zero at one percent per PMIC_LADDER_INTERVAL_US and then
     * stayed at zero for as long as the terminal voltage stayed under the floor,
     * however many coulombs went in. That is not a corner case here: a flat pack is
     * exactly the pack somebody plugs in, and it sits under 3450 mV for the first
     * several minutes of a charge while it is genuinely filling.
     *
     * It was invisible until the AUXADC scale was corrected, because at the old
     * doubled full scale bat_mv never came near 3450 at all.
     *
     * A terminal voltage under the floor with current going IN is not a pack about
     * to die, it is a flat pack being rescued, and the coulombs are the better
     * authority. So the ramp is now the DISCHARGE floor it was always described as,
     * and a charging sample falls through to the integrator.
     *
     * ── AND IT IS THE CELL THAT HAS TO BE AT THE FLOOR, NOT THE RAIL ──
     *
     * The test was on bat_mv, which is stock's: BMT_status.bat_vol is a terminal
     * voltage and mt_battery_0Percent_tracking_check() compares it raw. Stock is
     * entitled to. ON A BOARD WHOSE VBAT IS THE SYSTEM NODE THIS DRIVER IS NOT —
     * half an amp across a couple of hundred milliohms is over a hundred millivolts
     * of sag, so a pack with real charge in it crosses 3450 mV at the connector
     * while the cell behind it is nowhere near empty, and the ramp then costs a
     * percent per second.
     *
     * The IR-compensated number computed above IS the cell. It is what target_dep
     * is already taken from, and under a load it is the HIGHER of the two — so this
     * only ever declines to ramp, never starts one bat_mv would not have. A cell
     * genuinely at the floor still ramps: at rest the two numbers are the same one.
     */
    if (ocv <= PMIC_V_0PERCENT_TRACKING_MV && g_soc_dep >= 0 && !(ma_valid && ma > 0)) {
        if (g_soc_slew_us == 0u ||
            (now >= g_soc_slew_us &&
             (now - g_soc_slew_us) >= (uint64_t)PMIC_LADDER_INTERVAL_US)) {
            if (g_soc_dep < 100) ++g_soc_dep;
            g_soc_slew_us = (now != 0u) ? now : 1u;
        }
        /* A ramp that owns the level for a while ends any charge run, so the next
         * charging sample re-bases here instead of billing this whole detour to
         * the accumulator as coulombs it never saw. */
        g_soc_car_base = -1;
        g_soc_car = 0;
        return 100 - g_soc_dep;
    }

    if (g_soc_dep < 0) {
        /*
         * SEED. With a cable in, the live lookup is a statement about the rail, so
         * the wakeup latch is used if there is one; unplugged, the live reading is
         * itself a clean OCV (only the board's own tens of milliamps across it)
         * and it is newer than the latch, so it wins.
         *
         * If the latch is unreadable AND a cable is in, the rail is all there is
         * and the level starts optimistic — but it starts, and the ladder and the
         * cap keep it honest from there. Refusing to publish anything would leave
         * the status bar and the charge park blank for the whole session.
         */
        int seed = target_dep;

        if (online == 1 && g_hw_ocv_mv > 0) {
            const int hp = mt6592_battery_percent_from_ocv(g_hw_ocv_mv);
            if (hp >= 0) seed = 100 - ((hp > 100) ? 100 : hp);
        }
        g_soc_dep = seed;
        g_soc_slew_us = (now != 0u) ? now : 1u;
    } else if (ma_valid && ma > 0) {
        /*
         * CHARGING RISES ON COULOMBS, NOT ON THE CURVE.
         *
         * gFG_Is_Charging: current INTO the cell. Stock does not walk the OCV
         * table upwards while a charger is attached, and it cannot, for the
         * reason this board demonstrates: with a cable in, the terminal reading
         * is the charger's rail, the IR correction in
         * mt6592_battery_ocv_mv() subtracts the pack's own impedance drop back
         * out of it, and on the flat part of a lithium curve the two very nearly
         * cancel. Measured on this board at 557 mA the corrected OCV came back
         * within a percent of where it started, so a "step towards the target"
         * rule has nothing to step towards and the display sits still for
         * minutes while the pack genuinely fills. That is the bug.
         *
         * oam_run() (battery_meter.c:1574) integrates instead:
         *
         *     oam_car_1 = (oam_i_1*delta_time/3600) + oam_car_1;  // 0.1mAh
         *     oam_d_1   = oam_d0 + (oam_car_1*100/10)/gFG_BATT_CAPACITY_aging;
         *
         * Same thing here, in mA*us against PMIC_BATT_CAPACITY_MAH. No slew
         * gate: the pack's own capacity IS the rate limit. At the ~550 mA this
         * board draws, a percent of 1499 mAh is a hundred seconds, which is
         * slower than the 30 s ladder would have allowed anyway and is a real
         * measurement rather than a display preference.
         */
        if (g_soc_car_base < 0 || now <= g_soc_car_us) {
            g_soc_car_base = g_soc_dep;
            g_soc_car = 0;
        } else {
            int moved;
            g_soc_car += (int64_t)ma * (int64_t)(now - g_soc_car_us);
            moved = (int)(g_soc_car / PMIC_SOC_CAR_PER_PCT);
            g_soc_dep = g_soc_car_base - moved;
            /*
             * AND IT SATURATES. A pack cannot hold more than a pack, so the count
             * stops at the boundary instead of running on past it. Without this the
             * accumulator keeps growing after the level has hit 100 and its
             * remainder -- the tenth this driver publishes -- sweeps 0..9 and wraps
             * on every further percent's worth of charge, which is the 99.0 -> 99.9
             * -> 99.0 sawtooth an operator sees while the 99% hold is in force.
             * Pinned exactly on the boundary, the remainder is 0 and full reads
             * 100.0%.
             */
            if (g_soc_dep <= 0) {
                g_soc_dep = 0;
                g_soc_car = (int64_t)g_soc_car_base * PMIC_SOC_CAR_PER_PCT;
            }
        }
        g_soc_car_us = (now != 0u) ? now : 1u;
        g_soc_charge_us = (now != 0u) ? now : 1u;
        /* So a charge run that ends leaves the voltage path a fresh interval
         * rather than a step it is instantly owed. */
        g_soc_slew_us = (now != 0u) ? now : 1u;
    } else if (g_soc_car_base >= 0 && g_soc_charge_us != 0u && now >= g_soc_charge_us &&
               (now - g_soc_charge_us) < (uint64_t)PMIC_SOC_CAR_GRACE_US) {
        /*
         * A GAP INSIDE A LIVE RUN IS A GAP, NOT AN UNPLUG.
         *
         * See PMIC_SOC_CAR_GRACE_US. The clock advances so the gap is not billed
         * as charge that was never delivered, the count itself is untouched, and
         * the level holds. The slew timer is advanced too: a run that ends for
         * real should hand the voltage path a fresh interval rather than a step
         * it is owed the instant it takes over.
         */
        g_soc_car_us = (now != 0u) ? now : 1u;
        g_soc_slew_us = (now != 0u) ? now : 1u;
    } else if (g_soc_slew_us == 0u ||
               (now >= g_soc_slew_us &&
                (now - g_soc_slew_us) >= (uint64_t)PMIC_SOC_SLEW_INTERVAL_US)) {
        /* Discharging, or a current this driver could not read — which is not a
         * charge, and is not treated as one by stock either (oam_i >= 0 means
         * discharging). Here the terminal voltage IS the cell, so the curve is
         * the best estimate there is and the level walks towards it.
         *
         * ── TOWARDS IT MEANS BOTH WAYS, AND IT ONLY WENT ONE ──
         *
         * This used to be `if (g_soc_dep < target_dep) ++g_soc_dep' and nothing
         * else: depletion could rise and never fall. On a board where nothing else
         * moves the level downward that is a ratchet — the only other path that
         * decreases depletion is the coulomb integrator, and that only runs while a
         * charger is delivering current. Any sag drags the level towards empty and
         * the recovery cannot bring it back, so the meter walks to 0% and stays
         * there for the rest of the session whatever the cell recovers to. That is
         * "it says 0% and it is mostly full": not a scale error, a latch.
         *
         * The same 30 s gate governs both directions, so a transient cannot lift
         * the level any faster than it could drop it, and stock's oam_run()
         * converges in both directions too. */
        g_soc_car_base = -1;
        g_soc_car = 0;
        if (g_soc_dep < target_dep) {
            ++g_soc_dep;
        } else if (g_soc_dep > target_dep) {
            --g_soc_dep;
        }
        g_soc_slew_us = (now != 0u) ? now : 1u;
    }

    /*
     * ── FULL IS 100, AND THE INTEGRATOR IS TOLD SO ──
     *
     * The ladder terminating IS what full means on this board: the node has been at
     * or above top-off in this run and the pack has taken nothing for six samples
     * together. Against that the integrator is not the better authority — it has
     * been counting coulombs into a pack that stopped accepting them — so the level
     * is SET to 100 here rather than merely displayed as 100 below.
     *
     * Setting it matters at the unplug. A gauge left at, say, 97 while the display
     * was forced to 100 would snap back to 97 the moment the cable came out, which
     * is the same lie told in the other direction and the one that looks like a
     * fault. Ending the coulomb run with it means a recharge re-bases from 100
     * instead of billing this pin to the accumulator.
     *
     * It cannot flap: g_charge_full is cleared by ONE thing, the pack sitting under
     * the recharge voltage for six samples together. The cable is not one of them —
     * see the head of ladder_advance().
     */
    if (g_charge_full) {
        g_soc_dep = 0;
        g_soc_car_base = -1;
        g_soc_car = 0;
    }

    if (g_soc_dep < 0) g_soc_dep = 0;
    if (g_soc_dep > 100) g_soc_dep = 100;
    pct = 100 - g_soc_dep;

    /* 100% is the ladder's word, not the curve's — except when the node
     * is already sitting on the CV rail.  Stock's 99% hold assumed a
     * 150 mA termination that this shunt cannot see (it includes the
     * system's own draw).  4200 mV is CV; a pack still filling in CC
     * is well below that. */
    if (online == 1 && !g_charge_full && pct >= 100 &&
        bat_mv < PMIC_CV_HOLD_MV)
        pct = 99;
    return pct;
}

/*
 * The coulomb accumulator's remainder, as tenths of a percent, 0..9.
 *
 * A percent of this pack is ~97 s at 550 mA, so the whole-percent level is
 * genuinely still for minutes at a time and a screen showing it looks stuck. The
 * remainder is the part of the next percent already delivered -- the same count,
 * just not rounded away -- so it moves every few seconds and it moves for a real
 * reason.
 *
 * Zero whenever no charge is being counted. The voltage path moves in whole
 * percents and has no fraction to report, and inventing one would be a decimal
 * place with nothing behind it.
 */
static int gauge_frac_tenths(void) {
    int64_t rem;

    if (g_soc_car_base < 0 || g_soc_car <= 0) return 0;
    rem = g_soc_car % PMIC_SOC_CAR_PER_PCT;
    if (rem < 0) return 0;
    rem = (rem * 10) / PMIC_SOC_CAR_PER_PCT;
    if (rem < 0) return 0;
    return (rem > 9) ? 9 : (int)rem;
}

/* oam_d_3, published for a console: the live lookup the level is slewing toward,
 * with none of the seeding, slewing or capping applied. Diagnostic only — this is
 * the number that reads 99% with a cable in, which is the whole reason the
 * published level is not it. */
int mt6592_pmic_soc_target_percent(void) {
    const int mv = sense_mv_from_counts(g_adc[ADC_BATSNS].median);
    int ma_valid = 0;
    int ma;
    int pct;

    if (mv <= 0) return -1;
    ma = gauge_current_ma(&ma_valid);
    pct = mt6592_battery_percent_from_ocv(mt6592_battery_ocv_mv(mv, ma_valid ? ma : 0));
    if (pct < 0) return -1;
    return (pct > 100) ? 100 : pct;
}

/* POWER_SUPPLY_STATUS_*, from what is actually known. Callers pass online == 1
 * only; the -1 case is answered UNKNOWN before this is reached. */
static int gauge_status(int online, int ma, int ma_valid) {
    if (online != 1) return (int)MT6592_PMIC_STATUS_DISCHARGING;
    if (g_charge_full) return (int)MT6592_PMIC_STATUS_FULL;
    /* A cable with an unmeasurable current is not NOT_CHARGING — it is unknown,
     * and saying NOT_CHARGING would be an assertion about the charger drawn from
     * the state of this driver's ring buffers. */
    if (!ma_valid) return (int)MT6592_PMIC_STATUS_UNKNOWN;
    return (ma > 0) ? (int)MT6592_PMIC_STATUS_CHARGING
                    : (int)MT6592_PMIC_STATUS_NOT_CHARGING;
}

int mt6592_pmic_gauge_ocv(int terminal_mv, int* out_ocv_mv, int* out_r_mohm,
                          int* out_charge_ma) {
    int ma_valid = 0;
    int ma;
    int ocv;

    if (terminal_mv <= 0) return -1;

    ma = gauge_current_ma(&ma_valid);
    if (!ma_valid) ma = 0;
    ocv = mt6592_battery_ocv_mv(terminal_mv, ma);

    if (out_ocv_mv) *out_ocv_mv = ocv;
    /* The R the curve used at the SOLVED voltage, not at the terminal one — that
     * is the point of the iteration and the number worth showing. */
    if (out_r_mohm) *out_r_mohm = mt6592_battery_r_bat_mohm(ocv);
    if (out_charge_ma) *out_charge_ma = ma;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * PROPERTIES
 * ══════════════════════════════════════════════════════════════════════════ */

int mt6592_pmic_get_property(mt6592_psy_prop_t prop, int* val) {
    const uint64_t now = mt6592_timer_microseconds();
    int ma_valid = 0;
    int online;
    int ma;
    int mv;

    if (!val) return -1;

    /* Every path below either returns 0 having written *val, or returns -1
     * having touched nothing. That is the -ENODATA contract in the header and it
     * is what lets a caller seed a default and ignore the result. */
    switch (prop) {
        case MT6592_PSY_ONLINE:
            online = charger_detect_poll(now);
            if (online < 0) return -1;
            *val = online;
            return 0;

        case MT6592_PSY_STATUS:
            online = charger_detect_poll(now);
            if (online < 0) return -1;
            ma = gauge_current_ma(&ma_valid);
            *val = gauge_status(online, ma, ma_valid);
            return 0;

        case MT6592_PSY_VOLTAGE_NOW:
            mv = sense_mv_from_counts(g_adc[ADC_BATSNS].median);
            if (mv <= 0) return -1;
            *val = mv * 1000;
            return 0;

        case MT6592_PSY_VOLTAGE_OCV:
            mv = sense_mv_from_counts(g_adc[ADC_BATSNS].median);
            if (mv <= 0) return -1;
            ma = gauge_current_ma(&ma_valid);
            *val = mt6592_battery_ocv_mv(mv, ma_valid ? ma : 0) * 1000;
            return 0;

        case MT6592_PSY_CURRENT_NOW:
            ma = gauge_current_ma(&ma_valid);
            if (!ma_valid) return -1;
            *val = ma * 1000;
            return 0;

        case MT6592_PSY_CAPACITY: {
            int pct;
            mv = sense_mv_from_counts(g_adc[ADC_BATSNS].median);
            if (mv <= 0) return -1;
            online = charger_detect_poll(now);
            ma = gauge_current_ma(&ma_valid);
            pct = gauge_percent(online, mv, ma, ma_valid, (int*)0);
            if (pct < 0) return -1;
            *val = pct;
            return 0;
        }

        case MT6592_PSY_INPUT_VOLTAGE_NOW:
            mv = vchr_mv_from_counts(g_adc[ADC_VCHR].median);
            if (mv < 0) return -1;
            *val = mv * 1000;
            return 0;

        case MT6592_PSY_CONSTANT_CHARGE_VOLTAGE:
            mv = mt6592_pmic_charger_cv_mv();
            if (mv < 0) return -1;
            *val = mv * 1000;
            return 0;

        /* The USB side. All four are functions of state BC1.2 already settled, so
         * none of them can fail -- but UNKNOWN is -ENODATA rather than a zero,
         * because "no cable" and "a cable I have not classified yet" are different
         * facts and a caller that seeds a default deserves to keep it. */
        case MT6592_PSY_USB_ONLINE:
            if (charger_detect_poll(now) < 0) return -1;
            *val = mt6592_pmic_usb_online();
            return 0;

        case MT6592_PSY_MAINS_ONLINE:
            if (charger_detect_poll(now) < 0) return -1;
            *val = mt6592_pmic_mains_online();
            return 0;

        case MT6592_PSY_USB_TYPE:
            *val = g_bc11_type;
            return 0;

        case MT6592_PSY_CURRENT_MAX:
            if (g_bc11_limit_ma <= 0) return -1;
            *val = g_bc11_limit_ma * 1000;
            return 0;

        default: return -1;
    }
}

int mt6592_pmic_sample_blocking(mt6592_psy_prop_t prop, int* val) {
    if (!val) return -1;

    /* Pump only the channels this property actually needs. Failures are ignored
     * on purpose: get_property() below is the single place that decides whether
     * there is an answer, so a half-filled ring reports -ENODATA rather than a
     * second, differently-worded failure. */
    switch (prop) {
        case MT6592_PSY_VOLTAGE_NOW:
            (void)adc_pump_blocking((uint32_t)ADC_BATSNS, (uint32_t)ADC_MEDIAN);
            break;

        case MT6592_PSY_VOLTAGE_OCV:
        case MT6592_PSY_CURRENT_NOW:
        case MT6592_PSY_CAPACITY:
        case MT6592_PSY_STATUS:
            /* All four are functions of the shunt, so of ADJACENT pairs across it
             * — stock's own interleave, not five of one rail then five of the
             * other. Five rounds is ten conversions, ~30 ms at 1 ms of settle. */
            (void)adc_pump_pair((uint32_t)ADC_MEDIAN);
            g_last_chrdet_poll_us = 0u;
            break;

        case MT6592_PSY_INPUT_VOLTAGE_NOW:
            (void)adc_pump_blocking((uint32_t)ADC_VCHR, (uint32_t)ADC_MEDIAN);
            break;

        case MT6592_PSY_ONLINE:
        case MT6592_PSY_USB_ONLINE:
        case MT6592_PSY_MAINS_ONLINE:
            /* One comparator read, no conversion — but force it to be a fresh
             * one, since "blocking" means the caller wants now, not 100 ms ago.
             *
             * The two USB variants do NOT wait for a classification. Nothing here
             * may block for the 660 ms BC1.2 takes, and pumping the state machine
             * from a "blocking" call would be exactly the frame-path stall the
             * split between these two functions exists to prevent. A caller that
             * genuinely needs the type waits on
             * mt6592_pmic_charger_generation() instead. */
            g_last_chrdet_poll_us = 0u;
            break;

        case MT6592_PSY_CONSTANT_CHARGE_VOLTAGE:
        default: break;
    }

    return mt6592_pmic_get_property(prop, val);
}

int mt6592_pmic_battery_read(mt6592_pmic_battery_t* out) {
    const uint64_t now = mt6592_timer_microseconds();
    int ma_valid = 0;
    int online;
    int ma;
    int bat_mv;
    int ocv = -1;

    if (!out) return -1;

    /*
     * EVERY FIELD, BEFORE ANYTHING CAN FAIL. The header states this and it is
     * enforced here because it has been broken twice, and both times a caller
     * that ignored the return code read a stack value as a charge level and
     * parked the loader in a charge screen it could not leave.
     */
    out->charger_online = -1;
    out->status = (int)MT6592_PMIC_STATUS_UNKNOWN;
    out->battery_mv = -1;
    out->ocv_mv = -1;
    out->battery_percent = -1;
    out->battery_permille = -1;
    out->current_ma = 0;
    out->current_valid = 0;
    out->charger_mv = -1;
    out->usb_type = (int)MT6592_PMIC_USB_TYPE_UNKNOWN;
    out->input_limit_ma = 0;

    if (!pwrap_up()) return -1;

    online = charger_detect_poll(now);
    out->charger_online = online;

    /* Both are already-settled state, so they are filled here rather than behind
     * any of the measurement guards below: a wedged converter costs the voltages,
     * not the knowledge of what is plugged in. */
    out->usb_type = g_bc11_type;
    out->input_limit_ma = g_bc11_limit_ma;

    bat_mv = sense_mv_from_counts(g_adc[ADC_BATSNS].median);
    if (bat_mv > 0) out->battery_mv = bat_mv;

    ma = gauge_current_ma(&ma_valid);
    out->current_valid = ma_valid;
    out->current_ma = ma_valid ? ma : 0;

    if (bat_mv > 0) {
        out->battery_percent = gauge_percent(online, bat_mv, ma, ma_valid, &ocv);
        out->ocv_mv = ocv;
        if (out->battery_percent >= 0) {
            out->battery_permille = out->battery_percent * 10 + gauge_frac_tenths();
            /* The tenth is the coulomb accumulator's remainder, and the count keeps
             * running after the level itself has saturated at 100 -- so a full pack
             * would render "100.4%". Full is 100.0% and nothing above it. */
            if (out->battery_permille > 1000) out->battery_permille = 1000;
        }
    }

    /* VCHR is only converted while a cable is detected, and its ring is forgotten
     * on unplug, so this is -1 offline rather than the last cable's voltage. */
    if (online == 1) {
        const int cmv = vchr_mv_from_counts(g_adc[ADC_VCHR].median);
        if (cmv >= 0) out->charger_mv = cmv;
    }

    out->status = (online < 0) ? (int)MT6592_PMIC_STATUS_UNKNOWN
                               : gauge_status(online, ma, ma_valid);

    /*
     * Non-zero while the cable state is still unread. lk_charge_park() boots
     * through on a non-zero return, which is the correct default for "no
     * information": a park screen entered on a guess is a board that will not
     * boot, and this driver has done that before.
     */
    return (online < 0) ? -1 : 0;
}

mt6592_pmic_supply_icon_t mt6592_pmic_supply_icon(const mt6592_pmic_battery_t* bat) {
    /* Two states, because two states is what the board can tell apart. A null
     * sample or an undetermined cable draws the battery, which is the one that
     * claims less. */
    if (bat && bat->charger_online == 1) return MT6592_PMIC_ICON_BATTERY_CABLE;
    return MT6592_PMIC_ICON_BATTERY;
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE CHARGER, WRITE SIDE
 * ══════════════════════════════════════════════════════════════════════════ */

/* Read-modify-write that reports whether it changed anything: 1 a real write,
 * 0 already correct, -1 pwrap failed. The distinction matters because this runs
 * twice a second and most of the time every register is already right. */
static int charger_rmw(uint32_t addr, uint32_t clear_mask, uint32_t set_mask) {
    uint32_t cur = 0;
    uint32_t want;

    if (mt6592_pwrap_read(addr, &cur) != 0) return -1;
    want = (cur & ~clear_mask) | set_mask;
    if (want == cur) return 0;
    if (mt6592_pwrap_write(addr, want) != 0) return -1;
    return 1;
}

/*
 * ── HOW MUCH CURRENT TO ASK FOR ──
 *
 * charging_hw_pmic.c's CS_VTH[] table, index to milliamps, descending. Stock's
 * charging_set_current() walks it for the largest step that does not exceed the
 * limit the charger type allows, and that is what this does.
 *
 * It matters because the alternative — one fixed step — is what this file did, and
 * 450 mA is not obviously more than this firmware's own draw. VBAT is VSYS on this
 * board: there is no power-path FET, so the panel, the backlight and a park loop
 * that spins rather than idles all come out of the same node the charger feeds. Ask
 * for less than the board burns and the pack quietly makes up the difference, which
 * presents as a charger that does nothing while every register reads back armed.
 */
static const uint16_t CHARGER_CS_VTH_MA[16] = {1600, 1500, 1400, 1300, 1200, 1100,
                                               1000, 900,  800,  700,  650,  550,
                                               450,  300,  200,  70};

/*
 * The cap, and the reason there is one.
 *
 * CS_VTH sets how hard the CSDAC drives VBAT, and a cell-less board's VBAT is its
 * own supply, so raising this once drove the node into OVP and latched the PMIC
 * off. That measurement stands — but it was taken when CHR_CON3 still held its
 * unwritten power-on 29 and the CV loop was not armed, i.e. with nothing in the
 * hardware regulating the node at all. VBAT_CV_EN, HWCV_EN, the 4200 mV CV target
 * and VBAT_OV at 4300 mV are all armed below now, and between them they clamp the
 * node whatever this field says.
 *
 * Capped at the largest step a real wall charger's own limit reaches rather than at
 * the table's 1600 mA top, because nothing here has measured 1.6 A into this board
 * and an unmeasured ceiling is not a ceiling.
 */
enum { CHARGER_CS_VTH_CAP_MA = 950 };

/* The step CHR_CON4 is actually programmed to, in mA; -1 until the register has
 * been read once. This is the charger's own setting rather than a measurement,
 * and it is the one live number available for what the port is being asked to
 * deliver -- there is no input-side shunt on this board. */
static int g_charge_step_ma = -1;

int mt6592_pmic_charge_step_ma(void) { return g_charge_step_ma; }

/* The step to program for @p limit_ma, as an index into the table above. A limit of
 * 0 means BC1.2 has not answered yet, and the answer then is the conservative one
 * this file has always used: 450 mA, which is also stock LK's single fixed step. */
static uint32_t charger_cs_vth_code(int limit_ma) {
    uint32_t i;

    if (limit_ma <= 0) return (uint32_t)PMIC_CHR_CON4_CS_VTH_DEFAULT;
    if (limit_ma > (int)CHARGER_CS_VTH_CAP_MA) limit_ma = (int)CHARGER_CS_VTH_CAP_MA;

    for (i = 0u; i < 16u; ++i) {
        if ((int)CHARGER_CS_VTH_MA[i] <= limit_ma) return i;
    }
    /* Unreachable while the table ends at 70 mA and limit_ma is positive. */
    return (uint32_t)PMIC_CHR_CON4_CS_VTH_DEFAULT;
}

/*
 * ══ IS IT CHARGING? ══
 *
 * Three numbers, every ~10 s, on one line, and the reason it is three:
 *
 *   CS_DET is not the answer. It is an instantaneous comparator on a current
 *     source that HWCV and the CSDAC ramp are actively modulating, so a snapshot
 *     catching it low says nothing; it read 0 and 1 alternately on a board that
 *     was drawing a steady half-amp.
 *
 *   pack mA is stock's own measurement -- battery_meter.c:2695, ISENSE minus
 *     BATSNS over R_SENSE -- and stock CLAMPS IT TO ZERO when ISENSE <= BATSNS.
 *     That pair can only ever report a charge, never a discharge, so a positive
 *     number here is not by itself proof: a fixed offset between the two channels
 *     would read as a constant phantom current and look exactly like a charge.
 *     Stock has g_I_SENSE_offset for precisely this, defaulting to 0.
 *
 *   VBAT is the one that cannot be faked. A cell that is taking charge climbs. If
 *     this number rises over a couple of minutes the board is charging whatever
 *     the other two say, and if it falls it is not.
 *
 * Printed on BOTH paths, cable in and cable out, because the cable-out reading is
 * the control: ~0 mA with no VBUS means the shunt pair is honest, and the same
 * ~500 mA with no VBUS means it is an offset and the "charge current" is fiction.
 * The first version of this line only printed with a cable in, which is exactly the
 * half of the experiment that cannot distinguish the two.
 */
static void charging_line(int online) {
    static uint32_t kicks;
    int ma_valid = 0;
    int ma;
    uint32_t con2 = 0;
    int cs_det;

    if (++kicks % 20u != 0u) return;

    ma = gauge_current_ma(&ma_valid);
    cs_det = (mt6592_pwrap_read(PMIC_CHR_CON2, &con2) == 0)
                 ? ((con2 & PMIC_CHR_CON2_CS_DET) ? 1 : 0)
                 : -1;

    plog(online ? "pmic: charging: VBUS=1 CS_DET=" : "pmic: charging: VBUS=0 CS_DET=");
    if (cs_det < 0) {
        plog("?");
    } else {
        plog_dec((uint32_t)cs_det);
    }

    plog(" pack ");
    if (!ma_valid) {
        plog("(no sample)");
    } else {
        if (ma < 0) plog("-");
        plog_dec((uint32_t)(ma < 0 ? -ma : ma));
        plog(" mA");
    }

    plog(" VBAT ");
    if (g_adc[ADC_BATSNS].n != 0u) {
        plog_dec((uint32_t)sense_mv_from_counts(g_adc[ADC_BATSNS].median));
        plog(" mV");
    } else {
        plog("(no sample)");
    }
    plog("\n");
}

/*
 * Re-arm the input path and the CV target. Every write here is idempotent and
 * every one of them has been justified on hardware; the order is load-bearing:
 * UVLO and VCDT before anything is enabled, the CV TARGET before the loop that
 * regulates to it, and CHR_EN last.
 */
static void charger_arm(uint64_t now) {
    static int charger_was_online = -1;
    static int dumped;
    static uint32_t armed_kicks;
    uint32_t con0 = 0;

    if (g_last_charger_kick_us != 0u && now >= g_last_charger_kick_us &&
        (now - g_last_charger_kick_us) < (uint64_t)CHARGER_KICK_INTERVAL_US) {
        return;
    }
    g_last_charger_kick_us = (now != 0u) ? now : 1u;

    if (!pwrap_up() || mt6592_pwrap_read(PMIC_CHR_CON0, &con0) != 0) {
        static int fail_logged;
        /*
         * Nothing was done, so nothing has earned the full interval — and both
         * causes (a pwrap not up yet, a pwrap momentarily wedged) clear on their
         * own inside a few tens of milliseconds. Back-dated the way round that
         * cannot wrap; the static assert at the top of the file guarantees the
         * subtraction is positive.
         */
        const uint64_t backoff =
            (uint64_t)CHARGER_KICK_INTERVAL_US - (uint64_t)CHARGER_KICK_RETRY_US;
        g_last_charger_kick_us = (now > backoff) ? (now - backoff) : 1u;
        if (!fail_logged) {
            fail_logged = 1;
            plog("pmic: charger kick failed (pwrap down/wedged)\n");
        }
        return;
    }

    /* Widest ride-through on VSYS. With no cell fitted VBAT *is* VSYS, so a dip
     * that trips UVLO is not a brownout warning — it is the power cut, and it
     * presents to an operator as a spontaneous restart. */
    (void)charger_rmw(PMIC_CHR_CON16, PMIC_CHR_CON16_UVLO_VTHL_MASK,
                      PMIC_CHR_CON16_UVLO_VTHL_LOWEST);

    /*
     * ══ HAND THE CHARGER BACK TO SOFTWARE ══
     *
     * FIRST, ahead of every other charger write, because until this lands none of
     * them reach the block: see the note on CHR_CON16_USBDL_SET. The preloader
     * left the charger in hardware USBDL mode; clear the request, then pulse the
     * reset, which is the order the preloader's own exit path uses.
     *
     * Both are idempotent and both stay in this per-kick path rather than a
     * one-shot init, because a mode this firmware never asked for is a mode it
     * cannot assume stays gone — nothing here owns the preloader, and a warm
     * re-entry would put it straight back.
     */
    {
        const int cleared = charger_rmw(PMIC_CHR_CON16, PMIC_CHR_CON16_USBDL_SET, 0u);
        const int reset = charger_rmw(PMIC_CHR_CON16, 0u, PMIC_CHR_CON16_USBDL_RST);
        static int usbdl_logged;

        /* Worth one line the first time either bit actually moved: it is the
         * difference between "armed and charging" and "armed and ignored", and it
         * is invisible in a register dump taken afterwards. */
        if (!usbdl_logged && (cleared == 1 || reset == 1)) {
            usbdl_logged = 1;
            plog("pmic: left hardware USBDL charging mode (CHR_CON16 SET->0, RST->1)\n");
        }
    }

    /*
     * The rest of stock's charging_hw_init(), in its order, minus the one write
     * this board deliberately differs on: VCDT_HV_EN, which stays off because its
     * threshold register has never been written here. (The charger watchdog was
     * the other, and it was a mistake — see charger_watchdog_kick(), called from
     * the enable below where stock calls it.)
     */
    (void)charger_rmw(PMIC_CHR_CON23, PMIC_CHR_CON23_VCDT_MODE, 0u);
    (void)charger_rmw(PMIC_CHR_CON6,
                      PMIC_CHR_CON6_VBAT_OV_VTH_MASK,
                      PMIC_CHR_CON6_VBAT_OV_EN | PMIC_CHR_CON6_VBAT_OV_VTH_4300MV);
    (void)charger_rmw(PMIC_CHR_CON7, PMIC_CHR_CON7_BATON_HT_EN,
                      PMIC_CHR_CON7_BATON_EN);
    (void)charger_rmw(PMIC_CHR_CON23, 0u, PMIC_CHR_CON23_ULC_DET_EN);
    (void)charger_rmw(PMIC_CHR_CON22, PMIC_CHR_CON22_LOW_ICH_DB_MASK,
                      PMIC_CHR_CON22_LOW_ICH_DB_DEFAULT);

    /* VCDT_HV_EN gates the input on an over-voltage comparator whose threshold
     * this firmware has never set. Off, and re-read so the CHRDET test below
     * sees the register as it now is. */
    if ((con0 & PMIC_CHR_CON0_VCDT_HV_EN) != 0u &&
        mt6592_pwrap_write(PMIC_CHR_CON0, con0 & ~(uint32_t)PMIC_CHR_CON0_VCDT_HV_EN) == 0) {
        if (mt6592_pwrap_read(PMIC_CHR_CON0, &con0) != 0) {
            con0 &= ~(uint32_t)PMIC_CHR_CON0_VCDT_HV_EN;
        }
    }

    if ((con0 & PMIC_CHR_CON0_CHRDET) == 0u) {
        if (charger_was_online != 0) {
            charger_was_online = 0;
            plog("pmic: no VBUS; system on battery\n");
        }
        /* Cable out: whatever a console probe left in CHR_CON3 describes a
         * measurement that is over. Hand the register back so the next insertion
         * re-establishes the 4200 mV target below. */
        mt6592_pmic_charger_cv_release();
        /* And stop the watchdog, because the kick at the end of this function is
         * what feeds it and this return is the path that skips it. */
        charger_watchdog_disarm();
        /* The control reading. See charging_line(): this is the half of the
         * experiment that says whether the other half means anything. */
        charging_line(0);
        return;
    }

    if (!dumped) {
        dumped = 1;
        plog_reg("CHR_CON0", PMIC_CHR_CON0);
        plog_reg("CHR_CON1", PMIC_CHR_CON1);
        plog_reg("CHR_CON2", PMIC_CHR_CON2);
        plog_reg("CHR_CON3", PMIC_CHR_CON3);
        plog_reg("CHR_CON4", PMIC_CHR_CON4);
        plog_reg("CHR_CON6", PMIC_CHR_CON6);
        plog_reg("CHR_CON7", PMIC_CHR_CON7);
        plog_reg("CHR_CON13", PMIC_CHR_CON13);
        /* Bit 3 set here means the charger was still in hardware USBDL mode when
         * this dump ran, i.e. the release above did not take. */
        plog_reg("CHR_CON16", PMIC_CHR_CON16);
        plog_reg("CHR_CON20", PMIC_CHR_CON20);
        plog_reg("CHR_CON21", PMIC_CHR_CON21);
        plog_reg("CHR_CON22", PMIC_CHR_CON22);
        plog_reg("CHR_CON23", PMIC_CHR_CON23);
    }

    /* The charge current, from what BC1.2 said the port can give. 450 mA until it
     * has said anything, which is the step stock LK uses and never changes. */
    {
        const uint32_t code = charger_cs_vth_code(g_bc11_limit_ma);
        static int step_logged = -1;
        uint32_t con4 = 0u;

        (void)charger_rmw(PMIC_CHR_CON4, PMIC_CHR_CON4_CS_VTH_MASK, code);

        /*
         * READ THE FIELD BACK; DO NOT INFER IT FROM THE WRITE.
         *
         * This used to publish the step only when charger_rmw() returned 1, which is
         * "a real write happened". Its other success value is 0, "already correct",
         * and that arm is by far the common one -- this runs twice a second and the
         * field is right after the first pass. So on any boot where CHR_CON4 already
         * held this code, nothing ever latched, and the detail screen's charger gauge
         * drew a dash until an unplug moved CHR_CON4 out from under it and made the
         * replug's arm a genuine write. "The mA gauge only works when unplugged and
         * re-plugged" was that `== 1' and nothing else.
         *
         * What the gauge reports is what the charger is set to, so it comes from the
         * register rather than from our intent to write it -- which is also correct if
         * anything else ever moves the field.
         */
        if (mt6592_pwrap_read(PMIC_CHR_CON4, &con4) == 0) {
            g_charge_step_ma =
                (int)CHARGER_CS_VTH_MA[con4 & (uint32_t)PMIC_CHR_CON4_CS_VTH_MASK];
        }

        /* Logged off the readback for the same reason, and keyed on the step rather
         * than on the requested code: a write that silently did not take now says so,
         * by printing the step the charger is really on. A failed pwrap read leaves
         * the step unchanged, so it cannot print `con4' as 0 next to a stale value. */
        if (g_charge_step_ma > 0 && step_logged != g_charge_step_ma) {
            step_logged = g_charge_step_ma;
            plog("pmic: charge current ");
            plog_dec((uint32_t)g_charge_step_ma);
            plog(" mA (CS_VTH=");
            plog_dec(con4 & (uint32_t)PMIC_CHR_CON4_CS_VTH_MASK);
            plog(", port allows ");
            plog_dec((uint32_t)(g_bc11_limit_ma > 0 ? g_bc11_limit_ma : 0));
            plog(" mA)\n");
        }
    }

    /*
     * ══ THE CV TARGET, WHICH NOTHING HERE HAD EVER WRITTEN ══
     *
     * "It never charges." CHR_CON3[4:0] powers on at 29, and code 29 in stock's
     * own table is 4162 mV. The node sits at 4183. The setpoint was twenty-one
     * millivolts BELOW the cell, and a CV loop asked to regulate to a voltage the
     * pack has already passed sources nothing, because a charger cannot sink.
     * Every other register this function arms was armed correctly and then handed
     * a target the pack had already cleared.
     *
     * Raising is the safe direction and lowering is not: the CV sweep killed a
     * cell-less board in under a millisecond when the setpoint went below the
     * node, because the charger's output is the system rail. Written here, ahead
     * of CS_EN/CSDAC_EN/CHR_EN, so the loop comes up against the right target
     * rather than being corrected after the fact.
     */
    if (!cv_operator_holds(now) &&
        charger_rmw(PMIC_CHR_CON3, PMIC_CHR_CON3_CV_MASK, PMIC_CHR_CON3_CV_4200MV) == 1) {
        /* Read it back. A silently ignored write would look exactly like this bug
         * still being present, which is how it survived as long as it did. */
        const int code = mt6592_pmic_charger_cv_code();
        plog("pmic: CV target set to 4200 mV (CHR_CON3[4:0]=0), read back ");
        if (code < 0) {
            plog("FAILED\n");
        } else {
            plog_dec((uint32_t)code);
            plog(code == (int)PMIC_CHR_CON3_CV_4200MV ? " ok\n" : " MISMATCH\n");
        }
    }

    /* The CSDAC ramp: one step up, two down, four-unit delay, unit step. Stock's
     * values; a gentler rise than the default and the reason the arm sequence
     * does not slam the node. */
    (void)charger_rmw(PMIC_CHR_CON20, 0xffffu,
                      PMIC_CHR_CON20_CSDAC_STP_INC_1 | PMIC_CHR_CON20_CSDAC_STP_DEC_2);
    (void)charger_rmw(PMIC_CHR_CON21, 0xffffu,
                      PMIC_CHR_CON21_CSDAC_DLY_4 | PMIC_CHR_CON21_CSDAC_STP_1);
    (void)charger_rmw(PMIC_CHR_CON2, 0u, PMIC_CHR_CON2_VBAT_CV_EN | PMIC_CHR_CON2_CS_EN);
    (void)charger_rmw(PMIC_CHR_CON23, 0u,
                      PMIC_CHR_CON23_CSDAC_MODE | PMIC_CHR_CON23_HWCV_EN);

    /* The watchdog, immediately before the enable, because that is where all three
     * stock paths put it and because on this PMIC the charger gates on it. Not
     * folded into charger_rmw(): every write in it is a kick whose value is already
     * in the register, so "already correct, skip it" would skip the entire point. */
    charger_watchdog_kick();

    /* Last: the current source and the charger itself. */
    (void)charger_rmw(PMIC_CHR_CON0, PMIC_CHR_CON0_VCDT_HV_EN,
                      PMIC_CHR_CON0_CSDAC_EN | PMIC_CHR_CON0_CHR_EN);

    /*
     * ══ A SECOND DUMP, AFTER THE ARM ══
     *
     * The dump above is the state this function INHERITED — it runs before a
     * single arm write, which is why it showed CHR_CON2 with VBAT_CV_EN still
     * clear and CHR_CON3 still at its power-on 29. Useful for reading what the
     * preloader left, useless for answering the only question that matters:
     * armed and charging, or armed and ignored.
     *
     * CHR_CON2 is the register to read first, and it answers it in hardware's own
     * words rather than this driver's:
     *
     *   bit 5  RGS_CS_DET       the current source is delivering
     *   bit 7  RGS_VBAT_CC_DET  still in constant current, i.e. not yet at CV
     *   bit 6  RGS_VBAT_CV_DET  regulating at the CV target, i.e. nearly done
     *   bit 1  RG_VBAT_CV_EN    the CV loop, which the write above turns on
     *   bit 3  RG_CS_EN         the current source
     *
     * CS_DET set, with CHR_EN and CSDAC_EN set in CHR_CON0, is a charge in
     * progress whatever the gauge makes of it. CS_DET clear with all the enables
     * set is the charger refusing, and then the fault is upstream of every
     * register in this file.
     *
     * Taken on the SECOND kick, not this one: the CSDAC ramps at the rate
     * CHR_CON20/21 set, and a status bit read microseconds after CHR_EN went high
     * describes a converter that has not started yet.
     */
    if (armed_kicks < 2u && ++armed_kicks == 2u) {
        plog("pmic: post-arm charger state (CHR_CON2 bit5 CS_DET = current flowing)\n");
        plog_reg("CHR_CON0", PMIC_CHR_CON0);
        plog_reg("CHR_CON2", PMIC_CHR_CON2);
        plog_reg("CHR_CON3", PMIC_CHR_CON3);
        plog_reg("CHR_CON4", PMIC_CHR_CON4);
        /* CON13 bit 4 is the armed watchdog and CON15 bit 2 is RGS_CHRWDT_OUT, the
         * expiry. Neither register appeared in any dump while the no-charge bug was
         * being chased, which is most of why it took as long as it did. */
        plog_reg("CHR_CON13", PMIC_CHR_CON13);
        plog_reg("CHR_CON15", PMIC_CHR_CON15);
        plog_reg("CHR_CON23", PMIC_CHR_CON23);
    }

    charging_line(1);

    if (charger_was_online != 1) {
        charger_was_online = 1;
        plog("pmic: VBUS present; input path armed\n");
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE SERVICE
 * ══════════════════════════════════════════════════════════════════════════ */

void mt6592_pmic_service(void) {
    const uint64_t now = mt6592_timer_microseconds();
    int online;

    if (MVII_WITHOUT_BATTERY) {
        static int held_logged;
        static int failed_logged;

        /* Do not leave USBDL and do not rewrite current or CV. Doing either
         * before the panel is up has latched this PMIC off, with no splash.
         * Disable the charger timer across storage reads and kernel startup.
         * Use the same checked sequence as stage1 and Linux. */
        if (g_last_charger_kick_us == 0u || now < g_last_charger_kick_us ||
            now - g_last_charger_kick_us >= (uint64_t)CHARGER_KICK_INTERVAL_US) {
            if (pwrap_up() && external_power_hold() == 0) {
                g_last_charger_kick_us = now ? now : 1u;
                failed_logged = 0;
                if (!held_logged) {
                    held_logged = 1;
                    plog("pmic: batteryless; charger watchdog OFF (verified), UVLO widened, USBDL left alone\n");
                }
            } else if (!failed_logged) {
                failed_logged = 1;
                plog("pmic: batteryless hold FAILED; retrying\n");
            }
        }
        return;
    }

    /*
     * The charger watchdog used to be kicked — disarmed, in fact — from right here,
     * first and unconditionally, on the grounds that it was what kept a cell-less
     * board from rebooting itself. It belongs with the charger enable instead, and
     * it is now kicked from charger_arm() where stock kicks it. Nothing else in this
     * function needs it, and a timer armed outside the sequence that feeds it is the
     * arrangement that caused both bugs: the reboot, and then the no-charge.
     */

    /* The wakeup OCV latch, once, as soon as the wrapper is up. One read on one
     * boot; a no-op branch on every call after. */
    hw_ocv_prime();

    /* The cable state, on its own cheap cadence. Read before the converter step
     * because it decides whether VCHR is worth a conversion at all. */
    online = charger_detect_poll(now);
    if (online != g_online_last) {
        /*
         * THE PLUG EVENT INVALIDATES EVERY RING, IN BOTH DIRECTIONS.
         *
         * This used to forget only VCHR, and only on unplug. That left the flaky
         * window: a median is a claim about a stationary quantity, and inserting a
         * cable moves all three at once. With no power-path FET on this board, VBAT
         * *is* VSYS, so plugging in steps BATSNS by hundreds of counts and ISENSE
         * by more; for the following second the rings held a blend of before and
         * after, and the medians — which pick the middle sample, i.e. neither
         * state — reported a voltage the cell never had and a current nothing ever
         * drew. Worse for the pair: a BATSNS from after minus an ISENSE from
         * before is hundreds of fabricated milliamps, which is exactly the
         * "charging" reading that appears a beat before the cable does.
         *
         * Throwing the history away costs one conversion of latency (~60 ms, since
         * a median of one now publishes) and in exchange the numbers only ever
         * describe one side of the event.
         */
        adc_forget(&g_adc[ADC_BATSNS]);
        adc_forget(&g_adc[ADC_VSEN]);
        adc_forget(&g_adc[ADC_VCHR]);
        delta_forget();

        /*
         * AND IT IS THE ONE MOMENT WORTH CLASSIFYING THE CABLE.
         *
         * BC1.2 asks a question about the thing on the other end of the wire, so
         * the only time the answer can change is when the wire does. Arm it on
         * arrival; throw it away on departure, because the next cable need not be
         * this cable and a limit carried across a plug is a limit granted to
         * hardware that never earned it.
         *
         * -1 -> 1 counts as an arrival: that is a board that booted with a charger
         * already in, which on this one is the common case.
         */
        if (online == 1) bc11_start();
        else if (g_online_last == 1) bc11_abandon();

        /* Bumped LAST, so a consumer that wakes on the count and immediately reads
         * every property sees the forgotten rings and the armed classifier rather
         * than the state they were in a moment ago. */
        ++g_chg_gen;
        g_online_last = online;
    }

    /* At most one BC1.2 transition per call. Nothing else in the service depends
     * on it, so it sits outside the edge block and runs on its own dwells. */
    bc11_service(now);

    /* 1. One transaction, one channel, round robin. */
    adc_step();

    /*
     * 2. The ladder: BOTH a new BATSNS median AND a full PMIC_LADDER_INTERVAL_US
     *    since the last rung. Stock counts samples, but stock's samples are 10 s
     *    apart, so the count and the interval were the same statement; here they
     *    are not, and a rung must mean a second of elapsed time or "six consecutive
     *    samples under 150 mA" degenerates into "one brief dip".
     */
    if (g_adc[ADC_BATSNS].updates != g_ladder_gen &&
        (g_ladder_us == 0u || (now >= g_ladder_us &&
                               (now - g_ladder_us) >= (uint64_t)PMIC_LADDER_INTERVAL_US))) {
        int ma_valid = 0;
        const int ma = gauge_current_ma(&ma_valid);
        g_ladder_gen = g_adc[ADC_BATSNS].updates;
        g_ladder_us = (now != 0u) ? now : 1u;
        ladder_advance(online, sense_mv_from_counts(g_adc[ADC_BATSNS].median), ma, ma_valid);
    }

    /* 3. Every 500 ms, re-arm the input path and the CV target, and kick the
     *    charger watchdog that the two of them depend on. */
    charger_arm(now);
}

/* The old name, thirty call sites, same meaning. A straight alias — not a subset,
 * because a caller that services "the charger" and silently does not advance the
 * converter is how the gauge came to depend on which of two entry points a given
 * loop happened to use. */
void mt6592_pmic_charger_service(void) {
    mt6592_pmic_service();
}

/*
 * The load-step hook. Card reads, app launches, Wi-Fi scans: each steps the
 * board's draw up sharply, and with no cell fitted there is nothing between that
 * step and the rail. Acting BEFORE the load arrives is the only useful moment.
 *
 * Zeroing the kick stamp runs the whole arm sequence now rather than waiting out
 * the gate. What it must NOT do is push CS_VTH somewhere else — an early version
 * did, and the board stopped booting without a cell.
 */
void mt6592_pmic_power_hold(void) {
    g_last_charger_kick_us = 0u;
    mt6592_pmic_service();
}

/* ══════════════════════════════════════════════════════════════════════════
 * POWER OFF (RTC BBPU)
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * Retire one RTC write. FUN_81e08c6c in the stock loader, verbatim except for the
 * bound: the RTC is on a 32 kHz domain behind pwrap, so a write is not a write
 * until the bridge retires it. Returns 0 when CBUSY went low.
 */
static int rtc_write_trigger(void) {
    uint32_t i;

    if (mt6592_pwrap_write(PMIC_RTC_WRTGR, 1u) != 0) return -1;

    for (i = 0u; i < PMIC_RTC_CBUSY_TRIES; ++i) {
        /* Initialised BUSY, and a read failure is a hard return rather than a
         * continue: mt6592_pwrap_read() leaves its output alone when the
         * transaction does not complete, so a zeroed variable plus a swallowed
         * error is a CBUSY that reads clear because nobody looked. "Not read"
         * must never mean "retired". */
        uint32_t bbpu = PMIC_RTC_BBPU_CBUSY;
        if (mt6592_pwrap_read(PMIC_RTC_BBPU, &bbpu) != 0) return -1;
        if ((bbpu & PMIC_RTC_BBPU_CBUSY) == 0u) return 0;
    }
    return -1;
}

/* Open the write interface. FUN_81e08c98: both halves of the key, each with its
 * own trigger. A half that does not retire leaves the interface shut, so there is
 * no point continuing to the second word. */
static int rtc_writeif_unlock(void) {
    if (mt6592_pwrap_write(PMIC_RTC_PROT, PMIC_RTC_PROT_KEY1) != 0) return -1;
    if (rtc_write_trigger() != 0) return -1;
    if (mt6592_pwrap_write(PMIC_RTC_PROT, PMIC_RTC_PROT_KEY2) != 0) return -1;
    if (rtc_write_trigger() != 0) return -1;
    return 0;
}

/*
 * Power off. FUN_81e08cc0.
 *
 * Two things had to be fixed for this to work at all and both are here: the write
 * interface must be unlocked or every RTC write is discarded, and the BBPU value
 * must carry the command bits (0x4309, not 0x4300).
 *
 * The tail is a spin ending in a message because a successful power-off does not
 * return — the rail is gone before the loop finishes. Reaching the bottom of this
 * function IS the failure report.
 */
void mt6592_pmic_power_off(void) {
    if (!pwrap_up()) return;
    mt6592_uart_puts("  pmic: power off (RTC BBPU)\n");

    if (rtc_writeif_unlock() != 0) {
        mt6592_uart_puts("  pmic: RTC write interface would not unlock\n");
        return;
    }

    if (mt6592_pwrap_write(PMIC_RTC_BBPU, PMIC_RTC_BBPU_KEY | PMIC_RTC_BBPU_AUTO |
                                              PMIC_RTC_BBPU_PWREN) != 0) {
        mt6592_uart_puts("  pmic: RTC BBPU write failed\n");
        return;
    }
    if (rtc_write_trigger() != 0) {
        mt6592_uart_puts("  pmic: RTC BBPU write did not retire\n");
        return;
    }

    for (volatile uint32_t i = 0; i < 50000000u; ++i) {
    }
    mt6592_uart_puts("  pmic: power off did not latch\n");
}
