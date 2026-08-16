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
 * ROLE, AND THE BUG THAT WAS IN THIS FILE. The tail of the stock recover
 * sequence -- clear 0x6c bit 4, set 0x6c 0x2e, set 0x6d 0x3e -- is not a
 * wake-up, it is MTK's force-DEVICE-mode override, which is why the stock LK
 * ends up a gadget. That much was right. What was NOT right is what this file
 * used to call the mirror of it, transcribed from the stock Android kernel at
 * 0xc052eaa4 (on its 0xf1210000 mapping of SIFSLV, so +0x86c is this file's
 * 0x6c) and from MVII's musb_phy_force_host():
 *
 *   c052eaa4:  strb r2, [r3, #0x86d]   // 0x6d |= 0x3c
 *   c052eacc:  strb r2, [r3, #0x86c]   // 0x6c |= 0x10
 *   c052eaf4:  strb r2, [r3, #0x86c]   // 0x6c &= 0xd3, i.e. clear 0x2c
 *
 * Those three writes are real and they are quoted correctly. They are not host
 * mode. They are the VBUS/session half of a power-DOWN, and the board said so
 * for as long as they ran: every MUSB dump in every log read
 *
 *   DEVCTL 80 [PERIPHERAL BDEV] VBUS below SessionEnd
 *
 * at power-on, three seconds later and nine seconds later alike -- B-device,
 * HOSTMODE clear, no session -- while this driver reported it had forced host
 * mode and was sourcing 5 V. Nothing enumerated: no stick, no SSD, no mouse, no
 * hub. The readout in this file predicted its own bug in so many words ("HM
 * clear on a node that asked for host means the role override did not take")
 * and the prediction sat in the log unread.
 *
 * WHY IT IS KNOWABLE NOW. 0x6c and 0x6d used to be bare offsets here because
 * naming a bit nobody has documented is worse than the number. They are not
 * undocumented: this whole block is the U2PHYDTM0/DTM1 pair that mainline's
 * phy-mtk-tphy.c drives on every later MediaTek part, and EIGHT of the fields
 * this file already writes match that map exactly, which is not a coincidence
 * eight times over:
 *
 *   this file                     byte.bit -> reg.bit   phy-mtk-tphy.c
 *   J36_PHY_R1A_GPIO_CTL  0x80    0x1a.7 -> 0x18 b23    PA6_RG_U2_BC11_SW_EN
 *   J36_PHY_R1A_VBUSCMP_EN 0x10   0x1a.4 -> 0x18 b20    PA6_RG_U2_OTG_VBUSCMP_EN
 *   J36_PHY_R68_FORCE     0xf4    0x68   -> 0x68 b2,4-7 RG_TERMSEL, RG_XCVRSEL,
 *                                                       RG_DPPULLDOWN, RG_DMPULLDOWN
 *   J36_PHY_R69_FORCE     0x3c    0x69   -> 0x68 b10-13 P2C_RG_DATAIN
 *   J36_PHY_R6A_FORCE     0xbe    0x6a   -> 0x68 b17-21,23  the five FORCE_* plus
 *                                                       FORCE_DATAIN
 *   J36_PHY_R6A_FORCE_SUSPENDM    0x6a.2 -> 0x68 b18    P2C_FORCE_SUSPENDM
 *   J36_PHY_R6B_FORCE_UART_EN     0x6b.2 -> 0x68 b26    P2C_FORCE_UART_EN
 *   J36_PHY_R6E_UART_EN   0x01    0x6e.0 -> 0x6c b16    P2C_RG_UART_EN
 *   J36_PHY_R00_INTR_EN   0x20    0x00.5 -> 0x00 b5     PA0_RG_USB20_INTR_EN
 *   J36_PHY_R68_SUSPENDM  0x08    0x68.3 -> 0x68 b3     P2C_RG_SUSPENDM
 *
 * So 0x6c is U2PHYDTM1[7:0] -- RG_IDDIG b1, RG_AVALID b2, RG_BVALID b3,
 * RG_SESSEND b4, RG_VBUSVALID b5 -- and 0x6d is U2PHYDTM1[15:8], the five
 * force_* enables for exactly those, force_iddig lowest. Decode the three
 * writes above with that map and they say: force everything except IDDIG,
 * assert SESSEND, deassert VBUSVALID and AVALID and BVALID.
 *
 * WHICH IS HALF OF A HOST SWITCH AND NOT A POWER-DOWN AT ALL, and the stock
 * MT6592 Android kernel -- in the tree now, at Reference/MT6592LK -- says which
 * half. It is the opening of musb_session_restart(): the deliberate
 * take-VBUS-away step that the restart of the session is meant to be seen
 * against. Copied on its own, with the session never restarted and RG_IDDIG
 * left at the 1 that recover()'s force-device tail put there, it really is a
 * port pinned to B-device with the session forced ended -- a port that cannot
 * start one -- which is what every early log said. It was not the wrong
 * sequence. It was the first three lines of the right one, and the rest of it
 * is below.
 *
 * The stock host switch also clears DEVCTL.SESSION in the MUSB core before it
 * touches the PHY at all. This file used to skip that and say why -- the bit
 * belongs to musb_core, which sets it itself from musb_start(). It does, once,
 * when the hub driver powers the root port at boot, and never again; that was
 * enough while the role was fixed at boot and is not enough now. See
 * j36_musb_session().
 *
 * AND CLEARING IT IS NOT OPTIONAL EVEN AT BOOT, which is the second half of the
 * same bug and took a second log to find. With the PHY sequences fixed the board
 * finally reported an A-device on a good rail --
 *
 *   DEVCTL 99 [SESSION PERIPHERAL BDEV ] VBUS above VBusValid, POWER 70   at power-on
 *   DEVCTL 19 [SESSION PERIPHERAL ADEV ] VBUS above VBusValid, POWER e0   at +3 s and +9 s
 *
 * -- and still enumerated nothing, because HM is clear in both. MUSB decides
 * which end of the cable it is when a session STARTS. The LK starts one: it
 * brings this port up as a high-speed peripheral so the board can be flashed over
 * it, and 0x99 with POWER 0x70 is exactly that, handed to the kernel still
 * running. Forcing IDDIG low afterwards flips BDEVICE to 0 and changes nothing
 * else, and neither j36_musb_session() -- which returns early on a bit that
 * already reads the way it wants -- nor musb_start() -- which folds
 * `devctl &= ~SESSION' and `devctl |= SESSION' into one write -- ever produces the
 * 1 -> 0 -> 1 that would make the core ask again. So it stayed the peripheral the
 * bootloader made it, the root hub had one port it never scanned, and the log
 * says "1 port detected" and then nothing for the rest of the boot. Not a display
 * fault: NOTHING enumerates on that port, hub or stick or mouse alike.
 * j36_musb_stand_down() is the missing edge, and j36_musb_host_kick() is the poll
 * checking that it took.
 *
 * ── AND THE EDGE WAS NOT ENOUGH EITHER, AND THE CLOCK WAS NOT WHY ────────────
 *
 * The stand-down landed and the port still enumerated nothing, and the theory
 * that followed was that MUSB had no USB-side clock. Everything fitted it: HM
 * never setting, BDEVICE not tracking a forced ID, HSMODE stuck at whatever the
 * bootloader's high-speed gadget left in it, and above all
 *
 *   41: 0 0 0 0 0 0 0 0  MT_SYSIRQ 64 Level musb-hdrc.3.auto
 *
 * an interrupt line that had never fired on any of eight CPUs in the whole
 * uptime, when CONNECT, RESET, SESSREQ, VBUS_ERROR and SOF are all generated off
 * the 60 MHz the PHY hands back. So j36_phy_clock_on() was written: mainline's
 * u2_phy_instance_init() sets PA0_RG_USB20_INTR_EN in USBPHYACR0 under the
 * comment "switch to USB function, and enable usb pll", the LK's savecurrent()
 * clears it on its way out, and no routine in this file had ever written it.
 *
 * IT WAS ALREADY SET. The next log says so in the line the write itself prints:
 *
 *   PHY circuits enabled: USBPHYACR0 6e -> 6e (RG_USB20_INTR_EN was already set)
 *
 * and every dump after it agrees -- USBPHYACR0 6e, SuspendM 1 from the link,
 * L1INTM 0000000f once mediatek.c has unmasked the aggregator. The clock is
 * there, the L1 aggregator is open, the interrupt is wired to the SPI the device
 * tree names. Nothing has fired because nothing has HAPPENED: HM is still clear,
 * so the root hub still has one port it never scans, so nothing on that port is
 * ever reset or addressed and no event exists to raise. The write stays -- it is
 * mainline's and it costs one byte -- but it was never the fault, and this file
 * should stop saying it was.
 *
 * ── WHAT THE FAULT IS: MT6592 HAS NO VBUS SENSING IP ─────────────────────────
 *
 * The stock MT6592 Android kernel settles it, in its own comment, in
 * Reference/MT6592LK/.../usb20/usb20_host.c. musb_id_pin_work(), on the branch
 * that becomes a host:
 *
 *	musb_platform_set_vbus(mtk_musb, 1);
 *
 *	// for no VBUS sensing IP
 *	// wait VBUS ready
 *	msleep(100);
 *	// clear session
 *	musb_writeb(mregs, MUSB_DEVCTL, devctl & ~MUSB_DEVCTL_SESSION);
 *	// USB MAC OFF: VBUSVALID=0, AVALID=0, BVALID=0, SESSEND=1, IDDIG=X
 *	USBPHY_SET8(0x6c, 0x10);
 *	USBPHY_CLR8(0x6c, 0x2e);
 *	USBPHY_SET8(0x6d, 0x3e);
 *	// wait
 *	msleep(5);
 *	// restart session
 *	musb_writeb(mregs, MUSB_DEVCTL, devctl | MUSB_DEVCTL_SESSION);
 *	// USB MAC ON and Host Mode: VBUSVALID=1, AVALID=1, BVALID=1, SESSEND=0, IDDIG=0
 *	USBPHY_CLR8(0x6c, 0x10);
 *	USBPHY_SET8(0x6c, 0x2c);
 *	USBPHY_SET8(0x6d, 0x3e);
 *
 *	musb_start(mtk_musb);
 *
 * Read the ORDER rather than the bytes. The valids are taken DOWN before the
 * session is started and put back UP after it, so what the core sees while it is
 * arbitrating is VBUS RISING. That is the whole trick, and the comment names the
 * reason: this part has no VBUS sensing IP. DEVCTL's VBUS field is not a
 * comparator on the pin unless the PHY is told to make it one -- it is whatever
 * U2PHYDTM1 was last forced to -- and a level that was already high before the
 * session began is not an event. MUSB latches HOSTMODE on the transition.
 *
 * Everything this file did was the other way round. force_host() asserted the
 * valids and only THEN was SESSION set: at power-on, on every role change, and
 * on all four host kicks. The core was handed a session that was already,
 * statically, on a valid bus, found nothing to arbitrate, and stayed whatever it
 * had been. Which is exactly what the log holds still at:
 *
 *   DEVCTL 19 at power-on   A-device, SESSION, VBUS above VBusValid, HM clear
 *   DEVCTL 99 at every poll B-device, SESSION, VBUS above VBusValid, HM clear
 *
 * -- and 0x19 and 0x99 are not arbitrary numbers either. They are
 * MUSB_QUIRK_A_DISCONNECT_19 and MUSB_QUIRK_B_DISCONNECT_99, the two values
 * musb_core keeps named constants for, because they are what this core reads
 * when a session is running and the far end was never seen.
 *
 * j36_musb_host_arm() is that sequence in that order, and every path that wants
 * host mode goes through it now.
 *
 * TWO SMALLER THINGS THE STOCK SOURCE CORRECTS, both wrong here before:
 *
 *   B-VALID IS SET IN HOST MODE. 0x2c is AVALID | BVALID | VBUSVALID. This file
 *   used to clear BVALID on the host path on the grounds that it belonged to the
 *   other role; MediaTek asserts all three in both roles and moves only SESSEND
 *   and IDDIG between them.
 *
 *   THE GAP IS 5 ms, not the hundred that was guessed at. The hundred belongs
 *   BEFORE the whole thing, waiting for the rail -- and it is spent with the
 *   session down, which is what J36_VBUS_RISE_MS was really fighting: musb's own
 *   A-device VBUS timer cannot expire during a session that has not started.
 *
 * AND THERE IS A SECOND ARM, also stock, for when the first does not take.
 * musb_session_restart() -- the same file's VBUS_ERROR recovery -- ends by
 * RELEASING the four value forces instead of asserting them:
 *
 *	USBPHY_CLR8(0x6d, 0x3c);   // stop forcing avalid/bvalid/sessend/vbusvalid
 *	USBPHY_CLR8(0x6c, 0x3c);
 *	musb_writeb(mregs, MUSB_DEVCTL, devctl | MUSB_DEVCTL_SESSION);
 *
 * with force_iddig left on, so the role is still ours while the VBUS state comes
 * from the PHY's own comparator on the actual pin -- which recover() enables, and
 * which is what R1A bit 4 is. On this board DRVVBUS really is driven, so that
 * comparator really should read valid. The host kick alternates the two: odd
 * attempts force the valids, even attempts hand them back to the comparator. If
 * a sensed attempt comes back "VBUS below SessionEnd" then the 5 V this driver
 * believes it is sourcing is not reaching the PHY, and that is a different fault
 * in a different place -- the load switch, the pad, or the pad's mode -- which is
 * worth being able to tell apart from one boot log.
 *
 * SUSPENDM is a knob rather than a decision, and it stays one. UTMI's
 * SuspendM gates the PHY's clock output, and once the force bit is clear -- as
 * mainline leaves it, as recover() leaves it -- the link drives it, which means
 * MUSB does. MUSB holds it high while POWER.ENSUSPEND is 0, and POWER.ENSUSPEND
 * is 0 in every dump this board has produced, so the default follows mainline
 * and lets the link have it. If the port is still dead with the circuits on,
 * phy_suspendm=1 pins SuspendM high in the PHY instead and takes MUSB out of
 * that loop entirely -- from the kernel command line, without a rebuild, which
 * on a board that has to be reflashed to try an idea is the difference between
 * a reboot and an afternoon.
 *
 * ── WHICH ROLE, THOUGH, AND WHY THE ANSWER STOPPED BEING A MEASUREMENT ────────
 *
 * THIS BOARD HAS TWO CONNECTORS. A DC inlet, which charges and has no data lines
 * in it, and this port, which carries the data. CHRIN hangs off the inlet;
 * DRVVBUS hangs off this port; they are separate sockets on separate nets and
 * neither one can be mistaken for the other.
 *
 * Everything below was written against the opposite belief -- one socket doing
 * both jobs, mutually exclusive in hardware, so the role had to be measured on
 * every poll because no device-tree constant could pick between two things one
 * cable might be. On a board with a dedicated inlet there is nothing to pick.
 * The data port is a host, it sources 5 V, and the charger arrives somewhere
 * else entirely. So the default is now vbus=1, and what that buys is the thing
 * the measurement was costing: the port holds still. Measuring means dropping
 * DRVVBUS, and dropping DRVVBUS on the port that IS the data port is a hundred
 * millisecond power cut to whatever is plugged into it, every role_probe_every
 * polls, for the life of the board.
 *
 * THE MEASUREMENT IS STILL HERE, under vbus=-1, because the belief is true of
 * some boards and this driver should still work on them. With DRVVBUS low,
 * DEVCTL's VBUS field is a live comparator on the pin (recover() enables it:
 * that is what R1A bit 4 is), and the force_* overrides are the only thing that
 * hides it -- release them for the 800 us the stock sequences settle for and the
 * reading is real:
 *
 *   above AValid   somebody outside this board is driving the bus. A charger,
 *                  or a PC. Be a device, leave DRVVBUS low, let the PMIC see
 *                  its own CHRDET and charge.
 *   below AValid   nothing is feeding the port. Be a host and source 5 V, and a
 *                  bus-powered stick, SSD, mouse or hub comes up.
 *
 * Re-asked every role_poll_ms, because on such a board plugging a charger into a
 * running console is the normal case. The measurement needs our own boost off, so
 * the poll drops DRVVBUS for J36_VBUS_FALL_MS before reading -- and skips the
 * whole thing whenever DEVCTL reports FSDEV or LSDEV, which is the core's own
 * pre-enumeration attach flag.
 *
 * That last skip was too broad for one case, and the case is the charger.
 * FSDEV/LSDEV is a pull-up on D+ or D-, and a divider-type charger presents one
 * -- so it read as attached, the probe stayed suspended, DRVVBUS stayed high and
 * the PMIC's interlock refused to charge for the whole uptime. A charger is not a
 * device and never becomes one: it answers no reset and is given no address. So
 * the latch asks usbcore whether anything on the bus has an address, and only
 * holds when something has. Nothing that enumerates is ever power-cycled by the
 * poll; something that holds the lines high and never enumerates gets measured
 * once, after attach_grace_polls. See j36_usb_devices() and
 * j36_usb_phy_decide_role().
 *
 * vbus= is therefore the whole choice: vbus=1 (default) is host always and
 * sourcing always, which is what a board with its own charge inlet wants;
 * vbus=-1 is the measurement above, for a board where the two are one socket;
 * vbus=0 forbids sourcing without forbidding host, which is the self-powered-hub
 * case and the no-cell case, and it measures too.
 *
 * .set_mode still picks between the two sequences for anything that asks, but
 * in auto it answers a host request without acting on it: mediatek.c calls it
 * once, straight after power_on, with the device tree's dr_mode, and honouring
 * that would undo the measurement that had just been taken three lines earlier.
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
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
/*
 * usbcore, for usb_for_each_dev() alone -- see j36_usb_devices().  It is the
 * only question this driver cannot answer from a register: "did the thing on the
 * port ever get a USB address", which is what separates a device from a charger
 * that holds the same line high.  usbcore is already this module's one `modinfo
 * -F depends' edge and already loads ahead of it, so this costs no new module and
 * no change to load.order.
 */
#include <linux/usb.h>
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
/* Both of these are peripheral-only bits, and both were missing from this list,
 * which is why "POWER e0 [HSENAB ]" looked harmless in a log that was reporting
 * the fault: e0 is ISOUPDATE | SOFTCONN | HSENAB, and SOFTCONN is the D+ pull-up
 * a peripheral raises to say "I am here". A port that has decided to be the host
 * has no business holding it. They are named in the dump now. */
#define J36_MUSB_POWER_SOFTCONN		BIT(6)
#define J36_MUSB_POWER_ISOUPDATE	BIT(7)
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
/*
 * USBPHYACR0[7:0], and bit 5 of it is the one register in this whole file that
 * decides whether MUSB has a clock. PA0_RG_USB20_INTR_EN enables the PHY's
 * internal circuitry -- the 48 MHz reference, the PLL, and the 60 MHz UTMI clock
 * the controller's entire USB-side state machine runs on. With it clear the core
 * still answers every APB read, still takes a write to DEVCTL.SESSION, and
 * arbitrates nothing: HM never sets, BDEVICE never tracks the forced ID, HSMODE
 * stays at whatever the bootloader left in it, and not one interrupt is raised
 * for the life of the boot. See the file header.
 */
#define J36_PHY_R00			0x00	/* USBPHYACR0[7:0] */
#define J36_PHY_R00_INTR_EN		0x20	/* PA0_RG_USB20_INTR_EN */

#define J36_PHY_R06			0x06	/* VRT / TERM_VREF trim, [2:0] kept */
#define J36_PHY_R06_TRIM_KEEP		0x07
#define J36_PHY_R06_TRIM_DEFAULT	0x68
#define J36_PHY_R1A			0x1a
#define J36_PHY_R1A_GPIO_CTL		0x80	/* rg_usb20_gpio_ctl */
/*
 * RG_USB20_OTG_VBUSSCMP_EN -- the PHY's own comparator on the VBUS pin. The
 * stock recover() sets it and savecurrent() clears it, and it is what makes the
 * sensed arm in j36_musb_host_arm() mean anything: with the four value forces
 * released, this comparator is the only thing driving AVALID, BVALID, SESSEND
 * and VBUSVALID, and therefore the only thing DEVCTL's VBUS field reports.
 */
#define J36_PHY_R1A_VBUSCMP_EN		0x10
#define J36_PHY_R1D			0x1d
#define J36_PHY_R1D_BIT4		0x10
#define J36_PHY_R22			0x22
#define J36_PHY_R22_PULLDOWN		0x03	/* DP/DM 100k pull-downs */
#define J36_PHY_R68			0x68
#define J36_PHY_R68_FORCE		0xf4
/* Bit 3 is deliberately NOT in J36_PHY_R68_FORCE above and never has been: it is
 * P2C_RG_SUSPENDM, the VALUE half of the suspend override, and it gates the
 * clock the controller runs on rather than being one more line-state force. */
#define J36_PHY_R68_SUSPENDM		0x08
#define J36_PHY_R69			0x69
#define J36_PHY_R69_FORCE		0x3c
#define J36_PHY_R6A			0x6a
#define J36_PHY_R6A_FORCE		0xbe
#define J36_PHY_R6A_FORCE_SUSPENDM	0x04
#define J36_PHY_R6B			0x6b
#define J36_PHY_R6B_FORCE_UART_EN	0x04
/* 0x6c and 0x6d are the two low bytes of U2PHYDTM1 and they carry the role, the
 * session state and the force_* enables that decide whether any of it is the
 * PHY's own comparators or a value written from here. These used to be bare
 * offsets with a note saying that guessing IDDIG from the surrounding bits was
 * not a guess worth making; the eight-field correspondence in the file header is
 * what turned it from a guess into a read, and the bug it found is described
 * there too. Bit for bit these are phy-mtk-tphy.c's P2C_RG_* and P2C_FORCE_*. */
#define J36_PHY_R6C			0x6c	/* U2PHYDTM1[7:0] */
#define J36_PHY_R6C_IDDIG		0x02	/* 1 = B-device, 0 = A-device */
#define J36_PHY_R6C_AVALID		0x04
#define J36_PHY_R6C_BVALID		0x08
#define J36_PHY_R6C_SESSEND		0x10
#define J36_PHY_R6C_VBUSVALID		0x20
#define J36_PHY_R6D			0x6d	/* U2PHYDTM1[15:8] -- the force_* enables */
#define J36_PHY_R6D_FORCE_IDDIG		0x02
#define J36_PHY_R6D_FORCE_AVALID	0x04
#define J36_PHY_R6D_FORCE_BVALID	0x08
#define J36_PHY_R6D_FORCE_SESSEND	0x10
#define J36_PHY_R6D_FORCE_VBUSVALID	0x20
#define J36_PHY_R6D_FORCE_ALL		(J36_PHY_R6D_FORCE_IDDIG | \
					 J36_PHY_R6D_FORCE_AVALID | \
					 J36_PHY_R6D_FORCE_BVALID | \
					 J36_PHY_R6D_FORCE_SESSEND | \
					 J36_PHY_R6D_FORCE_VBUSVALID)
/*
 * FORCE_ALL without force_iddig, and the four RG_* values it covers: 0x3c in
 * both bytes, which is what musb_session_restart() clears to hand the VBUS state
 * back to the PHY's own comparator while keeping the role forced. See
 * j36_phy_sense_vbus().
 */
#define J36_PHY_R6D_FORCE_VBUS		(J36_PHY_R6D_FORCE_AVALID | \
					 J36_PHY_R6D_FORCE_BVALID | \
					 J36_PHY_R6D_FORCE_SESSEND | \
					 J36_PHY_R6D_FORCE_VBUSVALID)
#define J36_PHY_R6C_VBUS_ALL		(J36_PHY_R6C_AVALID | J36_PHY_R6C_BVALID | \
					 J36_PHY_R6C_SESSEND | J36_PHY_R6C_VBUSVALID)

/* Device: ID high so the core is a B-device, the session valid and VBUS present,
 * which is the state a gadget plugged into a host is in. 0x6c ends at 0x2e and
 * 0x6d at 0x3e, which is the stock recover() tail unchanged -- this is the one
 * of the two sequences that was already right. */
#define J36_PHY_R6C_DEV_SET		(J36_PHY_R6C_IDDIG | J36_PHY_R6C_AVALID | \
					 J36_PHY_R6C_BVALID | J36_PHY_R6C_VBUSVALID)
#define J36_PHY_R6C_DEV_CLR		J36_PHY_R6C_SESSEND

/*
 * Host: ID low so the core is an A-device, all three valids asserted, session
 * explicitly not ended. 0x6c ends at 0x2c, which is
 *
 *	USBPHY_CLR8(0x6c, 0x10);   // SESSEND = 0
 *	USBPHY_SET8(0x6c, 0x2c);   // AVALID = BVALID = VBUSVALID = 1
 *
 * from the stock host switch, byte for byte. BVALID used to be in the CLR list
 * here on the theory that it belonged to the other role; MediaTek asserts all
 * three in both roles and moves only SESSEND and IDDIG between them.
 */
#define J36_PHY_R6C_HOST_SET		(J36_PHY_R6C_AVALID | J36_PHY_R6C_BVALID | \
					 J36_PHY_R6C_VBUSVALID)		/* 0x2c */
#define J36_PHY_R6C_HOST_CLR		(J36_PHY_R6C_IDDIG | J36_PHY_R6C_SESSEND)

/*
 * And the state the PHY is parked at IN BETWEEN, which is the step this file
 * never had and the whole of the fix: MediaTek's "USB MAC OFF". Session ended,
 * every valid deasserted, RG_IDDIG already low so the role the session is about
 * to be arbitrated against is A. 0x6c gets 0x10 set and 0x2e cleared, exactly as
 * the stock switch does it, and the session is started from here so that the
 * valids RISE inside it. See j36_phy_park_idle() and the file header.
 */
#define J36_PHY_R6C_IDLE_SET		J36_PHY_R6C_SESSEND		/* 0x10 */
#define J36_PHY_R6C_IDLE_CLR		(J36_PHY_R6C_IDDIG | J36_PHY_R6C_AVALID | \
					 J36_PHY_R6C_BVALID | \
					 J36_PHY_R6C_VBUSVALID)		/* 0x2e */

#define J36_PHY_R6E			0x6e
#define J36_PHY_R6E_UART_EN		0x01

/* The stock routines' own settle time, in microseconds. */
#define J36_PHY_SETTLE_US		800

/* And how long a ROLE change is given on top of it, in milliseconds, before
 * anything is allowed to put a session edge on DEVCTL. See j36_phy_force_host().
 *
 * Fifty and not a smaller round number because fifty is the gap the one boot
 * that latched A-device actually had -- decide_role()'s VBUS rise time, which
 * happened to sit between the role write and the session edge -- and every boot
 * that latched B-device had the 800 us above and nothing else. This is not a
 * timing margin picked for comfort; it is the measurement. */
#define J36_PHY_ROLE_SETTLE_MS		50

/* How long the port is given to fall after DRVVBUS is dropped, before DEVCTL's
 * VBUS field is read to find out whether anything ELSE is holding it up. It is
 * an unloaded net with the load switch open, so this is discharging the
 * connector's own capacitance and nothing else; 60 ms is far more than that
 * needs and still short enough that a device plugged during the gap only loses
 * one attach attempt, which musb retries. It is only ever spent with nothing
 * attached -- see j36_port_attached(). */
#define J36_VBUS_FALL_MS		60

/*
 * And how long the RAIL is given to come up, before anything else in the arm
 * happens at all.  This is MediaTek's own "wait VBUS ready" and it is their
 * number: msleep(100), immediately after musb_platform_set_vbus(1) and before
 * the session is touched.
 *
 * It used to be 50 and it used to sit in the wrong place -- between the role
 * write and the session edge -- where it was fighting musb's A-device VBUS
 * timer.  That timer gives the VBUS field about a hundred milliseconds to reach
 * AValid after a session is granted, and missing it is
 *
 *     VBUS_ERROR in a_idle (80, <SessEnd), retry #0
 *
 * which is what MVII's log was full of.  Spending the rise time BEFORE the
 * session is started takes the whole race away: the timer cannot expire during a
 * session that has not begun, and by the time one has, the load switch has been
 * closed for a tenth of a second.
 */
#define J36_VBUS_RISE_MS		100

/*
 * And the gap between "USB MAC OFF" and the session restart, which is the one
 * delay in the arm that is load-bearing rather than generous: it is how long the
 * core is given to observe a dead bus before it is asked to start a session on
 * one.  Five milliseconds, because five is what the stock host switch sleeps
 * there -- this file guessed a hundred before the source was in the tree.
 */
#define J36_PHY_MAC_OFF_MS		5

/*
 * And how long DEVCTL.SESSION is held DOWN in between, which is the other half
 * of the same story and the one that took a second log to find.
 *
 * MUSB decides which end of the cable it is when a session STARTS, and never
 * again while that session runs. The bootloader starts one: MVII's LK brings the
 * port up as a peripheral so it can be flashed over USB, and it hands the kernel
 * DEVCTL 0x99 -- SESSION set, BDEVICE set, VBUS above VBusValid -- with POWER 0x70,
 * SOFTCONN and HSMODE, a B-device that has actually chirped high-speed at
 * somebody. Forcing IDDIG low after that flips BDEVICE to 0, and DEVCTL duly
 * reads ADEV, and the core goes on being the peripheral it became, because
 * nothing asked it the question again.
 *
 * "Ask it again" is a real 1 -> 0 -> 1 on this bit with the PHY already forced to
 * A. Neither of the two places that could have done it did: this driver's own
 * j36_musb_session() returns early when the bit already reads the way it wants,
 * and musb_start() computes `devctl &= ~SESSION' and `devctl |= SESSION' into one
 * value and writes it once, so the core sees no edge either. The result is in
 * every log this board has produced -- DEVCTL 19 at three seconds and again at
 * nine, SESSION set, ADEV, VBUS good, and HM clear -- and HM clear is a root hub
 * that never scans its one port. Nothing enumerates on that port: not a display
 * adapter, not a hub, not a stick, not a mouse.
 *
 * 20 ms is far longer than the core needs to retire a session -- it is a state
 * machine, not a bus transaction -- and short enough to disappear inside the
 * 50 ms rail rise that follows it.
 */
#define J36_MUSB_SESSION_GAP_MS		20

/* How many times the poll will re-arm a session that came up without HM before
 * it gives up and says so once. Four is a retry rather than a loop: each attempt
 * costs about a third of a second of sleeps inside the poll worker and they are
 * spread over the first fifteen seconds of uptime, and if the core will not take
 * host mode after four full stock arms the fault is not the edge.
 *
 * Four is also the smallest count that runs BOTH arms twice, which matters now
 * that the attempts alternate: odd ones force the valids and even ones hand them
 * to the PHY's comparator, so a board that fails every attempt still produces one
 * measured reading of the actual VBUS pin per pair. See j36_musb_host_kick() and
 * j36_phy_sense_vbus(). One of the things the retries may also have to outlast is
 * MUSB's own A-device connect timeout, which drops the session about a second
 * after it is granted if no connect interrupt arrives; giving up while that race
 * is still open is giving up too early. */
#define J36_MUSB_HM_KICKS		4u

/*
 * And how long the core is given to answer after a session is started, before
 * anything reads DEVCTL and calls the result a failure.
 *
 * This file used to read DEVCTL on the line after the write that set SESSION --
 * microseconds later, off the same CPU -- and print "after the restart DEVCTL
 * reads 99: the core is still not the host". Which it was not, yet: HM and
 * BDEVICE are set by the core once it has sampled ID and the VBUS comparators
 * over its own clock, and that is a state machine taking its own time, not a
 * register that changes with the store. The reading was therefore guaranteed to
 * say "not the host" whether the role had taken or not, which spent both retries
 * and produced a log line that has been wrong in three consecutive boots.
 *
 * 150 ms is comfortably past MUSB's own hundred-millisecond A-device window and
 * still short enough to sit inside one role poll, and the wait is skipped the
 * moment HM appears, so on a working port it costs one register read.
 */
#define J36_MUSB_SETTLE_MS		150u
#define J36_MUSB_SETTLE_STEP_MS		5u

/* DEVCTL's VBUS field is four thresholds; this is the lowest one that means
 * somebody is actually driving the bus rather than leakage holding it off the
 * floor. */
#define J36_MUSB_VBUS_AVALID		2

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

static int vbus = 1;
module_param(vbus, int, 0644);
MODULE_PARM_DESC(vbus,
		 "who drives the 5 V on the OTG port. "
		 "1 (default) is host always and sourcing always, which is what a "
		 "board with a separate DC charge inlet wants: the inlet has no "
		 "data lines, CHRDET is never this port's 5 V, and the data port "
		 "can simply stay up so a stick, an SSD, a mouse or a bus-powered "
		 "hub keeps working. "
		 "-1 measures it instead, for a board where the charger and the "
		 "port are one socket: with the DRVVBUS pad named by "
		 "j36,drvvbus-pad held low, DEVCTL's VBUS field says whether "
		 "anything outside is feeding the port, and the role follows -- "
		 "fed means a charger or a PC, so be a device and let the PMIC "
		 "charge; unfed means be a host and source 5 V. The cost is that "
		 "measuring drops the pad, so the port loses 5 V for about a tenth "
		 "of a second every role_probe_every polls -- see that parameter, "
		 "and see chrin_shared in j36_mt6592_pmic for the other half. "
		 "vbus=0 forbids SOURCING without pinning the role -- the port is "
		 "still measured, it just never drives the pad -- and it drives "
		 "the pad LOW rather than leaving it as found. Two cases want it: "
		 "a self-powered hub, which brings its own 5 V, and a board with "
		 "no cell fitted, where the boost comes off VBAT and VBAT here is "
		 "the system node. "
		 "Writable at runtime; the next role poll picks it up.");

static unsigned int role_poll_ms = 3000;
module_param(role_poll_ms, uint, 0644);
MODULE_PARM_DESC(role_poll_ms,
		 "how often to re-ask which way the port is being driven, in ms "
		 "(default 3000, 0 stops the poll and freezes whatever the "
		 "power-on measurement decided). Plugging a charger into a running "
		 "console is the normal case and there is no interrupt for it on "
		 "this board, so it is a poll. It is skipped entirely whenever "
		 "something that has enumerated is attached to the port, and "
		 "while the port is an idle host only every role_probe_every'th "
		 "poll pays for the DRVVBUS drop -- see that parameter for why "
		 "the drop is not something to do twenty times a minute, and "
		 "attach_grace_polls for what happens when the thing on the port "
		 "holds the lines high without ever becoming a device.");

/*
 * ── THE ARM THAT RAN TOO EARLY, AND THE BOOT THAT NEVER CAME BACK ────────────
 *
 * .power_on is not a callback that happens near musb; it happens INSIDE it.
 * mtk_musb_init() is musb's platform .init, musb_init_controller() calls it as
 * its first act, and phy_power_on() is three lines into that. So everything
 * .power_on did used to run in the middle of musb_init_controller(), before the
 * core had read its own config, before musb_generic_disable(), before the IRQ
 * was requested and long before musb_start(). What it did there was not a
 * register read: it raised DRVVBUS, slept a hundred milliseconds, wrote DEVCTL
 * and POWER, parked the PHY, slept again, started a session and forced the
 * valids -- about a sixth of a second of live sequencing on the MAC and on an
 * external power net, interleaved with a core that was mid-initialisation on
 * the same registers.
 *
 * AND IT WAS ALREADY KNOWN TO BE POINTLESS. The comments in
 * j36_usb_phy_decide_role() and j36_musb_host_kick() say it in as many words:
 * musb_generic_disable() writes DEVCTL = 0 a moment later and musb_start()
 * begins its own session from the hub thread, so whatever is armed here is torn
 * down before anything can use it, and the arm that has to win is the one the
 * first role poll runs after all of that has happened. Three seconds later, on
 * a bus nobody else is touching, from a workqueue.
 *
 * Which leaves a sixth of a second of the riskiest writes this driver makes,
 * placed at the one moment they race the core, buying nothing. On this SoC a
 * bad access to a peripheral is not an oops -- it stalls the bus and takes the
 * display with it, and the report is a handheld frozen with `mediatek.ko' as
 * the last word on the panel. That is not proof this was the stall; the same
 * code has booted this board hundreds of times and the fault is intermittent,
 * which is exactly what a race looks like from the outside. It is the reason
 * not to keep doing it: the deferred order is strictly closer to what musb
 * expects, it is what the file's own comments already recommend, and it costs
 * one poll interval of a port that is not yet sourcing 5 V.
 *
 * =1 puts it back, for bisecting against a board that regresses.
 */
static bool role_at_power_on;
module_param(role_at_power_on, bool, 0644);
MODULE_PARM_DESC(role_at_power_on,
		 "decide and apply the role from inside .power_on, which is inside "
		 "musb_init_controller() (default off: the first role poll does it "
		 "instead, after musb_start(), which is the arm that survives). Only "
		 "affects the first application; the poll owns the role either way.");

static unsigned int role_probe_every = 5;
module_param(role_probe_every, uint, 0644);
MODULE_PARM_DESC(role_probe_every,
		 "vbus=-1 only, and dead at the default. How many role polls to let "
		 "pass between two DRVVBUS drops while "
		 "the port is a host and idle (default 5, so one probe every 15 s "
		 "at the 3000 ms poll; 1 probes every poll, 0 probes once at "
		 "power-on and never again). Only the host answer costs anything "
		 "to re-ask: a port already standing down has its pad low, so the "
		 "measurement is free and runs every poll however this is set. "
		 "The drop cycles 5 V on the connector, which is why it is no "
		 "longer done every three seconds -- a device plugged in during "
		 "the gap loses an attach attempt, and a port that gives one up "
		 "twenty times a minute is a port nothing settles on.");

static unsigned int attach_grace_polls = 3;
module_param(attach_grace_polls, uint, 0644);
MODULE_PARM_DESC(attach_grace_polls,
		 "vbus=-1 only, and dead at the default. How many role polls "
		 "something may hold D+ or D- high without "
		 "getting a USB address before the port is measured anyway "
		 "(default 3, so about nine to twelve seconds at the 3000 ms "
		 "poll; 0 measures on the first poll of every attach). On a "
		 "shared-socket board this is what makes a divider-type charger "
		 "charge: it drives the same "
		 "line a device's pull-up does, so DEVCTL calls it attached and "
		 "the attach latch used to hold DRVVBUS high for the whole "
		 "uptime, which is exactly the state such a board's PMIC refuses "
		 "to charge in. Anything that enumerates is exempt for as long as it stays "
		 "on the bus, so a stick, a mouse or a hub is never power-cycled "
		 "by this however low it is set; raise it if some device on this "
		 "board takes longer than that to get an address.");

static bool musb_session = true;
module_param(musb_session, bool, 0644);
MODULE_PARM_DESC(musb_session,
		 "end and restart the MUSB session across a role change, by "
		 "clearing and setting DEVCTL.SESSION (default on). musb_core "
		 "sets that bit once, from musb_start(), when the hub driver "
		 "powers the root port at boot -- so a port that becomes a "
		 "charger port and then a host port again has nobody to restart "
		 "it. The stock host switch clears the same bit for the same "
		 "reason. musb_session=0 backs it out; host mode then works "
		 "until the first role change and not after it.");

static int phy_suspendm = -1;
module_param(phy_suspendm, int, 0644);
MODULE_PARM_DESC(phy_suspendm,
		 "who drives UTMI SuspendM, which gates the 60 MHz the MUSB core "
		 "runs its whole USB side on. -1 (default) is mainline's answer "
		 "and this board's: clear P2C_FORCE_SUSPENDM and let the link "
		 "have it, because MUSB holds it high while POWER.ENSUSPEND is 0 "
		 "and that bit is 0 in every dump this board has produced. "
		 "phy_suspendm=1 pins it high inside the PHY instead, taking the "
		 "controller out of the loop -- try that from the kernel command "
		 "line if the port still enumerates nothing with the PHY's "
		 "circuits enabled, because it needs a reboot rather than a "
		 "reflash. phy_suspendm=0 forces the PHY INTO suspend and is a "
		 "diagnostic only: it is what a dead port looks like, on purpose, "
		 "so the symptom can be confirmed from the other direction.");

static bool musb_probe_layout;
module_param(musb_probe_layout, bool, 0444);
MODULE_PARM_DESC(musb_probe_layout,
		 "at power-on, write a pattern to TXFUNCADDR at both candidate "
		 "multipoint bases (0x080 and 0x480) and report which one holds "
		 "it, then restore. Off by default because it has already been "
		 "run and answered: 0x480, the MT2701 layout mediatek.c assumes "
		 "unmodified. Turn it back on to re-ask on a board revision that "
		 "does not enumerate. Runs before musb_core starts, so no "
		 "transfer can be in flight.");

struct j36_usb_phy {
	struct device *dev;
	void __iomem *phy;
	void __iomem *pericfg;
	void __iomem *gicd;
	void __iomem *gpio;
	void __iomem *musb;
	struct phy *generic;

	int vbus_pin;			/* -1 when the device tree names none */
	/* Tri-state, -1 until this driver has written the pad, and that third
	 * value is load-bearing: the level the LK left is not knowable from the
	 * SET/RST alias, so "never written" has to be distinct from "written low"
	 * or the first drive-low gets optimised away as a no-op. */
	int vbus_on;

	struct delayed_work scan_work;
	u32 pending_baseline[J36_GICD_ISPENDR_WORDS];
	unsigned int scans_left;

	/* Tri-state again, and again load-bearing: -1 until a role has been
	 * decided, so the first decision logs and applies even when it agrees
	 * with what the sequences happen to have left behind. */
	int role_host;
	/* Polls since the last DRVVBUS drop.  Counts only while the port is a
	 * host, because that is the only role whose re-measurement costs the
	 * connector a power cycle -- see role_probe_every. */
	unsigned int polls_since_probe;
	/* Edge flag for the one dev_info the attach latch emits.  The latch runs
	 * every poll for as long as something is on the port, and a line per poll
	 * for the life of the board is a line nobody reads; this makes it a line
	 * per plug instead.  Nothing branches on it. */
	bool attach_logged;
	/* Polls this run of "something is holding D+ or D- high" has lasted
	 * without anything appearing on the USB bus.  Reset the moment the port
	 * reads clear, or the moment a device does enumerate. */
	unsigned int attach_polls;
	/* One measurement per attached run, and this is what makes it one: a
	 * thing that held the lines high, never enumerated and then measured as
	 * NOT feeding the port is a device after all, and re-measuring it every
	 * grace period would power-cycle it forever. */
	bool attach_probed;
	/* Times the poll has re-bounced the session because the core came up as
	 * host and DEVCTL.HM stayed clear. Bounded by J36_MUSB_HM_KICKS and reset
	 * on every role change, so a port that is unplugged and replugged gets its
	 * retries back and a port that simply cannot host says so once. */
	unsigned int hm_kicks;
	/* Edge flag for that one complaint. */
	bool hm_gave_up;
	/* Edge flag for j36_phy_clock_on()'s one line. It runs on every role
	 * apply so a phy_suspendm= written through sysfs takes effect on the next
	 * poll, and a line every three seconds is a line nobody reads. */
	bool clock_logged;
	struct delayed_work role_work;
	/* The role poll runs off a workqueue and .set_mode / .power_on /
	 * .power_off run off musb's probe and remove. The generic PHY framework
	 * serialises its own ops against each other and knows nothing about the
	 * poll, so the poll is what this exists for. */
	struct mutex lock;
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
	if (p->vbus_pin < 0 || !p->gpio)
		return;
	/*
	 * vbus=0 forbids SOURCING 5 V. It does not mean "do not touch the pad",
	 * and that distinction is the whole of this hunk. The early return used to
	 * cover both directions, so j36.usb=novbus left the pad exactly as the LK
	 * handed it over -- and if the LK handed it over high, the port went on
	 * sourcing 5 V that nothing in Linux had asked for, out of the parameter
	 * whose name says the opposite. On a board where CHRIN and this port share
	 * a net that also cost the whole boot's charging, because the PMIC read
	 * this same pad and concluded the 5 V on CHRIN was its own boost. Off is
	 * driven now, not assumed, which is right either way: a pad this driver
	 * has been told not to raise should not be found high.
	 */
	if (on && vbus == 0)
		return;
	if (p->vbus_on == (int)on)
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

/*
 * Both roles, and both in the same order for the same reason: write the RG_*
 * values FIRST and enable the force_* overrides after, so the override never
 * goes live onto a value left behind by the other role. The stock recover()
 * tail does exactly this, and the host sequence that used to be here did the
 * opposite -- enabled four overrides, then wrote the values -- which is one of
 * the smaller things wrong with it. See the file header for the larger one.
 *
 * The settle is the 800 us the stock routines take after their own role writes,
 * and it is on both paths now: .set_mode reaches force_device() with nothing
 * else between it and the caller, so the delay recover() takes before its own
 * tail does not cover that path.
 */

/*
 * ── GIVE THE CONTROLLER A CLOCK, WHICH COMES BEFORE GIVING IT A ROLE ─────────
 *
 * PA0_RG_USB20_INTR_EN is the PHY's internal-circuitry enable: the 48 MHz
 * reference, the PLL it drives, and the 60 MHz UTMI clock MUSB's USB side runs
 * on. The LK clears it on its way out and nothing here ever put it back, which
 * is why three boots' worth of role sequences landed on a core that answered
 * every register and arbitrated nothing. See the file header for the whole
 * reading.
 *
 * SuspendM is the gate downstream of it and phy_suspendm says who holds it. The
 * default releases the override, which is what mainline does and what recover()
 * already did on its own -- the link drives it then, and MUSB drives it high for
 * as long as POWER.ENSUSPEND is clear, which it is. The other two settings exist
 * so the question can be answered from a command line instead of a rebuild.
 *
 * Idempotent, cheap -- five byte accesses and one settle -- and called from both
 * role sequences, so a phy_suspendm written through sysfs takes effect on the
 * next role poll without a reload.
 */
static void j36_phy_clock_on(struct j36_usb_phy *p)
{
	u8 before = j36_phy_rd(p, J36_PHY_R00);

	j36_phy_set(p, J36_PHY_R00, J36_PHY_R00_INTR_EN);

	if (phy_suspendm < 0) {
		/* The value is written anyway: it is ignored while unforced, and
		 * it means anything that sets the force bit later -- a stock
		 * sequence, a future power_off -- finds SuspendM already high
		 * rather than latching the PHY asleep at the moment it forces. */
		j36_phy_set(p, J36_PHY_R68, J36_PHY_R68_SUSPENDM);
		j36_phy_clr(p, J36_PHY_R6A, J36_PHY_R6A_FORCE_SUSPENDM);
	} else {
		if (phy_suspendm)
			j36_phy_set(p, J36_PHY_R68, J36_PHY_R68_SUSPENDM);
		else
			j36_phy_clr(p, J36_PHY_R68, J36_PHY_R68_SUSPENDM);
		j36_phy_set(p, J36_PHY_R6A, J36_PHY_R6A_FORCE_SUSPENDM);
	}

	usleep_range(J36_PHY_SETTLE_US, J36_PHY_SETTLE_US * 2);

	if (p->clock_logged)
		return;
	p->clock_logged = true;
	dev_info(p->dev,
		 "PHY circuits enabled: USBPHYACR0 %02x -> %02x (RG_USB20_INTR_EN was %s), SuspendM %s -- without this the 60 MHz never reaches MUSB and the core answers registers while arbitrating nothing\n",
		 before, j36_phy_rd(p, J36_PHY_R00),
		 before & J36_PHY_R00_INTR_EN ? "already set" : "OFF, which is the fault",
		 phy_suspendm < 0 ? "left to the link, as mainline does"
				  : (phy_suspendm ? "pinned high in the PHY (phy_suspendm=1)"
						  : "FORCED LOW (phy_suspendm=0): the port is meant to be dead"));
}

/* And the other direction, for power_off: circuits off, PHY parked in suspend.
 * This is u2_phy_instance_power_off()'s half of the same pair, and it is what
 * the LK does on its way out -- which is how the bit came to be clear here in
 * the first place. */
static void j36_phy_clock_off(struct j36_usb_phy *p)
{
	j36_phy_clr(p, J36_PHY_R68, J36_PHY_R68_SUSPENDM);
	j36_phy_set(p, J36_PHY_R6A, J36_PHY_R6A_FORCE_SUSPENDM);
	j36_phy_clr(p, J36_PHY_R00, J36_PHY_R00_INTR_EN);
	p->clock_logged = false;
}

/*
 * ── IS THE ROLE ALREADY THE ONE BEING ASKED FOR? ─────────────────────────────
 *
 * One caller now, j36_phy_force_device(), and it is the only one that wants this
 * question asked. Device is the resting state -- it is the tail of both stock
 * sequences, so the PHY is already in it after recover() -- and the role poll
 * would otherwise rewrite it every three seconds for the life of the board.
 *
 * The host path deliberately does NOT skip. It used to, on the argument that
 * rewriting live force bits moves the outputs twice; the outputs moving is the
 * whole of what makes the core arbitrate, so there the argument was backwards.
 * See j36_musb_host_arm().
 */
static bool j36_phy_role_is(struct j36_usb_phy *p, u8 set, u8 clr)
{
	u8 dtm1 = j36_phy_rd(p, J36_PHY_R6C);

	if ((dtm1 & set) != set || (dtm1 & clr))
		return false;
	return (j36_phy_rd(p, J36_PHY_R6D) & J36_PHY_R6D_FORCE_ALL) ==
		J36_PHY_R6D_FORCE_ALL;
}

/* Device: B-device, session valid, VBUS present. The low-power resting state and
 * the tail both stock routines end on, so it is also what the PHY sits in after
 * recover(). 0x6c ends at 0x2e and 0x6d at 0x3e, byte for byte as transcribed. */
static void j36_phy_force_device(struct j36_usb_phy *p)
{
	j36_phy_clock_on(p);
	if (j36_phy_role_is(p, J36_PHY_R6C_DEV_SET, J36_PHY_R6C_DEV_CLR))
		return;
	j36_phy_clr(p, J36_PHY_R6C, J36_PHY_R6C_DEV_CLR);
	j36_phy_set(p, J36_PHY_R6C, J36_PHY_R6C_DEV_SET);
	j36_phy_set(p, J36_PHY_R6D, J36_PHY_R6D_FORCE_ALL);
	usleep_range(J36_PHY_SETTLE_US, J36_PHY_SETTLE_US * 2);
	msleep(J36_PHY_ROLE_SETTLE_MS);
}

/*
 * ── USB MAC OFF: THE HALF OF THE HOST SWITCH THIS FILE NEVER HAD ─────────────
 *
 * Session ended, every valid deasserted, RG_IDDIG low so the role is already A,
 * and all five forces live. Byte for byte the stock switch's
 *
 *	USBPHY_SET8(0x6c, 0x10);
 *	USBPHY_CLR8(0x6c, 0x2e);
 *	USBPHY_SET8(0x6d, 0x3e);
 *
 * and in that order: assert "the session ended" BEFORE taking the valids away,
 * so the PHY never briefly reports a live bus with no session on it.
 *
 * On its own this is a port that cannot do anything, which is the point. It is
 * only ever entered with SESSION down and left within five milliseconds, and
 * what it buys is that whatever comes up next comes up as an EVENT. See
 * j36_musb_host_arm().
 */
static void j36_phy_park_idle(struct j36_usb_phy *p)
{
	j36_phy_clock_on(p);
	j36_phy_set(p, J36_PHY_R6C, J36_PHY_R6C_IDLE_SET);
	j36_phy_clr(p, J36_PHY_R6C, J36_PHY_R6C_IDLE_CLR);
	j36_phy_set(p, J36_PHY_R6D, J36_PHY_R6D_FORCE_ALL);
	usleep_range(J36_PHY_SETTLE_US, J36_PHY_SETTLE_US * 2);
}

/*
 * Host: A-device, all three valids asserted, session not ended -- the stock
 * switch's "USB MAC ON and Host Mode", and the write that has to land AFTER
 * DEVCTL.SESSION rather than before it.
 *
 * force_iddig is in FORCE_ALL and has to be: without it the core reads the ID
 * pin, which on a board with no OTG cable floats to B.
 *
 * NOT idempotent, and that is deliberate. This used to skip itself when the PHY
 * already read host-forced, on the argument that rewriting live force bits moves
 * the outputs twice near a session edge. That argument was upside down: moving
 * the outputs is the entire job. The caller parks the PHY at idle first, so
 * every call from the arm has real work to do anyway, and the one caller that
 * does not -- .set_mode -- is better off writing than skipping.
 */
static void j36_phy_force_host(struct j36_usb_phy *p)
{
	j36_phy_clock_on(p);
	j36_phy_clr(p, J36_PHY_R6C, J36_PHY_R6C_HOST_CLR);
	j36_phy_set(p, J36_PHY_R6C, J36_PHY_R6C_HOST_SET);
	j36_phy_set(p, J36_PHY_R6D, J36_PHY_R6D_FORCE_ALL);
	usleep_range(J36_PHY_SETTLE_US, J36_PHY_SETTLE_US * 2);
	msleep(J36_PHY_ROLE_SETTLE_MS);
}

/*
 * ── THE SAME STEP, BUT MEASURED INSTEAD OF ASSERTED ──────────────────────────
 *
 * musb_session_restart()'s tail: release the four VALUE forces so AVALID,
 * BVALID, SESSEND and VBUSVALID come from the PHY's own comparator on the VBUS
 * pin, keeping force_iddig so the role stays this driver's. The comparator is
 * R1A bit 4, which recover() enables; it is re-asserted here because
 * savecurrent() clears it and a port that has been powered off and on again
 * would otherwise release the forces onto a comparator that is not running.
 *
 * What this is for: the forced arm tells the core that VBUS rose whether it did
 * or not, so a forced arm that fails proves only that the core would not take
 * host mode. A sensed arm that fails, with "VBUS below SessionEnd" in the
 * readout, says something quite different and much more useful -- that the 5 V
 * this driver believes it is sourcing is not arriving at the PHY at all, which
 * is a fault in the load switch or the pad and not in any of this.
 */
static void j36_phy_sense_vbus(struct j36_usb_phy *p)
{
	j36_phy_clock_on(p);
	j36_phy_set(p, J36_PHY_R1A, J36_PHY_R1A_VBUSCMP_EN);
	j36_phy_clr(p, J36_PHY_R6D, J36_PHY_R6D_FORCE_VBUS);
	j36_phy_clr(p, J36_PHY_R6C, J36_PHY_R6C_VBUS_ALL);
	usleep_range(J36_PHY_SETTLE_US, J36_PHY_SETTLE_US * 2);
	msleep(J36_PHY_ROLE_SETTLE_MS);
}

/*
 * Neither role: drop the five force_* enables so U2PHYDTM1's RG_* bits stop
 * being what this driver last wrote and go back to being what the PHY's own
 * comparators see. That is the only way DEVCTL's VBUS field means anything --
 * with the overrides on it reports the role sequence back at you, which is
 * exactly why "VBUS below SessionEnd" appeared in every log while the DRVVBUS
 * pad was high.
 *
 * Leaves the RG_* values in 0x6c alone: they are ignored while unforced, and
 * the next role sequence overwrites the ones it cares about anyway.
 */
static void j36_phy_release_force(struct j36_usb_phy *p)
{
	j36_phy_clr(p, J36_PHY_R6D, J36_PHY_R6D_FORCE_ALL);
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
	j36_phy_set(p, J36_PHY_R1A, J36_PHY_R1A_VBUSCMP_EN);
	usleep_range(J36_PHY_SETTLE_US, J36_PHY_SETTLE_US * 2);
	j36_phy_force_device(p);
}

/* FUN_81e093b8, savecurrent().
 *
 * The clock_off tail is not transcribed and is deliberate: this runs from
 * .power_off, which is the port being given up, and the force-device tail it
 * ends on now turns the PHY's circuits back ON through j36_phy_clock_on(). A
 * routine whose whole job is to park the block cannot leave its PLL running, so
 * the park is stated explicitly and last. */
static void j36_phy_savecurrent(struct j36_usb_phy *p)
{
	j36_phy_uart_off_and_trim(p);
	j36_phy_clr(p, J36_PHY_R22, J36_PHY_R22_PULLDOWN);
	j36_phy_clr(p, J36_PHY_R6A, J36_PHY_R6A_FORCE_SUSPENDM);
	usleep_range(J36_PHY_SETTLE_US, J36_PHY_SETTLE_US * 2);
	j36_phy_force_device(p);
	j36_phy_clock_off(p);
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
 * for host means the role never took.
 *
 * That last one has two causes and they are told apart by BDEVICE. HM clear with
 * BDEV is the PHY override not landing -- the ID line still reads B. HM clear
 * with ADEV is the override landing on a session that was already running, which
 * MUSB will not re-arbitrate; that is the bootloader's peripheral session and it
 * is what j36_musb_stand_down() exists to end. Both used to print as
 * "PERIPHERAL", so the dump now says the second one out loud.
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

/*
 * ── WHAT "IT TOOK" ACTUALLY LOOKS LIKE, AND WHY IT IS NOT HM ─────────────────
 *
 * DEVCTL.HOSTMODE is set by the core when it is ACTING as a host, and an
 * A-device holding a session over an EMPTY socket is not acting as anything
 * yet: there is no connect to answer, no reset to drive, no SOF to send. HM
 * appears with the CONNECT interrupt, which needs a device pulling D+ up, which
 * needs the session -- so a driver that tears the session down because HM is
 * clear has removed the only thing that could ever set it. That loop is exactly
 * what the log reads as "attempt 1 of 4 ... 4 of 4 ... the core will not take
 * host mode", and the port it leaves behind has no session at all (DEVCTL 98),
 * which is why nothing enumerates and nothing is powered.
 *
 * The vendor driver this file is transcribed from never reads HM anywhere.
 * musb_id_pin_work() sets the role, calls musb_start(), and then says
 * MUSB_HST_MODE(musb) -- a software variable. Mainline does the same, from the
 * CONNECT and SESSREQ interrupt handlers, and never gates anything on the bit.
 *
 * So the test is the state a host is ARMED in, all of which the core sets
 * itself and all of which this driver has been printing as a failure: A-device
 * (arbitration went our way), SESSION held, and the VBUS comparators at or
 * above AValid. HM is accepted too -- a port with a device on it is obviously
 * fine -- but it is not required, and requiring it is the bug.
 */
static bool j36_musb_host_armed(u8 devctl)
{
	if (devctl & J36_MUSB_DEVCTL_HM)
		return true;

	return !(devctl & J36_MUSB_DEVCTL_BDEVICE) &&
	       (devctl & J36_MUSB_DEVCTL_SESSION) &&
	       ((devctl & J36_MUSB_DEVCTL_VBUS) >> J36_MUSB_DEVCTL_VBUS_SHIFT) >=
	       J36_MUSB_VBUS_AVALID;
}

static void j36_musb_dump(struct j36_usb_phy *p, const char *when)
{
	u8 faddr, power, devctl, epinfo, raminfo;
	u8 acr0, dtm0, dtm0_f;
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
		 "MUSB %s: DEVCTL %02x [%s%s%s%s%s%s] VBUS %s, POWER %02x [%s%s%s%s%s%s], FADDR %u\n",
		 when, devctl,
		 devctl & J36_MUSB_DEVCTL_SESSION ? "SESSION " : "",
		 devctl & J36_MUSB_DEVCTL_HM	  ? "HOST "    : "PERIPHERAL ",
		 devctl & J36_MUSB_DEVCTL_HR	  ? "HOSTREQ " : "",
		 devctl & J36_MUSB_DEVCTL_BDEVICE ? "BDEV "    : "ADEV ",
		 devctl & J36_MUSB_DEVCTL_FSDEV	  ? "FSDEV "   : "",
		 devctl & J36_MUSB_DEVCTL_LSDEV	  ? "LSDEV"    : "",
		 j36_musb_vbus_str(devctl), power,
		 power & J36_MUSB_POWER_SOFTCONN  ? "SOFTCONN " : "",
		 power & J36_MUSB_POWER_ISOUPDATE ? "ISOUPD "   : "",
		 power & J36_MUSB_POWER_HSENAB	  ? "HSENAB "  : "",
		 power & J36_MUSB_POWER_HSMODE	  ? "HSMODE "  : "",
		 power & J36_MUSB_POWER_RESET	  ? "RESET "   : "",
		 power & J36_MUSB_POWER_SUSPENDM  ? "SUSPEND"  : "",
		 faddr & 0x7f);

	/*
	 * And the one sentence a reader should not have to decode the hex for.
	 *
	 * This used to warn about an A-device holding a session with HM clear,
	 * which is what an armed host over an empty socket looks like -- so it
	 * cried wolf on the one state the port is supposed to reach, in every
	 * dump, for several boots. The states actually worth a warning are the
	 * two that cannot host anything: no session at all, and a session that
	 * arbitrated the wrong way round.
	 */
	if (!j36_musb_host_armed(devctl))
		dev_warn(p->dev,
			 "MUSB %s: not armed as a host -- %s, so the root hub will not scan its port and nothing plugged in can enumerate\n",
			 when,
			 !(devctl & J36_MUSB_DEVCTL_SESSION)
				? "there is no session on the bus"
				: (devctl & J36_MUSB_DEVCTL_BDEVICE
					? "the core arbitrated itself into the B-device role"
					: "VBUS never came up inside the session"));

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

	/*
	 * And the PHY side of the same question, because every bit printed above
	 * is meaningless if this one is off. Reading it here rather than only at
	 * the moment it is written is the point: it says whether anything took the
	 * clock away again between power_on and now.
	 */
	acr0 = j36_phy_rd(p, J36_PHY_R00);
	dtm0 = j36_phy_rd(p, J36_PHY_R68);
	dtm0_f = j36_phy_rd(p, J36_PHY_R6A);
	dev_info(p->dev,
		 "PHY %s: USBPHYACR0 %02x [INTR_EN %s], U2PHYDTM0 %02x/%02x [SuspendM %u, %s] -- %s\n",
		 when, acr0, acr0 & J36_PHY_R00_INTR_EN ? "on" : "OFF",
		 dtm0, dtm0_f, !!(dtm0 & J36_PHY_R68_SUSPENDM),
		 dtm0_f & J36_PHY_R6A_FORCE_SUSPENDM ? "forced" : "from the link",
		 acr0 & J36_PHY_R00_INTR_EN
			? "the core has a clock"
			: "THE CORE HAS NO CLOCK: HM, BDEVICE, HSMODE and every interrupt are dead registers until this is set");
}

/*
 * Which base the multipoint block decodes at, answered by writing to it.
 *
 * IT HAS BEEN ANSWERED: 0x480, with 0x080 reading back 00. That is the MT2701
 * layout, so drivers/usb/musb/mediatek.c had it right unmodified and the half of
 * linux/0003-musb-mediatek-mt6592.patch that moved BUSCTL back to 0x080 is gone.
 * This is off by default now (musb_probe_layout) and kept for the next board
 * revision that does not enumerate, where it is the first question to re-ask.
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
 * Writing to whichever base turns out to be the hole is safe: an undecoded
 * offset inside a peripheral window that IS decoded and IS ungated drops the
 * write. The hang case is the gated one, and the PERI gate was opened in .init
 * long before this runs. On this board neither base is a hole anyway -- 0x080 is
 * MUSB_RXTOG, which took the write and discarded it because MUSB_RXTOGEN was
 * still clear, and that is exactly the 00 readback reported above.
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
		dev_info(p->dev,
			 "MUSB multipoint block is at 0x480 (0x080 read back %02x): the MT2701 layout, which is what mediatek.c drives unmodified\n",
			 legacy);
	else if (legacy == J36_MUSB_BUSCTL_LEGACY_VAL)
		dev_warn(p->dev,
			 "MUSB multipoint block is at 0x080 (0x480 read back %02x): the legacy layout, so mediatek.c is writing every function address into a hole and nothing will enumerate\n",
			 mtk);
	else
		dev_warn(p->dev,
			 "MUSB multipoint probe is inconclusive: 0x080 read back %02x and 0x480 read back %02x, neither holding what was written\n",
			 legacy, mtk);
}

/* ── which way the port is being driven, and therefore which role ────────────
 *
 * None of this runs at the default. A J36 Ultra charges through a DC inlet with
 * no data lines in it, so this port is a host and holds its 5 V and there is
 * nothing to measure -- vbus=1, and everything below sits idle.
 *
 * It is kept for the board this driver was first written against, where one
 * connector carries both and the CHRIN net is the net this pad sources into: a
 * port cannot source and sink at once, no device-tree constant knows which is
 * wanted right now, so it gets measured. vbus=-1 turns it back on. See the
 * header for the whole argument.
 */

/*
 * The core's own pre-enumeration attach flag: FSDEV or LSDEV is set as soon as
 * the PHY sees a device's pull-up on D+ or D-, before any enumeration has got
 * anywhere. This is the guard on the whole measurement -- while it is true the
 * poll does nothing at all, so a working device never sees VBUS drop out from
 * under it, and neither does one that is halfway through coming up.
 */
static bool j36_port_attached(struct j36_usb_phy *p)
{
	u8 devctl;

	if (!p->musb)
		return false;

	devctl = readb(p->musb + J36_MUSB_DEVCTL);
	return !!(devctl & (J36_MUSB_DEVCTL_FSDEV | J36_MUSB_DEVCTL_LSDEV));
}

/*
 * ── DID THE THING ON THE PORT EVER BECOME A USB DEVICE? ──────────────────────
 *
 * The one question about this port that no register on this board can answer,
 * and the one that separates the two things FSDEV/LSDEV cannot tell apart.
 *
 * A device pulls D+ or D- up, gets a bus reset, answers it, and is given an
 * address; a divider-type charger pulls the same line up with a resistor and
 * answers nothing, because there is nothing in it to answer with.  The pull-up is
 * all DEVCTL sees, so DEVCTL calls both of them attached.  Enumeration is the
 * difference, and enumeration lives in usbcore.
 *
 * Root hubs are skipped because there is always exactly one and it is this
 * board's own controller, not something somebody plugged in.  Everything else on
 * the bus got its address from a reset it answered.
 *
 * ONE CONTROLLER, which is why this does not filter by bus: musb is the only USB
 * host on this SoC and this PHY is its PHY, so any usb_device that exists at all
 * came in through this connector.  A board with a second controller would want
 * udev->bus compared against musb's, and would have a second connector to decide
 * about as well.
 */
static int j36_usb_dev_seen(struct usb_device *udev, void *data)
{
	unsigned int *n = data;

	if (udev->parent)
		++*n;
	return 0;
}

static unsigned int j36_usb_devices(void)
{
	unsigned int n = 0;

	usb_for_each_dev(&n, j36_usb_dev_seen);
	return n;
}

/*
 * Is something OUTSIDE this board holding the port at 5 V?
 *
 * Only meaningful with DRVVBUS low and the force_* overrides released, and both
 * are the caller's job -- j36_usb_phy_decide_role() is the only caller and does
 * both immediately above.
 */
static int j36_port_externally_powered(struct j36_usb_phy *p)
{
	u8 devctl;
	unsigned int level;

	if (!p->musb)
		return -1;

	devctl = readb(p->musb + J36_MUSB_DEVCTL);
	level = (devctl & J36_MUSB_DEVCTL_VBUS) >> J36_MUSB_DEVCTL_VBUS_SHIFT;

	dev_dbg(p->dev, "port unfed by us reads DEVCTL %02x: VBUS %s\n",
		devctl, j36_musb_vbus_str(devctl));

	return level >= J36_MUSB_VBUS_AVALID;
}

/*
 * DEVCTL.SESSION, and the one place this driver reaches into another driver's
 * register block to write something.
 *
 * The header used to say this bit was not the PHY's to touch, because musb_core
 * sets it itself when it starts the host. That is true, and it is true exactly
 * once: musb_start() runs when the hub driver powers the root port, at boot, and
 * nothing calls it again. It worked as long as the role never changed after
 * boot -- which is precisely the assumption this file has just stopped making.
 *
 * A poll that takes the role away to charge, and then hands it back when the
 * charger comes out, leaves musb with a host that has no session and no reason
 * to think it needs one; nothing enumerates and nothing says why. So the driver
 * that changes the role is the driver that has to end and restart the session,
 * which is what the stock host switch does too -- it clears SESSION before it
 * touches the PHY at all.
 *
 * A read-modify-write on DEVCTL is what musb_core does with this bit as well.
 * The bits it reads back and writes again -- HOSTMODE, BDEVICE, FSDEV, LSDEV
 * and the VBUS field -- are all read-only, so returning them is not a write.
 * musb_session=0 backs the whole thing out at runtime if it ever turns out to
 * race with musb_core's own writes; the port then behaves as it did before,
 * which is to say host mode works until the first role change.
 */
static void j36_musb_session(struct j36_usb_phy *p, bool on)
{
	u8 devctl;

	if (!p->musb || !musb_session)
		return;

	devctl = readb(p->musb + J36_MUSB_DEVCTL);
	if (!!(devctl & J36_MUSB_DEVCTL_SESSION) == on)
		return;

	if (on)
		devctl |= J36_MUSB_DEVCTL_SESSION;
	else
		devctl &= (u8)~J36_MUSB_DEVCTL_SESSION;
	writeb(devctl, p->musb + J36_MUSB_DEVCTL);

	dev_dbg(p->dev, "DEVCTL.SESSION %s\n", on ? "set" : "cleared");
}

/*
 * ── STAND THE PERIPHERAL DOWN, SO THE HOST CAN BE STARTED ────────────────────
 *
 * The early return in j36_musb_session() above is correct and it is also how the
 * port spent every boot as a peripheral: the bit was ALREADY set, by the
 * bootloader, in the other role, so asking for it produced no write and no edge.
 * See J36_MUSB_SESSION_GAP_MS for the whole of that argument. This is the piece
 * that was missing -- the deliberate 1 -> 0 with the pull-up taken down with it,
 * so that the set which follows is a session the core arbitrates afresh.
 *
 * SOFTCONN is cleared here and not left to musb_start(). musb_start() does write
 * POWER, and what it writes has no SOFTCONN in it, so on a good boot this is
 * redundant -- but power_on runs BEFORE musb_start(), the session is restarted
 * here, and a core told to start a session while it is still advertising a
 * device's pull-up is being asked two things at once. Take the pull-up down
 * first and there is only one question on the wire.
 *
 * Cheap enough to be unconditional: two byte reads, at most two byte writes and
 * a 20 ms sleep, once per role change.
 */
static void j36_musb_stand_down(struct j36_usb_phy *p)
{
	u8 power;

	if (!p->musb || !musb_session)
		return;

	j36_musb_session(p, false);

	power = readb(p->musb + J36_MUSB_POWER);
	if (power & J36_MUSB_POWER_SOFTCONN) {
		writeb(power & (u8)~J36_MUSB_POWER_SOFTCONN,
		       p->musb + J36_MUSB_POWER);
		dev_dbg(p->dev,
			"POWER.SOFTCONN cleared: the port was still holding a peripheral's D+ pull-up\n");
	}

	msleep(J36_MUSB_SESSION_GAP_MS);
}

/*
 * Wait for the core to answer, up to J36_MUSB_SETTLE_MS, and hand back whatever
 * DEVCTL says at the end of it. Returns as soon as the port is armed, so the
 * cost on a port that works is one register read.
 *
 * Nothing here writes. It exists because HM and BDEVICE are not set by the store
 * that sets SESSION -- they are set by the core once it has sampled ID and the
 * VBUS comparators over its own clock -- and every caller that read DEVCTL on
 * the next line was reading a decision that had not been taken yet.
 */
static u8 j36_musb_settle(struct j36_usb_phy *p)
{
	unsigned int waited = 0;
	u8 devctl;

	for (;;) {
		devctl = readb(p->musb + J36_MUSB_DEVCTL);
		if (j36_musb_host_armed(devctl))
			return devctl;
		if (waited >= J36_MUSB_SETTLE_MS)
			return devctl;
		msleep(J36_MUSB_SETTLE_STEP_MS);
		waited += J36_MUSB_SETTLE_STEP_MS;
	}
}

/*
 * ── THE HOST ARM, IN MEDIATEK'S ORDER ────────────────────────────────────────
 *
 * Every path in this file that wants host mode goes through here, and the order
 * of the steps is the whole point -- see the file header for the stock
 * musb_id_pin_work() it is transcribed from and the comment in it that explains
 * why ("for no VBUS sensing IP").
 *
 * The short version: MT6592's MUSB has no comparator of its own on the VBUS pin.
 * DEVCTL's VBUS field is whatever U2PHYDTM1 was last forced to, so a rail that
 * was already high before the session started is not something the core can
 * arbitrate on -- and HOSTMODE is latched on a transition, not on a level. So
 * the valids are taken DOWN, the session is started against a dead bus, and the
 * valids are brought back UP inside it. That rise is the event, and it is the
 * one thing this driver never gave the core in three boots.
 *
 * `sensed' picks which of the two stock ways step 6 happens. false forces the
 * valids high (musb_id_pin_work()); true releases the four value forces so the
 * PHY's own comparator drives them from the actual pin
 * (musb_session_restart()). Both keep force_iddig, so the role stays ours
 * either way. See j36_phy_sense_vbus() for why the second is worth having.
 *
 * Returns DEVCTL after the settle, or 0 when there is no MUSB window mapped --
 * in which case the PHY half still ran and only the readout is missing.
 *
 * Call with p->lock held.
 */
static u8 j36_musb_host_arm(struct j36_usb_phy *p, bool sensed)
{
	j36_phy_clock_on(p);

	/*
	 * 1. The rail first, and MediaTek's hundred milliseconds to come up on.
	 *    Spent with the session still down, which is what keeps musb's
	 *    A-device VBUS timer from ever seeing a rail that is not there yet.
	 */
	j36_usb_phy_vbus(p, true);
	msleep(J36_VBUS_RISE_MS);

	/*
	 * 2. End whatever session is running -- the bootloader's at boot,
	 *    musb_start()'s afterwards -- and take the peripheral pull-up down
	 *    with it, so there is one question on the wire and not two.
	 */
	j36_musb_stand_down(p);

	/* 3. USB MAC OFF. The step that was missing. */
	j36_phy_park_idle(p);

	/* 4. The stock five milliseconds of a visibly dead bus. */
	msleep(J36_PHY_MAC_OFF_MS);

	/*
	 * 5. Start the session while the core still sees that dead bus, so what
	 *    happens next is an edge it can act on.
	 */
	j36_musb_session(p, true);

	/* 6. And bring VBUS up inside the session. */
	if (sensed)
		j36_phy_sense_vbus(p);
	else
		j36_phy_force_host(p);

	/*
	 * 7. AND TAKE THE PULL-UP DOWN AGAIN, on the far side of the session.
	 *
	 *    Step 2 cleared POWER.SOFTCONN and the dumps kept reading it back
	 *    set -- POWER f0 at every settle, over a session this driver had just
	 *    started as an A-device. Nothing in mainline's host path writes that
	 *    bit; musb_pullup() is the only writer and it cannot run in
	 *    MUSB_PORT_MODE_HOST. So it is the core's own reset value coming back
	 *    with the session, and while it is set the core is holding its D+
	 *    pull-up onto the bus it is supposed to be the host OF.
	 *
	 *    That is not cosmetic. A host detects a connect as a pull-up
	 *    appearing against its own 15k pull-downs; if the pull-up is already
	 *    there because the port put it there, the device that plugs in
	 *    changes nothing the core can see, no CONNECT interrupt is raised,
	 *    and HM stays clear forever -- which is the second half of a socket
	 *    that arms perfectly and still enumerates nothing.
	 */
	if (p->musb) {
		u8 power = readb(p->musb + J36_MUSB_POWER);

		if (power & J36_MUSB_POWER_SOFTCONN)
			writeb(power & (u8)~J36_MUSB_POWER_SOFTCONN,
			       p->musb + J36_MUSB_POWER);
	}

	/*
	 * 8. HM is not set by the store that set SESSION; it is set by the core
	 *    once it has sampled ID and the valids over its own clock. Give it
	 *    the window before calling the result anything.
	 */
	return p->musb ? j36_musb_settle(p) : 0;
}

/*
 * ── DID IT TAKE? ─────────────────────────────────────────────────────────────
 *
 * j36_musb_host_armed() is the test, and the comment on it is the important one
 * in this file: it is NOT DEVCTL.HM. A port that is an A-device, holding a
 * session, with the VBUS comparators up is armed, and that is as far as the core
 * will go on its own with nothing in the socket. If the poll finds less than
 * that it runs the stock arm again -- VBUS down inside the PHY, session
 * restarted against the dead bus, VBUS back up inside the session.
 *
 * THIS IS THE ONLY PLACE THAT CAN FIX IT, and that is a fact about ordering
 * rather than a design choice. power_on() runs inside musb_platform_init(), which
 * is the first thing musb_init_controller() does; whatever session it leaves
 * behind is wiped a moment later by musb_generic_disable()'s DEVCTL = 0, and the
 * session the port actually runs on is the one musb_start() begins from the hub
 * thread, after the root hub exists -- and musb_start() folds `devctl &= ~SESSION'
 * and `devctl |= SESSION' into a single write, so it produces no edge of its own
 * and arbitrates against whatever the PHY happens to be holding. The poll is the
 * first thing this driver runs after all of that, so it is the first opportunity
 * to give the core a VBUS rise it will still have when enumeration depends on it.
 *
 * WHAT KEEPS THIS FROM BEING A LOOP, in order:
 *
 *   nothing enumerated   j36_usb_devices() is the guard the attach latch already
 *                        uses. A device that is up is a role that worked, whatever
 *                        HM reads, and it is never disturbed.
 *   a bounded count      J36_MUSB_HM_KICKS. Past that the fault is not a missing
 *                        edge; saying so once beats retrying forever.
 *   reset on role change only, so unplugging and replugging earns fresh retries
 *                        but a port sitting still does not.
 *
 * Call with p->lock held, from the poll, and only when the role is host.
 */
static void j36_musb_host_kick(struct j36_usb_phy *p)
{
	bool sensed;
	u8 devctl;

	if (!p->musb || !musb_session || p->hm_gave_up)
		return;

	devctl = readb(p->musb + J36_MUSB_DEVCTL);
	if (j36_musb_host_armed(devctl))
		return;				/* armed as a host: done */
	if (j36_usb_devices())
		return;				/* something works; leave it alone */

	if (p->hm_kicks >= J36_MUSB_HM_KICKS) {
		p->hm_gave_up = true;

		/*
		 * GIVING UP IS NOT THE SAME AS WALKING AWAY. Whatever the last
		 * attempt was, the state it left is the state the port then has
		 * to live in for the rest of the uptime -- and a measured
		 * attempt leaves it with no session at all. Put the port back
		 * into the best shape this driver knows how to give it before
		 * saying anything: valids forced, session held, A-device. A
		 * socket in that state powers a device and answers a connect
		 * even if HM never appears, which is the whole point.
		 */
		j36_phy_force_host(p);
		if (!(readb(p->musb + J36_MUSB_DEVCTL) &
		      J36_MUSB_DEVCTL_SESSION))
			j36_musb_session(p, true);
		devctl = j36_musb_settle(p);

		dev_warn(p->dev,
			 "the port asked for host %u times and DEVCTL settles at %02x [%s, %s, VBUS %s]: leaving it forced and holding a session, which is as close to a host as this core will come without a device on the socket\n",
			 p->hm_kicks + 1, devctl,
			 devctl & J36_MUSB_DEVCTL_BDEVICE ? "B-device"
							  : "A-device",
			 devctl & J36_MUSB_DEVCTL_SESSION ? "session held"
							  : "NO SESSION",
			 j36_musb_vbus_str(devctl));
		return;
	}

	p->hm_kicks++;
	/*
	 * Odd attempts force the valids, even attempts hand them to the PHY's own
	 * comparator on the pin. Both are stock -- musb_id_pin_work() and
	 * musb_session_restart() -- and alternating them means one boot answers
	 * two different questions: whether the core will take host mode at all,
	 * and whether the 5 V this driver thinks it is sourcing is really there.
	 */
	sensed = !(p->hm_kicks & 1u);

	/*
	 * A/B is read out rather than asserted. This line used to say "A-device"
	 * whatever DEVCTL held, and it said it over a 99 -- BDEVICE set -- in three
	 * consecutive boots, which hid the whole of the fault: the core had not
	 * merely failed to finish becoming the host, it had arbitrated the other
	 * way round and settled there.
	 */
	dev_info(p->dev,
		 "DEVCTL %02x: the core is a %s with %s and is not armed as a host -- taking VBUS down inside the PHY, restarting the session against the dead bus and bringing VBUS back up %s, which is the rise MUSB latches host mode on (attempt %u of %u)\n",
		 devctl,
		 devctl & J36_MUSB_DEVCTL_BDEVICE ? "B-device" : "A-device",
		 devctl & J36_MUSB_DEVCTL_SESSION ? "a session" : "no session",
		 sensed ? "from the PHY's own comparator on the pin"
			: "forced",
		 p->hm_kicks, J36_MUSB_HM_KICKS);

	devctl = j36_musb_host_arm(p, sensed);

	dev_info(p->dev,
		 "after the arm DEVCTL reads %02x [%s, VBUS %s]: the core is %s\n",
		 devctl,
		 devctl & J36_MUSB_DEVCTL_BDEVICE ? "B-device" : "A-device",
		 j36_musb_vbus_str(devctl),
		 j36_musb_host_armed(devctl)
			? "armed as a host -- the root port can scan now"
			: "still not armed, after being given the full settle window");

	/*
	 * And the one reading the sensed arm exists for. With the value forces
	 * released the VBUS field is a comparator on the actual pin, so a low one
	 * here is not a role problem at all: DRVVBUS is high, this driver believes
	 * it is sourcing 5 V, and the PHY cannot see it.
	 */
	if (sensed && !j36_musb_host_armed(devctl) &&
	    ((devctl & J36_MUSB_DEVCTL_VBUS) >> J36_MUSB_DEVCTL_VBUS_SHIFT) <
	    J36_MUSB_VBUS_AVALID)
		dev_warn(p->dev,
			 "and it was measured, not forced: with DRVVBUS pad %d driven high the PHY's own comparator still reads VBUS %s, so the 5 V is not reaching the port -- look at the load switch and the pad, not at the role\n",
			 p->vbus_pin, j36_musb_vbus_str(devctl));

	/*
	 * ── AND THEN TAKE THE MEASUREMENT BACK DOWN ──────────────────────────
	 *
	 * The sensed arm is a diagnostic, and it is finished the moment the line
	 * above has read it. What it must not do is become the state the port
	 * then RUNS in, because with the value forces released VBUSVALID is a
	 * live comparator on the pin and any droop below its threshold ends the
	 * session under whatever is enumerating.
	 *
	 * That distinction is invisible to a mouse and decisive for a hub. The 5 V
	 * here comes through a load switch off VBAT, and what a hub presents when
	 * it is plugged in is the bulk capacitance of its own regulator plus one
	 * downstream port's worth for each socket -- hundreds of microfarads
	 * charging through a switch and a connector, which is a real droop for a
	 * few milliseconds. A mouse presents a fraction of that and never gets
	 * near the threshold. So a port left sensed hosts single devices perfectly
	 * and drops every hub at the instant of plug-in, which is a fault report
	 * that reads like "hubs are not supported" and is nothing of the kind.
	 *
	 * Forcing them back is three writes and a settle, and both sequences are
	 * idempotent: HOST_SET is AVALID|BVALID|VBUSVALID and HOST_CLR is
	 * IDDIG|SESSEND, which is what a working A-device session already has, so
	 * this asserts the state the bus is in rather than changing it.
	 *
	 * THE SESSION IS NOT SAFE, THOUGH, and that is what this used to miss. A
	 * measured arm on a port whose comparator reads nothing does not merely
	 * report "VBUS below SessionEnd" -- the core ENDS THE SESSION on it, in
	 * hardware, because that is what SessionEnd means, and DEVCTL comes back
	 * 80 and then 98: no session, either way. Restoring the valids afterwards
	 * puts the voltage back and leaves the port sitting there with nothing
	 * running on it, which is a dead socket that powers nothing and scans
	 * nothing. So put the session back too, and re-read, so the caller's next
	 * poll sees what it actually left behind.
	 */
	if (sensed) {
		j36_phy_force_host(p);
		if (!(readb(p->musb + J36_MUSB_DEVCTL) &
		      J36_MUSB_DEVCTL_SESSION)) {
			j36_musb_session(p, true);
			devctl = j36_musb_settle(p);
			dev_info(p->dev,
				 "the measurement ended the session, as a comparator that reads nothing must; restarted it against the forced valids, DEVCTL %02x [%s, VBUS %s]\n",
				 devctl,
				 devctl & J36_MUSB_DEVCTL_BDEVICE ? "B-device"
								  : "A-device",
				 j36_musb_vbus_str(devctl));
		}
	}
}

/*
 * True while the role is the measurement's to decide, which on this board is
 * false: vbus defaults to 1 because the charger has an inlet of its own and this
 * port never has to give up 5 V for one. Everything below is what happens on a
 * board that shares the socket and asks for vbus=-1.
 *
 * vbus=0 is in here and not excluded from it, which is worth a line: forbidding
 * the port to SOURCE 5 V is not the same question as which role it should be,
 * and the two cases vbus=0 exists for both want the measurement. A self-powered
 * hub does not drive the upstream cable -- the spec forbids it -- so the port
 * reads as unfed and hosts it, which is exactly what vbus=0 was for. And a
 * charger on a cell-less board reads as fed and gets charged into, which is what
 * anyone typing vbus=0 wanted even more.
 *
 * Only vbus=1 pins, and only a device tree with no j36,musb-controller leaves
 * nothing to measure with -- in which case host is the right default, because
 * that is the role that needs a driver's help and device is what the PHY falls
 * back to on its own anyway.
 */
static bool j36_role_is_auto(struct j36_usb_phy *p)
{
	return vbus <= 0 && p->musb;
}

/*
 * Decide, and apply. Call with p->lock held.
 *
 * The measurement costs a DRVVBUS drop, so it is only paid when there is a
 * decision to make and nothing plugged in to disturb. Everything else here is
 * bookkeeping: re-raising the pad the measurement had to lower, and logging
 * only on a change, because at role_poll_ms this would otherwise be twenty
 * lines a minute for the life of the board.
 */
static void j36_usb_phy_decide_role(struct j36_usb_phy *p)
{
	bool measure_now = false;
	bool released = false;
	const char *why;
	bool changed;
	bool host;
	int fed;

	if (!j36_role_is_auto(p)) {
		host = true;
		why = vbus > 0
			? "vbus=1, which is the default here: charging comes in on the DC inlet, so this port has nothing to arbitrate and never has to stand down"
			: "there is no j36,musb-controller to measure the port with";
		goto apply;
	}

	if (p->role_host == 1 && j36_port_attached(p)) {
		/*
		 * ── THE ATTACH LATCH, AND WHERE THE CHARGER USED TO GO MISSING ──
		 *
		 * None of this runs at the default, which is vbus=1: it is the
		 * shared-socket board's problem, kept because that board still has
		 * it.  Here the charger is on its own inlet and never touches D+/D-.
		 *
		 * FSDEV/LSDEV means "D+ or D- is being held high", and that is not
		 * quite the same claim as "a device is here".  A divider-type
		 * charger -- the Apple 2.4 A brick holds D+ near 2.7 V, and the
		 * Samsung scheme holds both near 1.2 V -- drives the same line a
		 * device's pull-up does.  So it read as attached, the probe was
		 * suspended for as long as it stayed plugged in, DRVVBUS never came
		 * back down, and on a board where CHRIN shares this net the PMIC's
		 * interlock reported no cable at the very moment there was one.
		 * Which is not only a display fault: on such a board the interlock
		 * is right to refuse, because charging off its own boost is charging
		 * the cell from the cell through two conversions.  Nothing charged.
		 *
		 * This file used to say the distinction could not be made from here,
		 * and left the latch alone rather than guess.  The distinction can be
		 * made -- it is just not in a register.  A device answers a bus reset
		 * and is given an address; a charger answers nothing, because there
		 * is nothing in it to answer with.  So:
		 *
		 *   something on the bus   a device really is here and is in use.
		 *                          Latch, exactly as before, for as long as
		 *                          it stays.  Nothing that enumerates is ever
		 *                          power-cycled by this.
		 *   nothing, but not yet   enumeration takes a reset, a descriptor
		 *                          read and an address, and the attach flag
		 *                          beats all three.  Wait attach_grace_polls.
		 *   nothing, still         it held the line high for ten seconds and
		 *                          never became a device.  Measure.
		 *
		 * ONE measurement per attached run, which is what attach_probed is
		 * for: a broken device that never enumerates would otherwise be
		 * power-cycled every grace period for the whole uptime.  It gets one,
		 * which is a retry rather than a loop, and then the latch holds.
		 */
		if (j36_usb_devices()) {
			if (!p->attach_logged) {
				p->attach_logged = true;
				dev_info(p->dev,
					 "port is in use: something enumerated, so the role is settled and DRVVBUS stays up until it comes out\n");
			}
			p->attach_polls = 0;
			p->attach_probed = false;
			/* Busy; ask again next poll -- and do not let the
			 * interval toward the next probe run down while the port
			 * is in use, or the first idle poll after a stick is
			 * unplugged spends its whole budget at once. */
			p->polls_since_probe = 0;
			return;
		}

		if (p->attach_probed) {
			p->polls_since_probe = 0;
			return;
		}
		if (++p->attach_polls < attach_grace_polls) {
			p->polls_since_probe = 0;
			return;
		}

		p->attach_probed = true;
		measure_now = true;
		dev_info(p->dev,
			 "something has held D+/D- high for %u polls and never got a USB address: measuring the port, because a charger looks exactly like this\n",
			 p->attach_polls);
	} else {
		p->attach_logged = false;
		p->attach_polls = 0;
		p->attach_probed = false;
	}

	/*
	 * ── HOW OFTEN THE EXPENSIVE ANSWER IS RE-ASKED ──
	 *
	 * Measuring means dropping the pad, and dropping the pad means the
	 * connector loses 5 V for J36_VBUS_FALL_MS plus a settle.  On a port
	 * that is already standing down that costs nothing -- the pad is low
	 * either way -- so the charger-came-out case still answers within one
	 * poll.  On a port that is hosting it is a power cycle, and MVII's log
	 * is what a power cycle every three seconds looks like: one VBUS_ERROR
	 * per cycle for the whole uptime and nothing ever enumerating.
	 *
	 * So the host answer is re-asked every role_probe_every polls instead of
	 * every one.  What that costs is latency on ONE transition -- a charger
	 * plugged into an idle console is noticed in up to fifteen seconds
	 * rather than three -- and what it buys is a port that holds still long
	 * enough for a device to come up on it.
	 *
	 * p->role_host < 0 is the power-on case and is never skipped: there is
	 * no answer yet to keep.  Neither is measure_now, which is the branch
	 * above having already decided this is worth a power cycle: making it
	 * queue behind an interval whose whole purpose is to protect a device
	 * that has just been shown not to exist would only add ten seconds to
	 * the time a charger spends not charging.
	 */
	if (p->role_host == 1 && !measure_now) {
		if (role_probe_every == 0)
			return;			/* pinned by the first probe */
		if (++p->polls_since_probe < role_probe_every)
			return;
	}
	p->polls_since_probe = 0;

	if (p->vbus_on != 0) {
		/*
		 * SESSION FIRST, AND THAT ORDER IS THE FIX.  musb holds an
		 * A-device session against a rail this driver is about to take
		 * away, and a session whose VBUS collapses is VBUS_ERROR --
		 * musb's own recovery, entered once per poll, which is what the
		 * log was full of.  Ending the session before the rail goes is
		 * exactly what the stock host switch does and it turns a fault
		 * into an ordinary role change.
		 */
		j36_musb_session(p, false);
		/* -1 counts: the LK may have left the pad high and this driver
		 * cannot read back which, so the first measurement pays the
		 * fall time too rather than guessing it does not have to. */
		j36_usb_phy_vbus(p, false);
		msleep(J36_VBUS_FALL_MS);
	}

	j36_phy_release_force(p);
	released = true;
	fed = j36_port_externally_powered(p);
	if (fed < 0) {
		host = true;
		why = "the port cannot be read";
	} else {
		host = !fed;
		why = fed ? "something outside is driving the bus"
			  : "nothing is feeding the port";
	}

apply:
	/*
	 * `released' is why this is not simply "return early if the answer has
	 * not changed". The measurement above turns the force_* overrides OFF to
	 * take its reading, so a poll that measures and then agrees with itself
	 * still has to put the role back -- otherwise the second poll after boot
	 * quietly leaves the PHY reading the ID pin, which floats to B on a board
	 * with no OTG cable, and the port goes back to being the B-device this
	 * whole change is about. Both sequences are idempotent, so re-applying
	 * costs three writes and one settle and no transient.
	 */
	if (p->role_host == (int)host && !released) {
		j36_usb_phy_vbus(p, host);
		if (host)
			j36_musb_host_kick(p);
		return;
	}
	changed = p->role_host != (int)host;
	p->role_host = host;
	/* A role that has just been applied gets its retries back: the check
	 * above is about a session that failed to become one, and this is a new
	 * session. */
	p->hm_kicks = 0;
	p->hm_gave_up = false;

	if (host) {
		/*
		 * THE WHOLE HOST BRING-UP IS ONE CALL NOW, and it is MediaTek's
		 * own order rather than this file's: rail up, wait, end the
		 * session, park the PHY at "no VBUS", start the session against
		 * that, and only then bring the valids up. See
		 * j36_musb_host_arm() and the file header.
		 *
		 * What used to be here asserted the valids first and set SESSION
		 * afterwards, so the core was handed a bus that had been
		 * statically valid the whole time and had nothing to arbitrate
		 * on. It answered with DEVCTL 19 at power-on and 99 at every
		 * poll after, HM clear in both, for three consecutive boots.
		 */
		u8 devctl = j36_musb_host_arm(p, false);

		/*
		 * And say whether it took, here rather than only from the poll
		 * three seconds later.
		 *
		 * NOT taking here is not yet a fault, and this line no longer
		 * pretends otherwise: power_on runs inside musb_platform_init(),
		 * and musb_generic_disable() writes DEVCTL = 0 a moment later
		 * and musb_start() sets SESSION again from the hub thread with
		 * the PHY holding perfectly still. So whatever is armed here is
		 * torn down before anything can use it, and the arm that has to
		 * win is the one j36_musb_host_kick() runs from the first role
		 * poll, after all of that has happened.
		 */
		if (p->musb && musb_session)
			dev_info(p->dev,
				 "session started as an A-device: DEVCTL %02x [VBUS %s], the core is %s\n",
				 devctl, j36_musb_vbus_str(devctl),
				 j36_musb_host_armed(devctl)
					? "armed as a host -- the root port can scan now"
					: "NOT armed yet; musb_start() has not run, so the poll arms it again once it has");
	} else {
		/* VBUS first. A B-device that went on driving 5 V would be
		 * fighting whatever just plugged in, and on this board the
		 * thing it would be fighting with is the system rail. */
		j36_usb_phy_vbus(p, false);
		j36_musb_session(p, false);
		j36_phy_force_device(p);
	}

	if (!changed)
		return;
	if (host)
		dev_info(p->dev,
			 "port is a HOST: %s, so it sources 5 V and a stick, an SSD, a mouse or a hub can come up\n",
			 why);
	else
		dev_info(p->dev,
			 "port is a DEVICE: %s, so DRVVBUS stays low -- and on a board where CHRIN shares this net, that is what lets the PMIC see a charger\n",
			 why);
}

static void j36_usb_phy_role_work(struct work_struct *work)
{
	struct j36_usb_phy *p = container_of(to_delayed_work(work),
					     struct j36_usb_phy, role_work);

	mutex_lock(&p->lock);
	j36_usb_phy_decide_role(p);
	mutex_unlock(&p->lock);

	/* Re-read the parameter every pass rather than caching it: both of these
	 * are 0644 so the dashboard, or a shell, can change the port's mind
	 * without a reload. role_poll_ms=0 parks the poll for good. */
	if (role_poll_ms)
		schedule_delayed_work(&p->role_work,
				      msecs_to_jiffies(role_poll_ms));
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
	 * The role, and the 5 V that goes with whichever it turns out to be.
	 * This used to be two unconditional lines -- force host, raise the pad --
	 * on the grounds that the kernel is built USB_MUSB_HOST and the node says
	 * dr_mode = "host", so nothing would ever ask for anything else. Both
	 * halves of that were true and the conclusion was still wrong: dr_mode is
	 * a build-time constant and the cable is not, and on a board where the
	 * charge port IS the host port, pinning host is also pinning "never
	 * charge". decide_role() measures instead. vbus=1 restores the old two
	 * lines exactly.
	 *
	 * OFF BY DEFAULT NOW, and it is the poll below that does this instead --
	 * see role_at_power_on for the whole of why. Nothing else moves: the
	 * gate, the recover and the force-device tail all still happen in
	 * phy_init(), so the MAC is clocked and the PHY is in its resting state
	 * by the time musb_init_controller() reads a register.
	 */
	if (role_at_power_on) {
		mutex_lock(&p->lock);
		j36_usb_phy_decide_role(p);
		mutex_unlock(&p->lock);
	}

	/*
	 * Both of these are here rather than in phy_init because DEVCTL is worth
	 * a look before the core has touched it, and because power_on is still
	 * ahead of musb_start() -- which is what makes the busctl write safe.
	 * Dump first, probe second, so the dump shows the function address as
	 * musb left it rather than as the probe restored it.
	 *
	 * The dump is reads only and stays whatever the line above does. The
	 * probe writes, and it is behind musb_probe_layout, which is off.
	 */
	j36_musb_dump(p, "at power-on");
	j36_musb_probe_busctl(p);

	j36_usb_phy_scan_arm(p);
	/*
	 * And this is now the first thing that applies a role, not merely the
	 * thing that re-checks it. role_poll_ms=0 with role_at_power_on=0 is
	 * therefore a port that is never brought up at all -- which is a
	 * legitimate way to ask for a dead port, and the only way to get one by
	 * accident is to ask for both.
	 */
	if (role_poll_ms)
		schedule_delayed_work(&p->role_work,
				      msecs_to_jiffies(role_poll_ms));
	else if (!role_at_power_on)
		dev_info(p->dev,
			 "role_poll_ms=0 and role_at_power_on=0: nothing will bring the port up, so it stays in the resting device state phy_init() left it in\n");
	return 0;
}

static int j36_usb_phy_power_off(struct phy *phy)
{
	struct j36_usb_phy *p = phy_get_drvdata(phy);

	/* Both syncs before the lock, not under it: the role work takes the same
	 * lock, so cancelling it from inside would wait on itself. */
	cancel_delayed_work_sync(&p->scan_work);
	cancel_delayed_work_sync(&p->role_work);

	mutex_lock(&p->lock);
	j36_usb_phy_vbus(p, false);
	j36_phy_savecurrent(p);
	p->role_host = -1;
	mutex_unlock(&p->lock);
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
	int ret = 0;

	mutex_lock(&p->lock);

	switch (mode) {
	case PHY_MODE_USB_HOST:
		/*
		 * Advisory while the role is the measurement's. mediatek.c calls
		 * this exactly once, from mtk_musb_init(), immediately after
		 * phy_power_on() and with the device tree's dr_mode -- so acting
		 * on it would undo the measurement taken three lines earlier and
		 * pin host after all, which is the bug this file just stopped
		 * having. The poll owns the role; this is told, not obeyed.
		 */
		if (j36_role_is_auto(p)) {
			dev_dbg(p->dev,
				"set_mode(host) noted; the role follows the port\n");
			break;
		}
		/*
		 * The full arm, not just the role write. mediatek.c calls this
		 * from the tail of mtk_musb_init(), which is the last thing to
		 * touch the PHY before musb_start() -- exactly where the stock
		 * driver puts its own arm. It will be undone by
		 * musb_generic_disable()'s DEVCTL = 0 like everything else at
		 * this stage, and the poll's kick is what finally makes it
		 * stick, but arming in the stock place costs a third of a second
		 * of boot and is the one chance to get it before the hub thread.
		 */
		p->role_host = 1;
		j36_musb_host_arm(p, false);
		break;
	case PHY_MODE_USB_DEVICE:
		/*
		 * Honoured in either mode: a gadget driver asking for this knows
		 * something the port cannot be asked. The poll will re-decide,
		 * and will agree, because a host that is feeding us is exactly
		 * what the measurement reads as "be a device".
		 *
		 * Drop the 5 V first. A B-device that still drove VBUS would be
		 * fighting whatever host just plugged in, and on this board the
		 * thing it would be fighting with is the system rail.
		 */
		p->role_host = 0;
		j36_usb_phy_vbus(p, false);
		j36_phy_force_device(p);
		break;
	case PHY_MODE_USB_OTG:
		/*
		 * Releasing the overrides is a thing this file can do now --
		 * j36_phy_release_force() is what the measurement is built on --
		 * but it is not OTG. What it hands the role to is the ID pin,
		 * and no schematic or stock routine here says that pin is
		 * connected to anything; the stock LK has no sequence that
		 * releases the overrides and leaves them released, which is what
		 * a board with a wired ID pin would have. So this still answers
		 * EINVAL rather than pretending, and the honest substitute is
		 * the default vbus=-1, which decides the same question off the
		 * one signal this board does have.
		 */
		dev_warn(p->dev, "OTG mode has no override sequence on this PHY; vbus=-1 follows the port instead\n");
		ret = -EINVAL;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&p->lock);
	return ret;
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

static void j36_usb_phy_cancel_work(void *data)
{
	struct j36_usb_phy *p = data;

	cancel_delayed_work_sync(&p->scan_work);
	cancel_delayed_work_sync(&p->role_work);
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
	p->role_host = -1;		/* not decided yet; see the struct */
	mutex_init(&p->lock);

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
	p->vbus_on = -1;		/* not written yet; see the struct */
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
	INIT_DELAYED_WORK(&p->role_work, j36_usb_phy_role_work);
	ret = devm_add_action_or_reset(dev, j36_usb_phy_cancel_work, p);
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
		dev_info(dev, "MT6592 U2 PHY ready (no DRVVBUS pad, so the port never sources 5 V and a hub must be self-powered)\n");
	else if (vbus > 0)
		dev_info(dev, "MT6592 U2 PHY ready (vbus=1 pins host mode; DRVVBUS pad %d will source 5 V off VBAT -- fit a cell, and the charger is held off for as long as it does)\n",
			 p->vbus_pin);
	else if (!p->musb)
		dev_info(dev, "MT6592 U2 PHY ready (host mode on power-on; no j36,musb-controller to measure the port with, so the role cannot follow it)\n");
	else if (vbus == 0)
		dev_info(dev, "MT6592 U2 PHY ready (the role follows the port, but vbus=0 holds the 5 V off: a hub must be self-powered, and a charger is charged from)\n");
	else
		dev_info(dev, "MT6592 U2 PHY ready (the role follows the port: sourcing 5 V off DRVVBUS pad %d when nothing feeds it -- fit a cell -- and standing down to charge when something does)\n",
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
