// SPDX-License-Identifier: GPL-2.0
/*
 * J36 Ultra MT6592 CONSYS: rails, MTCMOS, clock and the chip-ID probe.
 *
 * A mirror of PowerEngine/OS/MVII's mt6592_wifi_sdio.c, whose filename is a
 * historical accident -- there is no SDIO here.  The sequence follows the stock
 * J36 WMT and MTCMOS sources: MT6323 VCN rails, the CONSYS MTCMOS domain, the
 * INFRA_CONNMCU clock, then the chip-ID probe, with the EMI aperture pointed at
 * real DRAM before any of it.
 *
 * Nothing in here talks to the connectivity MCU.  It only makes it possible to.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>

#include "j36_mt6592_pmic.h"
#include "j36_mt6592_wifi.h"

static int j36_wifi_pmic_update(struct j36_wifi *w, u32 adr, u32 clr, u32 set)
{
	int ret = j36_pmic_pwrap_update(adr, clr, set);

	if (ret < 0)
		dev_err(w->dev, "PMIC register 0x%04x: %d\n", adr, ret);
	/* j36_pmic_pwrap_update() returns 1 for "changed" and 0 for "already
	 * that value", and neither is news here. */
	return ret < 0 ? ret : 0;
}

/*
 * VCN_1V8 and VCN28, which are the rails the subsystem needs before it is
 * powered at all.
 *
 * VCN_1V8 is a fixed digital LDO -- hwPowerOn(VCN_1V8, VOL_DEFAULT) selects no
 * voltage -- so this is exactly the two bits stock writes: lp_mode_set(0) then
 * enable (mtk_wcn_consys_hw_reg_ctrl(), mtk_wcn_consys_hw.c:215).
 *
 * VCN28 follows co_clock_flag, and it is NOT symmetric:
 *
 *	if (co_clock_en)  upmu_set_vcn28_on_ctrl(0);		<- and nothing else
 *	else		{ upmu_set_vcn28_on_ctrl(1);
 *			  hwPowerOn(MT6323_POWER_LDO_VCN28, VOL_DEFAULT); }
 *
 * (mtk_wcn_consys_hw.c:223-236.)  In co-clock mode the 2.8 V rail is left in SW
 * control and left OFF here; it is FM and GPS that later raise it.  This board's
 * WMT_SOC.cfg says co_clock_flag=1, so the co-clock branch is the one it takes,
 * and WLAN does not need VCN28 on either path.
 *
 * MVII took the other branch for a while -- ON_CTRL=1 plus the enable bit -- which
 * put VCN28 under hardware control on a board whose connectivity clock is shared.
 */
static int j36_wifi_rails_on(struct j36_wifi *w)
{
	int ret;

	ret = j36_wifi_pmic_update(w, J36_PMIC_DIGLDO_CON11,
				   J36_PMIC_VCN18_LP_MODE, J36_PMIC_VCN18_ENABLE);
	if (ret)
		return ret;
	usleep_range(150, 300);

	if (J36_WIFI_CO_CLOCK_FLAG)
		ret = j36_wifi_pmic_update(w, J36_PMIC_ANALDO_CON19,
					   J36_PMIC_VCN28_ON_CTRL, 0);
	else
		ret = j36_wifi_pmic_update(w, J36_PMIC_ANALDO_CON19, 0,
					   J36_PMIC_VCN28_ON_CTRL | J36_PMIC_VCN28_ENABLE);
	if (ret)
		return ret;

	w->rails_programmed = true;
	return 0;
}

/*
 * The two VCN33 PALDOs, in the order and with the register writes stock uses.
 *
 * mtk_wcn_consys_hw_bt_paldo_ctrl() / _wifi_paldo_ctrl() (mtk_wcn_consys_hw.c:468,
 * :491) are both three steps in this order:
 *
 *	on:	hwPowerOn(VCN33_x, VOL_3300)	-> VOSEL := 0, then the enable bit
 *		upmu_set_vcn33_on_ctrl_x(1)	-> hand the rail to HW control
 *	off:	upmu_set_vcn33_on_ctrl_x(0)	-> take it back to SW control
 *		hwPowerDown(VCN33_x)		-> then clear the enable bit
 *
 * The order is the point.  ON_CTRL hands the rail's enable over to a hardware
 * strobe, so it must be raised only once the LDO is already up and lowered before
 * the LDO comes down -- otherwise the rail is momentarily enabled by nobody.
 * Writing ON_CTRL and the enable bit in one transaction cannot express that,
 * which is what MVII did until it did not work.
 *
 * VOSEL is shared between the two paths, so it is written on the way up by
 * whichever goes up first and deliberately left alone on the way down --
 * hwPowerDown does not touch it either, and clearing it would reset the other
 * path's voltage.
 */
static int j36_wifi_set_vcn33_rail(struct j36_wifi *w, u32 reg, u32 enable_bit,
				   u32 on_ctrl_bit, bool enable)
{
	int ret;

	if (enable) {
		ret = j36_wifi_pmic_update(w, J36_PMIC_ANALDO_CON16,
					   J36_PMIC_VCN33_VOSEL_MASK,
					   J36_PMIC_VCN33_VOSEL_3300);
		if (!ret)
			ret = j36_wifi_pmic_update(w, reg, 0, enable_bit);
		if (!ret)
			ret = j36_wifi_pmic_update(w, reg, 0, on_ctrl_bit);
		return ret;
	}

	ret = j36_wifi_pmic_update(w, reg, on_ctrl_bit, 0);
	if (!ret)
		ret = j36_wifi_pmic_update(w, reg, enable_bit, 0);
	return ret;
}

int j36_wifi_set_bt_rail(struct j36_wifi *w, bool enable)
{
	return j36_wifi_set_vcn33_rail(w, J36_PMIC_ANALDO_CON16,
				       J36_PMIC_VCN33_BT_ENABLE,
				       J36_PMIC_VCN33_BT_ON_CTRL, enable);
}

int j36_wifi_set_wifi_rail(struct j36_wifi *w, bool enable)
{
	int ret = j36_wifi_set_vcn33_rail(w, J36_PMIC_ANALDO_CON17,
					  J36_PMIC_VCN33_WIFI_ENABLE,
					  J36_PMIC_VCN33_WIFI_ON_CTRL, enable);

	if (!ret)
		w->wifi_rail_on = enable;
	return ret;
}

/*
 * The connectivity MTCMOS domain, in stock's order.
 *
 * mtk_wcn_consys_hw_reg_ctrl() does not open-code this -- it calls
 * conn_power_on() (mtk_wcn_consys_hw.c:276), which lands in
 * spm_mtcmos_ctrl_connsys(STA_POWER_ON) at mt_spm_mtcmos.c:1380-1406.  That is
 * nine operations and they are the nine below, in this order:
 *
 *	CONN_PWR_CON |= PWR_ON
 *	CONN_PWR_CON |= PWR_ON_S
 *	spin until (PWR_STATUS & CONN) && (PWR_STATUS_S & CONN)
 *	CONN_PWR_CON &= ~PWR_CLK_DIS
 *	CONN_PWR_CON &= ~PWR_ISO
 *	CONN_PWR_CON |= PWR_RST_B
 *	CONN_PWR_CON &= ~MD_SRAM_PDN
 *	TOPAXI_PROT_EN &= ~CONN_PROT_MASK
 *	spin until (TOPAXI_PROT_STA1 & CONN_PROT_MASK) == 0
 *
 * The order is not decorative.  The bus protection comes off last, after reset is
 * released, so nothing can issue a transaction into a domain that is still
 * isolated.
 */
static void j36_spm_rmw(struct j36_wifi *w, u32 clr, u32 set)
{
	u32 value = readl(w->spm + J36_SPM_CONN_PWR_CON);

	writel((value & ~clr) | set, w->spm + J36_SPM_CONN_PWR_CON);
}

static int j36_wifi_mtcmos_on(struct j36_wifi *w)
{
	u32 value;
	int ret;

	j36_spm_rmw(w, 0, J36_PWR_ON);
	j36_spm_rmw(w, 0, J36_PWR_ON_S);

	ret = readl_poll_timeout(w->spm + J36_SPM_PWR_STATUS, value,
				 value & J36_CONN_PWR_STA_MASK,
				 10, J36_CONSYS_POLL_TIMEOUT_US);
	if (!ret)
		ret = readl_poll_timeout(w->spm + J36_SPM_PWR_STATUS_S, value,
					 value & J36_CONN_PWR_STA_MASK,
					 10, J36_CONSYS_POLL_TIMEOUT_US);
	if (ret) {
		w->pwr_status = readl(w->spm + J36_SPM_PWR_STATUS);
		return ret;
	}

	j36_spm_rmw(w, J36_PWR_CLK_DIS, 0);
	j36_spm_rmw(w, J36_PWR_ISO, 0);
	j36_spm_rmw(w, 0, J36_PWR_RST_B);
	j36_spm_rmw(w, J36_CONN_SRAM_PDN, 0);

	value = readl(w->infracfg + J36_INFRA_TOPAXI_PROT_EN);
	writel(value & ~J36_CONN_PROT_MASK, w->infracfg + J36_INFRA_TOPAXI_PROT_EN);
	ret = readl_poll_timeout(w->infracfg + J36_INFRA_TOPAXI_PROT_STA1, value,
				 !(value & J36_CONN_PROT_MASK),
				 10, J36_CONSYS_POLL_TIMEOUT_US);
	if (ret)
		return ret;

	w->pwr_status = readl(w->spm + J36_SPM_PWR_STATUS);
	w->mtcmos_ready = true;
	return 0;
}

static int j36_wifi_infra_clock_on(struct j36_wifi *w)
{
	u32 value;
	int ret;

	writel(J36_INFRA_CONNMCU_GATE, w->infracfg + J36_INFRA_PDN_CLR);
	ret = readl_poll_timeout(w->infracfg + J36_INFRA_PDN_STA, value,
				 !(value & J36_INFRA_CONNMCU_GATE),
				 10, J36_CONSYS_POLL_TIMEOUT_US);
	if (ret)
		return ret;

	w->infra_clock_ready = true;
	return 0;
}

/*
 * Point the connectivity subsystem's EMI aperture at real DRAM.
 *
 * Stock does this in mtk_wcn_consys_hw_init() at probe, long before any firmware
 * moves (mtk_wcn_consys_hw.c:650-696, with the steal at :637-648):
 *
 *	gConEmiPhyBase = arm_memblock_steal(0x100000, 0x100000);
 *	emi_mpu_set_region_protection(base + 0x80000, base + 0x100000, 5, 0xa28);
 *	r2 = *CONSYS_EMI_MAPPING; r2 |= 0x1000; r2 |= gConEmiPhyBase >> 20;
 *	*CONSYS_EMI_MAPPING = r2; dsb;
 *	pEmiVirtBase = ioremap_nocache(base + 0x80000, 0x55c00);
 *	memset(pEmiVirtBase, 0, 0x55c00);
 *
 * ONE DELIBERATE DIFFERENCE, inherited from MVII.  Stock ORs its value in (:675),
 * which only lands on the intended base because the field reads zero from cold.
 * We clear the twelve base bits first and then set ours, so a second call, or a
 * bootloader that got there first, cannot leave the aperture pointing at the
 * bitwise union of two addresses.  Stock's own base computation masks with
 * 0xFFF00000 before the shift (:672), so twelve bits is its number too.
 *
 * The MPU call is not reproduced and is not missing: an EMI MPU region left
 * unconfigured is unprotected, and configuring one can only ever take access
 * away.  The memset is reproduced, and it is the instrument -- the upper part of
 * the window is where the RUNNING firmware writes, so anything non-zero in it
 * afterwards was written by connectivity firmware that was executing.
 *
 * arm_memblock_steal() has no Linux equivalent that a module may call, so the
 * region is a no-map reserved-memory node in the device tree instead.  That is
 * the stronger form of the same thing: the kernel never had the pages, rather
 * than having them taken back.  It is also what makes ioremap_wc() legal here --
 * ARM32 refuses to ioremap memory that is in the linear map, precisely to stop
 * two mappings of one page disagreeing about cacheability, and a no-map region is
 * not in it.
 */
static void j36_wifi_emi_zero(struct j36_wifi *w)
{
	memset_io(w->emi + J36_CONSYS_EMI_SHARE_OFFSET, 0,
		  J36_CONSYS_EMI_SHARE_SIZE);
}

static int j36_wifi_emi_share_window(struct j36_wifi *w)
{
	u32 desired;

	desired = readl(w->infracfg + J36_INFRA_CONSYS_EMI_MAP);
	desired &= ~(J36_CONSYS_EMI_BASE_MASK | J36_CONSYS_EMI_ENABLE);
	desired |= ((u32)(w->emi_phys >> 20) & J36_CONSYS_EMI_BASE_MASK);
	desired |= J36_CONSYS_EMI_ENABLE;

	writel(desired, w->infracfg + J36_INFRA_CONSYS_EMI_MAP);
	w->emi_mapping = readl(w->infracfg + J36_INFRA_CONSYS_EMI_MAP);
	if (w->emi_mapping != desired) {
		dev_err(w->dev, "CONSYS EMI mapping read back 0x%08x, wanted 0x%08x\n",
			w->emi_mapping, desired);
		return -EIO;
	}

	j36_wifi_emi_zero(w);
	return 0;
}

static bool j36_wifi_known_chip_id(u32 id)
{
	return id == 0x6592 || id == 0x6582 || id == 0x6572;
}

/*
 * Ten reads of CONN_MCU_CONFIG + 0x08, 20 ms apart, against the three
 * PLATFORM_SOC_CHIP values -- mtk_wcn_consys_hw_reg_ctrl()'s retry loop, and the
 * first thing in the bring-up that the connectivity subsystem itself has to
 * answer.
 *
 * Stock compares the whole 32-bit word; the register reads as the bare chip ID
 * with no revision bits above it, so masking to 16 bits only widens what is
 * accepted.  The full compare is the primary test, with a low-half match kept as
 * a LOGGED fallback: it is the difference between "this is an MT6592" and
 * "something at that address has 0x6592 in it", and a log that says which one it
 * saw is worth more than one that silently accepts both.
 *
 * Stock logs a mismatch and carries on regardless, because in its world the
 * subsystem is already known to exist from platform data.  Here the probe is the
 * only evidence there is, so a timeout is fatal.
 */
static int j36_wifi_chip_probe(struct j36_wifi *w)
{
	int i;

	for (i = 0; i < J36_CHIP_ID_RETRIES; i++) {
		w->chip_id = readl(w->consys + J36_CONSYS_CHIP_ID);
		if (j36_wifi_known_chip_id(w->chip_id)) {
			w->consys_responds = true;
			return 0;
		}
		msleep(20);
	}

	if (j36_wifi_known_chip_id(w->chip_id & 0xffff)) {
		dev_warn(w->dev, "CONSYS chip id has an unexpected high half: 0x%08x\n",
			 w->chip_id);
		w->consys_responds = true;
		return 0;
	}
	return -ENODEV;
}

int j36_wifi_consys_bind(struct j36_wifi *w)
{
	int ret;

	if (w->consys_responds)
		return 0;

	/* Before the subsystem is powered, so the aperture is never live and
	 * wrong.  INFRACFG_AO is always on, so this works from cold. */
	ret = j36_wifi_emi_share_window(w);
	if (ret) {
		j36_wifi_fail(w, "consys-emi-mapping-failed",
			      "could not map the CONSYS EMI share window");
		return ret;
	}
	dev_info(w->dev, "CONSYS EMI share window at %pa, mapping 0x%08x\n",
		 &w->emi_phys, w->emi_mapping);

	ret = j36_wifi_rails_on(w);
	if (ret) {
		j36_wifi_fail(w, "vcn18-vcn28-pwrap-failed",
			      "could not raise the VCN18/VCN28 rails");
		return ret;
	}

	ret = j36_wifi_mtcmos_on(w);
	if (ret) {
		j36_wifi_fail(w, "consys-mtcmos-timeout",
			      "the CONSYS power domain did not come up (PWR_STATUS 0x%08x)",
			      w->pwr_status);
		return ret;
	}

	/* conn_power_on(); udelay(10); enable_clock(MT_CG_INFRA_CONNMCU).  The
	 * settle belongs between the power domain coming up and its clock being
	 * ungated, not after the clock. */
	udelay(10);

	ret = j36_wifi_infra_clock_on(w);
	if (ret) {
		j36_wifi_fail(w, "infra-connmcu-clock-timeout",
			      "the INFRA CONNMCU clock did not ungate");
		return ret;
	}

	ret = j36_wifi_chip_probe(w);
	if (ret) {
		j36_wifi_fail(w, "consys-chip-id-timeout",
			      "unexpected CONSYS chip id 0x%08x", w->chip_id);
		return ret;
	}

	/*
	 * msleep(5) closes mtk_wcn_consys_hw_reg_ctrl()'s power-on path, before
	 * anything speaks to the subsystem.  Keep it, so the first BTIF frame
	 * cannot race the clock settling.
	 *
	 * VCN33_WIFI is deliberately NOT raised here, and that is worth saying
	 * because it looks like an omission.  Searching conn_soc for WIFI_PALDO
	 * finds exactly one pair of users -- wmt_ic_soc.c:766 and :779, bracketing
	 * the RF calibration script -- and wmt_func.c's wifi_on path (:681) does
	 * not touch it at all.  What that search misses is the WLAN driver's own
	 * probe raising the same regulator under a name no WIFI_PALDO search
	 * matches: HifAhbProbe does hwPowerOn(MT6323_POWER_LDO_VCN33_WIFI,
	 * VOL_3300, "WLAN") immediately before pfWlanProbe (ahb.c:1823-1826) and
	 * only drops it in HifAhbRemove.  So the 3.3 V rail is a calibration-time
	 * rail AND the transmit PA supply for the life of the WLAN driver, and the
	 * second lifetime starts when the HIF stage does -- which is not in this
	 * build yet.
	 *
	 * The one step of stock's power-on not reproduced anywhere here is
	 * mtk_wcn_consys_hw_gpio_ctrl(1) (mtk_wcn_consys_hw.c:426, body :366-393).
	 * Nothing in it is for WLAN: it initialises PIN_GPS_SYNC, PIN_GPS_LNA and
	 * PIN_I2S_GRP -- GPS and BT audio, and this board's WMT_SOC.cfg turns the
	 * GPS LNA off anyway -- and then registers the BGF external interrupt only
	 * to disable it two lines later (:386-389).  The state it leaves behind for
	 * the connectivity interrupt is "off", which is where a polled driver needs
	 * it and where a cold boot already has it.
	 */
	msleep(5);

	dev_info(w->dev, "CONSYS chip id 0x%08x, power domain up\n", w->chip_id);
	w->blocked = "firmware-not-loaded";
	return 0;
}
