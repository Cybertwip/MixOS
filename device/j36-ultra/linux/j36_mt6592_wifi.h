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
 *   3. FIRMWARE.  WIFI_RAM_CODE_SOC over the AHB HIF, and WLAN_READY.
 *   4. cfg80211 and a netdev.
 *
 * Stages 1 and 2 are what this build does.  Stages 3 and 4 are not here yet, and
 * the driver says so in its own log rather than implying a radio it has not got.
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
 */
#define J36_CONSYS_CHIP_ID		0x0008

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

	/* Not on the stack: a patch fragment frame is 1011 bytes and this runs on
	 * a workqueue thread whose stack is two pages. */
	u8 frame[J36_WMT_FRAME_BUFFER_SIZE];
	u8 command[J36_WMT_FRAME_BUFFER_SIZE];
	u8 event[J36_WMT_EVENT_BUFFER_SIZE];

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
