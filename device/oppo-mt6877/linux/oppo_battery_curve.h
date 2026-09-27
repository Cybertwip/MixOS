/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * OPPO MT6877 battery voltage-to-capacity curve.
 *
 * The fuel gauge is the MT6359 FGADC, which has no mainline driver yet. Until
 * it lands, capacity is estimated from the open-circuit voltage the same way
 * the J36 driver does: a monotonically decreasing table walked at 1 Hz, with
 * the charge/full/discharge state from the charger drivers. The curve below
 * is a standard single-cell Li-ion OCV table; per-device calibration (the
 * 20181 pack's exact chemistry) is a board/ addition, not an edit here.
 */
#ifndef OPPO_BATTERY_CURVE_H
#define OPPO_BATTERY_CURVE_H

/* Millivolts, index = percent. 4200 mV full, 3400 mV empty. */
static const unsigned short oppo_batt_mv[101] = {
	3400, 3416, 3432, 3448, 3464, 3480, 3496, 3512, 3528, 3544,
	3560, 3572, 3584, 3596, 3608, 3620, 3632, 3644, 3656, 3668,
	3680, 3690, 3700, 3710, 3720, 3730, 3740, 3750, 3760, 3770,
	3780, 3788, 3796, 3804, 3812, 3820, 3828, 3836, 3844, 3852,
	3860, 3868, 3876, 3884, 3892, 3900, 3908, 3916, 3924, 3932,
	3940, 3948, 3956, 3964, 3972, 3980, 3988, 3996, 4004, 4012,
	4020, 4026, 4032, 4038, 4044, 4050, 4056, 4062, 4068, 4074,
	4080, 4086, 4092, 4098, 4104, 4110, 4116, 4122, 4128, 4134,
	4140, 4146, 4152, 4158, 4164, 4170, 4176, 4182, 4188, 4194,
	4200, 4200, 4200, 4200, 4200, 4200, 4200, 4200, 4200, 4200,
	4200
};

static inline int oppo_batt_pct(unsigned int mv)
{
	int lo = 0, hi = 100;

	if (mv >= oppo_batt_mv[100])
		return 100;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;

		if (oppo_batt_mv[mid] <= mv)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

#endif /* OPPO_BATTERY_CURVE_H */
