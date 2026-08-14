// SPDX-License-Identifier: GPL-2.0
/*
 * J36 Ultra MT6592 PMIC: battery gauge, linear charger, BC1.2 and power-off.
 *
 * Ported from PowerEngine/OS/MVII's mt6592_pmic.c, which is a from-scratch
 * reimplementation of the stock Android charging path measured against this
 * board.  Everything hardware-facing here -- every register offset, every
 * threshold, every ordering constraint -- came out of that file, and its comments
 * carry the account of how each one was established.  This file keeps the parts
 * of that account a reader of the Linux driver needs and drops the rest.
 *
 *
 * ── WHAT THIS EXISTS TO FIX ──
 *
 * Until now this kernel had no power_supply at all.  mixdash's battery indicator
 * scans /sys/class/power_supply for a supply whose type is "Battery" and finds
 * nothing, so it draws nothing; the rootfs's own batt_led.service opens
 * /sys/class/power_supply/battery/capacity by path, fails, and is masked on the
 * kernel command line to keep it from spinning.  The supply registered here is
 * named literally `battery' so that both are answered at once: mixdash matches on
 * the type and does not care about the name, and the LED daemon needs that exact
 * path and nothing else.
 *
 *
 * ── THE ONE FACT THAT SHAPES EVERYTHING ──
 *
 * There is no power-path FET on this PMIC family.  VBAT *is* VSYS.  Every live
 * ADC channel that looks like it measures the cell is actually measuring the
 * system rail, and with a cable in, the system rail is whatever the charger's CV
 * loop is holding it at.  So:
 *
 *   - a terminal voltage read through a rest curve says "full" at any real state
 *     of charge, which is why the published percentage is an integrator seeded
 *     from the wakeup OCV latch and not a lookup (see j36_gauge_percent);
 *   - the charge current cannot be had from the difference of two independently
 *     filtered channels, because the common mode both share is the thing that
 *     moves; it comes from the median of ADJACENT-PAIR differences instead;
 *   - the charger's own output is the rail that powers the board, so the CV
 *     setpoint may be raised but must never be lowered below the node -- a sweep
 *     downwards killed a cell-less board in under a millisecond;
 *   - there is deliberately no POWER_SUPPLY_PROP_PRESENT.  Presence is not
 *     decidable here.  BATON is armed because stock arms it, and it decides
 *     nothing.
 *
 *
 * ── WHAT CHANGED IN THE PORT ──
 *
 * MVII runs inside a frame pump that must never block, so its AUXADC conversion
 * and its BC1.2 classification are both non-blocking state machines advanced one
 * step per call.  Here they run on a workqueue, which may sleep, so both collapse
 * into straight-line code: a conversion is write-the-request-bit-low,
 * write-it-high, wait a millisecond, read the data register, and the whole 660 ms
 * BC1.2 run executes inline on the plug edge.  Three consequences worth naming:
 *
 *   1. BATSNS and ISENSE are converted back to back inside one poll, so their
 *      adjacency is structural.  MVII has to check a conversion sequence number
 *      to prove a pair was not formed across a third channel or across a plug
 *      event; that bookkeeping is gone.
 *   2. The gauge advances exactly once per poll, so MVII's generation counter --
 *      which existed to stop a status bar redrawing at 60 Hz from driving the
 *      slew at 60 Hz -- is gone too.  get_property serves a snapshot.
 *   3. The 1 ms AUXADC settle is usleep_range() and not msleep().  ARM here is
 *      HZ=100, so msleep(1) would be 10-20 ms and each poll converts three
 *      channels.
 *
 * The charger watchdog's separate 500 ms kick timer is also gone: TD is the 4 s
 * window and the poll re-arms the charger every second, which feeds it four times
 * per window from the one place that was already doing the arming.
 *
 *
 * ── THE PERI CLOCK GATE, WHICH CAN TAKE THE BOARD DOWN ──
 *
 * BC1.2's comparator is in the PMIC, but the D+/D- mux is one bit in the USB PHY
 * at 0x11210800, and that window is behind the PERI clock gate.  On MediaTek an
 * APB access to a gated peripheral does not fault -- it stalls the bus until the
 * watchdog resets the board.  So this driver clears PDN0 itself before it ever
 * touches the PHY window, with the same blunt write j36_mt6592_usb_phy makes, and
 * skips BC1.2 entirely when either phandle is absent.  Without a classification
 * the charger runs at the 450 mA CS_VTH default, which is what stock LK does and
 * never changes; it charges, just not fast.
 *
 * The other half of that interaction runs the other way: j36_mt6592_usb_phy's
 * recover()/savecurrent() sequences clear the same mux bit as part of dropping
 * the ROM's UART-over-USB overrides.  A phy_init() landing inside a BC1.2 run
 * would therefore cut the probe short.  The cost is a misclassification, the
 * fallback is the conservative limit, and the fix is not worth cross-module
 * locking for an event that needs a cable to be plugged in during the same
 * hundred milliseconds a USB module is loading.
 *
 *
 * ── AND THE VBUS INTERLOCK ──
 *
 * This board sources its own 5 V on the port: DRVVBUS is GPIO pad 15, driven high
 * by j36_mt6592_usb_phy on power_on, and it is a boost off VBAT.  That 5 V lands
 * on the same net the PMIC's CHRIN pin senses, so a board in USB host mode looks
 * to CHRDET exactly like a board with a charger plugged in -- and arming the
 * charger against that would be the battery charging itself through a boost.
 *
 * The interlock is a live read of the pad rather than a call into the other
 * driver: mode == GPIO, direction == out, DOUT == 1 is precisely the state
 * j36_usb_phy_vbus(true) leaves behind, and reading it costs three register reads
 * per poll and creates no dependency in either direction.  While it reads
 * asserted the charger is disarmed and the supply reports offline.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>	/* scnprintf; 6.9 moved it to sprintf.h, which
				 * kernel.h still pulls in, so this spelling is
				 * the one that does not care which we build on */
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/reboot.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "j36_battery_curve.h"
#include "j36_mt6592_pmic.h"

/* ── module parameters ───────────────────────────────────────────────────────
 *
 * All four exist to be turned OFF from an insmod line, because /init loads this
 * from the vfat BOOT partition and a card that has been made unbootable by one of
 * them is fixed by editing boot.conf on any machine that reads SD cards.  A
 * kernel-cmdline `modname.param=' would not do: that only reaches modules built
 * into the image, and this one is loadable.
 */
static unsigned int poll_ms;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "gauge poll interval in ms (0 = use the device tree)");

static bool charge = true;
module_param(charge, bool, 0444);
MODULE_PARM_DESC(charge, "arm the charger (0 = read-only gauge, no charger writes)");

static bool bc11 = true;
module_param(bc11, bool, 0444);
MODULE_PARM_DESC(bc11, "classify the cable with BC1.2 (0 = assume the 450 mA default)");

static bool poweroff = true;
module_param(poweroff, bool, 0444);
MODULE_PARM_DESC(poweroff, "register the RTC BBPU power-off handler");

static bool chgreboot = true;
module_param(chgreboot, bool, 0444);
MODULE_PARM_DESC(chgreboot,
		 "restart rather than hang when power-off is asked for with a charger attached");

/* ── PWRAP (the PMIC wrapper), WACS2 ─────────────────────────────────────────
 *
 * The same transport j36_mt6592_input uses for the volume keys' AUXADC channel,
 * and the same stale-WFVLDCLR recovery at the top of every transaction: a
 * transaction abandoned by anyone leaves the state machine holding a result
 * nobody read, and the next reader has to clear it before it can start.
 */
#define J36_PWRAP_WACS2_CMD		0x009c
#define J36_PWRAP_WACS2_RDATA		0x00a0
#define J36_PWRAP_WACS2_VLDCLR		0x00a4
#define J36_PWRAP_FSM_IDLE		0x0
#define J36_PWRAP_FSM_WFVLDCLR		0x6
#define J36_PWRAP_STATE_SHIFT		16
#define J36_PWRAP_STATE_MASK		0x7
#define J36_PWRAP_INIT_DONE		BIT(21)
#define J36_PWRAP_POLL_LIMIT		10000

/* ── AUXADC ──────────────────────────────────────────────────────────────────
 *
 * RQST0 bit 4 is RG_VBUF_EN and is NOT a channel select: it is the buffer in
 * front of the divider chain, set once and latched.  CON's low nine bits are the
 * request list, and a conversion is launched by writing the channel's bit low and
 * then high -- the edge is the strobe.  The data register's READY bit is STICKY:
 * it stays set from the previous conversion, so the millisecond of settling is
 * not optional and cannot be replaced by polling READY.  That 1 ms is the
 * vendor's own blind msleep(1), and every attempt here to shorten it has produced
 * a reading from the conversion before.
 */
#define J36_ADC_RQST0			0x0758
#define J36_ADC_RQST0_VBUF_EN		BIT(4)
#define J36_ADC_CON			0x076e
#define J36_ADC_CON_FIELD		0x01ff
#define J36_ADC_BATSNS_CHANNEL		7
#define J36_ADC_BATSNS_DATA		0x0714
#define J36_ADC_ISENSE_CHANNEL		6	/* the system side of the shunt */
#define J36_ADC_ISENSE_DATA		0x0716
#define J36_ADC_VCHR_CHANNEL		4	/* VCDT, the charger input */
#define J36_ADC_VCHR_DATA		0x0718
#define J36_ADC_HW_OCV_DATA		0x0724	/* the wakeup latch; no request bit */
#define J36_ADC_HW_OCV_TUNE_MV		8
#define J36_ADC_READY			BIT(15)
#define J36_ADC_VALUE_MASK		0x7fff	/* the whole 15-bit field */
#define J36_ADC_VALUE_BITS		15
#define J36_ADC_SETTLE_US		1000
#define J36_ADC_POLL_LIMIT		16

/*
 * 1800 mV full range, x4 divider arm on channels 6 and 7: 3600 mV full scale,
 * which puts 19181 counts at 4214 mV -- a cell at its CV setpoint.  Channel 4
 * takes the x1 arm and a 330k/39k board divider instead.
 */
#define J36_ADC_FULL_SCALE_MV		7200
#define J36_ADC_VCHR_FULL_SCALE_MV	3600
#define J36_VCHR_DIVIDER_NUM		369
#define J36_VCHR_DIVIDER_DEN		39
#define J36_R_SENSE_MOHM		68

/* Five, from stock, and a median rather than a mean: the only failure this ADC
 * actually shows is a single bad conversion, which a median rejects completely
 * and a mean spreads over five. */
#define J36_ADC_MEDIAN			5

/* ── the linear charger ──────────────────────────────────────────────────────
 *
 * CHR_CONn lives at offset n*2.  Only the registers this driver writes are named;
 * the bit names are the stock kernel's own accessor names (upmu_set_rg_*).
 */
#define J36_CHR_CON0			0x0000
#define J36_CHR_CON0_VCDT_HV_EN		BIT(0)
#define J36_CHR_CON0_CSDAC_EN		BIT(3)
#define J36_CHR_CON0_CHR_EN		BIT(4)
#define J36_CHR_CON0_CHRDET		BIT(5)	/* live comparator on the CHRIN pin */
#define J36_CHR_CON2			0x0004
#define J36_CHR_CON2_VBAT_CV_EN		BIT(1)
#define J36_CHR_CON2_CS_EN		BIT(3)
/*
 * RGS_CS_DET, the charge-current-source comparator.  Read-only status, and read
 * ONLY for the diagnostic line -- never as the answer to "is it charging".  See
 * j36_charging_line() for why a snapshot of it is worth so little.
 */
#define J36_CHR_CON2_CS_DET		BIT(5)
/*
 * CHR_CON3[4:0] indexes stock's CV table.  Code 0 is 4200 mV.  The register
 * POWERS ON AT 29, which is 4162 mV -- twenty-one millivolts below this pack's
 * measured 4183 -- and a CV loop asked to regulate to a voltage the node has
 * already passed does nothing at all, because a charger cannot sink.  That one
 * unwritten register was the whole of "it never charges".
 */
#define J36_CHR_CON3			0x0006
#define J36_CHR_CON3_CV_MASK		0x001f
#define J36_CHR_CON3_CV_4200MV		0
/*
 * CS_VTH sets how hard the CSDAC drives VBAT, and VBAT is VSYS here: with no cell
 * to clamp the node, raising this drove it into OVP and latched the PMIC off.
 * Measured, on this board.  0xc is ~450 mA and is the step stock LK uses.
 */
#define J36_CHR_CON4			0x0008
#define J36_CHR_CON4_CS_VTH_MASK	0x000f
#define J36_CHR_CON4_CS_VTH_DEFAULT	0x000c
/* The comparator that stops the charger if the node runs away, which on a board
 * where VBAT is VSYS is the only thing between a stuck CSDAC and the rail. */
#define J36_CHR_CON6			0x000c
#define J36_CHR_CON6_VBAT_OV_EN		BIT(0)
#define J36_CHR_CON6_VBAT_OV_VTH_MASK	(0x7 << 1)
#define J36_CHR_CON6_VBAT_OV_VTH_4300MV	(0x1 << 1)
/* BATON is the battery-detect comparator.  It decides nothing here -- presence
 * stays undecidable on this board -- but stock arms it in charging_hw_init ahead
 * of CHR_EN and the charger's state machine is documented against that. */
#define J36_CHR_CON7			0x000e
#define J36_CHR_CON7_BATON_EN		BIT(0)
#define J36_CHR_CON7_BATON_HT_EN	BIT(1)
/* The charger watchdog.  Armed if and only if charging, in all three stock paths,
 * with no exception -- and its EXPIRY, not its arming, is what took VBAT down on
 * a cell-less board: the preloader arms it and nothing in a naive port kicks it.
 * TD code 0 is the 4 s window. */
#define J36_CHR_CON13			0x001a
#define J36_CHR_CON13_CHRWDT_TD_MASK	0x000f
#define J36_CHR_CON13_CHRWDT_TD_4S	0x0000
#define J36_CHR_CON13_CHRWDT_EN		BIT(4)
#define J36_CHR_CON13_CHRWDT_WR		BIT(8)
#define J36_CHR_CON15			0x001e
#define J36_CHR_CON15_CHRWDT_INT_EN	BIT(0)
#define J36_CHR_CON15_CHRWDT_FLAG_WR	BIT(1)
#define J36_CHR_CON15_CHRWDT_OUT	BIT(2)	/* 1 = it has already expired */
/*
 * USBDL is a HARDWARE charging mode, and it is why a board that read back every
 * register as intended still would not charge.  The preloader's pl_charging(1) is
 * hw_set_cc(450) then USBDL_SET = 1, and from there the PMIC runs the charge on
 * its own state machine with its own current setting and its own watchdog that
 * nothing in this kernel kicks.  Software CHR_EN, CSDAC_EN, HWCV_EN and the CV
 * target are the path it displaces, not the path it obeys.  Every vendor
 * charging_hw_init() opens by force-leaving it; the preloader's own exit clears
 * SET and then pulses RST, which is the order used here -- dropping the request
 * before releasing the latch cannot re-arm behind the reset.
 *
 * UVLO_VTHL is in the same register: below the threshold the PMIC latches off,
 * and VBAT is VSYS here, so a dip is a power cut that presents to an operator as
 * a spontaneous restart.  0 is the widest dip it will ride.
 */
#define J36_CHR_CON16			0x0020
#define J36_CHR_CON16_UVLO_VTHL_MASK	0x0003
#define J36_CHR_CON16_UVLO_VTHL_LOWEST	0x0000
#define J36_CHR_CON16_USBDL_RST		BIT(2)
#define J36_CHR_CON16_USBDL_SET		BIT(3)
#define J36_CHR_CON20			0x0028
#define J36_CHR_CON20_CSDAC_STP_INC_1	0x0001
#define J36_CHR_CON20_CSDAC_STP_DEC_2	0x0020
#define J36_CHR_CON21			0x002a
#define J36_CHR_CON21_CSDAC_DLY_4	0x0004
#define J36_CHR_CON21_CSDAC_STP_1	0x0010
#define J36_CHR_CON22			0x002c
#define J36_CHR_CON22_LOW_ICH_DB_MASK	0x003f
#define J36_CHR_CON22_LOW_ICH_DB_DEFAULT 0x0001
#define J36_CHR_CON23			0x002e
#define J36_CHR_CON23_VCDT_MODE		BIT(1)
#define J36_CHR_CON23_CSDAC_MODE	BIT(2)
#define J36_CHR_CON23_HWCV_EN		BIT(6)
#define J36_CHR_CON23_ULC_DET_EN	BIT(7)

/* ── BC1.2 ───────────────────────────────────────────────────────────────────
 *
 * THE BATTERY-CHARGING COMPARATOR IS IN THE PMIC, NOT IN THE USB PHY.  On MT6592
 * the D+/D- source, sink, reference and comparator are these two registers behind
 * pwrap, and the only USB-side involvement is one mux bit.  Classifying a cable
 * therefore needs no USB stack, no controller and no gadget.
 *
 * The 2-bit fields take 0/1/2 as distinct settings and not as a bitmask, so they
 * are written as shifted values under a mask rather than OR'd bits.
 */
#define J36_CHR_CON18			0x0024
#define J36_CHR_CON18_BB_CTRL		BIT(0)
#define J36_CHR_CON18_RST		BIT(1)
#define J36_CHR_CON18_VSRC_EN_MASK	(0x3 << 2)
#define J36_CHR_CON18_VSRC_EN_SHIFT	2
#define J36_CHR_CON18_CMP_OUT		BIT(7)	/* RGS_, read-only */
#define J36_CHR_CON19			0x0026
#define J36_CHR_CON19_VREF_VTH_MASK	(0x3 << 0)
#define J36_CHR_CON19_VREF_VTH_SHIFT	0
#define J36_CHR_CON19_CMP_EN_MASK	(0x3 << 2)
#define J36_CHR_CON19_CMP_EN_SHIFT	2
#define J36_CHR_CON19_IPD_EN_MASK	(0x3 << 4)
#define J36_CHR_CON19_IPD_EN_SHIFT	4
#define J36_CHR_CON19_IPU_EN_MASK	(0x3 << 6)
#define J36_CHR_CON19_IPU_EN_SHIFT	6
#define J36_CHR_CON19_BIAS_EN		BIT(8)

/* Dwells, from stock's hw_bc11_* helpers.  660 ms end to end. */
#define J36_BC11_DWELL_INIT_MS		100
#define J36_BC11_DWELL_DCD_MS		400
#define J36_BC11_DWELL_STEP_MS		80

/* What each verdict allows the port to draw. */
#define J36_BC11_LIMIT_SDP_MA		500
#define J36_BC11_LIMIT_CDP_MA		650
#define J36_BC11_LIMIT_DCP_MA		950
#define J36_BC11_LIMIT_NONSTD_MA	500
#define J36_BC11_LIMIT_APPLE_0_5A_MA	500
#define J36_BC11_LIMIT_APPLE_1_0A_MA	1000
#define J36_BC11_LIMIT_APPLE_2_1A_MA	2100

/* ── RTC, where power-off lives on this family ───────────────────────────────
 *
 * Three things gate a power-off and a naive port has one of them.  The RTC is on
 * a 32 kHz domain behind pwrap, so a write is not a write until the bridge
 * retires it (WRTGR, then poll BBPU's CBUSY).  The write interface has to be
 * unlocked with both halves of the key or EVERY RTC WRITE IS DISCARDED, which is
 * what "power off did not latch" was actually reporting.  And BBPU must be
 * written 0x4309, not 0x4300 -- the low bits are the command.
 */
#define J36_RTC_BBPU			0x8000
#define J36_RTC_BBPU_KEY		(0x43 << 8)
#define J36_RTC_BBPU_PWREN		BIT(0)
#define J36_RTC_BBPU_AUTO		BIT(3)
#define J36_RTC_BBPU_CBUSY		BIT(6)
#define J36_RTC_WRTGR			0x803c
#define J36_RTC_PROT			0x8036
#define J36_RTC_PROT_KEY1		0x586a
#define J36_RTC_PROT_KEY2		0x9136
/* Stock's CBUSY loop is unbounded; retiring one RTC write costs a few 32 kHz
 * ticks, and a wedged pwrap must not take the shutdown with it. */
#define J36_RTC_CBUSY_TRIES		4000

/* ── the two SoC windows this driver borrows ─────────────────────────────────
 *
 * PDN_CLR clears power-down bits, i.e. turns clocks ON, and the gate is
 * deliberately never restored: PDN0 is one register shared by every peripheral on
 * that bus, and this driver does not know which of the bits it turned on were
 * already on.  The USB PHY driver makes exactly the same write and says the same
 * thing about it.
 *
 * The GPIO offsets are j36_mt6592_usb_phy's, used read-only here for the DRVVBUS
 * interlock.
 */
#define J36_PERI_PDN0_CLR		0x0010
#define J36_PERI_PDN0_STA		0x0018
#define J36_PERI_PDN0_ALL		0xffffffff

#define J36_PHY_R1A			0x1a
#define J36_PHY_R1A_GPIO_CTL		0x80	/* rg_usb20_gpio_ctl == BC11_SW_EN */

#define J36_GPIO_DIR			0x0000
#define J36_GPIO_DOUT			0x0400
#define J36_GPIO_MODE			0x0600
#define J36_GPIO_BANK_STRIDE		0x0010
#define J36_GPIO_PINS_PER_BANK		16
#define J36_GPIO_PINS_PER_MODE_REG	5
#define J36_GPIO_MODE_FIELD_BITS	3
#define J36_GPIO_MODE_GPIO		0
#define J36_GPIO_PIN_MAX		0xa8

/* ── gauge constants ─────────────────────────────────────────────────────────
 *
 * Stock's, from battery_common.c and battery_meter.c, with stock's names.
 */
#define J36_V_0PERCENT_TRACKING_MV	3450
#define J36_CHARGING_FULL_CURRENT_MA	150
#define J36_V_CC2TOPOFF_MV		4050
#define J36_RECHARGING_MV		4110
#define J36_FULL_CHECK_TIMES		6
#define J36_SOC_SLEW_INTERVAL_US	30000000	/* one percent per 30 s */
#define J36_LADDER_INTERVAL_US		1000000		/* the sub-3450 mV ramp */
#define J36_SOC_CAR_GRACE_US		10000000
#define J36_BATT_CAPACITY_MAH		1499
#define J36_HW_OCV_MIN_MV		2500
#define J36_HW_OCV_MAX_MV		4500
#define J36_POLL_MS_DEFAULT		1000
#define J36_POLL_MS_MIN			200
#define J36_POLL_MS_MAX			10000

/* mA*us per percent of this pack: 1499 mAh * 3600 s * 1e6 us/s / 100 percent. */
#define J36_SOC_CAR_PER_PCT		((s64)36000000 * (s64)J36_BATT_CAPACITY_MAH)

/* Stock's CV table, indexed by CHR_CON3[4:0].  Kept whole so a code read out of
 * the register can be reported in millivolts, which is the only way a wrong
 * setpoint is visible at all. */
static const u16 j36_cv_mv[32] = {
	4200, 4212, 4225, 4237, 4250, 4262, 4275, 4300,
	4325, 4350, 4375, 4400, 4425, 4162, 4175, 2200,
	4050, 4100, 4125, 3775, 3800, 3850, 3900, 4000,
	4050, 4100, 4125, 4137, 4150, 4162, 4175, 4187,
};

/* CS_VTH, descending, so the first entry at or under the port's limit is the
 * code to write.  Capped at 950 mA: that is the most this board's charger is
 * asked for regardless of what a 2.1 A brick claims to offer. */
static const u16 j36_cs_vth_ma[16] = {
	1600, 1500, 1400, 1300, 1200, 1100, 1000, 900,
	 800,  700,  650,  550,  450,  300,  200,  70,
};
#define J36_CS_VTH_CAP_MA		950

/* ── the running state ───────────────────────────────────────────────────────
 *
 * Everything under `worker-owned' is touched only from the poll work, which is on
 * an ordered workqueue, so it needs no locking.  `pub' is what get_property
 * serves and is the only thing the lock is really for; the lock is also taken
 * per PWRAP transaction, because WACS2 is one channel and the power-off path is
 * allowed to interleave with a poll but not with a half-finished transaction.
 */
struct j36_ring {
	int v[J36_ADC_MEDIAN];
	unsigned int n;
	unsigned int pos;
	int median;
	bool valid;
};

struct j36_pub {
	int status;		/* POWER_SUPPLY_STATUS_* */
	int usb_type;		/* POWER_SUPPLY_USB_TYPE_* */
	int online;		/* 0/1 */
	int capacity;		/* percent, -1 when nothing is established yet */
	int voltage_uv;		/* BATSNS, i.e. the system node */
	int ocv_uv;		/* IR-corrected, -1 when unknown */
	int current_ua;		/* signed, positive into the cell */
	bool current_valid;
	int charger_uv;		/* VCHR, -1 when no cable */
	int cv_uv;		/* the CV setpoint read back, -1 when unknown */
	int input_limit_ua;	/* what BC1.2 says the PORT allows */
	int charge_step_ua;	/* what the CHARGER is set to, read back */
};

struct j36_pmic {
	struct device *dev;
	void __iomem *pwrap;
	void __iomem *pericfg;	/* optional: the PERI clock gate */
	void __iomem *usbphy;	/* optional: the BC1.2 mux bit */
	void __iomem *gpio;	/* optional: the DRVVBUS interlock */
	int vbus_pin;		/* < 0 when the tree did not say */

	spinlock_t lock;
	struct delayed_work poll_work;
	unsigned int poll_ms;

	struct power_supply *battery;
	struct power_supply *usb;

	/* worker-owned: AUXADC */
	u32 adc_con_shadow;
	bool adc_con_valid;
	bool adc_vbuf_on;
	struct j36_ring batsns;	/* raw counts */
	struct j36_ring vchr;	/* raw counts */
	struct j36_ring delta;	/* signed counts, ISENSE - BATSNS */

	/* worker-owned: the plug edge and the classifier */
	int online;		/* -1 before the first poll */
	int bc11_type;
	int bc11_limit_ma;	/* what the PORT licenses, from BC1.2 */
	int charge_step_ma;	/* what CHR_CON4 is really set to; -1 unread */
	bool bc11_done;
	bool vbus_warned;
	unsigned int poll_kicks;	/* rate-limits j36_charging_line() */

	/* When an operator's CV write stops the re-arm stamping 4200 back over
	 * it.  Written from the sysfs path, read from the worker; a torn read of
	 * a ktime_t on a 32-bit machine costs at worst one extra held or unheld
	 * poll, which is why this is not worth the lock. */
	ktime_t cv_hold;

	/* worker-owned: the gauge */
	int hw_ocv_mv;		/* -1 when the latch was unreadable */
	bool hw_ocv_done;
	int soc_dep;		/* depletion, 0..100; -1 until seeded */
	ktime_t soc_slew;
	int soc_car_base;	/* -1 when no charge run is live */
	s64 soc_car;		/* mA*us accumulated in this run */
	ktime_t soc_car_time;
	ktime_t soc_charge_time;
	unsigned int full_count;
	unsigned int recharge_count;	/* consecutive samples under the recharge line */
	bool topoff_seen;
	bool charge_full;

	/* published */
	struct j36_pub pub;
};

/* ══════════════════════════════════════════════════════════════════════════
 * PWRAP TRANSPORT
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * One WACS2 transaction.  Caller holds the lock; nothing in here sleeps, which is
 * what makes a spinlock the right primitive and what makes this callable from the
 * power-off path.
 *
 * The stale-WFVLDCLR recovery at the top is mandatory and not defensive: a
 * transaction abandoned by anyone -- the LK, a previous probe, a driver unbound
 * mid-read -- leaves the state machine holding a result nobody collected, and it
 * will not accept a command until that result is cleared.
 */
static int j36_pwrap_xfer_locked(struct j36_pmic *p, bool write, u32 adr,
				 u32 wdata, u32 *rdata)
{
	unsigned int i;
	u32 value;

	if (adr & ~0xffffu || wdata & ~0xffffu)
		return -EINVAL;
	if (!write && !rdata)
		return -EINVAL;

	value = readl(p->pwrap + J36_PWRAP_WACS2_RDATA);
	if (((value >> J36_PWRAP_STATE_SHIFT) & J36_PWRAP_STATE_MASK) ==
	    J36_PWRAP_FSM_WFVLDCLR)
		writel(1, p->pwrap + J36_PWRAP_WACS2_VLDCLR);

	for (i = 0; i < J36_PWRAP_POLL_LIMIT; ++i) {
		value = readl(p->pwrap + J36_PWRAP_WACS2_RDATA);
		if (((value >> J36_PWRAP_STATE_SHIFT) & J36_PWRAP_STATE_MASK) ==
		    J36_PWRAP_FSM_IDLE)
			break;
		cpu_relax();
	}
	if (i == J36_PWRAP_POLL_LIMIT)
		return -ETIMEDOUT;

	writel(((u32)write << 31) | ((adr >> 1) << 16) | wdata,
	       p->pwrap + J36_PWRAP_WACS2_CMD);
	if (write)
		return 0;

	for (i = 0; i < J36_PWRAP_POLL_LIMIT; ++i) {
		value = readl(p->pwrap + J36_PWRAP_WACS2_RDATA);
		if (((value >> J36_PWRAP_STATE_SHIFT) & J36_PWRAP_STATE_MASK) ==
		    J36_PWRAP_FSM_WFVLDCLR) {
			*rdata = value & 0xffff;
			writel(1, p->pwrap + J36_PWRAP_WACS2_VLDCLR);
			return 0;
		}
		cpu_relax();
	}
	return -ETIMEDOUT;
}

static int j36_pmic_read(struct j36_pmic *p, u32 adr, u32 *rdata)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&p->lock, flags);
	ret = j36_pwrap_xfer_locked(p, false, adr, 0, rdata);
	spin_unlock_irqrestore(&p->lock, flags);
	return ret;
}

static int j36_pmic_write(struct j36_pmic *p, u32 adr, u32 wdata)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&p->lock, flags);
	ret = j36_pwrap_xfer_locked(p, true, adr, wdata, NULL);
	spin_unlock_irqrestore(&p->lock, flags);
	return ret;
}

/*
 * Read-modify-write under one lock, so nothing can slip a write in between.
 * Returns 1 if the register actually changed, 0 if it already held the value,
 * negative on a transport failure -- the distinction matters for the log lines
 * that are only worth printing when a bit really moved.
 */
static int j36_pmic_update(struct j36_pmic *p, u32 adr, u32 clr, u32 set)
{
	unsigned long flags;
	u32 old, new;
	int ret;

	spin_lock_irqsave(&p->lock, flags);
	ret = j36_pwrap_xfer_locked(p, false, adr, 0, &old);
	if (ret)
		goto out;
	new = (old & ~(clr | set)) | set;
	if (new == old) {
		ret = 0;
		goto out;
	}
	ret = j36_pwrap_xfer_locked(p, true, adr, new, NULL);
	if (!ret)
		ret = 1;
out:
	spin_unlock_irqrestore(&p->lock, flags);
	return ret;
}

/* A field write: clear the mask, set the shifted value.  The BC1.2 registers are
 * full of 2-bit fields that take 0/1/2 as settings rather than as bits. */
static int j36_pmic_field(struct j36_pmic *p, u32 adr, u32 mask, u32 shift, u32 val)
{
	return j36_pmic_update(p, adr, mask, (val << shift) & mask);
}

/*
 * ══════════════════════════════════════════════════════════════════════════
 * THE ONE DOOR ONTO PWRAP, and why it is a door rather than a second key.
 * ══════════════════════════════════════════════════════════════════════════
 *
 * j36_mt6592_wifi needs the four MT6323 connectivity rails -- VCN_1V8, VCN28 and
 * the two halves of VCN33 -- and they are behind the same WACS2 bridge this
 * driver reads the gauge through.  WACS2 is ONE state machine with ONE result
 * register: a transaction is a write to CMD followed by a poll of RDATA followed
 * by a write to VLDCLR, and there is nothing in the hardware that keeps two
 * drivers' transactions apart.  Two modules each holding their own spinlock over
 * their own ioremap of the same window is not two locks, it is no lock -- one
 * would collect the other's result, clear the valid flag under it, and both would
 * report a plausible wrong number.  On a gauge that is a wrong percentage; on a
 * rail it is a regulator enable that silently did not happen.
 *
 * So the WiFi driver does not map pwrap at all.  It calls this, which runs on the
 * same spinlock as every other transaction here.
 *
 * A singleton, and honestly so: there is one MT6592 die, one MT6323 companion and
 * one bridge between them, and a second instance of this driver would be a device
 * tree describing a board that does not exist.  The pointer is published at the
 * end of probe (so a caller cannot catch a half-built state) and cleared by a
 * devm action on the way out.
 *
 * The load-order consequence is deliberate and is the good kind: j36_mt6592_wifi
 * links against this symbol, so insmod REFUSES it outright if this module is not
 * loaded, in a log line naming the missing symbol.  The alternative -- an
 * optional lookup that degrades -- would boot a radio whose transmit PA supply
 * was never raised and leave it to be diagnosed as an RF fault.
 */
static struct j36_pmic *j36_pmic_singleton;

/**
 * j36_pmic_pwrap_update() - read-modify-write one MT6323 register
 * @adr: the PMIC-side register address (16 bit)
 * @clr: bits to clear
 * @set: bits to set
 *
 * Returns 1 if the register changed, 0 if it already held that value, -ENODEV if
 * the PMIC driver is not bound, and a negative transport error otherwise.
 */
int j36_pmic_pwrap_update(u32 adr, u32 clr, u32 set)
{
	struct j36_pmic *p = READ_ONCE(j36_pmic_singleton);

	if (!p)
		return -ENODEV;
	return j36_pmic_update(p, adr, clr, set);
}
EXPORT_SYMBOL_GPL(j36_pmic_pwrap_update);

/**
 * j36_pmic_pwrap_read() - read one MT6323 register
 * @adr: the PMIC-side register address (16 bit)
 * @rdata: where the 16-bit value lands
 *
 * Returns 0, -ENODEV if the PMIC driver is not bound, or a transport error.
 */
int j36_pmic_pwrap_read(u32 adr, u32 *rdata)
{
	struct j36_pmic *p = READ_ONCE(j36_pmic_singleton);

	if (!p)
		return -ENODEV;
	return j36_pmic_read(p, adr, rdata);
}
EXPORT_SYMBOL_GPL(j36_pmic_pwrap_read);

/* INIT_DONE in the RDATA word.  Everything here is a no-op until it is set: the
 * bridge is brought up by the LK and this driver never initialises it. */
static bool j36_pwrap_up(struct j36_pmic *p)
{
	return !!(readl(p->pwrap + J36_PWRAP_WACS2_RDATA) & J36_PWRAP_INIT_DONE);
}

/* ══════════════════════════════════════════════════════════════════════════
 * COUNTS TO ENGINEERING UNITS
 * ══════════════════════════════════════════════════════════════════════════
 *
 * All three widen to 64-bit where MVII uses 32.  That is the one arithmetic
 * departure in this port and it is not cosmetic: raw * 1800 * 369 overflows a u32
 * above ~6400 counts, and delta * 7200 * 1000 overflows an s32 above ~298 counts,
 * which is only ~950 mA -- exactly the current a DCP is allowed to deliver.  The
 * values MVII actually sees stay inside both bounds, so this changes no result;
 * it removes a cliff the port had no reason to keep.
 */
static int j36_sense_mv(int raw)
{
	if (raw < 0)
		return -1;
	return (int)(((u32)raw * J36_ADC_FULL_SCALE_MV) >> J36_ADC_VALUE_BITS);
}

static int j36_vchr_mv(int raw)
{
	if (raw < 0)
		return -1;
	/* The divider is applied before the shift so the 9.46x multiply does not
	 * lose the low bits of a pin reading that is only ~450 counts at 5 V. */
	return (int)div_u64((u64)raw * J36_ADC_VCHR_FULL_SCALE_MV *
			    J36_VCHR_DIVIDER_NUM,
			    (u64)J36_VCHR_DIVIDER_DEN << J36_ADC_VALUE_BITS);
}

/* I = 1000 * delta_mV / R_SENSE_mOhm, with the counts-to-millivolts scale folded
 * in so the division happens once. */
static int j36_sense_ma(int delta_counts)
{
	s64 num = (s64)delta_counts * J36_ADC_FULL_SCALE_MV * 1000;

	return (int)div_s64(num, (s64)J36_R_SENSE_MOHM << J36_ADC_VALUE_BITS);
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE MEDIAN RINGS
 * ══════════════════════════════════════════════════════════════════════════ */

static void j36_ring_forget(struct j36_ring *r)
{
	r->n = 0;
	r->pos = 0;
	r->median = -1;
	r->valid = false;
}

/* Publishes from the first sample rather than waiting for five: a median of one
 * is that one sample, and a status bar that says nothing for five seconds after a
 * plug event is worse than one that is briefly unfiltered. */
static void j36_ring_push(struct j36_ring *r, int v)
{
	int sorted[J36_ADC_MEDIAN];
	unsigned int i, j;

	r->v[r->pos] = v;
	r->pos = (r->pos + 1) % J36_ADC_MEDIAN;
	if (r->n < J36_ADC_MEDIAN)
		++r->n;

	for (i = 0; i < r->n; ++i)
		sorted[i] = r->v[i];
	for (i = 1; i < r->n; ++i) {
		const int key = sorted[i];

		j = i;
		while (j > 0 && sorted[j - 1] > key) {
			sorted[j] = sorted[j - 1];
			--j;
		}
		sorted[j] = key;
	}
	/* For an even n this takes the upper of the two middle values, which for
	 * counts is a rounding choice and nothing more. */
	r->median = sorted[r->n / 2];
	r->valid = true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE AUXADC
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * One conversion, blocking.  Three pwrap transactions and one millisecond.
 *
 * The two caches are the reason this is three transactions and not eight: stock
 * re-reads AUXADC_CON before each of its two writes because in the kernel the
 * request list is shared with accdet, the thermal driver and the audio path.  In
 * THIS kernel nothing else touches it, and the field is register-governed --
 * hardware never writes it back -- so it is read once into a shadow.  RG_VBUF_EN
 * gets the same treatment: it is not a channel select, so it is set once and
 * latched.  Any transport failure drops both caches, because a write that may or
 * may not have landed leaves a register this driver cannot claim to know.
 */
static int j36_adc_convert(struct j36_pmic *p, u32 channel, u32 data_reg, int *out)
{
	unsigned int i;
	u32 v;
	int ret;

	if (!p->adc_vbuf_on) {
		ret = j36_pmic_update(p, J36_ADC_RQST0, 0, J36_ADC_RQST0_VBUF_EN);
		if (ret < 0)
			goto fail;
		p->adc_vbuf_on = true;
	}
	if (!p->adc_con_valid) {
		ret = j36_pmic_read(p, J36_ADC_CON, &v);
		if (ret)
			goto fail;
		p->adc_con_shadow = v & J36_ADC_CON_FIELD;
		p->adc_con_valid = true;
	}

	/* Low, then high: the EDGE is the strobe, not the level. */
	ret = j36_pmic_write(p, J36_ADC_CON, p->adc_con_shadow & ~BIT(channel));
	if (ret)
		goto fail;
	ret = j36_pmic_write(p, J36_ADC_CON, p->adc_con_shadow | BIT(channel));
	if (ret)
		goto fail;

	/*
	 * usleep_range and not msleep: HZ is 100 on this build, so msleep(1) is
	 * 10-20 ms and a poll converts three channels.  READY is sticky -- it is
	 * still set from the previous conversion -- so this wait cannot be
	 * replaced by polling it, and the loop below only exists for the case
	 * where the latch has genuinely never been written.
	 */
	usleep_range(J36_ADC_SETTLE_US, J36_ADC_SETTLE_US * 2);

	for (i = 0; i < J36_ADC_POLL_LIMIT; ++i) {
		ret = j36_pmic_read(p, data_reg, &v);
		if (ret)
			goto fail;
		if (v & J36_ADC_READY) {
			*out = (int)(v & J36_ADC_VALUE_MASK);
			return 0;
		}
		usleep_range(J36_ADC_SETTLE_US, J36_ADC_SETTLE_US * 2);
	}
	return -ETIMEDOUT;

fail:
	p->adc_con_valid = false;
	p->adc_vbuf_on = false;
	return ret < 0 ? ret : -EIO;
}

/*
 * get_hw_ocv(): the wakeup VBAT latch, in millivolts.  Not a live channel --
 * there is no request bit for it and nothing to strobe.  One read, once.
 *
 * This is the whole fix for "it reads 99% while charging".  Every live channel on
 * this board measures VSYS, so with a cable in they all read the charger's
 * setpoint whatever the cell is doing.  This one was taken by the PMIC at wakeup,
 * before the pre-charger path was enabled, and it is the only number available
 * here that is about the cell.
 *
 * -1 means "no seed available" and never a number: seeding an integrator from a
 * bad reading is worse than not seeding it.
 */
static int j36_hw_ocv_read(struct j36_pmic *p)
{
	u32 v;
	int mv;

	if (j36_pmic_read(p, J36_ADC_HW_OCV_DATA, &v))
		return -1;
	if (!(v & J36_ADC_READY))
		return -1;	/* the latch never happened: a warm path */

	/* r_val_temp is 4 here, the same x4 arm as channels 6 and 7, then stock's
	 * flat +8 mV trim. */
	mv = (int)(((v & J36_ADC_VALUE_MASK) * J36_ADC_FULL_SCALE_MV) >>
		   J36_ADC_VALUE_BITS) + J36_ADC_HW_OCV_TUNE_MV;

	if (mv < J36_HW_OCV_MIN_MV || mv > J36_HW_OCV_MAX_MV)
		return -1;
	return mv;
}

static void j36_hw_ocv_prime(struct j36_pmic *p)
{
	if (p->hw_ocv_done)
		return;
	p->hw_ocv_done = true;
	p->hw_ocv_mv = j36_hw_ocv_read(p);

	/* Not decoration: when the published level looks wrong, the first question
	 * is which seed it came from, and this is the only place that can say. */
	if (p->hw_ocv_mv > 0)
		dev_info(p->dev,
			 "wakeup OCV latch %d mV (%d%%); seeding the gauge from the cell\n",
			 p->hw_ocv_mv,
			 j36_battery_percent_from_ocv(p->hw_ocv_mv));
	else
		dev_info(p->dev,
			 "wakeup OCV latch unreadable; the gauge will seed from the live rail\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE DRVVBUS INTERLOCK
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * True when this board is itself sourcing 5 V on the port.
 *
 * mode == GPIO, direction == out, DOUT == 1 is exactly the state
 * j36_usb_phy_vbus(true) leaves the pad in, and reading it is cheaper and more
 * honest than asking the other driver: the pad IS the ground truth, a stale
 * answer is impossible, and neither module has to know the other exists.
 *
 * Any of the three not matching means the pad is not ours to interpret -- the LK
 * may have left it in a peripheral mode, a future board may not wire it at all --
 * and the safe reading of "I cannot tell" here is "not sourcing", because the
 * alternative is refusing to charge on every board that does not have this pad.
 */
static bool j36_drvvbus_asserted(struct j36_pmic *p)
{
	unsigned int bank, bit, shift;
	u32 mode, dir, dout;

	if (!p->gpio || p->vbus_pin < 0)
		return false;

	mode = readl(p->gpio + J36_GPIO_MODE +
		     (p->vbus_pin / J36_GPIO_PINS_PER_MODE_REG) *
		     J36_GPIO_BANK_STRIDE);
	shift = (p->vbus_pin % J36_GPIO_PINS_PER_MODE_REG) *
		J36_GPIO_MODE_FIELD_BITS;
	if (((mode >> shift) & 0x7) != J36_GPIO_MODE_GPIO)
		return false;

	bank = (p->vbus_pin / J36_GPIO_PINS_PER_BANK) * J36_GPIO_BANK_STRIDE;
	bit = p->vbus_pin % J36_GPIO_PINS_PER_BANK;

	dir = readl(p->gpio + J36_GPIO_DIR + bank);
	if (!(dir & BIT(bit)))
		return false;	/* an input cannot be sourcing anything */

	dout = readl(p->gpio + J36_GPIO_DOUT + bank);
	return !!(dout & BIT(bit));
}

/* ══════════════════════════════════════════════════════════════════════════
 * BC1.2, THE CABLE CLASSIFIER
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * The mux bit, and the reason this driver knows about the PERI gate at all.  The
 * PHY window is behind PDN0, and an APB read of a gated MediaTek peripheral does
 * not fault -- it stalls the bus until the watchdog resets the board -- so the
 * gate is cleared before the first access and never after it.  The write is
 * idempotent and identical to the one j36_mt6592_usb_phy makes; whichever driver
 * gets there first, the second one changes nothing.
 */
static void j36_bc11_phy_mux(struct j36_pmic *p, bool on)
{
	u8 v;

	if (!p->usbphy || !p->pericfg)
		return;

	writel(J36_PERI_PDN0_ALL, p->pericfg + J36_PERI_PDN0_CLR);

	v = readb(p->usbphy + J36_PHY_R1A);
	if (on)
		v |= J36_PHY_R1A_GPIO_CTL;
	else
		v &= (u8)~J36_PHY_R1A_GPIO_CTL;
	writeb(v, p->usbphy + J36_PHY_R1A);
}

static void j36_bc11_bb_ctrl(struct j36_pmic *p, u32 v)
{
	j36_pmic_update(p, J36_CHR_CON18, J36_CHR_CON18_BB_CTRL,
			v ? J36_CHR_CON18_BB_CTRL : 0);
}

static void j36_bc11_rst(struct j36_pmic *p, u32 v)
{
	j36_pmic_update(p, J36_CHR_CON18, J36_CHR_CON18_RST,
			v ? J36_CHR_CON18_RST : 0);
}

static void j36_bc11_vsrc_en(struct j36_pmic *p, u32 v)
{
	j36_pmic_field(p, J36_CHR_CON18, J36_CHR_CON18_VSRC_EN_MASK,
		       J36_CHR_CON18_VSRC_EN_SHIFT, v);
}

static void j36_bc11_vref_vth(struct j36_pmic *p, u32 v)
{
	j36_pmic_field(p, J36_CHR_CON19, J36_CHR_CON19_VREF_VTH_MASK,
		       J36_CHR_CON19_VREF_VTH_SHIFT, v);
}

static void j36_bc11_cmp_en(struct j36_pmic *p, u32 v)
{
	j36_pmic_field(p, J36_CHR_CON19, J36_CHR_CON19_CMP_EN_MASK,
		       J36_CHR_CON19_CMP_EN_SHIFT, v);
}

static void j36_bc11_ipd_en(struct j36_pmic *p, u32 v)
{
	j36_pmic_field(p, J36_CHR_CON19, J36_CHR_CON19_IPD_EN_MASK,
		       J36_CHR_CON19_IPD_EN_SHIFT, v);
}

static void j36_bc11_ipu_en(struct j36_pmic *p, u32 v)
{
	j36_pmic_field(p, J36_CHR_CON19, J36_CHR_CON19_IPU_EN_MASK,
		       J36_CHR_CON19_IPU_EN_SHIFT, v);
}

static void j36_bc11_bias_en(struct j36_pmic *p, u32 v)
{
	j36_pmic_update(p, J36_CHR_CON19, J36_CHR_CON19_BIAS_EN,
			v ? J36_CHR_CON19_BIAS_EN : 0);
}

/* The comparator, read BEFORE the step's own teardown writes.  Reading it after
 * would read a comparator that had already been switched off, which is the bug
 * that the arm/retire split in the MVII state machine exists to prevent and that
 * the straight-line form here has to preserve by hand. */
static bool j36_bc11_cmp_out(struct j36_pmic *p)
{
	u32 con18;

	if (j36_pmic_read(p, J36_CHR_CON18, &con18))
		return false;
	return !!(con18 & J36_CHR_CON18_CMP_OUT);
}

static void j36_bc11_settle(struct j36_pmic *p, int type, int limit_ma,
			    const char *what)
{
	p->bc11_type = type;
	p->bc11_limit_ma = limit_ma;
	dev_info(p->dev, "BC1.2 says %s (%d mA input)\n", what, limit_ma);
}

/* Put the pins back whatever state the run was in.  Called on unplug, where the
 * classification is not merely stale but meaningless -- the next cable need not
 * be the same cable. */
static void j36_bc11_teardown(struct j36_pmic *p)
{
	j36_bc11_vsrc_en(p, 0);
	j36_bc11_vref_vth(p, 0);
	j36_bc11_cmp_en(p, 0);
	j36_bc11_ipu_en(p, 0);
	j36_bc11_ipd_en(p, 0);
	j36_bc11_bias_en(p, 0);
	j36_bc11_phy_mux(p, false);
}

static void j36_bc11_forget(struct j36_pmic *p)
{
	if (p->bc11_done)
		j36_bc11_teardown(p);
	p->bc11_done = false;
	p->bc11_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	p->bc11_limit_ma = 0;
}

/*
 * The whole classification, inline, ~660 ms.
 *
 * MVII runs this as a state machine because it lives inside a frame pump; here it
 * is straight-line code on a workqueue, which makes it readable for the first
 * time.  Every drive write below is stock's, in stock's order and with stock's
 * values, INCLUDING the two quirks: step A1 leaves VREF_VTH at 2 rather than
 * clearing it, and step B1 drives IPD where the vendor's own comment says IPU.
 * Neither is corrected.  A charger classifier is a resistor-code guessing game
 * against real hardware in the wild, and "the vendor's sequence, exactly" is the
 * only version of it anyone has ever tested.
 *
 * The branch structure past DCD is the divergent, non-BC1.2 half: a charger that
 * answers the data-contact probe is speaking a proprietary D+/D- code, and A1/B1
 * and C1 are the Apple ladder.
 */
static void j36_bc11_run(struct j36_pmic *p)
{
	bool avail;

	/* hw_bc11_init */
	j36_bc11_phy_mux(p, true);
	j36_bc11_bias_en(p, 1);
	j36_bc11_vsrc_en(p, 0);
	j36_bc11_vref_vth(p, 0);
	j36_bc11_cmp_en(p, 0);
	j36_bc11_ipu_en(p, 0);
	j36_bc11_ipd_en(p, 0);
	j36_bc11_rst(p, 1);
	j36_bc11_bb_ctrl(p, 1);
	msleep(J36_BC11_DWELL_INIT_MS);

	/* hw_bc11_DCD */
	j36_bc11_ipu_en(p, 2);
	j36_bc11_ipd_en(p, 1);
	j36_bc11_vref_vth(p, 1);
	j36_bc11_cmp_en(p, 2);
	msleep(J36_BC11_DWELL_DCD_MS);
	avail = j36_bc11_cmp_out(p);
	j36_bc11_ipu_en(p, 0);
	j36_bc11_ipd_en(p, 0);
	j36_bc11_cmp_en(p, 0);
	j36_bc11_vref_vth(p, 0);

	if (avail) {
		/* hw_bc11_stepA1 */
		j36_bc11_ipu_en(p, 2);
		j36_bc11_vref_vth(p, 2);
		j36_bc11_cmp_en(p, 2);
		msleep(J36_BC11_DWELL_STEP_MS);
		avail = j36_bc11_cmp_out(p);
		j36_bc11_ipu_en(p, 0);
		j36_bc11_cmp_en(p, 0);	/* VREF_VTH left at 2, as in stock */

		if (avail) {
			/* hw_bc11_stepB1 -- IPD where the comment says IPU */
			j36_bc11_ipd_en(p, 1);
			j36_bc11_vref_vth(p, 0);
			j36_bc11_cmp_en(p, 1);
			msleep(J36_BC11_DWELL_STEP_MS);
			avail = j36_bc11_cmp_out(p);
			j36_bc11_ipu_en(p, 0);
			j36_bc11_cmp_en(p, 0);
			j36_bc11_vref_vth(p, 0);
			if (avail)
				j36_bc11_settle(p, POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID,
						J36_BC11_LIMIT_APPLE_2_1A_MA,
						"an Apple 2.1 A charger");
			else
				/*
				 * NONSTANDARD has no bucket of its own in
				 * power_supply's enum, and DCP is the closest
				 * honest one: it is a charger, not a host.  What
				 * actually differs is the limit, and that is
				 * carried in CURRENT_MAX, which is where anything
				 * making a decision should be looking anyway.
				 */
				j36_bc11_settle(p, POWER_SUPPLY_USB_TYPE_DCP,
						J36_BC11_LIMIT_NONSTD_MA,
						"a non-standard charger");
		} else {
			/* hw_bc11_stepC1 */
			j36_bc11_ipu_en(p, 1);
			j36_bc11_vref_vth(p, 2);
			j36_bc11_cmp_en(p, 1);
			msleep(J36_BC11_DWELL_STEP_MS);
			avail = j36_bc11_cmp_out(p);
			j36_bc11_ipu_en(p, 0);
			j36_bc11_cmp_en(p, 0);
			j36_bc11_vref_vth(p, 0);
			j36_bc11_settle(p, POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID,
					avail ? J36_BC11_LIMIT_APPLE_1_0A_MA
					      : J36_BC11_LIMIT_APPLE_0_5A_MA,
					avail ? "an Apple 1.0 A charger"
					      : "an Apple 0.5 A charger");
		}
	} else {
		/* hw_bc11_stepA2 */
		j36_bc11_vsrc_en(p, 2);
		j36_bc11_ipd_en(p, 1);
		j36_bc11_vref_vth(p, 0);
		j36_bc11_cmp_en(p, 1);
		msleep(J36_BC11_DWELL_STEP_MS);
		avail = j36_bc11_cmp_out(p);
		j36_bc11_vsrc_en(p, 0);
		j36_bc11_ipd_en(p, 0);
		j36_bc11_cmp_en(p, 0);

		if (!avail) {
			j36_bc11_settle(p, POWER_SUPPLY_USB_TYPE_SDP,
					J36_BC11_LIMIT_SDP_MA,
					"a standard USB host");
		} else {
			/* hw_bc11_stepB2 */
			j36_bc11_ipu_en(p, 2);
			j36_bc11_vref_vth(p, 1);
			j36_bc11_cmp_en(p, 1);
			msleep(J36_BC11_DWELL_STEP_MS);
			avail = j36_bc11_cmp_out(p);
			j36_bc11_ipu_en(p, 0);
			j36_bc11_cmp_en(p, 0);
			j36_bc11_vref_vth(p, 0);
			if (avail)
				j36_bc11_settle(p, POWER_SUPPLY_USB_TYPE_DCP,
						J36_BC11_LIMIT_DCP_MA,
						"a dedicated charger");
			else
				j36_bc11_settle(p, POWER_SUPPLY_USB_TYPE_CDP,
						J36_BC11_LIMIT_CDP_MA,
						"a charging host");
		}
	}

	/* hw_bc11_done */
	j36_bc11_teardown(p);
	p->bc11_done = true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE CHARGER
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * The watchdog, and the deal it represents.
 *
 * Armed if and only if charging, in the preloader, in LK and in the kernel, with
 * no exception -- and its expiry is what took VBAT down on a cell-less board,
 * because the preloader arms it and a port that does not know about it never
 * kicks it.  So it is armed here, from j36_charger_arm(), which runs on every
 * poll: the window is 4 s and the poll is 1 s.
 *
 * Two deliberate departures from stock, both about the expiry and not the timer.
 * INT_EN stays 0: this driver has no handler and nothing to do with the news, and
 * masked, an expiry is a charge that stops rather than an interrupt into nowhere.
 * And FLAG_WR is written 1 here and 0 in the disarm -- 1 clears the latched
 * expiry, 0 is part of shutting the timer down, and writing 0 to clear a latch
 * leaves the latch exactly where it was.
 *
 * Not routed through j36_pmic_update(): every write here is a kick whose value is
 * already in the register, so "already correct, skip it" would skip the point.
 */
static void j36_charger_watchdog_kick(struct j36_pmic *p)
{
	u32 con13, con15;

	if (!j36_pmic_read(p, J36_CHR_CON15, &con15)) {
		/* OUT set means the timer really had run out, which is the direct
		 * confirmation that this was what was holding the charger off
		 * rather than a plausible story about it. */
		if (con15 & J36_CHR_CON15_CHRWDT_OUT)
			dev_info_once(p->dev,
				      "CHRWDT had expired (CON15 OUT set); clearing the latch\n");
		j36_pmic_write(p, J36_CHR_CON15,
			       (con15 & ~(u32)J36_CHR_CON15_CHRWDT_INT_EN) |
			       J36_CHR_CON15_CHRWDT_FLAG_WR);
	}

	if (j36_pmic_read(p, J36_CHR_CON13, &con13))
		return;
	j36_pmic_write(p, J36_CHR_CON13,
		       (con13 & ~(u32)J36_CHR_CON13_CHRWDT_TD_MASK) |
		       J36_CHR_CON13_CHRWDT_TD_4S |
		       J36_CHR_CON13_CHRWDT_EN |
		       J36_CHR_CON13_CHRWDT_WR);

	/* Read back, rather than trusting that the write returned 0: pwrap
	 * acknowledges a transaction it delivered and cannot promise the charger
	 * block honoured it.  A watchdog that silently refused to arm is the
	 * no-charge bug again, and this is the only place it would be visible. */
	if (!j36_pmic_read(p, J36_CHR_CON13, &con13)) {
		if (con13 & J36_CHR_CON13_CHRWDT_EN)
			dev_info_once(p->dev,
				      "charger watchdog armed and kicked (CON13=%04x)\n",
				      con13);
		else
			dev_warn_once(p->dev,
				      "charger watchdog REFUSED TO ARM (CON13=%04x)\n",
				      con13);
	}
}

/* stock's charging_enable(FALSE), in its order: mask, stop, release the flag.
 * Called when the cable is out, i.e. when the kick above is no longer going to
 * happen -- an armed timer that nothing kicks is exactly the arrangement that
 * took a cell-less board's own rail down. */
static void j36_charger_watchdog_disarm(struct j36_pmic *p)
{
	u32 reg;

	if (!j36_pmic_read(p, J36_CHR_CON15, &reg))
		j36_pmic_write(p, J36_CHR_CON15,
			       reg & ~(u32)J36_CHR_CON15_CHRWDT_INT_EN);
	if (!j36_pmic_read(p, J36_CHR_CON13, &reg))
		j36_pmic_write(p, J36_CHR_CON13,
			       reg & ~(u32)J36_CHR_CON13_CHRWDT_EN);
	if (!j36_pmic_read(p, J36_CHR_CON15, &reg))
		j36_pmic_write(p, J36_CHR_CON15,
			       reg & ~(u32)J36_CHR_CON15_CHRWDT_FLAG_WR);
}

/* The first CS_VTH step at or under what the port allows.  450 mA until BC1.2 has
 * said anything, which is the step stock LK uses and never changes. */
static u32 j36_cs_vth_code(int limit_ma)
{
	unsigned int i;

	if (limit_ma <= 0)
		return J36_CHR_CON4_CS_VTH_DEFAULT;
	if (limit_ma > J36_CS_VTH_CAP_MA)
		limit_ma = J36_CS_VTH_CAP_MA;

	for (i = 0; i < ARRAY_SIZE(j36_cs_vth_ma); ++i)
		if (j36_cs_vth_ma[i] <= limit_ma)
			return i;
	return J36_CHR_CON4_CS_VTH_DEFAULT;
}

/* The CV setpoint as the register currently holds it, in microvolts, for
 * CONSTANT_CHARGE_VOLTAGE.  -1 when it cannot be read. */
static int j36_charger_cv_uv(struct j36_pmic *p)
{
	u32 con3;

	if (j36_pmic_read(p, J36_CHR_CON3, &con3))
		return -1;
	return j36_cv_mv[con3 & J36_CHR_CON3_CV_MASK] * 1000;
}

/*
 * ── THE OPERATOR CV HOLD ──
 *
 * Writing constant_charge_voltage by hand is how you find out what this pack
 * does at a setpoint, and the re-arm above would otherwise stamp 4200 mV back
 * over it inside a second -- the operator would watch their value evaporate and
 * conclude the write did not take.  So an accepted write buys a few seconds
 * during which the periodic arm leaves CHR_CON3 alone: long enough to read a
 * current back, short enough that a forgotten override cannot leave the board on
 * a wrong setpoint indefinitely.
 *
 * Thirty seconds rather than MVII's three, and the difference is the operator.
 * There the caller is a console command in a loop; here it is a person at a
 * shell running cat, and three seconds is not enough time to type the second
 * command.
 */
#define J36_CV_HOLD_LINGER_MS		30000

static bool j36_cv_operator_holds(struct j36_pmic *p, ktime_t now)
{
	if (!p->cv_hold)
		return false;
	/* Clock backwards: assume held.  Erring towards leaving the operator's
	 * value alone costs one stale setpoint; erring the other way overwrites
	 * a register somebody is in the middle of measuring. */
	if (ktime_before(now, p->cv_hold))
		return true;
	return ktime_ms_delta(now, p->cv_hold) < J36_CV_HOLD_LINGER_MS;
}

/*
 * The code for a requested setpoint: the LOWEST table entry that still reaches
 * it.  The table is not sorted -- 4162 mV sits at code 13 between 4425 and 4175,
 * and three values appear twice -- so this is a scan and not a search, and the
 * tie is broken towards the lower code because that is the one stock uses.
 *
 * Returns < 0 when nothing in the table reaches the request.
 */
static int j36_cv_code_for_uv(int uv)
{
	int mv = uv / 1000;
	int best = -1;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(j36_cv_mv); ++i) {
		if (j36_cv_mv[i] < mv)
			continue;
		if (best < 0 || j36_cv_mv[i] < j36_cv_mv[best])
			best = i;
	}
	return best;
}

/*
 * Take an operator's setpoint.
 *
 * The one refusal is the one that has killed a board: LOWERING the CV below the
 * node.  The charger's output IS the system rail here -- no power-path FET --
 * and a CV loop told to regulate to a voltage the node has already passed does
 * not coast, it collapses the rail, in under a millisecond and with no cell
 * fitted to hold it up.  MVII takes any code the console offers because there
 * the operator is holding the board in their hand with a bench supply on it; a
 * sysfs file is reachable from a script, and this one refuses instead.
 *
 * Everything else is allowed, including values above 4200: raising is the safe
 * direction, and VBAT_OV at 4300 mV is still armed underneath whatever is
 * written here.
 */
static int j36_charger_cv_set(struct j36_pmic *p, int uv)
{
	int code = j36_cv_code_for_uv(uv);
	unsigned long flags;
	int node_uv, readback;

	/* The worker rewrites p->pub wholesale once a second and this path runs
	 * from a userspace write, so the one field the refusal turns on is taken
	 * under the lock rather than read out from under a store. */
	spin_lock_irqsave(&p->lock, flags);
	node_uv = p->pub.voltage_uv;
	spin_unlock_irqrestore(&p->lock, flags);

	if (!charge)
		return -EPERM;
	if (code < 0)
		return -ERANGE;
	if (node_uv > 0 && j36_cv_mv[code] * 1000 < node_uv) {
		dev_warn(p->dev,
			 "refusing CV %d uV: the node is at %d uV and this charger cannot sink\n",
			 uv, node_uv);
		return -ERANGE;
	}

	/* < 0 is the failure; 1 "changed" and 0 "already correct" are both fine,
	 * and the second is the common one when an operator re-writes the value
	 * they just set to refresh the hold. */
	if (j36_pmic_field(p, J36_CHR_CON3, J36_CHR_CON3_CV_MASK, 0, code) < 0)
		return -EIO;

	/* Refreshed on every accepted write, including one that changed nothing:
	 * the operator asked for this value, so keep the re-arm out of it either
	 * way. */
	p->cv_hold = ktime_get();

	readback = j36_charger_cv_uv(p);
	if (readback < 0)
		return -EIO;
	if (readback != j36_cv_mv[code] * 1000) {
		dev_warn(p->dev, "CV write did not take (asked %d uV, reads %d uV)\n",
			 j36_cv_mv[code] * 1000, readback);
		return -EIO;
	}

	dev_info(p->dev, "CV set to %d mV by hand; the re-arm will leave it alone for %d s\n",
		 j36_cv_mv[code], J36_CV_HOLD_LINGER_MS / 1000);
	return 0;
}

/*
 * stock's charging_hw_init() plus the enable, in stock's order, every poll.
 *
 * The order is load-bearing at three points and the rest is stock's:
 *
 *   - USBDL is left FIRST, ahead of every other charger write, because until it
 *     lands none of them reach the block.  It stays in the per-poll path rather
 *     than a one-shot init because a mode this kernel never asked for is a mode
 *     it cannot assume stays gone.
 *   - the CV target is written BEFORE CS_EN/CSDAC_EN/CHR_EN, so the loop comes up
 *     against the right setpoint rather than being corrected after the fact.
 *   - the watchdog is kicked immediately before the enable, where all three stock
 *     paths put it, because on this PMIC the charger gates on it.
 *
 * VCDT_HV_EN is the one write this board deliberately differs from stock on: it
 * gates the input on an over-voltage comparator whose threshold this driver has
 * never set, so it is turned off rather than on.
 */
static void j36_charger_arm(struct j36_pmic *p, bool online)
{
	u32 con0, con4, code;

	if (!charge)
		return;

	/* Widest ride-through on VSYS.  With no cell fitted VBAT is VSYS, so a dip
	 * that trips UVLO is not a brownout warning -- it is the power cut, and it
	 * presents to an operator as a spontaneous restart. */
	j36_pmic_update(p, J36_CHR_CON16, J36_CHR_CON16_UVLO_VTHL_MASK,
			J36_CHR_CON16_UVLO_VTHL_LOWEST);

	/*
	 * Hand the charger back to software.  Clear the request, then pulse the
	 * reset: dropping the request before asking the latch to release cannot
	 * re-arm behind the reset.  Both calls are made unconditionally and their
	 * results collected afterwards -- a short-circuiting || here would skip
	 * the reset on exactly the boot where the request bit was set, which is
	 * the only boot where either matters.
	 */
	{
		int cleared = j36_pmic_update(p, J36_CHR_CON16,
					      J36_CHR_CON16_USBDL_SET, 0);
		int reset = j36_pmic_update(p, J36_CHR_CON16, 0,
					    J36_CHR_CON16_USBDL_RST);

		/* Worth one line the first time either bit actually moved: it is
		 * the difference between "armed and charging" and "armed and
		 * ignored", and it is invisible in a register dump taken after. */
		if (cleared == 1 || reset == 1)
			dev_info_once(p->dev,
				      "left hardware USBDL charging mode (CHR_CON16 SET->0, RST->1)\n");
	}

	j36_pmic_update(p, J36_CHR_CON23, J36_CHR_CON23_VCDT_MODE, 0);
	j36_pmic_update(p, J36_CHR_CON6, J36_CHR_CON6_VBAT_OV_VTH_MASK,
			J36_CHR_CON6_VBAT_OV_EN |
			J36_CHR_CON6_VBAT_OV_VTH_4300MV);
	j36_pmic_update(p, J36_CHR_CON7, J36_CHR_CON7_BATON_HT_EN,
			J36_CHR_CON7_BATON_EN);
	j36_pmic_update(p, J36_CHR_CON23, 0, J36_CHR_CON23_ULC_DET_EN);
	j36_pmic_update(p, J36_CHR_CON22, J36_CHR_CON22_LOW_ICH_DB_MASK,
			J36_CHR_CON22_LOW_ICH_DB_DEFAULT);
	j36_pmic_update(p, J36_CHR_CON0, J36_CHR_CON0_VCDT_HV_EN, 0);

	if (!online) {
		/* Nothing to charge into.  Stop the timer, because the kick at the
		 * end of this function is what feeds it and this is the path that
		 * skips it, and drop the enables so a cable arriving finds the
		 * block in the state the arm sequence expects. */
		j36_pmic_update(p, J36_CHR_CON0,
				J36_CHR_CON0_CSDAC_EN | J36_CHR_CON0_CHR_EN, 0);
		j36_charger_watchdog_disarm(p);
		/* Forgotten rather than kept: with CHR_EN clear the field still
		 * reads whatever it last held, and reporting that as the charge
		 * current on a board with no cable in it would be a number that
		 * describes nothing. */
		p->charge_step_ma = -1;
		return;
	}

	/*
	 * The charge current, from what BC1.2 said the port can give.
	 *
	 * READ THE FIELD BACK; DO NOT INFER IT FROM THE WRITE.  j36_pmic_update
	 * returns 1 for "a write happened" and 0 for "already correct", and on a
	 * function that runs once a second the second arm is by far the common
	 * one -- the field is right after the first pass.  Publishing off the ==1
	 * arm therefore latches nothing on any boot where CHR_CON4 already held
	 * this code, and the reported step stays unknown until an unplug moves
	 * the field out from under it and makes the replug's arm a real write.
	 * MVII shipped exactly that bug, and it presented as "the mA reading only
	 * appears after you unplug and plug back in".
	 *
	 * What is published is what the charger is SET TO rather than what we
	 * meant to set it to, which is also the right answer if anything else
	 * ever moves the field.
	 */
	code = j36_cs_vth_code(p->bc11_limit_ma);
	j36_pmic_update(p, J36_CHR_CON4, J36_CHR_CON4_CS_VTH_MASK, code);
	if (j36_pmic_read(p, J36_CHR_CON4, &con4) == 0) {
		int step = j36_cs_vth_ma[con4 & J36_CHR_CON4_CS_VTH_MASK];

		/* Keyed on the step rather than on the requested code, so a write
		 * that silently did not take says so by printing the step the
		 * charger is really on.  A failed read leaves the last value
		 * alone rather than printing a zero beside a stale one. */
		if (p->charge_step_ma != step) {
			p->charge_step_ma = step;
			dev_info(p->dev,
				 "charge current %d mA (CS_VTH=%u, port allows %d mA)\n",
				 step, con4 & J36_CHR_CON4_CS_VTH_MASK,
				 p->bc11_limit_ma > 0 ? p->bc11_limit_ma : 0);
		}
	}

	/*
	 * ══ THE CV TARGET, WHICH NOTHING HAD EVER WRITTEN ══
	 *
	 * CHR_CON3[4:0] powers on at 29, and code 29 is 4162 mV.  The node sits at
	 * 4183.  The setpoint was twenty-one millivolts BELOW the cell, and a CV
	 * loop asked to regulate to a voltage the pack has already passed sources
	 * nothing, because a charger cannot sink.  Every other register armed here
	 * was armed correctly and then handed a target the pack had already
	 * cleared.
	 *
	 * Raising is the safe direction and lowering is not: a CV sweep downwards
	 * killed a cell-less board in under a millisecond, because the charger's
	 * output is the system rail.
	 */
	if (!j36_cv_operator_holds(p, ktime_get()) &&
	    j36_pmic_update(p, J36_CHR_CON3, J36_CHR_CON3_CV_MASK,
			    J36_CHR_CON3_CV_4200MV) == 1) {
		/* Read back.  A silently ignored write would look exactly like
		 * this bug still being present, which is how it survived as long
		 * as it did. */
		int uv = j36_charger_cv_uv(p);

		if (uv == 4200 * 1000)
			dev_info(p->dev, "CV target set to 4200 mV, read back ok\n");
		else
			dev_warn(p->dev, "CV target write did not take (read back %d uV)\n",
				 uv);
	}

	/* The CSDAC ramp: one step up, two down, four-unit delay, unit step.
	 * Stock's values, and the reason the arm sequence does not slam the node. */
	j36_pmic_update(p, J36_CHR_CON20, 0xffff,
			J36_CHR_CON20_CSDAC_STP_INC_1 |
			J36_CHR_CON20_CSDAC_STP_DEC_2);
	j36_pmic_update(p, J36_CHR_CON21, 0xffff,
			J36_CHR_CON21_CSDAC_DLY_4 | J36_CHR_CON21_CSDAC_STP_1);
	j36_pmic_update(p, J36_CHR_CON2, 0,
			J36_CHR_CON2_VBAT_CV_EN | J36_CHR_CON2_CS_EN);
	j36_pmic_update(p, J36_CHR_CON23, 0,
			J36_CHR_CON23_CSDAC_MODE | J36_CHR_CON23_HWCV_EN);

	j36_charger_watchdog_kick(p);

	/* Last: the current source and the charger itself. */
	j36_pmic_update(p, J36_CHR_CON0, J36_CHR_CON0_VCDT_HV_EN,
			J36_CHR_CON0_CSDAC_EN | J36_CHR_CON0_CHR_EN);

	if (!j36_pmic_read(p, J36_CHR_CON0, &con0))
		dev_dbg(p->dev, "armed, CHR_CON0=%04x\n", con0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE GAUGE
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * The termination ladder.  Full is a sequence, not a threshold: six consecutive
 * samples within 150 mA of zero, and only after the node has been seen at or
 * above the top-off voltage at least once in this charge run.  Once full, only
 * dropping below the recharge voltage clears it.
 */
static void j36_ladder_advance(struct j36_pmic *p, bool online, int bat_mv,
			       int ma, bool ma_valid)
{
	if (!online) {
		/* Unplugged: nothing about full is meaningful, and the next plug
		 * must re-earn top-off from scratch. */
		p->full_count = 0;
		p->recharge_count = 0;
		p->topoff_seen = false;
		p->charge_full = false;
		return;
	}

	if (bat_mv >= J36_V_CC2TOPOFF_MV)
		p->topoff_seen = true;

	if (p->charge_full) {
		/*
		 * ── LEAVING FULL IS ALSO A SEQUENCE ──
		 *
		 * AND THIS IS THE 99-100-99 FLICKER.  Earning full takes six samples
		 * in a row; losing it took ONE, and the one it took was a comparison
		 * against 4110 mV of a node that sits only a little above that at the
		 * charger's setpoint.  A single ADC sample dipping a few counts under
		 * the line therefore threw the latch away, the level fell back to the
		 * 99% cap, the ladder spent another six seconds re-earning it, and the
		 * display walked 99 - 100 - 99 forever.
		 *
		 * A real recharge is not a sample, it is a condition: the pack has
		 * genuinely relaxed away from the setpoint and stayed there.  Requiring
		 * the same six in a row costs six seconds of latency on a transition
		 * that only matters over hours, and it makes the latch mean what its
		 * name says.
		 */
		if (bat_mv > 0 && bat_mv < J36_RECHARGING_MV) {
			if (++p->recharge_count >= J36_FULL_CHECK_TIMES) {
				p->charge_full = false;
				p->full_count = 0;
				p->recharge_count = 0;
				dev_info(p->dev, "battery no longer full (%d mV, under the %d mV recharge line)\n",
					 bat_mv, J36_RECHARGING_MV);
			}
		} else {
			p->recharge_count = 0;
		}
		return;
	}
	p->recharge_count = 0;

	if (!p->topoff_seen)
		return;

	/* An unknown current is not a small current. */
	if (!ma_valid) {
		p->full_count = 0;
		return;
	}

	/*
	 * ── A BAND AROUND ZERO, NOT A ONE-SIDED THRESHOLD ──
	 *
	 * AND THIS IS WHY THE LEVEL SAT AT 99 AND NEVER MOVED.  At termination the
	 * charger holds the node at its CV setpoint and the pack takes nothing, so
	 * what this driver measures is a couple of millivolts across a 68 mOhm
	 * shunt -- a handful of ADC counts, which is noise, and noise has a sign.
	 * Rejecting `ma < 0' therefore threw away the negative half of a reading
	 * centred on zero: the six-in-a-row count was cleared every second or two,
	 * charge_full could never be earned, and the 99% cap in the gauge -- which
	 * is only meant to hold until the ladder says otherwise -- had no way out.
	 *
	 * What disqualifies a full pack is CURRENT, in either direction.  In means
	 * it is still filling; more than a trickle out means the charger cannot
	 * carry the system and the pack is being drained.  Both are the same
	 * question about magnitude, so one threshold answers both ends of it.
	 */
	if (ma > J36_CHARGING_FULL_CURRENT_MA || ma < -J36_CHARGING_FULL_CURRENT_MA) {
		p->full_count = 0;
		return;
	}
	if (++p->full_count >= J36_FULL_CHECK_TIMES) {
		p->full_count = 0;
		p->charge_full = true;
		dev_info(p->dev, "battery full (six samples within %d mA of zero past top-off)\n",
			 J36_CHARGING_FULL_CURRENT_MA);
	}
}

/*
 * ── THE PUBLISHED PERCENT IS AN INTEGRATOR SEEDED FROM THE CELL ──
 *
 * Not a lookup.  A lookup is wrong on this board in a way no amount of care
 * inside the lookup can fix, and it is the "reads 99% always while charging" an
 * operator reported: with no power-path FET, BATSNS reads the charger's CV
 * setpoint whenever a cable is in.  IR compensation takes a little of that back
 * -- 145 mV at 926 mA -- but the charger holds the rail wherever it likes, so the
 * compensated number is still a fact about the POWER RAIL.
 *
 * The kernel this was ported from does not do a lookup either.  This is stock's
 * oam_run() structure:
 *
 *   - the SEED comes from the wakeup OCV latch, taken before the charger existed.
 *     That single reading is what anchors the absolute level.
 *   - the live compensated lookup is a TARGET, not the answer.
 *   - the published depletion moves toward that target by ONE PERCENT per
 *     interval, and the direction is gated on whether current is flowing IN:
 *     charging can only ever raise the percentage and discharging can only ever
 *     lower it.  A cable going in cannot make the number jump, because nothing
 *     here is allowed to jump.
 *
 * Charging does not walk the curve at all -- it integrates coulombs.  On the flat
 * part of a lithium curve the rail's rise and the IR correction very nearly
 * cancel, so a "step towards the target" rule has nothing to step towards and the
 * display sits still for minutes while the pack genuinely fills.  Against 1499
 * mAh at ~550 mA a percent is about a hundred seconds, which is a measurement
 * rather than a display preference.
 *
 * Returns percent remaining, or -1 before anything has been established.
 */
static int j36_gauge_percent(struct j36_pmic *p, bool online, int bat_mv,
			     int ma, bool ma_valid, int *out_ocv_mv)
{
	const ktime_t now = ktime_get();
	int target_dep;
	int ocv;
	int pct;

	*out_ocv_mv = -1;

	/* A missing sample does not un-publish a level that has already been
	 * established; the integrator holds. */
	if (bat_mv <= 0)
		return p->soc_dep >= 0 ? 100 - p->soc_dep : -1;

	ocv = j36_battery_ocv_mv(bat_mv, ma_valid ? ma : 0);
	*out_ocv_mv = ocv;

	pct = j36_battery_percent_from_ocv(ocv);
	if (pct < 0)
		return p->soc_dep >= 0 ? 100 - p->soc_dep : -1;
	if (pct > 100)
		pct = 100;
	target_dep = 100 - pct;

	/*
	 * ── EMPTY IS A RAMP, NOT A SNAP ──
	 *
	 * This was once a hard "under the floor means zero", on the reasoning that
	 * a cell below the floor is the one state where a smooth number is the
	 * wrong answer.  Stock says otherwise: its 0% tracking check does nothing
	 * but decrement by one per battery-task period, in BOTH branches, even at
	 * or below the system-off voltage.
	 *
	 * The snap is also why the level read 0% when the cable came OUT.  Every
	 * ring is forgotten on a plug edge, so the next conversion is a median of
	 * ONE -- and with no power-path FET the supply is genuinely interrupted
	 * for a few milliseconds as the cable parts.  One unfiltered sample of
	 * that dip latched the level to empty, and from there the ordinary slew
	 * crawls back at one percent per thirty seconds.  As a ramp the same dip
	 * costs at most one percent.
	 *
	 * Only once there IS a level to walk down.  A first-ever sample this low
	 * is not a ramp, it is a seed, and the seed below already reads ~0% off
	 * the curve for any OCV under the table's bottom row.
	 */
	if (bat_mv <= J36_V_0PERCENT_TRACKING_MV && p->soc_dep >= 0) {
		if (!p->soc_slew ||
		    ktime_us_delta(now, p->soc_slew) >= J36_LADDER_INTERVAL_US) {
			if (p->soc_dep < 100)
				++p->soc_dep;
			p->soc_slew = now;
		}
		/* A ramp that owns the level for a while ends any charge run, so
		 * the next charging sample re-bases here instead of billing the
		 * whole detour to the accumulator as coulombs it never saw. */
		p->soc_car_base = -1;
		p->soc_car = 0;
		return 100 - p->soc_dep;
	}

	if (p->soc_dep < 0) {
		/*
		 * SEED.  With a cable in, the live lookup is a statement about
		 * the rail, so the wakeup latch is used if there is one;
		 * unplugged, the live reading is itself a clean OCV -- only the
		 * board's own tens of milliamps across it -- and it is newer than
		 * the latch, so it wins.
		 *
		 * If the latch is unreadable AND a cable is in, the rail is all
		 * there is and the level starts optimistic.  But it starts, and
		 * the ladder and the 99% cap keep it honest from there; refusing
		 * to publish would leave the status bar blank for the session.
		 */
		int seed = target_dep;

		if (online && p->hw_ocv_mv > 0) {
			int hp = j36_battery_percent_from_ocv(p->hw_ocv_mv);

			if (hp >= 0)
				seed = 100 - min(hp, 100);
		}
		p->soc_dep = seed;
		p->soc_slew = now;
	} else if (ma_valid && ma > 0) {
		/* Charging: integrate, do not look up.  No slew gate -- the
		 * pack's own capacity IS the rate limit. */
		if (p->soc_car_base < 0 || ktime_compare(now, p->soc_car_time) <= 0) {
			p->soc_car_base = p->soc_dep;
			p->soc_car = 0;
		} else {
			int moved;

			p->soc_car += (s64)ma *
				      ktime_us_delta(now, p->soc_car_time);
			moved = (int)div64_s64(p->soc_car, J36_SOC_CAR_PER_PCT);
			p->soc_dep = p->soc_car_base - moved;
		}
		p->soc_car_time = now;
		p->soc_charge_time = now;
		/* So a charge run that ends leaves the voltage path a fresh
		 * interval rather than a step it is instantly owed. */
		p->soc_slew = now;
	} else if (p->soc_car_base >= 0 && p->soc_charge_time &&
		   ktime_us_delta(now, p->soc_charge_time) < J36_SOC_CAR_GRACE_US) {
		/* A GAP INSIDE A LIVE RUN IS A GAP, NOT AN UNPLUG.  The clock
		 * advances so the gap is not billed as charge that was never
		 * delivered, the count itself is untouched, and the level holds. */
		p->soc_car_time = now;
		p->soc_slew = now;
	} else if (!p->soc_slew ||
		   ktime_us_delta(now, p->soc_slew) >= J36_SOC_SLEW_INTERVAL_US) {
		/* Discharging, or a current this driver could not read -- which
		 * is not a charge, and stock does not treat it as one either.
		 * Here the terminal voltage IS the cell, so the curve is the best
		 * estimate there is and the level walks towards it. */
		p->soc_car_base = -1;
		p->soc_car = 0;
		if (p->soc_dep < target_dep)
			++p->soc_dep;
		p->soc_slew = now;
	}

	/*
	 * ── FULL IS 100, AND THE INTEGRATOR IS TOLD SO ──
	 *
	 * The ladder terminating IS what full means on this board: the node has
	 * been at or above top-off in this run and the pack has taken nothing for
	 * six samples together.  Against that, the integrator is not the better
	 * authority -- it has been counting coulombs into a pack that stopped
	 * accepting them -- so the level is SET to 100 here rather than merely
	 * displayed as 100 further down.
	 *
	 * Setting it matters at the unplug.  A gauge left at, say, 97 while the
	 * display was forced to 100 would snap back to 97 the moment the cable came
	 * out, which is the same lie told in the other direction and the one that
	 * looks like a fault.  Ending the coulomb run with it means a recharge
	 * cycle re-bases from 100 instead of billing this pin to the accumulator.
	 *
	 * It cannot flap: charge_full is cleared only by the pack dropping under
	 * the recharge voltage, or by the cable coming out, so this pins the level
	 * for exactly as long as the pack is actually full.
	 */
	if (p->charge_full) {
		p->soc_dep = 0;
		p->soc_car_base = -1;
		p->soc_car = 0;
	}

	p->soc_dep = clamp(p->soc_dep, 0, 100);
	pct = 100 - p->soc_dep;

	/* Short of that, 100% is the ladder's word and not the curve's -- stock's
	 * mt_battery_100Percent_tracking_check(): "charging is not full, UI keep
	 * 99% if reaching 100%".  Kept because showing full while still drawing
	 * current is the one reading an operator will call a lie; it is a holding
	 * position now rather than a destination, because the ladder above can
	 * always get out of it. */
	if (online && !p->charge_full && pct >= 100)
		pct = 99;
	return pct;
}

static int j36_gauge_status(struct j36_pmic *p, bool online, int ma, bool ma_valid)
{
	if (!online)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	if (p->charge_full)
		return POWER_SUPPLY_STATUS_FULL;
	/* A cable with an unmeasurable current is not NOT_CHARGING -- it is
	 * unknown, and saying NOT_CHARGING would be an assertion about the charger
	 * drawn from the state of this driver's ring buffers. */
	if (!ma_valid)
		return POWER_SUPPLY_STATUS_UNKNOWN;
	return ma > 0 ? POWER_SUPPLY_STATUS_CHARGING
		      : POWER_SUPPLY_STATUS_NOT_CHARGING;
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE POLL
 * ══════════════════════════════════════════════════════════════════════════ */

/* CHRDET is a live comparator on the CHRIN pin: no arming, no settling, one pwrap
 * read.  The DRVVBUS interlock overrides it, because this board's own boost lands
 * on the same net and would otherwise read as a charger. */
static int j36_charger_online(struct j36_pmic *p)
{
	u32 con0;

	if (j36_drvvbus_asserted(p)) {
		if (!p->vbus_warned) {
			p->vbus_warned = true;
			dev_info(p->dev,
				 "DRVVBUS is asserted: the port is sourcing 5 V, so CHRDET is this board's own boost -- charger held off\n");
		}
		return 0;
	}
	p->vbus_warned = false;

	if (j36_pmic_read(p, J36_CHR_CON0, &con0))
		return -1;
	return (con0 & J36_CHR_CON0_CHRDET) ? 1 : 0;
}

/* Twenty polls, which at the one-second default is one line per twenty seconds
 * -- slow enough to be free, fast enough that a plug event is never more than a
 * third of a minute from its first confirmation. */
#define J36_CHARGING_LINE_EVERY		20

/*
 * ══ IS IT CHARGING? ══
 *
 * Three numbers, roughly every twenty polls, on one line, and the reason it is
 * three:
 *
 *   CS_DET is not the answer.  It is an instantaneous comparator on a current
 *     source that HWCV and the CSDAC ramp are actively modulating, so a snapshot
 *     catching it low says nothing; on MVII it read 0 and 1 alternately on a
 *     board that was drawing a steady half-amp.
 *
 *   pack mA is the shunt pair -- ISENSE minus BATSNS over R_SENSE -- and a fixed
 *     offset between the two channels would read as a constant phantom current
 *     that looks exactly like a charge.  A positive number here is not by itself
 *     proof of anything.
 *
 *   VBAT is the one that cannot be faked.  A cell that is taking charge climbs.
 *     If this number rises over a couple of minutes the board is charging
 *     whatever the other two say, and if it falls it is not.
 *
 * Printed on BOTH paths, cable in and cable out -- which here is one call site
 * taking `online` as an argument rather than MVII's two, because this poll has a
 * single trunk -- because the cable-out reading is the CONTROL: ~0 mA with no
 * VBUS means the shunt pair is honest, and the same ~500 mA with no VBUS means it
 * is an offset and the "charge current" is fiction.  MVII's first version of this
 * line only printed with a cable in, which is exactly the half of the experiment
 * that cannot distinguish the two.
 *
 * dev_info and not dev_dbg: this is the line someone reads back off a serial
 * console or out of dmesg on a board that will not charge, and a diagnostic that
 * needs a rebuild to turn on is a diagnostic nobody has when they need it.  At
 * one line per twenty seconds it costs the ring buffer nothing.
 */
/* Twenty polls, which at the one-second default is one line per twenty seconds
 * -- slow enough to be free, fast enough that a plug event is never more than a
 * third of a minute from its first confirmation. */
#define J36_CHARGING_LINE_EVERY		20

static void j36_charging_line(struct j36_pmic *p, int online, int ma,
			      bool ma_valid, int bat_mv)
{
	char pack_buf[16], vbat_buf[16];
	const char *pack, *vbat;
	u32 con2;
	int cs_det;

	if (++p->poll_kicks % J36_CHARGING_LINE_EVERY)
		return;

	cs_det = j36_pmic_read(p, J36_CHR_CON2, &con2) == 0
		 ? !!(con2 & J36_CHR_CON2_CS_DET) : -1;

	if (ma_valid) {
		scnprintf(pack_buf, sizeof(pack_buf), "%d mA", ma);
		pack = pack_buf;
	} else {
		pack = "(no sample)";
	}

	if (bat_mv > 0) {
		scnprintf(vbat_buf, sizeof(vbat_buf), "%d mV", bat_mv);
		vbat = vbat_buf;
	} else {
		vbat = "(no sample)";
	}

	dev_info(p->dev, "charging: VBUS=%d CS_DET=%s pack %s VBAT %s\n",
		 online ? 1 : 0, cs_det < 0 ? "?" : (cs_det ? "1" : "0"),
		 pack, vbat);
}

/*
 * One poll.  Everything in here runs in order and blocks where it needs to.
 *
 * BATSNS and ISENSE are converted BACK TO BACK, and that is the whole of the
 * pairing rule: the shunt delta is its own filtered quantity, differenced first
 * and filtered after, because the common mode both channels share is exactly what
 * moves on this board.  Subtracting two independently filtered channels is only
 * equal to filtering the differences when the rail is stationary, and here a cable
 * going in shifts both by hundreds of counts at once.
 */
static void j36_pmic_poll(struct work_struct *work)
{
	struct j36_pmic *p = container_of(to_delayed_work(work),
					  struct j36_pmic, poll_work);
	struct j36_pub pub;
	unsigned long flags;
	int online, bat_raw, isense_raw, vchr_raw;
	int bat_mv = -1, ocv_mv = -1, ma = 0;
	bool ma_valid = false;
	bool changed;

	if (!j36_pwrap_up(p)) {
		dev_warn_once(p->dev, "pwrap is not up; nothing can be read yet\n");
		goto again;
	}

	j36_hw_ocv_prime(p);

	online = j36_charger_online(p);
	if (online < 0) {
		dev_warn_once(p->dev, "CHRDET unreadable\n");
		goto again;
	}

	/*
	 * THE PLUG EDGE.  Everything filtered is about a supply topology that has
	 * just changed, so all three rings are forgotten rather than allowed to
	 * average across the event -- a median that straddles a plug is a number
	 * about neither side of it.
	 */
	if (p->online != online) {
		p->online = online;
		dev_info(p->dev, online ? "VBUS present\n"
					: "no VBUS; system on battery\n");
		j36_ring_forget(&p->batsns);
		j36_ring_forget(&p->vchr);
		j36_ring_forget(&p->delta);
		j36_bc11_forget(p);
		if (online && bc11 && p->usbphy && p->pericfg)
			j36_bc11_run(p);
		else if (online)
			dev_info_once(p->dev,
				      "BC1.2 skipped; the charger runs at the %u mA default\n",
				      j36_cs_vth_ma[J36_CHR_CON4_CS_VTH_DEFAULT]);
	}

	if (!j36_adc_convert(p, J36_ADC_BATSNS_CHANNEL, J36_ADC_BATSNS_DATA,
			     &bat_raw) &&
	    !j36_adc_convert(p, J36_ADC_ISENSE_CHANNEL, J36_ADC_ISENSE_DATA,
			     &isense_raw)) {
		j36_ring_push(&p->batsns, bat_raw);
		j36_ring_push(&p->delta, isense_raw - bat_raw);
	}
	if (online && !j36_adc_convert(p, J36_ADC_VCHR_CHANNEL, J36_ADC_VCHR_DATA,
				       &vchr_raw))
		j36_ring_push(&p->vchr, vchr_raw);

	if (p->batsns.valid)
		bat_mv = j36_sense_mv(p->batsns.median);
	if (p->delta.valid) {
		ma = j36_sense_ma(p->delta.median);
		ma_valid = true;
	}

	j36_ladder_advance(p, online, bat_mv, ma, ma_valid);

	pub.online = online;
	pub.capacity = j36_gauge_percent(p, online, bat_mv, ma, ma_valid, &ocv_mv);
	pub.status = j36_gauge_status(p, online, ma, ma_valid);
	pub.voltage_uv = bat_mv > 0 ? bat_mv * 1000 : -1;
	pub.ocv_uv = ocv_mv > 0 ? ocv_mv * 1000 : -1;
	pub.current_ua = ma * 1000;
	pub.current_valid = ma_valid;
	pub.charger_uv = (online && p->vchr.valid)
			 ? j36_vchr_mv(p->vchr.median) * 1000 : -1;
	pub.cv_uv = j36_charger_cv_uv(p);
	pub.usb_type = p->bc11_type;
	pub.input_limit_ua = p->bc11_limit_ma > 0 ? p->bc11_limit_ma * 1000 : -1;

	j36_charger_arm(p, online);

	/* AFTER the arm, not before: charge_step_ma is read back out of CHR_CON4
	 * inside it, so sampling it above would publish the previous second's
	 * setting -- and on the first poll after a plug, that is the value from
	 * before the cable arrived. */
	pub.charge_step_ua = p->charge_step_ma > 0 ? p->charge_step_ma * 1000 : -1;

	j36_charging_line(p, online, ma, ma_valid, bat_mv);

	/* Only the fields a consumer would redraw for.  A uevent per second per
	 * microamp of ADC noise would wake mixdash sixty times more often than it
	 * has anything to show. */
	spin_lock_irqsave(&p->lock, flags);
	changed = p->pub.status != pub.status ||
		  p->pub.online != pub.online ||
		  p->pub.capacity != pub.capacity ||
		  p->pub.usb_type != pub.usb_type;
	p->pub = pub;
	spin_unlock_irqrestore(&p->lock, flags);

	if (changed) {
		power_supply_changed(p->battery);
		power_supply_changed(p->usb);
	}

again:
	schedule_delayed_work(&p->poll_work, msecs_to_jiffies(p->poll_ms));
}

/* ══════════════════════════════════════════════════════════════════════════
 * THE TWO POWER SUPPLIES
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * The name is `battery', literally, and it is load-bearing twice over: mixdash
 * scans /sys/class/power_supply for a supply whose type is "Battery" and does not
 * care what it is called, but the rootfs's batt_led.service opens
 * /sys/class/power_supply/battery/capacity by path and cares about nothing else.
 * One name answers both, which is why batt_led.service no longer has to be masked.
 *
 * POWER_SUPPLY_PROP_PRESENT is deliberately absent.  Presence is not decidable on
 * this board: VBAT is VSYS, there is no power-path FET, and every candidate bit --
 * BATON, CS_DET -- was checked and decides something else.  A property that would
 * have to guess is worse than one that is not there, because a consumer can see
 * that it is not there.
 */
static enum power_supply_property j36_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	/*
	 * What CHR_CON4's CS_VTH field is really set to, read back rather than
	 * inferred.  It is on the BATTERY and not on the usb supply on purpose,
	 * and the pair is worth reading together: usb/current_max is what the
	 * PORT licensed after BC1.2, battery/constant_charge_current is what the
	 * charger was then programmed to ask for.  They differ whenever the
	 * table has no exact step -- a 500 mA SDP becomes the 450 mA step -- and
	 * a board that charges slower than the wall should allow is that gap.
	 */
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
};

static enum power_supply_property j36_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static int j36_battery_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct j36_pmic *p = power_supply_get_drvdata(psy);
	struct j36_pub pub;
	unsigned long flags;

	spin_lock_irqsave(&p->lock, flags);
	pub = p->pub;
	spin_unlock_irqrestore(&p->lock, flags);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = pub.status;
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (pub.capacity < 0)
			return -ENODATA;
		val->intval = pub.capacity;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if (pub.voltage_uv < 0)
			return -ENODATA;
		val->intval = pub.voltage_uv;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		if (pub.ocv_uv < 0)
			return -ENODATA;
		val->intval = pub.ocv_uv;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		if (!pub.current_valid)
			return -ENODATA;
		val->intval = pub.current_ua;
		return 0;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		if (pub.cv_uv < 0)
			return -ENODATA;
		val->intval = pub.cv_uv;
		return 0;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		if (pub.charge_step_ua < 0)
			return -ENODATA;
		val->intval = pub.charge_step_ua;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = J36_BATT_CAPACITY_MAH * 1000;
		return 0;
	default:
		return -EINVAL;
	}
}

static int j36_usb_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct j36_pmic *p = power_supply_get_drvdata(psy);
	struct j36_pub pub;
	unsigned long flags;

	spin_lock_irqsave(&p->lock, flags);
	pub = p->pub;
	spin_unlock_irqrestore(&p->lock, flags);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		/* CHRDET, not the classifier: a cable is online the moment the
		 * comparator says so, six hundred milliseconds before BC1.2 has
		 * anything to report. */
		val->intval = pub.online;
		return 0;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = pub.usb_type;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		if (pub.input_limit_ua < 0)
			return -ENODATA;
		val->intval = pub.input_limit_ua;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if (pub.charger_uv < 0)
			return -ENODATA;
		val->intval = pub.charger_uv;
		return 0;
	default:
		return -EINVAL;
	}
}

/*
 * The one writable property, and the only place anything outside this driver can
 * move a charger register.  See j36_charger_cv_set for what it refuses and why.
 */
static int j36_battery_set_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    const union power_supply_propval *val)
{
	struct j36_pmic *p = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		return j36_charger_cv_set(p, val->intval);
	default:
		return -EINVAL;
	}
}

static int j36_battery_property_is_writeable(struct power_supply *psy,
					     enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE;
}

static const struct power_supply_desc j36_battery_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = j36_battery_props,
	.num_properties = ARRAY_SIZE(j36_battery_props),
	.get_property = j36_battery_get_property,
	.set_property = j36_battery_set_property,
	.property_is_writeable = j36_battery_property_is_writeable,
};

static const struct power_supply_desc j36_usb_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	/* A bitmask in 6.x, not the old pointer-and-count pair. */
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP) |
		     BIT(POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID),
	.properties = j36_usb_props,
	.num_properties = ARRAY_SIZE(j36_usb_props),
	.get_property = j36_usb_get_property,
};

/* ══════════════════════════════════════════════════════════════════════════
 * POWER OFF (RTC BBPU)
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * Retire one RTC write.  The RTC is on a 32 kHz domain behind pwrap, so a write
 * is not a write until the bridge retires it.
 *
 * CBUSY is initialised SET and a read failure is a hard return rather than a
 * continue: a zeroed variable plus a swallowed error is a CBUSY that reads clear
 * because nobody looked, and "not read" must never mean "retired".
 */
static int j36_rtc_trigger_locked(struct j36_pmic *p)
{
	unsigned int i;
	u32 bbpu;

	if (j36_pwrap_xfer_locked(p, true, J36_RTC_WRTGR, 1, NULL))
		return -EIO;

	for (i = 0; i < J36_RTC_CBUSY_TRIES; ++i) {
		bbpu = J36_RTC_BBPU_CBUSY;
		if (j36_pwrap_xfer_locked(p, false, J36_RTC_BBPU, 0, &bbpu))
			return -EIO;
		if (!(bbpu & J36_RTC_BBPU_CBUSY))
			return 0;
	}
	return -ETIMEDOUT;
}

/* Both halves of the key, each with its own trigger.  A half that does not retire
 * leaves the interface shut, so there is no point continuing to the second word. */
static int j36_rtc_unlock_locked(struct j36_pmic *p)
{
	if (j36_pwrap_xfer_locked(p, true, J36_RTC_PROT, J36_RTC_PROT_KEY1, NULL))
		return -EIO;
	if (j36_rtc_trigger_locked(p))
		return -EIO;
	if (j36_pwrap_xfer_locked(p, true, J36_RTC_PROT, J36_RTC_PROT_KEY2, NULL))
		return -EIO;
	return j36_rtc_trigger_locked(p);
}

/*
 * One attempt, whole, under one lock: a power-off is one atomic group of PMIC
 * transactions, and nothing in it sleeps, so it is safe from a context that may
 * not.  The unlock is INSIDE the attempt and not hoisted out of the loop above
 * it -- an unlock that retired but did not take leaves every following BBPU write
 * discarded in silence, and a retry that does not re-open the write interface is
 * a retry that cannot fix anything.
 *
 * A successful attempt does not return.
 */
static int j36_rtc_bbpu_power_down(struct j36_pmic *p)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&p->lock, flags);
	ret = j36_rtc_unlock_locked(p);
	if (!ret)
		ret = j36_pwrap_xfer_locked(p, true, J36_RTC_BBPU,
					    J36_RTC_BBPU_KEY | J36_RTC_BBPU_AUTO |
					    J36_RTC_BBPU_PWREN, NULL);
	if (!ret)
		ret = j36_rtc_trigger_locked(p);
	spin_unlock_irqrestore(&p->lock, flags);
	return ret;
}

/*
 * Ask eight times over eight hundred milliseconds rather than once over five
 * hundred.  Stock's mt_power_off() is a `while (1)' around this same write and it
 * is a loop for a reason: the RTC retires the word on a 32 kHz domain, and the
 * rail does not fall the instant it does.  One shot was strictly weaker than the
 * thing it was ported from.
 */
#define J36_BBPU_TRIES			8
#define J36_BBPU_SETTLE_MS		100

/*
 * WHY THIS FUNCTION HAS A TAIL AT ALL, AND WHY THE TAIL REBOOTS.
 *
 * There is no power-path FET on this PMIC family -- the fact at the top of this
 * file, arriving one more time in the place it hurts most.  VBAT is VSYS, and
 * with a cable in, VBUS is what holds VSYS up.  Pulling PWRBB low through the RTC
 * cannot take a board down while something else is powering it: the write retires,
 * the PMIC re-powers, and the kernel comes back from mdelay() on a board that is
 * still on.  do_kernel_power_off() then returns to machine_power_off(), which
 * halts the CPU with the rail up -- and because mixdash owns the panel and fbcon
 * was handed over at boot, the last frame it drew stays on the glass.  A warm
 * brick showing a menu.  That is the "shutdown just freezes the device" report,
 * and none of it is a driver bug: it is what the hardware does.
 *
 * Stock answers it in one line -- `if (upmu_is_chr_det()) arch_reset(0, "charger")'
 * -- and reboots into a loader that shows a charging animation.  This board's
 * loader has no such mode, so a restart lands back in MixOS.  That is still the
 * better of the two: a device that comes back up when you ask it to shut down
 * plugged in is annoying and legible, and you can act on it.  A frozen warm panel
 * is neither.  chgreboot=0 on the insmod line restores the halt.
 *
 * The charger is read BEFORE the first write and reported whatever the answer is,
 * because with no console and no panel that line is the only evidence a later
 * reader will have about which of these two stories they are looking at.
 */
static int j36_pmic_power_off(struct sys_off_data *data)
{
	struct j36_pmic *p = data->cb_data;
	unsigned int i;
	int online, ret;

	if (!j36_pwrap_up(p)) {
		dev_emerg(p->dev, "pwrap is down; cannot reach the RTC\n");
		return NOTIFY_DONE;
	}

	online = j36_charger_online(p);
	dev_emerg(p->dev, "power off: charger %s\n",
		  online < 0 ? "unreadable" : online ? "ATTACHED" : "absent");

	for (i = 0; i < J36_BBPU_TRIES; ++i) {
		ret = j36_rtc_bbpu_power_down(p);
		if (ret) {
			dev_emerg(p->dev,
				  "RTC power-off sequence failed (%d) on attempt %u\n",
				  ret, i + 1);
			break;
		}
		mdelay(J36_BBPU_SETTLE_MS);
	}

	if (online > 0 && chgreboot) {
		dev_emerg(p->dev,
			  "a charger is attached and this PMIC cannot latch off with VBUS present -- restarting instead.  Unplug the cable and power off again to shut down.\n");
		/*
		 * emergency_restart() and not machine_restart(): this is a loadable
		 * module, and on ARM machine_restart() has no EXPORT_SYMBOL, so a
		 * call to it is an unresolved symbol at insmod time -- the whole
		 * driver would fail to load and the board would lose its battery
		 * gauge over a line that only runs on the charger.  The exported
		 * wrapper reaches the same place: emergency_restart() dumps the
		 * ring buffer, which puts the dev_emerg lines above in front of
		 * whatever is reading, and then calls machine_emergency_restart(),
		 * which on this architecture IS machine_restart() -- and that walks
		 * the restart-handler chain down to mtk_wdt's SWRST.
		 */
		emergency_restart();
	}

	if (online > 0)
		dev_emerg(p->dev,
			  "power off did not latch, and a charger is attached -- unplug the cable and try again\n");
	else
		dev_emerg(p->dev,
			  "BBPU writes retired but the board is still up\n");
	return NOTIFY_DONE;
}

/* ══════════════════════════════════════════════════════════════════════════
 * PROBE
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * Map a window named by a phandle.  The region is deliberately NOT claimed: these
 * are shared SoC blocks -- the GPIO controller belongs to the input adapter as
 * much as to this driver, the PERI gate and the USB PHY window belong to
 * j36_mt6592_usb_phy -- and a request_mem_region here would make load order
 * decide which driver probes.  The same helper, and the same reasoning, as
 * j36_mt6592_input.
 */
static void __iomem *j36_iomap_phandle(struct device *dev, const char *property)
{
	struct device_node *node;
	struct resource resource;
	void __iomem *base;
	int ret;

	node = of_parse_phandle(dev->of_node, property, 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	ret = of_address_to_resource(node, 0, &resource);
	of_node_put(node);
	if (ret)
		return ERR_PTR(ret);

	base = devm_ioremap(dev, resource.start, resource_size(&resource));
	if (!base)
		return ERR_PTR(-ENOMEM);
	return base;
}

static void j36_pmic_cancel_poll(void *data)
{
	struct j36_pmic *p = data;

	cancel_delayed_work_sync(&p->poll_work);
}

/* Withdraw the door before the devm ioremaps behind it go away.  A WiFi bring-up
 * in flight at this moment gets -ENODEV from its next rail write and fails the
 * way it would have if this module had never loaded. */
static void j36_pmic_unpublish(void *data)
{
	if (READ_ONCE(j36_pmic_singleton) == data)
		WRITE_ONCE(j36_pmic_singleton, NULL);
}

static int j36_pmic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config cfg = { };
	struct j36_pmic *p;
	void __iomem *base;
	u32 value;
	int ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->dev = dev;
	p->vbus_pin = -1;
	p->online = -1;
	p->hw_ocv_mv = -1;
	p->soc_dep = -1;
	p->soc_car_base = -1;
	p->bc11_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	spin_lock_init(&p->lock);
	j36_ring_forget(&p->batsns);
	j36_ring_forget(&p->vchr);
	j36_ring_forget(&p->delta);
	p->pub.status = POWER_SUPPLY_STATUS_UNKNOWN;
	p->pub.usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	p->pub.capacity = -1;
	p->pub.voltage_uv = -1;
	p->pub.ocv_uv = -1;
	p->pub.charger_uv = -1;
	p->pub.cv_uv = -1;
	p->pub.input_limit_ua = -1;
	p->pub.charge_step_ua = -1;
	p->charge_step_ma = -1;

	/* The only mandatory one.  Everything this driver does goes through it. */
	base = j36_iomap_phandle(dev, "j36,pwrap-controller");
	if (IS_ERR(base))
		return dev_err_probe(dev, PTR_ERR(base), "map PWRAP\n");
	p->pwrap = base;

	/*
	 * The optional three, and what each one costs to be without.
	 *
	 * pericfg + usb-phy are a PAIR: BC1.2 needs the mux bit, the mux bit is
	 * behind the clock gate, and touching a gated MediaTek peripheral stalls
	 * the bus until the watchdog resets the board.  Either one missing means
	 * neither is used and the charger runs at the conservative default.
	 *
	 * gpio + drvvbus-pad are the VBUS interlock.  Without them a board in USB
	 * host mode reads its own boost as a charger.
	 */
	base = j36_iomap_phandle(dev, "j36,pericfg-controller");
	if (!IS_ERR(base))
		p->pericfg = base;
	base = j36_iomap_phandle(dev, "j36,usb-phy-controller");
	if (!IS_ERR(base))
		p->usbphy = base;
	if (!p->pericfg || !p->usbphy) {
		p->pericfg = NULL;
		p->usbphy = NULL;
		dev_info(dev,
			 "no pericfg/usb-phy pair: BC1.2 is off and the charger will use the %u mA default\n",
			 j36_cs_vth_ma[J36_CHR_CON4_CS_VTH_DEFAULT]);
	}

	base = j36_iomap_phandle(dev, "j36,gpio-controller");
	if (!IS_ERR(base))
		p->gpio = base;
	if (p->gpio && !of_property_read_u32(dev->of_node, "j36,drvvbus-pad", &value)) {
		if (value > J36_GPIO_PIN_MAX)
			dev_warn(dev, "j36,drvvbus-pad %u is out of range; the VBUS interlock is off\n",
				 value);
		else
			p->vbus_pin = value;
	}
	if (p->vbus_pin < 0)
		dev_info(dev,
			 "no DRVVBUS pad: if this port ever sources 5 V, CHRDET will read it as a charger\n");

	p->poll_ms = J36_POLL_MS_DEFAULT;
	if (!of_property_read_u32(dev->of_node, "poll-interval-ms", &value))
		p->poll_ms = value;
	if (poll_ms)
		p->poll_ms = poll_ms;
	p->poll_ms = clamp(p->poll_ms, (unsigned int)J36_POLL_MS_MIN,
			   (unsigned int)J36_POLL_MS_MAX);

	cfg.drv_data = p;
	p->battery = devm_power_supply_register(dev, &j36_battery_desc, &cfg);
	if (IS_ERR(p->battery))
		return dev_err_probe(dev, PTR_ERR(p->battery),
				     "register the battery supply\n");
	p->usb = devm_power_supply_register(dev, &j36_usb_desc, &cfg);
	if (IS_ERR(p->usb))
		return dev_err_probe(dev, PTR_ERR(p->usb),
				     "register the USB supply\n");

	if (poweroff) {
		ret = devm_register_sys_off_handler(dev, SYS_OFF_MODE_POWER_OFF,
						    SYS_OFF_PRIO_DEFAULT,
						    j36_pmic_power_off, p);
		if (ret)
			return dev_err_probe(dev, ret,
					     "register the power-off handler\n");
	}

	INIT_DELAYED_WORK(&p->poll_work, j36_pmic_poll);
	ret = devm_add_action_or_reset(dev, j36_pmic_cancel_poll, p);
	if (ret)
		return ret;

	/* Open the door only now: everything a caller could reach through it --
	 * the pwrap mapping and the lock that serialises it -- is up. */
	WRITE_ONCE(j36_pmic_singleton, p);
	ret = devm_add_action_or_reset(dev, j36_pmic_unpublish, p);
	if (ret)
		return ret;

	dev_info(dev, "MT6592 PMIC: %ums poll, charger %s, BC1.2 %s%s\n",
		 p->poll_ms, charge ? "on" : "OFF (read-only gauge)",
		 (bc11 && p->usbphy) ? "on" : "off",
		 p->vbus_pin >= 0 ? ", VBUS interlock on" : "");

	/* Not scheduled at zero: the LK has just been through its own charging
	 * path and pwrap wants a moment, and nothing is waiting on the first
	 * reading closely enough for a hundred milliseconds to matter. */
	schedule_delayed_work(&p->poll_work, msecs_to_jiffies(100));
	return 0;
}

static const struct of_device_id j36_pmic_of_match[] = {
	{ .compatible = "j36,j36-ultra-pmic" },
	{ }
};
MODULE_DEVICE_TABLE(of, j36_pmic_of_match);

static struct platform_driver j36_pmic_driver = {
	.probe = j36_pmic_probe,
	.driver = {
		.name = "j36-mt6592-pmic",
		.of_match_table = j36_pmic_of_match,
	},
};
module_platform_driver(j36_pmic_driver);

MODULE_DESCRIPTION("J36 Ultra MT6592 PMIC battery, charger and power-off");
MODULE_AUTHOR("MixOS / PowerEngine integration");
MODULE_LICENSE("GPL");
