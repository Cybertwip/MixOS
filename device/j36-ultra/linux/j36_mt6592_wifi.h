/* SPDX-License-Identifier: GPL-2.0 */
/*
 * J36 Ultra MT6592 CONSYS Wi-Fi: the register map and the state the six
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
 *   4. THE RADIO.  The firmware's own command and event protocol -- scan, channel
 *      privilege, station record, BSS info, keys -- in
 *      j36_mt6592_wifi_cmd.c, and cfg80211 plus wlan0 on top of it in
 *      j36_mt6592_wifi_net.c.
 *
 * All four are here.  Stage 4 is a FULLMAC driver and not a mac80211 one, which
 * is the single most consequential fact about this port: the firmware owns the
 * MAC.  It builds and answers management frames itself, it keeps the station
 * record and the BSS, it does the encryption once we hand it a key, and what
 * crosses the HIF in either direction is either a command, an event, a whole
 * 802.11 management frame or an Ethernet frame.  There is nothing for mac80211
 * to do here and it is not in this build.
 *
 * The division of labour with userspace is the ordinary fullmac one, and it is
 * what wpa_supplicant and NetworkManager on this rootfs already expect:
 *
 *   this driver	scan, open-system authentication, association, the keys
 *			the supplicant hands down, and the Ethernet data path.
 *   wpa_supplicant	the PSK, the four-way handshake, and the saved networks.
 *
 * So the EAPOL frames of the handshake go out and come back through wlan0 like
 * any other traffic -- see the is_1x flag on the transmit path, which is what
 * puts them on TC4 and asks for an acknowledgement -- and the keys arrive back
 * through cfg80211's .add_key.  Nothing in here computes a PMK.
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
#include <linux/ieee80211.h>
#include <linux/if_ether.h>
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

/* WHISR's low nibble.  Only two of the four are acted on: TX_DONE says pages
 * have been freed and WTSR should be read, ABNORMAL says the FIFO pair has
 * faulted and nothing further will be carried on it.  The two RX_DONE bits are
 * not consulted, because WRPLR reports a pending length directly and polling
 * that is both cheaper and honest about a packet that arrived between the two
 * reads. */
#define J36_WHISR_TX_DONE_INT		BIT(0)
#define J36_WHISR_RX0_DONE_INT		BIT(1)
#define J36_WHISR_RX1_DONE_INT		BIT(2)
#define J36_WHISR_ABNORMAL_INT		BIT(3)

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

/* Two seconds, and not a guess: the ROM handles the probe request asynchronously
 * on wifi_task, and the probe's own SPI helper spins up to 32000 iterations per
 * transfer, so the flag can be several hundred milliseconds behind the command. */
#define J36_ADIE_PROBE_POLL_INTERVAL_MS	20
#define J36_ADIE_PROBE_POLL_LIMIT	100

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

	/* Stage 4, kept here rather than in the netdev's own statistics because
	 * these are HIF-level counts and they exist whether a netdev has been
	 * registered or not: a link that is up and passing nothing is a different
	 * fault from one whose frames never left the FIFO. */
	u32 tx_waits;
	u32 tx_starved;
	u32 tx_forced;
	u32 rx_events;
	u32 rx_management;
	u32 rx_data;
	u32 tx_commands;
	u32 tx_frames;
};

/* ── stage 4: the firmware's own command and event protocol ──────────────────
 *
 * Everything below WLAN_READY is the boot ROM's INIT protocol, above; everything
 * here is the running firmware's, and the two share nothing but the FIFO pair.
 *
 * The command IDs and event IDs are MediaTek's own CMD_ID_* / EVENT_ID_* from
 * include/nic_cmd_event.h, confirmed against that header rather than guessed
 * from the disassembly.  Three of them cost MVII a working link to get wrong and
 * are worth naming out loud:
 *
 *   0x17 is UPDATE_STA_RECORD and 0x18 is REMOVE.  Sending the station record
 *   under 0x18 removes a record that was never added, and the firmware answers
 *   by going silent rather than by refusing.
 *   0x20 CH_PRIVILEGE is a lease, not a request.  A grant that is never released
 *   pins the radio to the operating channel, and every later scan comes back
 *   empty for a reason nothing reports.
 *   0x1a and 0x1b are a PAIR, and 0x1a is not optional.  SET_BSS_INFO has no
 *   beacon-interval field -- check nic_cmd_event.h:1322 if that seems unlikely --
 *   so INDICATE_PM_CONNECTED is the only command in the entire protocol that ever
 *   tells the firmware how often the AP it is watching transmits.  A link brought
 *   up without it is supervised against a beacon interval of zero, and the
 *   firmware declares a beacon timeout roughly a minute later with the AP a metre
 *   away.  INDICATE_PM_ABORT is its counterpart and stock sends it FIRST on every
 *   teardown, before the station record goes and before the BSS is deactivated.
 */
#define J36_CMD_POWER_SAVE_MODE		0x06
#define J36_CMD_ADD_REMOVE_KEY		0x08
#define J36_CMD_SET_RX_FILTER		0x0b
#define J36_CMD_SET_DOMAIN_INFO		0x13
#define J36_CMD_BSS_ACTIVATE_CTRL	0x15
#define J36_CMD_SET_BSS_INFO		0x16
#define J36_CMD_UPDATE_STA_RECORD	0x17
#define J36_CMD_REMOVE_STA_RECORD	0x18
#define J36_CMD_INDICATE_PM_CONNECTED	0x1a
#define J36_CMD_INDICATE_PM_ABORT	0x1b
#define J36_CMD_SCAN_REQ		0x1e
#define J36_CMD_CH_PRIVILEGE		0x20
#define J36_CMD_BASIC_CONFIG		0xc1

#define J36_EVENT_CMD_RESULT		0x01
#define J36_EVENT_SCAN_RESULT		0x04
#define J36_EVENT_BASIC_CONFIG		0x09
#define J36_EVENT_ACTIVATE_STA_REC	0x13
#define J36_EVENT_SCAN_DONE		0x15
#define J36_EVENT_TX_DONE		0x17
#define J36_EVENT_CH_PRIVILEGE		0x18
#define J36_EVENT_BSS_BEACON_TIMEOUT	0x1b

/* The HIF's own framing.  A command is 8 bytes of header and a payload; a frame
 * is 16 bytes of transmit descriptor and either an Ethernet frame or a whole
 * 802.11 management frame.  Receive is 12 bytes of header in every direction,
 * with the two low bits of the second half-word saying which of the three kinds
 * of packet this is. */
#define J36_HIF_CMD_HEADER_SIZE		8
#define J36_HIF_DATA_HEADER_SIZE	16
#define J36_HIF_RX_HEADER_SIZE		12
#define J36_HIF_TX_PACKET_TYPE_SHIFT	6
#define J36_HIF_TX_RESOURCE_SHIFT	2
#define J36_HIF_PACKET_TYPE_DATA	0
#define J36_HIF_PACKET_TYPE_CMD		1
#define J36_HIF_PACKET_TYPE_EVENT	1
#define J36_HIF_PACKET_TYPE_MGMT	3
#define J36_HIF_RX_PACKET_TYPE_MASK	0x3

/* Traffic classes.  TC4 is the command and management class and has FOUR pages,
 * which is why nothing writes a port without acquiring one first -- an AHB write
 * into a full FIFO does not fault, it stops the CPU inside a bus transaction with
 * no exception and no watchdog. */
#define J36_HIF_TC_DATA			1
#define J36_HIF_TC_COMMAND		4
#define J36_HIF_TC_BROADCAST		5

/* NETWORK_TYPE_AIS: the infrastructure-station network index.  Every command
 * below carries it, because this driver is a station and nothing else. */
#define J36_NETWORK_TYPE_AIS		0

/* cnmStaRecChangeState's three states, and its own numbering: STA_STATE_1 is 0.
 * The join sends 0 before authenticating and 2 after associating; 1 is never
 * sent, because stock's own dispatch returns before the send for a 1 -> 2
 * transition (mgmt/cnm_mem.c:1049-1062). */
#define J36_STA_STATE_1			0
#define J36_STA_STATE_3			2
#define J36_STA_RECORD_INDEX		0
#define J36_STA_INDEX_NOT_FOUND		0xfe
/* Deliberately defined and deliberately never used.  BMCAST is the record an
 * ACCESS POINT transmits group-addressed frames under, and a station in an
 * infrastructure BSS has no such frames: its broadcasts go to the AP as unicast,
 * under the AP's record and the pairwise key.  Sending them here instead is what
 * used to break DHCP -- see j36_wlan_frame(). */
#define J36_STA_INDEX_BMCAST		0xff

/* CMD_SCAN_REQ_T, nic_cmd_event.h:1434-1449.  110 bytes with no IEs, which is
 * OFFSET_OF(CMD_SCAN_REQ, aucIE) and what stock sends for a wildcard sweep. */
#define J36_SCAN_COMMAND_SIZE		110
#define J36_SCAN_TYPE_PASSIVE		0
#define J36_SCAN_TYPE_ACTIVE		1
#define J36_SCAN_SSID_WILDCARD		BIT(0)
#define J36_SCAN_CHANNEL_2G4		1

#define J36_WLAN_MAX_SCAN_RESULTS	48
#define J36_WLAN_MAX_RX_PER_POLL	32
#define J36_WLAN_MAX_ASSOC_FRAME	512
#define J36_WLAN_MAX_IES		384

/* How long each step of a join is given before it is retried or abandoned.  The
 * numbers are MVII's, measured against this radio.
 *
 * The scan timeout is short on purpose.  A full 2.4 GHz active sweep takes about
 * a second and a half of radio time and the firmware reports SCAN_DONE within
 * two; anything past that is not a slow scan, it is a firmware that has stopped
 * answering, and the only thing a long timeout buys is a longer wait before the
 * recovery below gets a turn.  wpa_supplicant's own scan timeout is 10 s and it
 * starts refusing to queue new triggers once it has one outstanding, so a driver
 * that sits on a dead scan for 12 s takes the supplicant down with it. */
#define J36_WLAN_SCAN_TIMEOUT_MS	6000
#define J36_WLAN_CHANNEL_TIMEOUT_MS	1500
#define J36_WLAN_AUTH_TIMEOUT_MS	1000
#define J36_WLAN_ASSOC_TIMEOUT_MS	1500
/* How long a fresh link waits for a beacon from its own AP before giving up on
 * learning the real DTIM period and telling the firmware to assume one.  See
 * j36_wlan_cmd_pm_connected(): the alternative to a fabricated DTIM period is
 * never sending INDICATE_PM_CONNECTED at all, which is strictly worse. */
#define J36_WLAN_PM_BEACON_WAIT_MS	1500
/* The window between "the pairwise key went in" and "no group key is coming".
 * The supplicant installs the GTK immediately after the PTK in every handshake
 * that has one; this only exists so a network that has not got one still opens
 * its port. */
#define J36_WLAN_GTK_GRACE_MS		400
/* How long the pairwise key install waits for the last EAPOL frame to leave the
 * firmware's own transmit queue.  Generous for one already-queued frame, and
 * bounded because the alternative to giving up is never installing a key. */
#define J36_WLAN_1X_DONE_TIMEOUT_MS	100
/* How hard a transmit waits for a free page before it gives up and lets the
 * caller decide.  J36_HIF_TX_POLL_ROUNDS is for commands and management frames:
 * a join has a 1.5 s deadline on every step and dropping an authentication frame
 * to save a millisecond is a bad trade.  J36_HIF_TX_TRY_ROUNDS is for the data
 * path, and it is zero because THE POLL WORKER HOLDS w->lock WHILE IT DRAINS.
 * Sleeping there does not just delay one frame: it stops the event pump for the
 * duration, so no TX_DONE comes back to credit the very pages being waited on,
 * the receive FIFO fills, and .scan and .connect block behind the same mutex.
 * Sixteen frames at 200 ms each is six seconds of a radio that looks hung. */
#define J36_HIF_TX_POLL_ROUNDS		200
#define J36_HIF_TX_TRY_ROUNDS		0
#define J36_HIF_TX_POLL_INTERVAL_US	1000

/*
 * One scanned BSS, in the terms the firmware's commands want it rather than the
 * terms cfg80211 wants it.
 *
 * cfg80211 keeps its own BSS table and this is not a second copy of it for the
 * sake of one: SET_BSS_INFO and UPDATE_STA_RECORD need the operational and basic
 * rate BITMAPS, the raw supported-rate bytes to echo back in the association
 * request, the DTIM period out of the TIM element and the channel as a number --
 * none of which survives cfg80211_get_bss(), which hands back the IEs and the
 * signal and expects the driver to have kept whatever else it needs.
 */
struct j36_wlan_bss {
	u8 bssid[ETH_ALEN];
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	u8 ssid_len;
	u8 channel;
	u8 dtim_period;
	u8 rcpi;
	u16 beacon_interval;
	u16 capability;
	u16 operational_rates;
	u16 basic_rates;
	u8 rates[16];
	u8 rate_count;
	s16 signal;
	bool valid;
	bool privacy;
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

	/*
	 * BTIF transmit pacing: how many bytes go into the transmit FIFO per fill,
	 * and how long to wait afterwards before looking at it again.  A burst of
	 * zero means "as much as the FIFO will take", which is the unpaced link.
	 *
	 * These exist because the far end has no way to stop us.  btif_init()
	 * enables the hardware handshake, and that handshake protects OUR receive
	 * FIFO -- it stops the connectivity MCU while ours is full.  Nothing in
	 * the other direction stops us while THEIRS is full, and the MCU running
	 * the unpatched bootstrap is the slowest reader on this link.  See the
	 * pacing ladder in j36_mt6592_wifi_wmt.c for what the device said about it.
	 */
	u32 tx_burst;
	u32 tx_gap_us;

	/* Which rung of that ladder last carried a patch down.  The second image
	 * starts where the first one succeeded rather than back at the top. */
	u32 pace_rung;

	struct j36_stp_stats stats;
	struct j36_hif_stats hif_stats;

	/* The HIF's own sequence counter.  Separate from the STP one because the
	 * two links are unrelated: this one is echoed back in the init event's
	 * fourth byte and is how an answer is matched to its question. */
	u8 command_sequence;

	/*
	 * Stage 4's four counters, here rather than in j36_mt6592_wifi_cmd.c
	 * because they belong to the device and not to the file: a second radio
	 * on a second probe would need its own set, and file statics would give
	 * it somebody else's.
	 *
	 * frame_sequence is NOT command_sequence.  That one tags a command and
	 * comes back in an event's header; this one tags a transmitted frame in
	 * the HIF TX descriptor and comes back in EVENT_TX_DONE's payload.  They
	 * are different fields of different packets and sharing a counter between
	 * them would match a frame's completion to a command's answer.
	 *
	 * pending_1x is the tag of the last EAPOL frame handed to the firmware,
	 * zero once its TX_DONE has arrived.  The pairwise key must not go in
	 * while it is nonzero -- see j36_wlan_cmd_install_key().
	 */
	u8 frame_sequence;
	u8 scan_sequence;
	u8 channel_token;
	u8 pending_1x;

	/* Not on the stack: a patch fragment frame is 1011 bytes and this runs on
	 * a workqueue thread whose stack is two pages. */
	u8 frame[J36_WMT_FRAME_BUFFER_SIZE];
	u8 command[J36_WMT_FRAME_BUFFER_SIZE];
	u8 event[J36_WMT_EVENT_BUFFER_SIZE];

	/* Same reasoning, one stage down: a download packet is 2072 bytes. */
	u8 hif_tx[J36_HIF_TX_BUFFER_SIZE];
	u8 hif_rx[J36_HIF_RX_BUFFER_SIZE];

	/*
	 * Stage 4.  NULL until the netdev layer has attached, which happens once
	 * -- at the end of a bring-up that reached WLAN_READY -- and is what every
	 * command sender in j36_mt6592_wifi_cmd.c tests before it touches the
	 * radio.  Declared as an opaque pointer so that the three stages below it
	 * are not compiled against cfg80211's headers.
	 */
	struct j36_wlan *wlan;

	/* The station address the firmware answered with, or the board's own if it
	 * would not.  Stage 4 needs it in three places -- the netdev, SET_BSS_INFO
	 * and every management frame's address 2 -- so it is read once, here. */
	u8 mac[ETH_ALEN];
	bool mac_from_firmware;

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

/* j36_mt6592_wifi_hif.c */
int j36_wifi_hif_bind(struct j36_wifi *w);
int j36_wifi_hif_load_firmware(struct j36_wifi *w, const void *data, size_t size);
void j36_wifi_hif_trace(struct j36_wifi *w);

/*
 * The transport primitives stage 4 needs, and only those.
 *
 * The FIFO pair, the transfer descriptor and the ownership handshake stay
 * private to j36_mt6592_wifi_hif.c -- there is one place that knows how a word
 * gets into the port and it is not the command layer.  What crosses this line is
 * a whole packet in either direction plus the page accounting, because the page
 * accounting is the thing a command sender must not be able to skip.
 */
int j36_wifi_hif_own(struct j36_wifi *w);
u32 j36_wifi_hif_status(struct j36_wifi *w, u32 offset);
int j36_wifi_hif_tx_acquire(struct j36_wifi *w, unsigned int tc);
/* The same accounting without the sleep.  For callers that hold w->lock and have
 * somewhere to put the packet back -- see the J36_HIF_TX_TRY_ROUNDS note. */
int j36_wifi_hif_tx_try(struct j36_wifi *w, unsigned int tc);
void j36_wifi_hif_tx_credit(struct j36_wifi *w);
void j36_wifi_hif_submit(struct j36_wifi *w, unsigned int tc, const u8 *packet,
			 u32 size);
bool j36_wifi_hif_pending(struct j36_wifi *w, u8 *port, u32 *length);
int j36_wifi_hif_collect(struct j36_wifi *w, u8 port, u32 length);
u8 j36_wifi_hif_sequence(struct j36_wifi *w);

/* j36_mt6592_wifi_cmd.c -- the firmware's own protocol, one function per
 * command, plus the receive pump that turns a FIFO into events and frames. */
int j36_wlan_cmd_configure(struct j36_wifi *w);
/* Which classes of received frame the firmware may hand up at all.  Stock sends
 * this from ndo_set_rx_mode and Linux calls that out of dev_open(), so it is
 * sent from both places this driver has an equivalent of: the adapter start and
 * every interface up. */
int j36_wlan_cmd_rx_filter(struct j36_wifi *w);
int j36_wlan_cmd_scan(struct j36_wifi *w, bool active, u8 *sequence_out);
int j36_wlan_cmd_channel_request(struct j36_wifi *w, const struct j36_wlan_bss *bss,
				 u8 *token_out);
int j36_wlan_cmd_channel_abort(struct j36_wifi *w, u8 token);
int j36_wlan_cmd_sta_record(struct j36_wifi *w, const struct j36_wlan_bss *bss,
			    u8 state, u16 aid);
int j36_wlan_cmd_sta_remove(struct j36_wifi *w, const struct j36_wlan_bss *bss);
int j36_wlan_cmd_bss_info(struct j36_wifi *w, const struct j36_wlan_bss *bss,
			  bool secure);
int j36_wlan_cmd_bss_disconnect(struct j36_wifi *w, const struct j36_wlan_bss *bss);
int j36_wlan_cmd_bss_reactivate(struct j36_wifi *w);
/* dtim_fallback: send the command with an assumed DTIM period of 1 rather than
 * refusing when no beacon has supplied the real one.  Fabricating it is safe
 * ONLY because j36_wlan_cmd_configure() leaves the radio in CAM -- the receiver
 * never sleeps, so the DTIM period cannot cause a missed multicast; it sets the
 * cadence the firmware watches for and nothing else. */
int j36_wlan_cmd_pm_connected(struct j36_wifi *w, const struct j36_wlan_bss *bss,
			      u16 aid, bool dtim_fallback);
/* The teardown half of the pair.  Sent first on every disconnect path, exactly
 * as stock's aisFsmDisconnect() does (mgmt/ais_fsm.c:3947). */
int j36_wlan_cmd_pm_abort(struct j36_wifi *w);
int j36_wlan_cmd_install_key(struct j36_wifi *w, const u8 *peer, u8 key_id,
			     bool pairwise, const u8 *key);
int j36_wlan_cmd_remove_key(struct j36_wifi *w, const u8 *peer, u8 key_id,
			    bool pairwise);
int j36_wlan_cmd_auth(struct j36_wifi *w, const struct j36_wlan_bss *bss);
/* The one management frame stock's own join never sends, because stock never
 * leaves a network on purpose.  Without it the AP keeps the association alive
 * until its inactivity timer expires, and goes on buffering for a station that
 * is not listening -- which is visible from the other side as a network that is
 * slow to let the same device back on. */
int j36_wlan_cmd_deauth(struct j36_wifi *w, const struct j36_wlan_bss *bss,
			u16 reason);
int j36_wlan_cmd_assoc(struct j36_wifi *w, const struct j36_wlan_bss *bss,
		       const u8 *ies, u32 ies_len);
/* may_wait: whether this frame is allowed to sleep waiting for a transmit page.
 * False from the poll worker's drain, which holds w->lock and can requeue. */
int j36_wlan_cmd_tx_ethernet(struct j36_wifi *w, const u8 *frame, u32 len,
			     u8 sta_index, bool is_1x, bool may_wait);
unsigned int j36_wlan_cmd_pump(struct j36_wifi *w);

/*
 * What the pump hands upwards.  Implemented in j36_mt6592_wifi_net.c, called
 * from j36_mt6592_wifi_cmd.c, and every one of them runs with w->lock held on
 * the poll worker -- so none of them may sleep on anything the worker owns.
 */
void j36_wlan_on_beacon(struct j36_wifi *w, const u8 *frame, u32 frame_len,
			u8 channel, u8 rcpi);
/*
 * The same news by the other route.  This firmware reports what it heard in two
 * different shapes -- the whole beacon as a management packet, and a digested
 * descriptor as EVENT_SCAN_RESULT -- and which of the two a given build uses is
 * not something this driver gets to choose.  Both are taken, because the cost of
 * handling the one that never arrives is nothing and the cost of ignoring the one
 * that does is an empty network list, which is the entire feature.
 */
void j36_wlan_on_scan_result(struct j36_wifi *w, const u8 *bssid, u16 capability,
			     u8 channel, s32 signal, const u8 *ies, u32 ies_len);
void j36_wlan_on_scan_done(struct j36_wifi *w, u8 sequence);
void j36_wlan_on_channel_grant(struct j36_wifi *w, u8 token);
void j36_wlan_on_auth_response(struct j36_wifi *w, const u8 *frame, u32 frame_len);
void j36_wlan_on_assoc_response(struct j36_wifi *w, const u8 *frame, u32 frame_len);
void j36_wlan_on_ap_disconnect(struct j36_wifi *w, const u8 *frame, u32 frame_len,
			       bool deauth);
void j36_wlan_on_beacon_timeout(struct j36_wifi *w);
void j36_wlan_on_ethernet(struct j36_wifi *w, const u8 *frame, u32 frame_len);
/* The firmware has validated the station record, which is the gate on the whole
 * unicast data path: until this arrives every data frame has to go out as
 * STA_INDEX_NOT_FOUND on the broadcast class. */
void j36_wlan_on_sta_active(struct j36_wifi *w, const u8 *peer);
/* Whose beacons and disconnects we are listening for, or NULL when there is no
 * BSS in hand.  The pump needs it to tell an AP's deauth from a stranger's. */
const u8 *j36_wlan_peer_bssid(struct j36_wifi *w);

/* j36_mt6592_wifi_net.c -- cfg80211 and wlan0. */
int j36_wlan_net_attach(struct j36_wifi *w);
void j36_wlan_net_detach(struct j36_wifi *w);
void j36_wlan_net_trace(struct j36_wifi *w);
/* The interface's name, or NULL if stage 4 never attached.  Exists so the one
 * line the bring-up always logs can name what it produced. */
const char *j36_wlan_net_interface(struct j36_wifi *w);

/* Shared by all five: record why we stopped, once, in one place. */
void j36_wifi_fail(struct j36_wifi *w, const char *blocked, const char *fmt, ...)
	__printf(3, 4);

#endif /* J36_MT6592_WIFI_H */
