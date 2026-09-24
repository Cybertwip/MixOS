/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* Shared by the J36 Linux driver and the standalone LK/flash firmware.
 *
 * With no cell, the preloader's charger output is the system rail. Leaving
 * its hardware-charging mode, or rewriting charge current or charge voltage
 * before the splash, has latched this PMIC off. Disable the charger watchdog
 * in this explicitly batteryless mode: no code can service it while Linux
 * decompresses and boots to the initramfs PMIC module. A final kick in LK
 * only buys four seconds. Keep the brownout limit at its lowest setting.
 * USBDL, the charge voltage, and the charge current are left untouched.
 */
#ifndef J36_EXTERNAL_POWER_H
#define J36_EXTERNAL_POWER_H

static inline int j36_ep_rmw(
	void *ctx,
	int (*read_reg)(void *, unsigned int, unsigned int *),
	int (*write_reg)(void *, unsigned int, unsigned int),
	unsigned int reg, unsigned int clear, unsigned int set)
{
	unsigned int cur, want;
	int ret;

	ret = read_reg(ctx, reg, &cur);
	if (ret)
		return ret;
	want = (cur & ~clear) | set;
	if (want == cur)
		return 0;
	return write_reg(ctx, reg, want);
}

static inline int j36_external_power_hold(
	void *ctx,
	int (*read_reg)(void *, unsigned int, unsigned int *),
	int (*write_reg)(void *, unsigned int, unsigned int))
{
	unsigned int timer, status, uvlo;
	int ret;

	/* Mask its interrupt, stop the timer, then clear any latched expiry.
	 * Do not use the battery charging shutdown path: it also clears CHR_EN.
	 * Preserve TD; clearing WR avoids accidentally stroking a reset command. */
	/* OUT (bit 2) is read-only status; never echo it back as a one. */
	ret = j36_ep_rmw(ctx, read_reg, write_reg, 0x001eu, 5u, 0);
	if (ret)
		return ret;
	ret = j36_ep_rmw(ctx, read_reg, write_reg, 0x001au,
			 (1u << 4) | (1u << 8), 0);
	if (ret)
		return ret;
	/* FLAG_WR is a strobe: write even when it reads back set. */
	ret = read_reg(ctx, 0x001eu, &status);
	if (ret)
		return ret;
	ret = write_reg(ctx, 0x001eu, (status & ~5u) | 2u);
	if (ret)
		return ret;
	ret = read_reg(ctx, 0x001au, &timer);
	if (ret)
		return ret;
	if (timer & (1u << 4))
		return -1;
	ret = read_reg(ctx, 0x001eu, &status);
	if (ret)
		return ret;
	if (status & (1u | (1u << 2)))
		return -1;

	/* UVLO bits 1:0 only. Bits 2 and 3 are USBDL and must stay as found. */
	ret = j36_ep_rmw(ctx, read_reg, write_reg, 0x0020u, 0x3u, 0);
	if (ret)
		return ret;
	ret = read_reg(ctx, 0x0020u, &uvlo);
	if (ret)
		return ret;
	return (uvlo & 0x3u) ? -1 : 0;
}
#endif
