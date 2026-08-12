/* SPDX-License-Identifier: GPL-2.0 */
/*
 * J36 Ultra battery characterisation: VBAT -> state of charge.
 *
 * Ported from PowerEngine/OS/MVII's mt6592_battery_curve.h, which is itself a
 * transcription of the tables and the three interpolators out of the stock
 * Android kernel image this device shipped with.  The MVII header is header-only
 * because it is linked into a bootloader stage that has no libc; here that no
 * longer matters, but it is kept header-only anyway so the two files stay
 * diffable line for line.  If the numbers below ever have to be re-derived, the
 * MVII header carries the full account of where each one came from; this comment
 * keeps only what a reader of the Linux driver needs.
 *
 *
 * ── WHY A TERMINAL VOLTAGE IS NOT A PERCENTAGE ──
 *
 * There is no power-path FET on this PMIC family, so BATSNS watches the node the
 * charger regulates.  Plug the board in and that node goes to the CV setpoint.
 * Read it through a rest curve and the answer is 100% for a pack that is at that
 * moment taking the better part of an amp -- which is what the hand-drawn curve
 * this replaced always said, because its own top endpoint was the setpoint.
 *
 * A cell under charge sits at OCV + I*R_internal and under load at
 * OCV - I*R_internal, and both tables here are indexed by OCV, so the terminal
 * voltage has to be walked back to one first.  That is j36_battery_ocv_mv(), and
 * it iterates because R is itself a function of the voltage being solved for.
 * At 4175 mV terminal and 926 mA in, the loop settles on 157 mOhm, takes 145 mV
 * off, and reads 97% instead of 100%.  Three points sounds like nothing, and at
 * the top of the curve it is: the same 926 mA is worth thirty-nine points at
 * 4000 mV and thirty-three at 3800.  That is the size of the error anywhere the
 * pack is not nearly full, and it is why the old number could only ever go up
 * while a cable was in.
 *
 *
 * ── WHERE THE TABLES CAME FROM ──
 *
 *   k_ocv    battery_profile_t[0..3] @ 0xc0b8294c and its three siblings, four
 *            77-entry {percentage_consumed, OCV mV} tables, one per temperature
 *            bucket.  A diff across all four finds four entries differing by
 *            exactly 1 mV -- they are the same curve -- so one copy is kept and
 *            temperature is not selected on.  There is no trustworthy battery
 *            thermistor on this port to select with, and inventing one to index
 *            four identical tables would be theatre.
 *
 *   k_rbat   r_profile @ 0xc0b832ec, 77 entries of {internal resistance mOhm,
 *            OCV mV} on the same voltage axis.  The unit is not a guess: stock's
 *            own loop recovers a current as 10000 * (v_ocv - v_bat) / R, and
 *            10000 * (mV / mOhm) is tenths of a milliamp, which is the unit every
 *            other current in that file carries.  mV over mOhm can only be amps.
 *
 * The interpolators are stock's instruction for instruction, including the
 * endpoint behaviour -- above the top row is full, below the bottom row is empty,
 * the resistance saturates at both ends rather than extrapolating -- and the
 * (x/1000 + 5)/10 rounding, which is a round-half-up that is half an LSB
 * asymmetric for negative arguments.  It is left asymmetric because half a
 * millivolt does not reach the answer and because a rewritten rounding rule is a
 * second thing to have to trust.
 */

#ifndef J36_BATTERY_CURVE_H
#define J36_BATTERY_CURVE_H

/* Both tables, 77 rows each, on the same voltage axis for the first 66. */
#define J36_BATTERY_PROFILE_ENTRIES	77

/* mtk_imp_tracking's `recursion_time', 5 at its only call site. */
#define J36_BATTERY_IMP_ROUNDS		5

/*
 * cust +0x16c and cust +0x110, the two resistances stock adds to the pack's own.
 * BOTH ARE ZERO ON THIS BOARD -- the initialiser stores zero to each -- and they
 * are named and kept at zero rather than dropped, so a board that does populate
 * them has somewhere to say so.
 *
 * The 68 mOhm sense shunt is deliberately not one of them: that is what the
 * ISENSE/BATSNS pair measures the charge current ACROSS, and BATSNS is on the
 * cell side of it, so its drop is already excluded and adding it here would
 * count it twice.
 */
#define J36_BATTERY_R_FG_MOHM		0
#define J36_BATTERY_R_METER_MOHM	0

/*
 * fgauge_read_r_bat_by_v: the pack's internal resistance in milliohms at an
 * open-circuit voltage.
 */
static inline int j36_battery_r_bat_mohm(int mv)
{
	/* {resistance mOhm, OCV mV}, r_profile @ 0xc0b832ec. */
	static const short k_rbat[J36_BATTERY_PROFILE_ENTRIES][2] = {
		{ 125, 4190}, { 153, 4064}, { 158, 4032}, { 148, 4014}, { 145, 4007},
		{ 143, 3997}, { 140, 3993}, { 140, 3987}, { 138, 3982}, { 135, 3974},
		{ 138, 3970}, { 138, 3962}, { 138, 3958}, { 140, 3951}, { 145, 3949},
		{ 145, 3943}, { 148, 3939}, { 153, 3935}, { 153, 3930}, { 155, 3928},
		{ 158, 3922}, { 163, 3918}, { 163, 3909}, { 168, 3905}, { 170, 3895},
		{ 173, 3890}, { 173, 3880}, { 173, 3876}, { 170, 3866}, { 170, 3861},
		{ 165, 3850}, { 155, 3840}, { 150, 3837}, { 145, 3828}, { 148, 3825},
		{ 145, 3815}, { 145, 3811}, { 145, 3803}, { 143, 3800}, { 143, 3794},
		{ 145, 3791}, { 150, 3785}, { 148, 3783}, { 148, 3777}, { 150, 3772},
		{ 153, 3768}, { 153, 3763}, { 155, 3760}, { 155, 3753}, { 153, 3750},
		{ 155, 3743}, { 155, 3739}, { 153, 3732}, { 153, 3729}, { 145, 3721},
		{ 148, 3715}, { 150, 3706}, { 150, 3701}, { 150, 3690}, { 153, 3669},
		{ 153, 3658}, { 155, 3636}, { 158, 3625}, { 173, 3604}, { 188, 3591},
		{ 198, 3550}, { 233, 3500}, { 228, 3470}, { 205, 3460}, { 195, 3455},
		{ 188, 3450}, { 183, 3445}, { 180, 3444}, { 180, 3443}, { 175, 3442},
		{ 178, 3441}, { 175, 3440},
	};
	const int n = J36_BATTERY_PROFILE_ENTRIES;
	int i;

	if (mv > k_rbat[0][1])
		return k_rbat[0][0];
	if (mv < k_rbat[n - 1][1])
		return k_rbat[n - 1][0];

	for (i = 0; i < n - 1; ++i) {
		const int v_hi = k_rbat[i][1];
		const int v_lo = k_rbat[i + 1][1];

		if (mv > v_hi || mv < v_lo)
			continue;
		if (v_hi == v_lo)
			return k_rbat[i][0];
		return k_rbat[i][0] + (k_rbat[i + 1][0] - k_rbat[i][0]) *
				      (v_hi - mv) / (v_hi - v_lo);
	}
	return k_rbat[n - 1][0];
}

/*
 * mtk_imp_tracking: terminal volts -> open-circuit volts.
 *
 * @charge_ma  signed milliamps at the cell: POSITIVE INTO IT (charging), negative
 *             out of it (load), zero if it is not known.  Zero is not "no
 *             current", it is "no correction is defensible", and the terminal
 *             voltage comes back untouched.
 *
 * Stock carries current in tenths of a milliamp and signs it so that charging is
 * negative, because it then adds unconditionally.  Both conventions are preserved
 * inside; the argument uses the one every other function on this port uses, so
 * callers do not have to remember two.
 */
static inline int j36_battery_ocv_mv(int terminal_mv, int charge_ma)
{
	const int i_tenth_ma = -charge_ma * 10;
	int v = terminal_mv;
	int r;
	int k;

	if (charge_ma == 0 || terminal_mv <= 0)
		return terminal_mv;

	for (k = 0; k < J36_BATTERY_IMP_ROUNDS; ++k) {
		r = j36_battery_r_bat_mohm(v) + J36_BATTERY_R_FG_MOHM;
		v = terminal_mv + ((i_tenth_ma * r) / 1000 + 5) / 10;
	}

	r = j36_battery_r_bat_mohm(v) + J36_BATTERY_R_FG_MOHM +
	    J36_BATTERY_R_METER_MOHM;
	return terminal_mv + ((i_tenth_ma * r) / 1000 + 5) / 10;
}

/*
 * fgauge_read_capacity_by_v: an OPEN-CIRCUIT voltage to percent REMAINING.  The
 * table stores percentage CONSUMED, which is why the interpolated value is
 * subtracted from 100 on the way out.
 */
static inline int j36_battery_percent_from_ocv(int ocv_mv)
{
	/*
	 * {percentage consumed, OCV mV}, battery_profile_t2 @ 0xc0b82e1c and its
	 * three siblings.  The last eleven rows are the vendor's own padding to a
	 * fixed 77 and are kept so the table is byte for byte what the device
	 * shipped with; they are flat at 100%/3500 mV and interpolate harmlessly.
	 */
	static const short k_ocv[J36_BATTERY_PROFILE_ENTRIES][2] = {
		{   0, 4190}, {   1, 4064}, {   3, 4032}, {   5, 4014}, {   6, 4007},
		{   8, 3997}, {   9, 3993}, {  11, 3987}, {  12, 3982}, {  14, 3974},
		{  15, 3970}, {  17, 3962}, {  18, 3958}, {  20, 3951}, {  21, 3949},
		{  23, 3943}, {  25, 3939}, {  26, 3935}, {  28, 3930}, {  29, 3928},
		{  31, 3922}, {  32, 3918}, {  34, 3909}, {  35, 3905}, {  37, 3895},
		{  38, 3890}, {  40, 3880}, {  41, 3876}, {  43, 3866}, {  44, 3861},
		{  46, 3850}, {  48, 3840}, {  49, 3837}, {  51, 3828}, {  52, 3825},
		{  54, 3815}, {  55, 3811}, {  57, 3803}, {  58, 3800}, {  60, 3794},
		{  61, 3791}, {  63, 3785}, {  64, 3783}, {  66, 3777}, {  68, 3772},
		{  69, 3768}, {  71, 3763}, {  72, 3760}, {  74, 3753}, {  75, 3750},
		{  77, 3743}, {  78, 3739}, {  80, 3732}, {  81, 3729}, {  83, 3721},
		{  84, 3715}, {  86, 3706}, {  87, 3701}, {  89, 3690}, {  91, 3669},
		{  92, 3658}, {  94, 3636}, {  95, 3625}, {  97, 3604}, {  98, 3591},
		{ 100, 3550}, { 100, 3500}, { 100, 3500}, { 100, 3500}, { 100, 3500},
		{ 100, 3500}, { 100, 3500}, { 100, 3500}, { 100, 3500}, { 100, 3500},
		{ 100, 3500}, { 100, 3500},
	};
	const int n = J36_BATTERY_PROFILE_ENTRIES;
	int i;

	if (ocv_mv > k_ocv[0][1])
		return 100;
	if (ocv_mv < k_ocv[n - 1][1])
		return 0;

	for (i = 0; i < n - 1; ++i) {
		const int v_hi = k_ocv[i][1];
		const int v_lo = k_ocv[i + 1][1];
		int consumed;

		if (ocv_mv > v_hi || ocv_mv < v_lo)
			continue;
		consumed = (v_hi == v_lo)
			   ? k_ocv[i][0]
			   : k_ocv[i][0] + (k_ocv[i + 1][0] - k_ocv[i][0]) *
					   (v_hi - ocv_mv) / (v_hi - v_lo);
		return 100 - consumed;
	}
	return 100;
}

/*
 * The pair, which is what callers want: a MEASURED TERMINAL voltage and the
 * current that was flowing when it was measured, to a percentage.
 *
 * Pass charge_ma = 0 only when the current genuinely is not known -- it disables
 * the correction, and on a plugged-in board that is the old bug back again.
 */
static inline int j36_battery_percent_from_mv(int mv, int charge_ma)
{
	return j36_battery_percent_from_ocv(j36_battery_ocv_mv(mv, charge_ma));
}

#endif /* J36_BATTERY_CURVE_H */
