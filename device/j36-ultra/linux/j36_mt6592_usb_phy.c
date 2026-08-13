// SPDX-License-Identifier: GPL-2.0-only
/*
 * J36 Ultra MT6592 USB 2.0 PHY, as a generic-PHY provider for drivers/usb/musb.
 *
 * WHY THIS DRIVER EXISTS AT ALL, in one sentence: mainline's mediatek.c glue
 * takes its PHY with devm_of_phy_get_by_index() and will not probe without one,
 * and MT6592's U2 PHY has no driver anywhere upstream -- phy-mtk-tphy is the
 * later T-PHY, a different register map at a different base.
 *
 * But the load-bearing reason is the ORDER. On MediaTek SoCs an APB access to a
 * clock-gated peripheral does not fault, it stalls the bus until the watchdog
 * fires, and both the MUSB MAC at 0x11200000 and this PHY window at 0x11210800
 * sit behind the PERI clock gate at 0x10003010. musb_core's very first hardware
 * contact is musb_platform_init(), which for MediaTek is mtk_musb_init(), whose
 * first two calls are phy_init() and phy_power_on() -- this file. So .init here
 * is the only hook that runs before a MUSB register is read, and clearing the
 * gate is the first thing it does. MVII records the same hazard from the other
 * side: its mt6592_musb_host.c is compiled out by default behind
 * MVII_MT6592_ENABLE_USB_HOST precisely because a first-frame hang was traced to
 * this init path.
 *
 * WHERE THE REGISTER SEQUENCES COME FROM. Every byte below is transcribed from
 * the stock LK's own PHY routines by way of the freestanding drivers written
 * against this board:
 *
 *   PowerEngine OS/MVII/.../Drivers/mt6592_usb_gadget.c   phy_recover(),
 *       phy_savecurrent() -- themselves transcriptions of FUN_81e09520 and
 *       FUN_81e093b8 in Reference/j36-lk-reverse/lk.full-decompile.c
 *   PowerEngine OS/MVII/.../Drivers/mt6592_musb_host.c    musb_phy_force_host()
 *
 * The one deviation, inherited from MVII and restated here so it is not
 * rediscovered: the stock routine reads eFuse word 0x13 to pick the VRT/TERM_VREF
 * trim for PHY register 0x06 and falls back to the constant 0x68 when the field
 * is blank. This takes the fallback unconditionally, because the eFuse accessor
 * is a driver we do not have and 0x68 is what an untrimmed part of this family
 * runs. Trim is high-speed eye margin; a mouse, a keyboard and a DisplayLink
 * surface at 640x480 are not where that shows up first.
 *
 * ROLE. The tail of the stock recover sequence -- clear 0x6c bit 4, set 0x6c
 * 0x2e, set 0x6d 0x3e -- is not a wake-up, it is MTK's force-DEVICE-mode
 * override, which is why the stock LK ends up a gadget. Host mode is its exact
 * mirror (set 0x6d 0x3c, set 0x6c bit 4, clear 0x6c 0x2c), taken from MVII's
 * musb_phy_force_host() -- which had never run on this board, because MVII's
 * host driver is compiled out behind MVII_MT6592_ENABLE_USB_HOST for the
 * clock-gate reason above.
 *
 * It no longer rests on MVII alone. The stock Android kernel's own host switch,
 * at 0xc052eaa4, is these three writes and nothing else:
 *
 *   c052eaa4:  strb r2, [r3, #0x86d]   // 0x6d |= 0x3c
 *   c052eacc:  strb r2, [r3, #0x86c]   // 0x6c |= 0x10
 *   c052eaf4:  strb r2, [r3, #0x86c]   // 0x6c &= 0xd3, i.e. clear 0x2c
 *
 * on the stock kernel's 0xf1210000 mapping of the SIFSLV window, so +0x86c and
 * +0x86d are this file's 0x6c and 0x6d. Byte for byte j36_phy_force_host().
 * The exit-host path at 0xc052eb8c is its inverse (0x6d &= 0xc3, 0x6c &= 0xc3).
 * One thing the stock does first that this does not: it clears DEVCTL.SESSION
 * in the MUSB core before touching the PHY. That belongs to musb_core, which
 * sets the session bit itself when it starts the host, and is not the PHY's to
 * take away.
 *
 * .set_mode picks between the two sequences, so the role does not depend on how
 * the OTG ID pin happens to float. The kernel is configured USB_MUSB_HOST and
 * the device tree says dr_mode = "host", so in practice only the host branch
 * runs -- but the device branch is what phy_power_off leaves behind, which is
 * also the low-power state.
 *
 * VBUS. It is a GPIO on this board, not a PMIC boost register, and the stock
 * Android kernel says so in four instructions. mt_usb_set_vbus() -- found at
 * 0xc052e938 through the __func__ pointer its own printk loads, line 60 of
 * MTK's musb glue -- does, on the `on' path and nothing else:
 *
 *   mt_set_gpio_mode(0x8000000f, 0);   ops slot 0x3c, writes GPIO base + 0x600
 *   mt_set_gpio_out (0x8000000f, 1);   ops slot 0x30, writes GPIO base + 0x400
 *
 * 0x80000000 is MTK's marker bit on a GPIO_..._PIN constant, stripped by the
 * wrappers before they bounds-check against pad 0xa8, so the pad is 15 and the
 * drive is active high. The two callees were identified from the ops table, not
 * from symbol names, which this image does not have for them: slot 0x3c divides
 * the pad by five and rewrites a 3-bit field at base+0x600, which is MODE, and
 * slot 0x30 writes the SET/RST alias of base+0x400, which is DOUT. Those are the
 * same offsets mt6592_led.c drives on this board today. Pin 15 appears in
 * exactly two functions in the whole 12 MB image -- that one and the pad setup
 * beside it -- so nothing else here wants the pad.
 *
 * BUT FIT A CELL. The 5 V is a boost off VBAT, and VBAT on this PMIC is the
 * system node rather than a battery-only rail: a bus-powered hub is the same
 * class of load as the class-D amp, which MVII measured pulling VBAT under the
 * undervoltage lockout on a cell-less board. vbus=0 turns the whole thing off
 * and leaves the pad exactly as the LK left it, which is how this port has
 * behaved up to now.
 *
 * If the hub is self-powered it may already hold 5 V on the cable. Driving the
 * pad as well is not a short -- the pad feeds a load switch, not the rail --
 * but it is also not needed, and vbus=0 is the honest setting for that case.
 *
 * ── AND THE MEASUREMENTS ──────────────────────────────────────────────────────
 *
 * The interrupt number used to be a guess and used to be measured from here, by
 * snapshotting GICD_ISPENDR and watching for a level-sensitive line that went
 * pending and stayed pending because nothing had claimed it. That is answered
 * now: mt_usb_init in the stock kernel assigns it outright,
 *
 *   c052bb30:  mov r3, #96
 *   c052bb44:  str r3, [r4, #0x290]     // musb->nIrq = 96
 *
 * INTID 96 is SPI 64, which is what the device tree already said, and the map it
 * sits in cross-checks twice against nodes that work today -- UART0 at INTID 115
 * and Mali_GP at INTID 234. So scan_irq now defaults off. The code stays because
 * it costs one delayed work and is the only way to answer the question again if
 * the cell is ever changed.
 *
 * What is NOT answered, and what j36,musb-controller is for, is the controller's
 * own view of the port. This driver already has the PERI gate open before
 * anything reads MUSB, so it is the one place that can look at the MAC early and
 * cheaply. It reads, once at power-on and again a few seconds later:
 *
 *   DEVCTL  0x60   HOSTMODE, and the two VBUS bits -- the difference between
 *                  "5 V never came up" and "5 V is fine, nothing attached"
 *   POWER   0x01   HS enable, reset, suspend
 *   FADDR   0x00   the function address the core is actually addressing
 *   EPINFO  0x78   TX and RX endpoint counts, from the silicon
 *   RAMINFO 0x79   FIFO RAM address width and DMA channel count
 *   L1INTM  0xa4   whether the MediaTek L1 aggregator was unmasked
 *
 * Every one of those is a plain register. The read-to-clear ones -- INTRUSB,
 * INTRTX, INTRRX -- are deliberately NOT among them, because reading those from
 * here would swallow a connect interrupt out from under musb_core.
 *
 * And one active probe, at power-on only, before musb_core has started: write a
 * pattern to the ep0 function-address register at BOTH candidate busctl bases,
 * 0x080 and 0x480, read each back, then restore. MediaTek moved that block to
 * 0x480 in the MT2701 generation to make room for a TXTOG/RXTOG block at
 * 0x80..0x87, and mainline's mediatek.c assumes the move unconditionally. The
 * stock MT6592 kernel touches neither address, which is why
 * linux/0003-musb-mediatek-mt6592.patch puts this part back on the stock MUSB
 * base of 0x80 -- and this probe is what confirms or refutes that from the
 * board, in one line of boot log. Whichever base holds the pattern is the real
 * one. Writing there is safe because musb_core reprograms the function address
 * before every transfer, and this runs before it has started any.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

/* PERI clock gates. PDN_CLR clears power-down bits, i.e. turns clocks ON.
 *
 * PDN0 has 27 gates, not 32: the stock clkmgr's CG_PERI record at 0xc0b38670
 * carries 0x07ffffff as its valid mask, and bits 27..31 do not decode. Bit 17 is
 * USB2.0 and bit 18 is USBSIF, which is the whole of USB's clocking here -- the
 * MUSB MAC and the U2 PHY sit behind the pair, which is why a read of either
 * window before this write stalls the APB rather than faulting. */
#define J36_PERI_PDN0_CLR		0x0010
#define J36_PERI_PDN0_STA		0x0018
#define J36_PERI_PDN0_VALID		0x07ffffff
#define J36_PERI_PDN0_USB20		BIT(17)
#define J36_PERI_PDN0_USBSIF		BIT(18)

/* GIC distributor. Eight words of Set-Pending cover INTID 0..255, which is the
 * whole of this SoC's map: SPI = INTID - 32, and MT6592's polarity block covers
 * SPI 0..223. */
#define J36_GICD_ISPENDR		0x0200
#define J36_GICD_ISPENDR_WORDS		8
#define J36_GIC_INTID_SPI_BASE		32

/* MUSB MAC, read-only from here except for the one layout probe below. These are
 * the stock Mentor common-register offsets, unchanged since the core existed, and
 * they are the same numbers musb_core.h uses -- spelled out rather than included
 * because that header is private to drivers/usb/musb and this is a PHY driver.
 *
 * INTRTX 0x02, INTRRX 0x04 and INTRUSB 0x0a are deliberately absent: they clear
 * on read, so a read from here would consume a connect or reset interrupt before
 * musb_core's handler ever saw it. */
#define J36_MUSB_FADDR			0x00
#define J36_MUSB_POWER			0x01
#define J36_MUSB_POWER_SUSPENDM		BIT(1)
#define J36_MUSB_POWER_RESET		BIT(3)
#define J36_MUSB_POWER_HSMODE		BIT(4)
#define J36_MUSB_POWER_HSENAB		BIT(5)
#define J36_MUSB_DEVCTL			0x60
#define J36_MUSB_DEVCTL_SESSION		BIT(0)
#define J36_MUSB_DEVCTL_HR		BIT(1)
#define J36_MUSB_DEVCTL_HM		BIT(2)
#define J36_MUSB_DEVCTL_VBUS		GENMASK(4, 3)
#define J36_MUSB_DEVCTL_VBUS_SHIFT	3
#define J36_MUSB_DEVCTL_LSDEV		BIT(5)
#define J36_MUSB_DEVCTL_FSDEV		BIT(6)
#define J36_MUSB_DEVCTL_BDEVICE		BIT(7)
#define J36_MUSB_HWVERS			0x6c
#define J36_MUSB_EPINFO			0x78
#define J36_MUSB_RAMINFO		0x79
#define J36_MUSB_L1INTM			0xa4

/* The two candidate bases for the multipoint block's TXFUNCADDR, which is what
 * the layout probe writes. 0x080 is the stock MUSB one and 0x480 is where
 * MediaTek relocated it in the MT2701 generation. The two patterns differ so
 * that an aliasing window cannot read as a hit on both, and both fit in
 * TXFUNCADDR's seven bits so that neither is truncated where it does decode. */
#define J36_MUSB_BUSCTL_LEGACY		0x080
#define J36_MUSB_BUSCTL_LEGACY_VAL	0x2a
#define J36_MUSB_BUSCTL_MTK		0x480
#define J36_MUSB_BUSCTL_MTK_VAL		0x55

/* U2 PHY registers, byte-wide, offsets from 0x11210800. Named where the stock
 * decompile named them and left as bare offsets where it did not -- an invented
 * name for a bit whose meaning is unknown is worse than the number. */
#define J36_PHY_R06			0x06	/* VRT / TERM_VREF trim, [2:0] kept */
#define J36_PHY_R06_TRIM_KEEP		0x07
#define J36_PHY_R06_TRIM_DEFAULT	0x68
#define J36_PHY_R1A			0x1a
#define J36_PHY_R1A_GPIO_CTL		0x80	/* rg_usb20_gpio_ctl */
#define J36_PHY_R1A_BIT4		0x10
#define J36_PHY_R1D			0x1d
#define J36_PHY_R1D_BIT4		0x10
#define J36_PHY_R22			0x22
#define J36_PHY_R22_PULLDOWN		0x03	/* DP/DM 100k pull-downs */
#define J36_PHY_R68			0x68
#define J36_PHY_R68_FORCE		0xf4
#define J36_PHY_R69			0x69
#define J36_PHY_R69_FORCE		0x3c
#define J36_PHY_R6A			0x6a
#define J36_PHY_R6A_FORCE		0xbe
#define J36_PHY_R6A_BIT2		0x04
#define J36_PHY_R6B			0x6b
#define J36_PHY_R6B_FORCE_UART_EN	0x04
/* 0x6c and 0x6d are the two low bytes of U2PHYDTM1, and they carry the role
 * override. Bit 4 of 0x6c is the one that differs between the two sequences --
 * device clears it, host sets it -- and it is left named after its position
 * rather than after a field, because neither the stock decompile nor MVII names
 * it and guessing IDDIG from the surrounding bits is exactly the kind of
 * invention this bring-up does not make. What is known is that the two stock
 * routines both end with bit 4 CLEARED and the LK enumerates as a gadget, so
 * cleared is the device role and set is the other one. */
#define J36_PHY_R6C			0x6c	/* U2PHYDTM1, byte 0 */
#define J36_PHY_R6C_BIT4		0x10
#define J36_PHY_R6C_DEV_SET		0x2e
#define J36_PHY_R6C_HOST_CLR		0x2c
#define J36_PHY_R6D			0x6d	/* U2PHYDTM1, byte 1 -- the force_* enables */
#define J36_PHY_R6D_DEV_SET		0x3e
#define J36_PHY_R6D_HOST_SET		0x3c
#define J36_PHY_R6E			0x6e
#define J36_PHY_R6E_UART_EN		0x01

/* The stock routines' own settle time, in microseconds. */
#define J36_PHY_SETTLE_US		800

/* MT6592 GPIO controller, from mt6592_led.c, which drives three pads on this
 * board with it today. Every register is a 16-pin bank at a 0x10 stride and
 * +4 / +8 off a bank are its SET and RST aliases -- one bit written there sets
 * or clears that bit and leaves the other fifteen alone, so there is no
 * read-modify-write and no way to step on a pad this driver does not own.
 *
 * MODE is the exception and the only read-modify-write here: five 3-bit fields
 * per register, indexed base + 0x600 + (pin / 5) * 0x10, field (pin % 5) * 3.
 * That indexing is verbatim what the preloader's mt_set_gpio_mode and the stock
 * LK's copy both compute, and both also reject any pin above 0xa8. */
#define J36_GPIO_DIR			0x0000
#define J36_GPIO_DOUT			0x0400
#define J36_GPIO_MODE			0x0600
#define J36_GPIO_BANK_STRIDE		0x0010
#define J36_GPIO_BANK_SET		0x0004
#define J36_GPIO_BANK_RST		0x0008
#define J36_GPIO_PINS_PER_BANK		16
#define J36_GPIO_PINS_PER_MODE_REG	5
#define J36_GPIO_MODE_FIELD_BITS	3
#define J36_GPIO_MODE_GPIO		0
#define J36_GPIO_PIN_MAX		0xa8

static unsigned int pdn_mask = J36_PERI_PDN0_VALID;
module_param(pdn_mask, uint, 0444);
MODULE_PARM_DESC(pdn_mask,
		 "PERI PDN0 bits to ungate at phy_init (default: every bit the "
		 "register has, 0x07ffffff). USB is bits 17 and 18 -- USB2.0 and "
		 "USBSIF -- but MT6592 has no clock driver in this tree, so "
		 "nothing else ungates the rest of that bus either and the "
		 "default stays wide; pdn_mask=0x60000 narrows it to USB alone.");

static bool scan_irq;
module_param(scan_irq, bool, 0444);
MODULE_PARM_DESC(scan_irq,
		 "after power-on, sample GICD_ISPENDR and report SPIs that "
		 "became pending. Off by default: the MUSB interrupt is INTID 96 "
		 "and the stock kernel assigns it outright. scan_irq=1 measures "
		 "it again if the device tree cell is ever changed.");

static unsigned int scan_delay_ms = 3000;
module_param(scan_delay_ms, uint, 0444);
MODULE_PARM_DESC(scan_delay_ms,
		 "delay before the first GICD_ISPENDR sample (the second is at "
		 "three times this, to catch a connect that happens late)");

static bool vbus = true;
module_param(vbus, bool, 0444);
MODULE_PARM_DESC(vbus,
		 "drive the DRVVBUS pad named by j36,drvvbus-pad, so the port "
		 "sources 5 V and a bus-powered device enumerates (default on). "
		 "vbus=0 leaves the pad exactly as the LK left it -- use it with "
		 "a self-powered hub, and use it if no cell is fitted, because "
		 "the 5 V is a boost off VBAT and VBAT here is the system node.");

static bool musb_probe_layout = true;
module_param(musb_probe_layout, bool, 0444);
MODULE_PARM_DESC(musb_probe_layout,
		 "at power-on, write a pattern to TXFUNCADDR at both candidate "
		 "multipoint bases (0x080 and 0x480) and report which one holds "
		 "it, then restore. This is what says whether the busctl base in "
		 "linux/0003-musb-mediatek-mt6592.patch is right. Runs before "
		 "musb_core starts, so no transfer can be in flight.");

struct j36_usb_phy {
	struct device *dev;
	void __iomem *phy;
	void __iomem *pericfg;
	void __iomem *gicd;
	void __iomem *gpio;
	void __iomem *musb;
	struct phy *generic;

	int vbus_pin;			/* -1 when the device tree names none */
	bool vbus_on;

	struct delayed_work scan_work;
	u32 pending_baseline[J36_GICD_ISPENDR_WORDS];
	unsigned int scans_left;
};

/* ── register helpers ────────────────────────────────────────────────────────
 *
 * Byte accesses, because that is what the block is: the stock routines read and
 * write single bytes at these offsets and a 32-bit access would fold four
 * unrelated fields into one transaction.
 */

static u8 j36_phy_rd(struct j36_usb_phy *p, u32 off)
{
	return readb(p->phy + off);
}

static void j36_phy_wr(struct j36_usb_phy *p, u32 off, u8 v)
{
	writeb(v, p->phy + off);
}

static void j36_phy_set(struct j36_usb_phy *p, u32 off, u8 bits)
{
	j36_phy_wr(p, off, j36_phy_rd(p, off) | bits);
}

static void j36_phy_clr(struct j36_usb_phy *p, u32 off, u8 bits)
{
	j36_phy_wr(p, off, j36_phy_rd(p, off) & (u8)~bits);
}

/* ── DRVVBUS ─────────────────────────────────────────────────────────────────
 *
 * The two helpers are mt6592_led.c's, unchanged: a bank-relative SET/RST write
 * that touches one bit and reads nothing, and the one read-modify-write MODE
 * needs. Sharing the shape matters more than sharing the code -- if the pad
 * arithmetic here ever disagrees with the LED driver's, one of the two is
 * driving the wrong pad.
 */
static void j36_gpio_bank_bit(struct j36_usb_phy *p, u32 reg, unsigned int pin,
			      bool on)
{
	u32 bank = (pin / J36_GPIO_PINS_PER_BANK) * J36_GPIO_BANK_STRIDE;
	u32 alias = on ? J36_GPIO_BANK_SET : J36_GPIO_BANK_RST;

	writel(BIT(pin % J36_GPIO_PINS_PER_BANK), p->gpio + reg + bank + alias);
}

static void j36_gpio_mode_set(struct j36_usb_phy *p, unsigned int pin, u32 mode)
{
	void __iomem *addr = p->gpio + J36_GPIO_MODE +
			     (pin / J36_GPIO_PINS_PER_MODE_REG) * J36_GPIO_BANK_STRIDE;
	unsigned int shift = (pin % J36_GPIO_PINS_PER_MODE_REG) *
			     J36_GPIO_MODE_FIELD_BITS;

	writel((readl(addr) & ~(0x7u << shift)) | ((mode & 0x7u) << shift), addr);
}

/*
 * Drive the pad, in mt6592_led.c's order: DOUT first, then DIR, then MODE, so
 * the pad already holds the level intended by the time it becomes an output and
 * is not a GPIO at all until both are settled. Getting that backwards puts a
 * glitch on the rail every call, which on an LED is a flash and on a load switch
 * is a 5 V transient into whatever is plugged in.
 *
 * The stock mt_usb_set_vbus() writes only MODE and DOUT and never sets DIR --
 * the pad's direction is set once by the drvvbus pad-config routine beside it
 * (0xc052e9c4, which calls mode(0), dir(1) and two more ops slots). This driver
 * has no such separate setup path, so it sets DIR itself; the SET/RST alias
 * makes that idempotent.
 *
 * The other stock asymmetry is left alone rather than copied: mt_usb_set_vbus()
 * on the `off' path does NOT clear the pad, so once Android raised VBUS it
 * stayed raised. That is not something to reproduce on a board whose 5 V comes
 * off the system rail, so power_off drops DOUT here.
 */
static void j36_usb_phy_vbus(struct j36_usb_phy *p, bool on)
{
	if (p->vbus_pin < 0 || !p->gpio || !vbus)
		return;
	if (p->vbus_on == on)
		return;

	j36_gpio_bank_bit(p, J36_GPIO_DOUT, p->vbus_pin, on);
	j36_gpio_bank_bit(p, J36_GPIO_DIR, p->vbus_pin, true);
	j36_gpio_mode_set(p, p->vbus_pin, J36_GPIO_MODE_GPIO);
	p->vbus_on = on;

	dev_info(p->dev, "DRVVBUS pad %d %s: the port %s 5 V\n",
		 p->vbus_pin, on ? "high" : "low",
		 on ? "is sourcing" : "no longer sources");
}

/* ── the three transcribed sequences ─────────────────────────────────────────
 *
 * The shared head of recover() and savecurrent(): drop the UART-over-USB
 * overrides the ROM may have left, drop the force_* bits, and load the trim.
 * The two routines diverge after it and converge again on the same three-write
 * tail, which is what pins the role.
 */
static void j36_phy_uart_off_and_trim(struct j36_usb_phy *p)
{
	j36_phy_clr(p, J36_PHY_R6B, J36_PHY_R6B_FORCE_UART_EN);
	j36_phy_clr(p, J36_PHY_R6E, J36_PHY_R6E_UART_EN);
	j36_phy_clr(p, J36_PHY_R1A, J36_PHY_R1A_GPIO_CTL);
	j36_phy_wr(p, J36_PHY_R06,
		   (j36_phy_rd(p, J36_PHY_R06) & J36_PHY_R06_TRIM_KEEP) |
		   J36_PHY_R06_TRIM_DEFAULT);
}

/* Device-mode tail, and also the low-power resting state. This is the tail both
 * stock routines end on, so it is what the PHY sits in after recover(). */
static void j36_phy_force_device(struct j36_usb_phy *p)
{
	j36_phy_clr(p, J36_PHY_R6C, J36_PHY_R6C_BIT4);
	j36_phy_set(p, J36_PHY_R6C, J36_PHY_R6C_DEV_SET);
	j36_phy_set(p, J36_PHY_R6D, J36_PHY_R6D_DEV_SET);
}

/* Host-mode override, from MVII's musb_phy_force_host(): the mirror of the
 * above, and the only difference that decides whether a hub enumerates. The
 * settle delay is the same 800 us the stock routines take after their own
 * role writes. */
static void j36_phy_force_host(struct j36_usb_phy *p)
{
	j36_phy_set(p, J36_PHY_R6D, J36_PHY_R6D_HOST_SET);
	j36_phy_set(p, J36_PHY_R6C, J36_PHY_R6C_BIT4);
	j36_phy_clr(p, J36_PHY_R6C, J36_PHY_R6C_HOST_CLR);
	usleep_range(J36_PHY_SETTLE_US, J36_PHY_SETTLE_US * 2);
}

/* FUN_81e09520, usb_phy_recover(). */
static void j36_phy_recover(struct j36_usb_phy *p)
{
	j36_phy_clr(p, J36_PHY_R1D, J36_PHY_R1D_BIT4);
	j36_phy_clr(p, J36_PHY_R6B, J36_PHY_R6B_FORCE_UART_EN);
	j36_phy_clr(p, J36_PHY_R6E, J36_PHY_R6E_UART_EN);
	j36_phy_clr(p, J36_PHY_R68, J36_PHY_R68_FORCE);
	j36_phy_clr(p, J36_PHY_R69, J36_PHY_R69_FORCE);
	j36_phy_clr(p, J36_PHY_R6A, J36_PHY_R6A_FORCE);
	j36_phy_clr(p, J36_PHY_R1A, J36_PHY_R1A_GPIO_CTL);
	j36_phy_wr(p, J36_PHY_R06,
		   (j36_phy_rd(p, J36_PHY_R06) & J36_PHY_R06_TRIM_KEEP) |
		   J36_PHY_R06_TRIM_DEFAULT);
	j36_phy_set(p, J36_PHY_R1A, J36_PHY_R1A_BIT4);
	usleep_range(J36_PHY_SETTLE_US, J36_PHY_SETTLE_US * 2);
	j36_phy_force_device(p);
}

/* FUN_81e093b8, savecurrent(). */
static void j36_phy_savecurrent(struct j36_usb_phy *p)
{
	j36_phy_uart_off_and_trim(p);
	j36_phy_clr(p, J36_PHY_R22, J36_PHY_R22_PULLDOWN);
	j36_phy_clr(p, J36_PHY_R6A, J36_PHY_R6A_BIT2);
	usleep_range(J36_PHY_SETTLE_US, J36_PHY_SETTLE_US * 2);
	j36_phy_force_device(p);
}

/* ── the MUSB readout ─────────────────────────────────────────────────────────
 *
 * Read-only, and every offset here is a plain register -- see the note beside
 * the defines for why the three interrupt-status registers are not among them.
 *
 * The value of doing this from the PHY driver rather than from musb is that the
 * PHY is what ungates the MAC, so this is the earliest point at which MUSB can
 * be read at all, and it is still readable in the delayed sample seconds later
 * whether or not musb_hdrc ever bound. A port that never enumerates says
 * different things in DEVCTL depending on why: VBUS below the session-valid
 * threshold means the 5 V never came up, VBUS at 3 with nothing else set means
 * the rail is fine and nothing is attached, and HM clear on a node that asked
 * for host means the role override did not take.
 */

static const char *j36_musb_vbus_str(u8 devctl)
{
	static const char * const level[] = {
		"below SessionEnd",		/* 0 */
		"above SessionEnd, below AValid",
		"above AValid, below VBusValid",
		"above VBusValid",		/* 3 -- the only usable one */
	};

	return level[(devctl & J36_MUSB_DEVCTL_VBUS) >> J36_MUSB_DEVCTL_VBUS_SHIFT];
}

static void j36_musb_dump(struct j36_usb_phy *p, const char *when)
{
	u8 faddr, power, devctl, epinfo, raminfo;
	u16 hwvers;
	u32 l1intm;

	if (!p->musb)
		return;

	faddr	= readb(p->musb + J36_MUSB_FADDR);
	power	= readb(p->musb + J36_MUSB_POWER);
	devctl	= readb(p->musb + J36_MUSB_DEVCTL);
	hwvers	= readw(p->musb + J36_MUSB_HWVERS);
	epinfo	= readb(p->musb + J36_MUSB_EPINFO);
	raminfo	= readb(p->musb + J36_MUSB_RAMINFO);
	l1intm	= readl(p->musb + J36_MUSB_L1INTM);

	dev_info(p->dev,
		 "MUSB %s: DEVCTL %02x [%s%s%s%s%s%s] VBUS %s, POWER %02x [%s%s%s%s], FADDR %u\n",
		 when, devctl,
		 devctl & J36_MUSB_DEVCTL_SESSION ? "SESSION " : "",
		 devctl & J36_MUSB_DEVCTL_HM	  ? "HOST "    : "PERIPHERAL ",
		 devctl & J36_MUSB_DEVCTL_HR	  ? "HOSTREQ " : "",
		 devctl & J36_MUSB_DEVCTL_BDEVICE ? "BDEV "    : "ADEV ",
		 devctl & J36_MUSB_DEVCTL_FSDEV	  ? "FSDEV "   : "",
		 devctl & J36_MUSB_DEVCTL_LSDEV	  ? "LSDEV"    : "",
		 j36_musb_vbus_str(devctl), power,
		 power & J36_MUSB_POWER_HSENAB	  ? "HSENAB "  : "",
		 power & J36_MUSB_POWER_HSMODE	  ? "HSMODE "  : "",
		 power & J36_MUSB_POWER_RESET	  ? "RESET "   : "",
		 power & J36_MUSB_POWER_SUSPENDM  ? "SUSPEND"  : "",
		 faddr & 0x7f);

	/* EPINFO is TX count in [3:0] and RX count in [7:4], neither counting
	 * ep0; RAMINFO is the FIFO RAM address width in [3:0] and the DMA
	 * channel count in [7:4]. Both come from the silicon, so they are the
	 * check on MTK_MUSB_MAX_EP_NUM and MTK_MUSB_RAM_BITS, which mainline
	 * hardcodes at 8 and 11. */
	dev_info(p->dev,
		 "MUSB %s: RTL %u.%u%s, %u TX + %u RX endpoints, FIFO RAM %u bytes, %u DMA channels, L1INTM %08x\n",
		 when, (hwvers >> 10) & 0x1f, hwvers & 0x3ff,
		 hwvers & BIT(15) ? " RC" : "",
		 epinfo & 0xf, (epinfo >> 4) & 0xf,
		 1u << ((raminfo & 0xf) + 2), (raminfo >> 4) & 0xf, l1intm);
}

/*
 * Which base the multipoint block decodes at, answered by writing to it.
 *
 * TXFUNCADDR for ep0 is the first register of that block, and it is plain
 * read/write storage in both layouts -- musb_core rewrites it before every
 * transfer to whatever address the device being talked to has. So a pattern
 * written to it is either held, which means the block is there, or read back as
 * zero, which means the write landed in a hole.
 *
 * 0x480 is the decisive one and it is decisive in one direction only, which is
 * why it is read first: on the MT2701 layout it IS TXFUNCADDR, and on the legacy
 * layout there is nothing there at all. So a pattern that survives at 0x480
 * settles it outright.
 *
 * 0x080 corroborates but cannot settle it alone, because on the MT2701 layout
 * that address is not empty either -- it is MUSB_RXTOG, whose writes are gated
 * by MUSB_RXTOGEN, which is zero out of reset. It is written with a different
 * pattern so that the two cannot be confused if the window aliases.
 *
 * Both are restored to 0 afterwards, which is what musb_core would find on a
 * fresh core anyway, and this runs at power_on -- before musb_start(), before
 * any device has been addressed. It is never run from the delayed work, where a
 * transfer could be live and where clobbering a function address mid-transaction
 * would be a real fault rather than a measurement.
 *
 * Writing to whichever base turns out to be the hole is safe, and this board has
 * already demonstrated it: the unpatched mediatek.c writes 0x480 on every single
 * transfer, and what the board does in response is fail to enumerate, not hang.
 * An undecoded offset inside a peripheral window that IS decoded and IS ungated
 * drops the write. The hang case is the gated one, and the PERI gate was opened
 * in .init long before this runs.
 */
static void j36_musb_probe_busctl(struct j36_usb_phy *p)
{
	u8 legacy, mtk;

	if (!p->musb || !musb_probe_layout)
		return;

	writeb(J36_MUSB_BUSCTL_LEGACY_VAL, p->musb + J36_MUSB_BUSCTL_LEGACY);
	writeb(J36_MUSB_BUSCTL_MTK_VAL, p->musb + J36_MUSB_BUSCTL_MTK);
	legacy = readb(p->musb + J36_MUSB_BUSCTL_LEGACY);
	mtk    = readb(p->musb + J36_MUSB_BUSCTL_MTK);
	writeb(0, p->musb + J36_MUSB_BUSCTL_LEGACY);
	writeb(0, p->musb + J36_MUSB_BUSCTL_MTK);

	if (mtk == J36_MUSB_BUSCTL_MTK_VAL)
		dev_warn(p->dev,
			 "MUSB multipoint block is at 0x480 (0x080 read back %02x): the MT2701 layout, so linux/0003-musb-mediatek-mt6592.patch has it wrong and should be dropped\n",
			 legacy);
	else if (legacy == J36_MUSB_BUSCTL_LEGACY_VAL)
		dev_info(p->dev,
			 "MUSB multipoint block is at 0x080 (0x480 read back %02x): the legacy layout, which is what linux/0003-musb-mediatek-mt6592.patch assumes\n",
			 mtk);
	else
		dev_warn(p->dev,
			 "MUSB multipoint probe is inconclusive: 0x080 read back %02x and 0x480 read back %02x, neither holding what was written\n",
			 legacy, mtk);
}

/* ── the interrupt measurement, and the delayed sample that carries it ──────── */

static void j36_usb_phy_sample_pending(struct j36_usb_phy *p, u32 *out)
{
	unsigned int i;

	for (i = 0; i < J36_GICD_ISPENDR_WORDS; i++)
		out[i] = readl(p->gicd + J36_GICD_ISPENDR + i * 4);
}

/*
 * The delayed sample, which is both measurements: MUSB's own view of the port,
 * and -- when scan_irq asks for it -- whatever turned up pending at the GIC.
 * It runs twice, once soon after power-on and once well after, because a device
 * plugged in a few seconds into boot is the case worth catching.
 */
static void j36_usb_phy_scan(struct work_struct *work)
{
	struct j36_usb_phy *p = container_of(to_delayed_work(work),
					     struct j36_usb_phy, scan_work);
	u32 now[J36_GICD_ISPENDR_WORDS];
	unsigned int word, bit, found = 0;

	j36_musb_dump(p, p->scans_left > 1 ? "settled" : "late");

	if (!scan_irq || !p->gicd)
		goto again;

	j36_usb_phy_sample_pending(p, now);

	for (word = 0; word < J36_GICD_ISPENDR_WORDS; word++) {
		u32 fresh = now[word] & ~p->pending_baseline[word];

		for (bit = 0; bit < 32; bit++) {
			unsigned int intid = word * 32 + bit;

			if (!(fresh & BIT(bit)))
				continue;
			if (intid < J36_GIC_INTID_SPI_BASE)
				continue;	/* SGI/PPI, not ours to read */

			found++;
			dev_info(p->dev,
				 "GIC INTID %u became pending: device tree cell is <0 %u 8>\n",
				 intid, intid - J36_GIC_INTID_SPI_BASE);
		}
		/* Fold what was reported into the baseline so the second pass
		 * only ever names something new. */
		p->pending_baseline[word] = now[word];
	}

	if (!found)
		dev_info(p->dev,
			 "no new pending SPI (either the device tree already names MUSB's interrupt, or nothing on the port has asked for one yet)\n");

again:
	if (--p->scans_left)
		schedule_delayed_work(&p->scan_work,
				      msecs_to_jiffies(scan_delay_ms * 2));
}

static void j36_usb_phy_scan_arm(struct j36_usb_phy *p)
{
	bool gic = scan_irq && p->gicd;

	if (!gic && !p->musb)
		return;

	if (gic) {
		j36_usb_phy_sample_pending(p, p->pending_baseline);
		dev_info(p->dev,
			 "sampling GICD_ISPENDR in %u ms and again %u ms after that; an unclaimed level interrupt stays pending, so MUSB's line will show up if the device tree has the wrong one\n",
			 scan_delay_ms, scan_delay_ms * 2);
	}

	p->scans_left = 2;
	schedule_delayed_work(&p->scan_work, msecs_to_jiffies(scan_delay_ms));
}

/* ── phy_ops ─────────────────────────────────────────────────────────────────── */

static int j36_usb_phy_init(struct phy *phy)
{
	struct j36_usb_phy *p = phy_get_drvdata(phy);
	u32 before, after;

	/*
	 * This write is the whole reason the ordering matters, and it happens
	 * before the first read of the PHY window below -- that window is behind
	 * the same gate, and so is every MUSB register the readout touches.
	 *
	 * Still blunt, and still on purpose. USB's own gates are bits 17 and 18
	 * and those are reported by name below, but MT6592 has no clock driver in
	 * this tree, so the peripherals reached through PERICFG have nothing else
	 * ungating them either; MVII cleared the whole register here and the board
	 * enumerated. pdn_mask= narrows it without editing this file.
	 */
	before = readl(p->pericfg + J36_PERI_PDN0_STA);
	writel(pdn_mask, p->pericfg + J36_PERI_PDN0_CLR);
	after = readl(p->pericfg + J36_PERI_PDN0_STA);
	dev_info(p->dev,
		 "PERI PDN0_STA %08x -> %08x (ungated mask %08x); USB2.0 %s, USBSIF %s\n",
		 before, after, pdn_mask,
		 after & J36_PERI_PDN0_USB20  ? "STILL GATED" : "on",
		 after & J36_PERI_PDN0_USBSIF ? "STILL GATED" : "on");

	j36_phy_recover(p);
	return 0;
}

static int j36_usb_phy_power_on(struct phy *phy)
{
	struct j36_usb_phy *p = phy_get_drvdata(phy);

	/*
	 * Host is the default here rather than a neutral state, because this
	 * kernel is built USB_MUSB_HOST and the node says dr_mode = "host": there
	 * is no path through musb that would ever ask for anything else, and
	 * leaving the PHY in the recover() sequence's device-mode tail would mean
	 * the port sat as a B-device until set_mode arrived.
	 */
	j36_phy_force_host(p);

	/*
	 * After the role, not before. VBUS is what a downstream device sees
	 * first, and raising it while the PHY still sat in the recover()
	 * sequence's device-mode tail would let a hub start its own attach
	 * against a port that was still a B-device.
	 */
	j36_usb_phy_vbus(p, true);

	/*
	 * Both of these are here rather than in phy_init because VBUS has just
	 * been raised and DEVCTL's VBUS field is the reading worth having, and
	 * because power_on is still ahead of musb_start() -- which is what makes
	 * the busctl write safe. Dump first, probe second, so the dump shows the
	 * function address as musb left it rather than as the probe restored it.
	 */
	j36_musb_dump(p, "at power-on");
	j36_musb_probe_busctl(p);

	j36_usb_phy_scan_arm(p);
	return 0;
}

static int j36_usb_phy_power_off(struct phy *phy)
{
	struct j36_usb_phy *p = phy_get_drvdata(phy);

	cancel_delayed_work_sync(&p->scan_work);
	j36_usb_phy_vbus(p, false);
	j36_phy_savecurrent(p);
	return 0;
}

static int j36_usb_phy_exit(struct phy *phy)
{
	struct j36_usb_phy *p = phy_get_drvdata(phy);

	/*
	 * The PERI gate is deliberately NOT restored. PDN0 is one register shared
	 * by every peripheral on that bus -- the keypad's AUXADC gate is cleared
	 * out of the same word by j36_mt6592_input -- and this driver does not
	 * know which bits it turned on were already on. Re-gating from here would
	 * mean unloading the USB modules could stop the gamepad.
	 */
	dev_dbg(p->dev, "exit: leaving the PERI gate as found\n");
	return 0;
}

static int j36_usb_phy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct j36_usb_phy *p = phy_get_drvdata(phy);

	switch (mode) {
	case PHY_MODE_USB_HOST:
		j36_phy_force_host(p);
		j36_usb_phy_vbus(p, true);
		break;
	case PHY_MODE_USB_DEVICE:
		/*
		 * Drop the 5 V first. A B-device that still drove VBUS would be
		 * fighting whatever host just plugged in, and on this board the
		 * thing it would be fighting with is the system rail.
		 */
		j36_usb_phy_vbus(p, false);
		j36_phy_force_device(p);
		break;
	case PHY_MODE_USB_OTG:
		/*
		 * There is no ID-pin path here: both sequences are overrides
		 * that pin the role, and the stock LK has no third one that
		 * releases them. Answering EINVAL is better than silently
		 * leaving whichever role was set last, which is what a
		 * pass-through would do.
		 */
		dev_warn(p->dev, "OTG mode has no override sequence on this PHY\n");
		return -EINVAL;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct phy_ops j36_usb_phy_ops = {
	.init		= j36_usb_phy_init,
	.exit		= j36_usb_phy_exit,
	.power_on	= j36_usb_phy_power_on,
	.power_off	= j36_usb_phy_power_off,
	.set_mode	= j36_usb_phy_set_mode,
	.owner		= THIS_MODULE,
};

/* ── probe ───────────────────────────────────────────────────────────────────── */

/*
 * Same helper and same reasoning as j36_mt6592_input: the region is NOT claimed.
 * pericfg is a syscon shared with the input driver, and the GIC distributor
 * belongs to irq-gic, which mapped it with of_iomap and would not survive
 * anything here asking for exclusive ownership of it.
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

static void j36_usb_phy_cancel_scan(void *data)
{
	struct j36_usb_phy *p = data;

	cancel_delayed_work_sync(&p->scan_work);
}

static int j36_usb_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct j36_usb_phy *p;
	int ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;

	p->phy = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->phy))
		return dev_err_probe(dev, PTR_ERR(p->phy), "map the U2 PHY window\n");

	p->pericfg = j36_iomap_phandle(dev, "j36,pericfg-controller");
	if (IS_ERR(p->pericfg))
		return dev_err_probe(dev, PTR_ERR(p->pericfg), "map PERICFG\n");

	/*
	 * Optional, and only for the measurement. A device tree without it gives
	 * a working PHY and no interrupt hunt, which is the right behaviour once
	 * the number is known and written down.
	 */
	p->gicd = j36_iomap_phandle(dev, "j36,gic-controller");
	if (IS_ERR(p->gicd)) {
		dev_info(dev, "no j36,gic-controller phandle: the ISPENDR scan is off\n");
		p->gicd = NULL;
	}

	/*
	 * Optional too, and mapped without claiming for the same reason as the
	 * others: the MUSB window belongs to musb_hdrc, which does claim it, and
	 * an exclusive request from here would stop that driver binding. This
	 * mapping is read-only apart from the one restored write in the layout
	 * probe, and both drivers can hold a mapping of the same page.
	 */
	p->musb = j36_iomap_phandle(dev, "j36,musb-controller");
	if (IS_ERR(p->musb)) {
		dev_info(dev, "no j36,musb-controller phandle: MUSB is not read back\n");
		p->musb = NULL;
	}

	/*
	 * Also optional, and for the same reason as the GIC: a device tree
	 * without it gives the port the behaviour it had before this was
	 * written, which is a working PHY that never sources 5 V. There is no
	 * gpiochip driver for MT6592 anywhere upstream, so the pad arrives as a
	 * plain number plus a phandle to the register block -- the same idiom
	 * j36_mt6592_input already uses on this board.
	 */
	p->vbus_pin = -1;
	p->gpio = j36_iomap_phandle(dev, "j36,gpio-controller");
	if (IS_ERR(p->gpio)) {
		dev_info(dev, "no j36,gpio-controller phandle: VBUS is not driven\n");
		p->gpio = NULL;
	} else {
		u32 pin;

		if (of_property_read_u32(dev->of_node, "j36,drvvbus-pad", &pin)) {
			dev_info(dev, "no j36,drvvbus-pad: VBUS is not driven\n");
		} else if (pin > J36_GPIO_PIN_MAX) {
			/* The same bound the preloader and the stock LK both
			 * enforce. Past it the bank arithmetic walks off the end
			 * of the block into whatever is mapped next. */
			dev_warn(dev, "j36,drvvbus-pad %u is above pad %u: ignored\n",
				 pin, J36_GPIO_PIN_MAX);
		} else {
			p->vbus_pin = pin;
		}
	}

	INIT_DELAYED_WORK(&p->scan_work, j36_usb_phy_scan);
	ret = devm_add_action_or_reset(dev, j36_usb_phy_cancel_scan, p);
	if (ret)
		return ret;

	p->generic = devm_phy_create(dev, dev->of_node, &j36_usb_phy_ops);
	if (IS_ERR(p->generic))
		return dev_err_probe(dev, PTR_ERR(p->generic), "create the PHY\n");
	phy_set_drvdata(p->generic, p);
	platform_set_drvdata(pdev, p);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(provider))
		return dev_err_probe(dev, PTR_ERR(provider), "register the PHY provider\n");

	/*
	 * Nothing has been written to hardware yet, and that is deliberate: probe
	 * runs at boot, phy_init runs when musb asks. A boot that never loads
	 * musb_hdrc leaves this window untouched.
	 */
	if (p->vbus_pin < 0)
		dev_info(dev, "MT6592 U2 PHY ready (host mode on power-on; VBUS is not driven, so the hub must be self-powered)\n");
	else if (!vbus)
		dev_info(dev, "MT6592 U2 PHY ready (host mode on power-on; VBUS held off by vbus=0, so the hub must be self-powered)\n");
	else
		dev_info(dev, "MT6592 U2 PHY ready (host mode on power-on; DRVVBUS pad %d will source 5 V off VBAT -- fit a cell)\n",
			 p->vbus_pin);
	return 0;
}

static const struct of_device_id j36_usb_phy_of_match[] = {
	{ .compatible = "j36,mt6592-usb-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, j36_usb_phy_of_match);

static struct platform_driver j36_usb_phy_driver = {
	.probe = j36_usb_phy_probe,
	.driver = {
		.name = "j36-mt6592-usb-phy",
		.of_match_table = j36_usb_phy_of_match,
	},
};
module_platform_driver(j36_usb_phy_driver);

MODULE_DESCRIPTION("J36 Ultra MT6592 USB 2.0 PHY for drivers/usb/musb");
MODULE_AUTHOR("MixOS / PowerEngine integration");
MODULE_LICENSE("GPL");
