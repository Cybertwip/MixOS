#ifndef MT6592_BATTERY_CURVE_H
#define MT6592_BATTERY_CURVE_H

/*
 * VBAT -> state-of-charge, in one place because two very different callers need
 * the same answer and must not drift apart:
 *
 *   - mt6592_pmic.c, in the running OS, for the shell's battery indicator;
 *   - stage1.c, before there is an OS at all, for the percentage printed on the
 *     warm boot the power key triggers out of the charge park.
 *
 * stage1 links against almost nothing (four headers, no libc, no mt6592_pmic.c),
 * so this is header-only and static inline rather than a .c file -- it costs no
 * build-system change and cannot be linked into one image and not the other.
 *
 *
 * ── WHY THE HAND-DRAWN CURVE THAT USED TO LIVE HERE WAS ALWAYS GOING TO SAY
 *    "100%" ──
 *
 * It was thirteen points fitted to TERMINAL voltages read WHILE CHARGING, and
 * its own top endpoint was documented as "this board's CV setpoint, not 4.2 V"
 * -- 4170 mV mapped to 100%. There is no power-path FET on this PMIC family, so
 * BATSNS watches the node the charger regulates. Plug the board in and the node
 * goes to the setpoint; the setpoint is the top of the curve; the curve answers
 * 100%. It answered 100% for a pack the board's own ISENSE/BATSNS pair was at
 * that moment measuring 926 mA flowing INTO. A cell taking most of an amp is not
 * full, so the number being displayed was the charger's, not the battery's.
 *
 * Reading a terminal voltage taken under charge through a rest curve is the bug.
 * Not the endpoints, not the shape: the input. Two things have to happen first.
 *
 *
 * ── WHAT REPLACES IT: THIS DEVICE'S OWN CHARACTERISATION ──
 *
 * Both tables below were lifted out of the stock kernel image rather than drawn
 * by hand, and they are this pack's, measured by whoever built it:
 *
 *   k_ocv    battery_profile_t[0..3] @ 0xc0b8294c / 0xc0b82bb4 / 0xc0b82e1c /
 *            0xc0b83084, four 77-entry {percentage_consumed, OCV mV} tables, one
 *            per temperature bucket. They are the SAME CURVE -- a diff across all
 *            four finds four entries differing by exactly 1 mV -- so this keeps
 *            one copy and does not select on temperature. There is no trustworthy
 *            battery thermistor reading on this port to select with anyway, and
 *            inventing one to index four identical tables would be theatre.
 *
 *   k_rbat   r_profile @ 0xc0b832ec (and an identical copy at 0xc0b83554),
 *            77 entries of {internal resistance mOhm, OCV mV} on the same voltage
 *            axis. 125 mOhm at the top, ~150 across the plateau, 233 at 3500 mV.
 *
 * The units of the resistance column are not a guess. Stock's own OAM loop
 * recovers a current from a voltage difference at 0xc061176c with
 * `10000 * (v_ocv - v_bat) / R', and 10000 * (mV / mOhm) is 10000 * amps, i.e.
 * tenths of a milliamp -- which is exactly the unit every other current in that
 * file carries. mV over mOhm can only be amps, so the column is milliohms.
 *
 * The interpolators below are stock's, instruction for instruction:
 *
 *   mt6592_battery_percent_from_ocv   fgauge_read_capacity_by_v  @ 0xc06101b8
 *   mt6592_battery_r_bat_mohm         fgauge_read_r_bat_by_v     @ 0xc06106a8
 *   mt6592_battery_ocv_mv             mtk_imp_tracking           @ 0xc061126c
 *
 * including the endpoint behaviour (above the top of the table is full, below
 * the bottom is empty, the resistance saturates at both ends rather than
 * extrapolating) and the `(x/1000 + 5)/10' rounding, which is a round-half-up
 * that is half an LSB asymmetric for negative arguments. It is left asymmetric
 * because half a millivolt does not reach the answer and because a rewritten
 * rounding rule is a second thing to have to trust.
 *
 *
 * ── AND THE CORRECTION THAT MAKES THE INPUT MEAN SOMETHING ──
 *
 * A cell under charge sits at OCV + I*R_internal; under load it sits at
 * OCV - I*R_internal. The tables are indexed by OCV, so the terminal voltage has
 * to be walked back to one before it is looked up. That is mt6592_battery_ocv_mv,
 * and it is stock's mtk_imp_tracking: five rounds of "read R at the current best
 * guess of the OCV, recompute the drop, correct again", because R itself is a
 * function of the voltage being solved for.
 *
 * Stock adds two more resistances to the pack's own -- R_FG_VALUE at cust +0x16c
 * and FG_METER_RESISTANCE at cust +0x110. BOTH ARE ZERO ON THIS BOARD: the
 * initialiser sets r2 = 0 at 0xc060fbf0 and stores it to +0x110 at 0xc060fc8c and
 * to +0x16c at 0xc060fcdc. They are named and kept at zero here rather than
 * dropped, so that a board that does populate them has somewhere to say so.
 *
 * The 68 mOhm CUST_R_SENSE at cust +0x140 is deliberately NOT one of them. That
 * shunt is what the ISENSE/BATSNS pair measures the charge current ACROSS, and
 * BATSNS is on the cell side of it -- its drop is already excluded from the
 * reading, and adding it here would count it twice.
 *
 * At the numbers this board actually reported -- 4175 mV terminal, 926 mA in --
 * the loop settles on 157 mOhm, takes 145 mV off, and reads 4030 mV in the table
 * for 97% instead of 100%. The five rounds earn their keep even in that one case:
 * the resistance AT 4175 mV is 125 mOhm, and the corrected voltage lands in a
 * steeper part of the r-table, so a single pass would have under-corrected by
 * a quarter.
 *
 * Three points is a small change and the top of the curve is where it is
 * smallest, which is exactly where this board happened to be sitting. The same
 * 926 mA is worth THIRTY-NINE points at 4000 mV terminal (93% -> 54%) and
 * thirty-three at 3800 (42% -> 9%). That is the size of the bug at any charge
 * state that is not nearly full, and it is why the number could only ever go up
 * while a cable was in.
 */

enum {
    /* Both tables, 77 rows each, on the same voltage axis for the first 66. */
    MT6592_BATTERY_PROFILE_ENTRIES = 77,

    /* mtk_imp_tracking's `recursion_time', 5 at its only call site (0xc0611884
     * loads r2 = 5 before the branch at 0xc0611888). */
    MT6592_BATTERY_IMP_ROUNDS = 5,

    /* cust +0x16c and cust +0x110. Zero on this board -- see the note above. */
    MT6592_BATTERY_R_FG_MOHM = 0,
    MT6592_BATTERY_R_METER_MOHM = 0
};

/*
 * fgauge_read_r_bat_by_v @ 0xc06106a8: the pack's internal resistance in
 * milliohms at an open-circuit voltage. Saturates at both ends -- above the top
 * row it returns the top row's resistance, below the bottom row the bottom
 * row's -- which is what the two `ldr r0,[r0]' / `ldr r0,[r0,#0x260]' exits do.
 */
static inline int mt6592_battery_r_bat_mohm(int mv)
{
    /* {resistance mOhm, OCV mV}, r_profile @ 0xc0b832ec. */
    static const short k_rbat[MT6592_BATTERY_PROFILE_ENTRIES][2] = {
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
    const int n = MT6592_BATTERY_PROFILE_ENTRIES;
    int i;

    if (mv > k_rbat[0][1]) return k_rbat[0][0];
    if (mv < k_rbat[n - 1][1]) return k_rbat[n - 1][0];

    for (i = 0; i < n - 1; ++i) {
        const int v_hi = k_rbat[i][1];
        const int v_lo = k_rbat[i + 1][1];
        if (mv > v_hi || mv < v_lo) continue;
        if (v_hi == v_lo) return k_rbat[i][0];
        return k_rbat[i][0] +
               (k_rbat[i + 1][0] - k_rbat[i][0]) * (v_hi - mv) / (v_hi - v_lo);
    }
    return k_rbat[n - 1][0];
}

/*
 * mtk_imp_tracking @ 0xc061126c: terminal volts -> open-circuit volts.
 *
 * @p charge_ma  signed milliamps at the cell: POSITIVE INTO IT (charging),
 *               negative out of it (load), zero if it is not known. Zero is not
 *               "no current", it is "no correction is defensible", and the
 *               terminal voltage comes back untouched.
 *
 * Stock carries current in tenths of a milliamp and signs it so that charging is
 * negative, because it then does `v + comp' unconditionally. Both conventions are
 * preserved inside; the argument uses the one every other function on this port
 * uses, so callers do not have to remember two.
 */
static inline int mt6592_battery_ocv_mv(int terminal_mv, int charge_ma)
{
    const int i_tenth_ma = -charge_ma * 10;
    int v = terminal_mv;
    int r;
    int k;

    if (charge_ma == 0 || terminal_mv <= 0) return terminal_mv;

    for (k = 0; k < MT6592_BATTERY_IMP_ROUNDS; ++k) {
        r = mt6592_battery_r_bat_mohm(v) + MT6592_BATTERY_R_FG_MOHM;
        v = terminal_mv + ((i_tenth_ma * r) / 1000 + 5) / 10;
    }

    r = mt6592_battery_r_bat_mohm(v) + MT6592_BATTERY_R_FG_MOHM +
        MT6592_BATTERY_R_METER_MOHM;
    return terminal_mv + ((i_tenth_ma * r) / 1000 + 5) / 10;
}

/*
 * fgauge_read_capacity_by_v @ 0xc06101b8: an OPEN-CIRCUIT voltage to percent
 * REMAINING. The table stores percentage CONSUMED, which is why the interpolated
 * value is subtracted from 100 on the way out -- stock's `rsb r0, r0, #100'.
 */
static inline int mt6592_battery_percent_from_ocv(int ocv_mv)
{
    /* {percentage consumed, OCV mV}, battery_profile_t2 @ 0xc0b82e1c and its
     * three siblings. The last eleven rows are the vendor's own padding to a
     * fixed 77 and are kept so the table is byte-for-byte what the device
     * shipped with; they are flat at 100%/3500 mV and interpolate harmlessly. */
    static const short k_ocv[MT6592_BATTERY_PROFILE_ENTRIES][2] = {
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
    const int n = MT6592_BATTERY_PROFILE_ENTRIES;
    int i;

    if (ocv_mv > k_ocv[0][1]) return 100;
    if (ocv_mv < k_ocv[n - 1][1]) return 0;

    for (i = 0; i < n - 1; ++i) {
        const int v_hi = k_ocv[i][1];
        const int v_lo = k_ocv[i + 1][1];
        int consumed;

        if (ocv_mv > v_hi || ocv_mv < v_lo) continue;
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
static inline int mt6592_battery_percent_from_mv(int mv, int charge_ma)
{
    return mt6592_battery_percent_from_ocv(mt6592_battery_ocv_mv(mv, charge_ma));
}

#endif /* MT6592_BATTERY_CURVE_H */
