/* SPDX-License-Identifier: GPL-2.0 */
/*
 * J36 Ultra MT6592 CONSYS Wi-Fi: the register map and the state the three
 * translation units share.
 *
 * Ported from PowerEngine/OS/MVII's mt6592_wifi_sdio.c and mt6592_wifi_wmt.c.
 * Those two files are a from-scratch reimplementation of MediaTek's conn_soc
 * bring-up, established against this board a register at a time, and every
 * offset, bit and command byte below came out of them.  Their comments carry the
 * account of where each number was found and which of them cost a boot to get
 * wrong; this port keeps the parts a reader of the Linux driver needs.
 *
 *
 * ── WHAT THE HARDWARE IS ──
 *
 * There is no SDIO combo chip on this board.  The MT6592 die carries the
 * connectivity subsystem (CONSYS_6592) on-chip, behind its own power domain, and
 * bringing Wi-Fi up is four stages that must happen in this order:
 *
 *   1. POWER.  MT6323 rails through the PMIC wrapper, then the CONSYS MTCMOS
 *      domain, then the INFRA_CONNMCU clock gate, then a chip-ID read that is the
 *      first thing the subsystem itself has to answer.  j36_mt6592_wifi_consys.c.
 *   2. LINK.  BTIF -- a UART-shaped block wired die-to-die rather than to a pin --
 *      carrying MediaTek's STP framing, and WMT on top of that.  Two ROM patches
 *      go down this link before the connectivity MCU has anything patched to run.
 *      j36_mt6592_wifi_wmt.c.
 *   3. FIRMWARE.  WIFI_RAM_CODE_SOC over the AHB HIF, the A-die probe, the ROM
 *      handoff, WIFI_START, and WLAN_READY.  j36_mt6592_wifi_hif.c.
 *   4. cfg80211 and a netdev.
 *
 * Stages 1 to 3 are what this build does.  Stage 4 is not here yet, and the
 * driver says so in its own log rather than implying a radio it has not got:
 * WLAN_READY means the firmware is executing, not that anything can be sent
 * through it.  The scan, association and data paths are a further layer on top
 * of this transport and are not in this build.
 *
 *
 * ── WHY THIS DRIVER DOES NOT MAP THE PMIC WRAPPER ──
 *
 * The connectivity rails live on the MT6323 companion die, reached through the
 * same WACS2 bridge j36_mt6592_pmic reads the battery gauge through.  That bridge
 * is one state machine with one result register and no arbitration, so there is
 * one owner of it in this kernel and everyone else goes through the two calls in
 * j36_mt6592_pmic.h.  The load-order consequence is deliberate: this module will
 * not insmod at all without the PMIC module, and its probe defers until the PMIC
 * device has actually bound.
 *
 *
 * ── WHAT THE PORT CHANGED ──
 *
 * MVII runs inside a cooperative frame pump with no scheduler to hide behind, so
 * its BTIF code busy-polls, counts microseconds off the GPT by hand, and hands
 * the CPU back at named safe points because nothing else would.  Here the whole
 * bring-up is one work item on a workqueue and may sleep, so:
 *
 *   - deadlines are ktime rather than a hand-read timer, and the between-frames
 *     waits are real sleeps.  The mid-frame waits are still busy polls: the peer
 *     is pushing bytes into a small FIFO while we read, and a sleep there is
 *     measured against 39 microseconds of FIFO at this line rate.
 *   - the cooperative-yield bookkeeping is gone.  A workqueue that sleeps is
 *     already off the CPU, and the places MVII yields at are exactly the places
 *     this sleeps at.
 *   - the two ROM patches come from request_firmware() instead of being handed in
 *     by a bootstrap that had already read the stock system partition.
 *   - the diagnostics counters stay.  A ROM patch is around a hundred fragments
 *     of a thousand bytes each, and without them a single dropped byte anywhere
 *     in the transfer is a bring-up that failed with nothing to say about where.
 */
#ifndef J36_MT6592_WIFI_H
#define J36_MT6592_WIFI_H

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/workqueue.h>

/* ── INFRACFG_AO, by phandle ─────────────────────────────────────────────────
 *
 * MediaTek's own MT6592 platform sources, at the kernel virtual bases
 * (INFRACFG_AO 0xF0001000) whose physical alias is 0x10001000:
 *
 *	PDN_CLR / PDN_STA		mt_clkmgr.h:69-70
 *	TOPAXI_PROT_EN / _STA1		mt_clkmgr.h:72-73
 *
 * CONSYS_EMI_MAP is the one that looks wrong and is not: the connectivity header
 * calls it TOPCKGEN_BASE + 0x1310 and defines TOPCKGEN_BASE as INFRA_BASE
 * (mtk_wcn_consys_hw.h:120, :50), so it is 0xF0001310 -- the same register this
 * file reaches as INFRACFG_AO + 0x0310.  Two names, one address.
 *
 * INFRA_CONNMCU_GATE is derived rather than quoted, because stock never writes it
 * directly: it calls enable_clock(MT_CG_INFRA_CONNMCU), MT_CG_INFRA_CONNMCU is 44
 * (mt_clkmgr.h:166) and the CG_INFRA group starts at 32 (:157), so the bit is 12.
 * That group's descriptor (mt_clkmgr.c:2369-2375) names PDN_CLR as its enable
 * address and PDN_STA as its status, which is why enabling is a write of the bit
 * to _CLR and the check is for a zero in _STA.
 */
#define J36_INFRA_PDN_CLR		0x0044
#define J36_INFRA_PDN_STA		0x0048
#define J36_INFRA_TOPAXI_PROT_EN	0x0220
#define J36_INFRA_TOPAXI_PROT_STA1	0x0228
#define J36_INFRA_CONSYS_EMI_MAP	0x0310

#define J36_INFRA_CONNMCU_GATE		BIT(12)
#define J36_CONN_PROT_MASK		0x0104

/* CONSYS_EMI_MAPPING: [11:0] is the share window's DRAM base in 1 MiB units --
 * twelve bits is exactly 4 GiB of reach -- and bit 12 enables it. */
#define J36_CONSYS_EMI_BASE_MASK	0x00000fff
#define J36_CONSYS_EMI_ENABLE		BIT(12)

/* The half of the aperture stock ioremaps and zeroes: base + 0x80000 for 0x55c00
 * bytes.  Named in source as CONSYS_EMI_AP_PHY_OFFSET and CONSYS_EMI_MEM_SIZE
 * (343 * KBYTE, which is 0x55c00 exactly -- mtk_wcn_consys_hw.h:109, :112). */
#define J36_CONSYS_EMI_SHARE_OFFSET	0x00080000
#define J36_CONSYS_EMI_SHARE_SIZE	0x00055c00

/* ── SPM, by phandle ─────────────────────────────────────────────────────────
 *
 * SPM_CONN_PWR_CON is mt_spm.h:41 and the two status words are :102-103.  The
 * power-control bits are mt_spm_mtcmos.c:774-795 verbatim; the one renamed here
 * is CONN_SRAM_PDN, which stock calls MD_SRAM_PDN (:786) -- the same bit 8 of the
 * same CONN register, under a name inherited from the modem block it was copied
 * from.
 */
#define J36_SPM_CONN_PWR_CON		0x0280
#define J36_SPM_PWR_STATUS		0x060c
#define J36_SPM_PWR_STATUS_S		0x0610

#define J36_CONN_PWR_STA_MASK		BIT(1)
#define J36_PWR_RST_B			BIT(0)
#define J36_PWR_ISO			BIT(1)
#define J36_PWR_ON			BIT(2)
#define J36_PWR_ON_S			BIT(3)
#define J36_PWR_CLK_DIS			BIT(4)
#define J36_CONN_SRAM_PDN		BIT(8)

/* ── PERICFG, by phandle ─────────────────────────────────────────────────────
 *
 * The BTIF clock gate, in the same PDN0 bank j36_mt6592_usb_phy and the PMIC's
 * BC1.2 path clear their own bits of.  An APB access to a gated MediaTek
 * peripheral does not fault -- it stalls the bus until the watchdog resets the
 * board -- so nothing may touch the BTIF window before this bit reads zero.
 */
#define J36_PERI_PDN0_CLR		0x0010
#define J36_PERI_PDN0_STA		0x0018
#define J36_PERI_BTIF_GATE		BIT(20)

/* ── BTIF, our own window at 0x1100c000 ──────────────────────────────────────
 *
 * A UART-shaped block with no pins on it: the far end is the connectivity MCU on
 * the same die.  Interrupts are deliberately left masked and everything is
 * polled, because bring-up is a strictly sequential exchange on a workqueue and
 * an IRQ handler would buy nothing but a routing question nobody has answered for
 * this block.
 */
#define J36_BTIF_RBR			0x0000
#define J36_BTIF_THR			0x0000
#define J36_BTIF_IER			0x0004
#define J36_BTIF_FIFOCTRL		0x0008
#define J36_BTIF_FAKELCR		0x000c
#define J36_BTIF_LSR			0x0014
#define J36_BTIF_SLEEP_EN		0x0048
#define J36_BTIF_DMA_EN			0x004c
#define J36_BTIF_TRI_LVL		0x0060
#define J36_BTIF_WAK			0x0064
#define J36_BTIF_HANDSHAKE		0x006c

#define J36_BTIF_LSR_DR			BIT(0)
#define J36_BTIF_LSR_THRE		BIT(5)
#define J36_BTIF_LSR_TEMT		BIT(6)
#define J36_BTIF_FIFOCTRL_CLR_RX	BIT(1)
#define J36_BTIF_FIFOCTRL_CLR_TX	BIT(2)
#define J36_BTIF_DMA_AUTORESET		BIT(2)
#define J36_BTIF_HANDSHAKE_ENABLE	BIT(0)
#define J36_BTIF_TX_FIFO_SIZE		16
#define J36_BTIF_TX_THRESHOLD		8

/* ── CONSYS, our own window at 0x18070000 ────────────────────────────────────
 *
 * CONSYS_CHIP_ID is mtk_wcn_consys_hw.h:70, at CONN_MCU_CONFIG_BASE + 0x08.
 *
 * CONSYS_MCU_CPUPCR is the connectivity MCU's program counter, and it is the one
 * window onto what that core is actually doing.  Stock reads it and nothing else:
 * wmt_plat_read_cpupcr is three instructions, `movt r3, #0xf807' then `ldr r0,
 * [r3, #0x160]', and its whole post-mortem path (stp_dbg_poll_cpupcr) is that
 * read in a loop.  It needs neither the HIF nor a running firmware, so it is
 * valid at any point -- including after the boot ROM has stopped answering,
 * which is exactly when every other instrument here goes quiet.
 */
#define J36_CONSYS_CHIP_ID		0x0008
#define J36_CONSYS_MCU_CPUPCR		0x0160

/* ── the WLAN AHB HIF, our own window at 0x180f0000 ──────────────────────────
 *
 * Stage 3's transport.  A PIO FIFO pair with a descriptor register in front of
 * it, not a DMA engine: HSTCR says how many bytes are about to move and in which
 * direction, and then the data words go through one address.
 *
 * Offsets are MediaTek's own MCR_* names from hif_sdio / ahb.c.  The four
 * software mailboxes are worth having even though nothing writes them here --
 * D2HRM0R is the single word stock bothers to print when the readiness poll
 * expires ("Waiting for Ready bit: Timeout, ID=%u"), which makes it the one
 * chip-authored number MediaTek's own author thought useful on this failure.
 */
#define J36_HIF_WCIR			0x0000
#define J36_HIF_WHLPCR			0x0004
#define J36_HIF_WHCR			0x000c
#define J36_HIF_WHISR			0x0010
#define J36_HIF_WHIER			0x0014
#define J36_HIF_WASR			0x0018
#define J36_HIF_WTSR0			0x0020
#define J36_HIF_WTSR1			0x0024
#define J36_HIF_WTDR0			0x0028
#define J36_HIF_WTDR1			0x002c
#define J36_HIF_WRDR0			0x0030
#define J36_HIF_WRDR1			0x0034
#define J36_HIF_H2DSM0R			0x0038
#define J36_HIF_H2DSM1R			0x003c
#define J36_HIF_D2HRM0R			0x0040
#define J36_HIF_D2HRM1R			0x0044
#define J36_HIF_WRPLR			0x0050
#define J36_HIF_HSTCR			0x0058

#define J36_WCIR_WLAN_READY		BIT(21)
#define J36_WCIR_REVISION_MASK		0x000f0000
#define J36_WCIR_REVISION_SHIFT		16
#define J36_WCIR_CHIP_ID_MASK		0x0000ffff

#define J36_WHLPCR_DRIVER_OWN_REQ	BIT(9)
#define J36_WHLPCR_IS_DRIVER_OWN	BIT(8)
#define J36_WHLPCR_INT_EN_CLR		BIT(1)

/*
 * The two fields nicSDIOInit clears and nothing else.  MVII once also cleared
 * bits 1 and 2 and set the RX length count to 1; neither is anything stock does,
 * and with a firmware refusing to come up after a byte-identical download there
 * is no room for embellishment here.
 */
#define J36_WHCR_RX_ENHANCE_MODE_EN	BIT(16)
#define J36_WHCR_MAX_HIF_RX_LEN_MASK	0x000000f0
#define J36_WHIER_DEFAULT		0xffffff0f

#define J36_HSTCR_BURST_4_DW		1
#define J36_HSTCR_BURST_SHIFT		24
#define J36_HSTCR_TARGET_SHIFT		20
#define J36_HSTCR_COUNT_MASK		0x000ffffc

#define J36_HIF_TARGET_TXD0		0
#define J36_HIF_TARGET_TXD1		1
#define J36_HIF_TARGET_RXD0		2
#define J36_HIF_TARGET_RXD1		3

/*
 * HIF_RX_HW_APPENDED_LEN, hif_rx.h:120.  WRPLR reports the packet's own length
 * and the hardware queues one further DW of status behind it, so the read has to
 * take ALIGN(len + 4, 4) -- nicRxEnhanceReadBuffer does exactly that.  For a
 * length already a multiple of four the two differ by a whole DW, and leaving it
 * in the FIFO does not corrupt one frame, it misframes the port permanently.
 */
#define J36_HIF_RX_HW_APPENDED_LEN	4

/* ── the boot ROM's INIT protocol ────────────────────────────────────────────
 *
 * Everything below WIFI_START is answered by the WLAN boot ROM, not by firmware,
 * and every one of these commands stops working the instant WIFI_START is
 * dispatched -- the ROM's command handler is gone from that moment.
 */
#define J36_INIT_CMD_DOWNLOAD_BUF	1
#define J36_INIT_CMD_WIFI_START		2
#define J36_INIT_CMD_ACCESS_REG		3
/*
 * Not in any MediaTek header: read off the ROM's own INIT_CMD dispatcher, whose
 * type-7 arm copies the payload and then unconditionally posts message 46
 * parameter 3 to wifi_task -- which is the one thing that makes the ROM run the
 * MT6625L A-die probe.  See j36_hif_run_adie_probe().
 */
#define J36_INIT_CMD_ADIE_PROBE		7
#define J36_INIT_EVENT_CMD_RESULT	1
#define J36_INIT_EVENT_ACCESS_REG	2

/*
 * The download mode word.  DL_MODE_RESET_SEC_IV is deliberately NOT set: the
 * cipher is 16-byte ECB (section 1's ciphertext holds an unbroken run of 991
 * identical blocks over a run of zero plaintext, which chaining would make all
 * different), so there is no IV to reset -- and setting it made the ROM start
 * reporting a pending error naming section 0's own destination.
 */
#define J36_INIT_DOWNLOAD_ENCRYPTION	BIT(0)
#define J36_INIT_DOWNLOAD_ACK		BIT(31)

#define J36_INIT_DOWNLOAD_HEADER_SIZE	24
#define J36_INIT_START_PACKET_SIZE	16
#define J36_INIT_ACCESS_REG_PACKET_SIZE	20
#define J36_INIT_ADIE_PROBE_PACKET_SIZE	20

/*
 * MTK_WIFI_SIGNATURE, BUILD_SIGN('M','T','K','W') (wlan_lib.h:623-627).  The
 * macro shifts 'M' into the low byte, so on this little-endian part the four
 * bytes land in the file in the order written.  wlanAdapterStart tests the same
 * word at offset 0 to choose between a divided download and a flat one, and a
 * WMT ROM patch does not carry it -- which is what tells the two asset kinds
 * apart without knowing their filenames.
 */
#define J36_MTK_WIFI_SIGNATURE		0x574b544d
#define J36_MAX_FIRMWARE_SECTIONS	16
#define J36_FIRMWARE_CHUNK_SIZE		2048

#define J36_HIF_TX_BUFFER_SIZE		2176
#define J36_HIF_RX_BUFFER_SIZE		4096
/* Six, because that is how many bytes nicTxReleaseResource walks and how many
 * WTSR0/WTSR1 carry between them. */
#define J36_HIF_TX_CLASSES		6

#define J36_HIF_DRIVER_OWN_TIMEOUT_US	250000
#define J36_HIF_INIT_ACK_TIMEOUT_US	1000000
#define J36_HIF_READY_TIMEOUT_US	6000000

/* ── addresses inside the connectivity ROM's own data ────────────────────────
 *
 * Reached through INIT_CMD_ACCESS_REG, which is the ROM answering a read or a
 * write on our behalf.  That matters because no AP aperture reaches these: the
 * window onto CONSYS SRAM covers 0x18070000..0x180bffff and these are below it.
 *
 * The global pointer is out of the ROM's reset code -- `sethi r29, #0x2097' then
 * `ori r29, r29, #0x324'.  ORI's immediate is 15 bits, not 20; reading it as
 * 0x8324 puts gp 0x8000 too high and every offset below lands in nothing.
 */
#define J36_CONSYS_ROM_GP		0x02097324

/*
 * The A-die probe's "already probed" byte.  Firmware asserts at rlm_phy.c:4209
 * if the ROM routine that returns this byte returns 0, and the ONLY writer of it
 * is the last instruction of the MT6625L probe -- which is straight-line code,
 * so a zero here does not mean the probe ran and failed, it means the probe was
 * never entered.  Forcing it to 1 gets the firmware running and makes the probe
 * permanently impossible (the sequence returns early on exactly this byte),
 * which leaves the RF front end at reset values: a receiver that hears no
 * beacons and a transmitter that collapses the rail when it keys.  So the byte
 * is cleared, the ROM is asked to probe for real, and forcing it is the fallback.
 */
#define J36_CONSYS_ROM_ADIE_PROBED	(J36_CONSYS_ROM_GP - 27920)

/*
 * The firmware handoff gate.  WIFI_START stores the entry address and sets mode
 * 2, then the ROM's tail tests three bytes to decide whether to act on the mode
 * or enqueue a deferred item and go back to its receive loop.  With all three
 * zero -- which is what this chip reads back -- it takes the second arm and
 * never looks at the mode again, because WIFI_START is the only command that
 * reaches that test at all.  Setting bytes +1 and +2 takes the other arm.
 * Measured: the connectivity PC went from a fixed 0x00066382 (the WMT patch's
 * idle) to 597 changes in 598 samples, sampled inside the downloaded section 0.
 * Both bytes are in one word and that word reads back zero, so one write does it.
 */
#define J36_CONSYS_ROM_HANDOFF_GATE	(J36_CONSYS_ROM_GP - 14008)
#define J36_CONSYS_ROM_HANDOFF_RELEASE	0x00010100

/* What happened to the A-die probe on this boot.  The difference between "the RF
 * front end was configured by its own probe" and "we forged the flag that says
 * it was" is invisible in every other field, and it is the difference between a
 * receiver that hears beacons and one that does not. */
enum j36_adie_probe_state {
	J36_ADIE_PROBE_NOT_RUN = 0,
	J36_ADIE_PROBE_RAN,		/* the ROM set the flag itself	     */
	J36_ADIE_PROBE_STUCK,		/* the flag would not clear: forged  */
	J36_ADIE_PROBE_TIMEOUT,		/* asked, never came up: forged	     */
	J36_ADIE_PROBE_ROM_SILENT,	/* the ROM stopped answering: forged */
};

/* ── the four MT6323 registers the connectivity rails live in ────────────────
 *
 * upmu_hw.h:234-249 with PMIC_REG_BASE dropped, because pwrap addresses the die
 * directly.  Which bit is in which register is not guessable from the field
 * names, because VCN33 is split across two: BT's enable and mux are in CON16
 * alongside the SHARED VOSEL, and Wi-Fi's are in CON17.  The stock accessors
 * settle it, each naming its own register (drivers/power/upmu_common.c):
 *
 *	upmu_set_rg_vcn33_vosel		ANALDO_CON16  [3:2]	:11192
 *	upmu_set_vcn33_on_ctrl_bt	ANALDO_CON16  bit 5	:11220
 *	upmu_set_rg_vcn33_en_bt		ANALDO_CON16  bit 7	:11246
 *	upmu_set_rg_vcn33_en_wifi	ANALDO_CON17  bit 12	:11285
 *	upmu_set_vcn33_on_ctrl_wifi	ANALDO_CON17  bit 14	:11298
 *	upmu_set_rg_vcn28_en		ANALDO_CON19  bit 12	:11405
 *	upmu_set_vcn28_on_ctrl		ANALDO_CON19  bit 14	:11418
 *	upmu_set_vcn_1v8_lp_mode_set	DIGLDO_CON11  bit 1	:12317
 *	upmu_set_rg_vcn_1v8_en		DIGLDO_CON11  bit 14	:12359
 *
 * Two of those names mislead if taken at face value: VCN33_ON_CTRL_BT is bit 5
 * while VCN33_ON_CTRL_WIFI is bit 14, and VCN_1V8's low-power bit is 1 while its
 * enable is 14.  Neither pair is symmetric.
 */
#define J36_PMIC_ANALDO_CON16		0x0416
#define J36_PMIC_ANALDO_CON17		0x0418
#define J36_PMIC_ANALDO_CON19		0x041c
#define J36_PMIC_DIGLDO_CON11		0x0512

#define J36_PMIC_VCN18_LP_MODE		BIT(1)
#define J36_PMIC_VCN18_ENABLE		BIT(14)
#define J36_PMIC_VCN28_ENABLE		BIT(12)
#define J36_PMIC_VCN28_ON_CTRL		BIT(14)
#define J36_PMIC_VCN33_BT_ON_CTRL	BIT(5)
#define J36_PMIC_VCN33_BT_ENABLE	BIT(7)
#define J36_PMIC_VCN33_WIFI_ENABLE	BIT(12)
#define J36_PMIC_VCN33_WIFI_ON_CTRL	BIT(14)

/*
 * ANALDO_CON16[3:2] is RG_VCN33_VOSEL -- ONE selector for ONE regulator, even
 * though VCN33 has two independent enable/mux paths.  dct_pmic_VCN33_sel() maps
 * VOL_3300 to 0 while VOL_DEFAULT and VOL_3600 both map to 3 (pmic.c:2117), and
 * hwPowerOn(VCN33_BT|VCN33_WIFI, VOL_3300) is what the connectivity paldo
 * controls ask for -- so 0 is the only correct code here.  Leaving the field at
 * its reset value runs the connectivity front end at 3.6 V.
 */
#define J36_PMIC_VCN33_VOSEL_MASK	(0x3 << 2)
#define J36_PMIC_VCN33_VOSEL_3300	(0x0 << 2)

/*
 * co_clock_flag, straight out of the device's own /etc/firmware/WMT_SOC.cfg:
 *
 *	coex_wmt_ant_mode=1
 *	wmt_gps_lna_pin=0
 *	wmt_gps_lna_enable=0
 *	co_clock_flag=1
 *
 * The stock driver reads that file at probe and threads the value through the
 * whole bring-up: the VCN28 branch, the oscillator setup, and whether the
 * CO_CLOCK command is sent at all.  One definition here, so the branches cannot
 * disagree with each other.
 */
#define J36_WIFI_CO_CLOCK_FLAG		1

/* ── how long things are given ───────────────────────────────────────────────
 *
 * The two MTCMOS/clock acks are unbounded spins in stock.  Bounding them is not a
 * liberty: MediaTek reach the same conclusion themselves in the power-DOWN path,
 * where the equivalent wait carries an `if (count > 1000) break'
 * (mt_spm_mtcmos.c:1354-1357).  A driver that hangs here has taken the workqueue
 * with it and has nothing to say about why.
 */
#define J36_CONSYS_POLL_TIMEOUT_US	200000
#define J36_CHIP_ID_RETRIES		10
#define J36_BTIF_CLOCK_TIMEOUT_US	100000
#define J36_BTIF_IO_TIMEOUT_US		2000000
#define J36_WMT_COMMAND_TIMEOUT_US	2000000
#define J36_WMT_CALIBRATION_TIMEOUT_US	10000000

/* ── STP, MediaTek's serial transport ────────────────────────────────────────
 *
 * stp_exp.h:31-34 names all three task ids and caps the field: WMT_TASK_INDX 4,
 * STP_TASK_INDX 5, INFO_TASK_INDX 6, MTKSTP_MAX_TASK_NUM 7 -- which is why the
 * parser masks the type nibble with 7 rather than 15, exactly as stock does with
 * 0x70 >> 4 (stp_core.c:2089).
 *
 * Task 5 (firmware assert/coredump) and task 6 (runtime firmware log) are parsed
 * by MTKSTP_FW_MSG in the stock core: no header checksum, no CRC, no ACK, and no
 * receive-sequence consumption.  The dispatch is ahead of the checksum test
 * (stp_core.c:2193-2196), so a firmware message is taken on the strength of its
 * type byte alone -- which is the point, since the chip sends these while it is
 * dying.
 */
#define J36_STP_HEADER_SIZE		4
#define J36_STP_CRC_SIZE		2
#define J36_STP_WMT_TASK		4
#define J36_STP_FW_LOG_TASK		5
#define J36_STP_INFO_TASK		6
#define J36_STP_MAX_PAYLOAD		2048
#define J36_STP_RESYNC_SIZE		4
/* stp_core.h:76-77: MTKSTP_TX_TIMEOUT is 180 ms and MTKSTP_RETRY_LIMIT is 10. */
#define J36_STP_TX_TIMEOUT_US		180000
#define J36_STP_RETRY_LIMIT		10

/* ── the ROM patch image ─────────────────────────────────────────────────────
 *
 * struct WMT_PATCH (wmt_core.h:322-328) is ucDateTime[16] + ucPLat[4] + u2HwVer +
 * u2SwVer + u4PatchVer = 28 bytes, and mtk_wcn_soc_patch_dwn steps past exactly
 * that before the first fragment (wmt_ic_soc.c:1702-1703).
 */
#define J36_WMT_PATCH_HEADER_SIZE	28
/*
 * Byte 24 of that header: (patch_count << 4) | download_sequence, and the first
 * of the four u4PatchVer bytes.  The derivation -- and why the three bytes after
 * it are the patch's destination address rather than part of a version number --
 * is at the part_address[] declaration in j36_wifi_wmt_load_patch().
 */
#define J36_WMT_PATCH_METADATA_OFFSET	24
/* DEFAULT_PATCH_FRAG_SIZE, wmt_ic_soc.c:40. */
#define J36_WMT_PATCH_FRAGMENT_SIZE	1000
/* WMT_PATCH_FRAG_1ST/MID/LAST, wmt_ic_soc.c:41-43. */
#define J36_WMT_PATCH_FIRST		1
#define J36_WMT_PATCH_MIDDLE		2
#define J36_WMT_PATCH_LAST		3

#define J36_WMT_FRAME_BUFFER_SIZE	1100
#define J36_WMT_EVENT_BUFFER_SIZE	256

/* WMTDRV_TYPE_*, wmt_exp.h:69-73. */
#define J36_WMT_SUBSYSTEM_BT		0
#define J36_WMT_SUBSYSTEM_FM		1
#define J36_WMT_SUBSYSTEM_GPS		2
#define J36_WMT_SUBSYSTEM_WIFI		3
#define J36_WMT_SUBSYSTEM_WMT		4

/*
 * The link counters.
 *
 * These are not decoration and they are not for a bug report that will never be
 * filed.  A ROM patch is about a hundred fragments of a thousand bytes, and a
 * single byte lost anywhere in that transfer ends the bring-up at a fragment
 * index nobody recorded.  Each one separates a pair of failures that look
 * identical from the outside: acks says the peer took a frame it produced no
 * event for, retx says the link needed the resync path, short says a command's
 * event arrived smaller than the stock tables promise, refills says a frame was
 * assembled across FIFO loads rather than going out whole.
 */
struct j36_stp_stats {
	u32 header_errors;
	u32 length_errors;
	u32 crc_errors;
	u32 oversize_events;
	u32 last_oversize_size;
	u32 fw_messages;
	u32 out_of_order;
	u32 retransmits;
	u32 acks;
	u32 short_events;
	u32 tx_refills;
	u32 tx_bytes;
	u32 rx_bytes;
	u32 patch_bytes;
	u32 last_btif_lsr;
	u32 last_event_size;
	u32 last_event_opcode;
	u32 last_tx_us;
	u32 max_tx_us;
	u32 fragment_index;
	u32 fragment_count;
	u32 fragment_size;
};

/*
 * What the HIF stage saw, kept because almost none of it can be asked for twice.
 *
 * WCIR, the mailboxes and the FIFO status registers all clear or move on, and
 * the connectivity program counter describes a core that is either running or
 * has stopped -- by the time anything reads them from sysfs the answer is gone.
 * The two that decide the whole stage are cpupcr_changes and adie_probe_state:
 * the first says whether the downloaded firmware is executing at all (a stopped
 * core reports the same zero packets as a running one that dislikes us), and the
 * second says whether the RF front end was configured or merely declared to be.
 */
struct j36_hif_stats {
	u16 chip_id;
	u8 revision;

	u32 firmware_size;
	u32 firmware_sections;
	u32 downloaded_bytes;
	u32 start_address;

	u32 last_wcir;
	u32 last_whisr;
	u32 last_wasr;
	u32 last_wrplr;
	/* Sampled either side of the readiness poll.  Equal means the firmware
	 * never wrote a mailbox at all; a change means it lived long enough to
	 * say something, whatever it said. */
	u32 mailbox_at_start[2];
	u32 last_mailbox[2];

	/*
	 * Four back-to-back samples taken immediately before WIFI_START goes out.
	 * They are the control: at that instant the boot ROM is provably running,
	 * so if these four are already identical the register is not a program
	 * counter on this part and nothing after it means anything.
	 */
	u32 cpupcr_before[4];
	u32 cpupcr_first;
	u32 cpupcr_last;
	u32 cpupcr_min;
	u32 cpupcr_max;
	u32 cpupcr_samples;
	u32 cpupcr_changes;

	u32 adie_probe_polls;
	u8 adie_probe_state;
	/* The flag was already set on entry and had to be taken back.  Believing
	 * it would have meant reporting a previous boot's success as this one's. */
	bool adie_probe_stale;

	u32 dropped_packets;
	u32 tx_credited;
	u8 tx_free[J36_HIF_TX_CLASSES];
	u8 tx_max[J36_HIF_TX_CLASSES];

	/* The first thing the peer volunteered between WIFI_START and the ready
	 * bit.  Stock never looks; stock also gets a firmware that comes up.  If
	 * ours refuses the start, this is the only window the refusal exists in. */
	u8 start_event[8];
	u32 start_event_length;
	bool start_event_valid;
};

struct j36_wifi {
	struct device *dev;

	/* Borrowed by phandle: shared SoC blocks that belong to no one driver. */
	void __iomem *infracfg;
	void __iomem *pericfg;
	void __iomem *spm;

	/* Ours, from reg. */
	void __iomem *btif;
	void __iomem *consys;
	void __iomem *hif;

	/* The CONSYS EMI aperture: a no-map reserved-memory region, so it is not
	 * in the kernel's linear map and ioremap_wc() will take it. */
	void __iomem *emi;
	phys_addr_t emi_phys;
	resource_size_t emi_size;

	/* Serialises the whole bring-up against anything that pokes the driver
	 * from sysfs or a rebind while it runs. */
	struct mutex lock;
	struct work_struct bring_up;

	u32 chip_id;
	u32 pwr_status;
	u32 emi_mapping;

	bool rails_programmed;
	bool mtcmos_ready;
	bool infra_clock_ready;
	bool consys_responds;
	bool wifi_rail_on;
	bool btif_ready;
	bool stp_full_mode;
	bool calibrated;
	bool peer_answered_after_reset;
	bool ready;

	/* Stage 3.  firmware_alive is raised in exactly one place, where the ready
	 * bit is seen, so no path can reach a command sender with the download's
	 * eight-page credit table still installed. */
	bool hif_ready;
	bool driver_own;
	bool firmware_loaded;
	bool firmware_alive;

	/* Which of the patch set have gone down, and how many there are.  The
	 * count comes out of the first image's own header. */
	u8 patch_mask;
	u8 patch_count;

	/*
	 * The three sequence counters, with stock's opening values
	 * (stp_core.c:327-331): txseq 0, txack 7, expected_rxseq 0.  The 7 is not
	 * a sentinel for "nothing received yet" that we chose -- it is stock's own
	 * opening ack, and it is what the first frame carries in its low three
	 * bits.  Fields are three bits wide (MTKSTP_SEQ_SIZE 8, stp_core.h:72),
	 * which is the & 7 everywhere in the code.
	 */
	u8 tx_sequence;
	u8 expected_rx_sequence;
	u8 last_rx_sequence;
	u32 last_ack;

	struct j36_stp_stats stats;
	struct j36_hif_stats hif_stats;

	/* The HIF's own sequence counter.  Separate from the STP one because the
	 * two links are unrelated: this one is echoed back in the init event's
	 * fourth byte and is how an answer is matched to its question. */
	u8 command_sequence;

	/* Not on the stack: a patch fragment frame is 1011 bytes and this runs on
	 * a workqueue thread whose stack is two pages. */
	u8 frame[J36_WMT_FRAME_BUFFER_SIZE];
	u8 command[J36_WMT_FRAME_BUFFER_SIZE];
	u8 event[J36_WMT_EVENT_BUFFER_SIZE];

	/* Same reasoning, one stage down: a download packet is 2072 bytes. */
	u8 hif_tx[J36_HIF_TX_BUFFER_SIZE];
	u8 hif_rx[J36_HIF_RX_BUFFER_SIZE];

	/* Why the radio is not up, in the words the log used.  NULL once it is. */
	const char *blocked;
};

/* j36_mt6592_wifi_consys.c */
int j36_wifi_consys_bind(struct j36_wifi *w);
int j36_wifi_set_bt_rail(struct j36_wifi *w, bool enable);
int j36_wifi_set_wifi_rail(struct j36_wifi *w, bool enable);

/* j36_mt6592_wifi_wmt.c */
int j36_wifi_wmt_probe_link(struct j36_wifi *w);
int j36_wifi_wmt_load_patch(struct j36_wifi *w, const void *data, size_t size);
void j36_wifi_wmt_trace(struct j36_wifi *w, const char *phase);

/* Shared by all three: record why we stopped, once, in one place. */
void j36_wifi_fail(struct j36_wifi *w, const char *blocked, const char *fmt, ...)
	__printf(3, 4);

#endif /* J36_MT6592_WIFI_H */
