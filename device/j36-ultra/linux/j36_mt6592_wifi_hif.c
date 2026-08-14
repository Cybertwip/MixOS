// SPDX-License-Identifier: GPL-2.0
/*
 * J36 Ultra MT6592 CONSYS Wi-Fi, stage 3: the WLAN AHB HIF at 0x180f0000.
 *
 * Ported from PowerEngine/OS/MVII's mt6592_wifi_hif.c, which established this
 * sequence against this board.  Its comments carry the derivations -- which
 * stock function each register write came out of, and which of them cost a boot
 * to get wrong; the ones a reader of the Linux driver needs are kept here and in
 * j36_mt6592_wifi.h beside the constants they explain.
 *
 *
 * ── WHAT THIS FILE DOES ──
 *
 * Stage 2 leaves the connectivity MCU patched and calibrated but running ROM.
 * This file gets MediaTek's own WLAN firmware into it and executing:
 *
 *   1. bind the HIF -- raise the Wi-Fi TX PA rail, check the chip ID, take
 *      driver ownership, program WHCR/WHIER, install the download credit table
 *   2. walk WIFI_RAM_CODE_SOC's divided container and push each section down in
 *      2 KiB chunks, each one acknowledged by the boot ROM
 *   3. make the ROM run the MT6625L A-die probe, so the RF front end is
 *      configured rather than declared configured
 *   4. release the ROM's firmware handoff gate
 *   5. INIT_CMD_WIFI_START, then poll WCIR for WLAN_READY while sampling the
 *      connectivity program counter
 *
 * ── AND WHAT IT DOES NOT ──
 *
 * WLAN_READY is a running firmware, not a network interface.  Scanning,
 * association, key management and the data path are a further layer on top of
 * this transport, they are a great deal more code than this file, and they are
 * not in this build.  Nothing here registers a netdev or a wiphy, and the log
 * line at the end of the bring-up says which stage was actually reached rather
 * than implying a radio that can carry traffic.
 *
 *
 * ── WHAT THE PORT CHANGED ──
 *
 * The same three things as the WMT stage.  MVII busy-polls a hand-read GPT
 * because it has no scheduler; here the whole bring-up is one work item that may
 * sleep, so deadlines are ktime and the poll waits are real sleeps.  The
 * cooperative-yield calls are gone for the same reason -- a sleeping workqueue
 * is already off the CPU.  The firmware image comes from request_firmware()
 * rather than from a bootstrap that had already read the stock system partition.
 */

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/sched.h>
#include <linux/string.h>

#include "j36_mt6592_wifi.h"

/* ── little-endian fields, a byte at a time ──────────────────────────────────
 *
 * Byte-wise rather than through <linux/unaligned.h>, for the same reason the
 * FIFO writer below assembles its words by hand: the command buffers are u8
 * arrays whose alignment nothing declares, and every field here is a wire
 * layout the peer fixed, not a host word.  It also keeps this file off a header
 * that moved out of asm/ during the 6.x series.
 */
static void j36_put_le16(u8 *p, u16 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
}

static void j36_put_le32(u8 *p, u32 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
	p[2] = (u8)(value >> 16);
	p[3] = (u8)(value >> 24);
}

static u32 j36_get_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

/* ── the window ──────────────────────────────────────────────────────────────*/

static u32 j36_hif_read(struct j36_wifi *w, u32 offset)
{
	return readl(w->hif + offset);
}

static void j36_hif_write(struct j36_wifi *w, u32 offset, u32 value)
{
	writel(value, w->hif + offset);
}

/* The connectivity MCU's program counter.  A plain register read in the block
 * stage 1 already mapped, with no dependency on the HIF or on anything having
 * been downloaded -- which is what makes it readable after the boot ROM has
 * stopped answering, and that is the only interval anybody needs it in. */
static u32 j36_hif_cpupcr(struct j36_wifi *w)
{
	return readl(w->consys + J36_CONSYS_MCU_CPUPCR);
}

/* ── the transfer descriptor and the two FIFO ports ──────────────────────────*/

/*
 * Stock's AHB HIF workaround, and both halves of it.
 *
 * HifAhbDmaEnhanceModeConf does TWO dummy reads before composing a new HSTCR
 * word -- WHIER at 0x14 and then HSTCR itself -- each followed by a barrier.
 * MVII did only the first for a long time.  On a posted AHB bus the second read
 * is what guarantees the previous descriptor's write has retired before the next
 * one overwrites it, so omitting it is not cosmetic tidiness.
 *
 * readl() carries its own barrier on ARM, so the explicit dsb stock needs is
 * already there; the reads themselves are what has to happen.
 */
static void j36_hif_configure_transfer(struct j36_wifi *w, u32 target, u32 size)
{
	const u32 count = ALIGN(size, 4) & J36_HSTCR_COUNT_MASK;

	(void)j36_hif_read(w, J36_HIF_WHIER);
	(void)j36_hif_read(w, J36_HIF_HSTCR);
	j36_hif_write(w, J36_HIF_HSTCR,
		      (J36_HSTCR_BURST_4_DW << J36_HSTCR_BURST_SHIFT) |
		      (target << J36_HSTCR_TARGET_SHIFT) | count);
}

static void j36_hif_write_port(struct j36_wifi *w, u32 port, u32 target,
			       const u8 *data, u32 size)
{
	const u32 words = ALIGN(size, 4) / 4;
	u32 i;

	j36_hif_configure_transfer(w, target, size);
	for (i = 0; i < words; i++) {
		const u32 offset = i * 4;
		u32 value = 0;
		u32 byte;

		/* Assembled a byte at a time rather than with a 32-bit load: the
		 * tail word of an unaligned length has to be zero-filled, and the
		 * source is a firmware image at whatever offset its own section
		 * table put it at. */
		for (byte = 0; byte < 4 && offset + byte < size; byte++)
			value |= (u32)data[offset + byte] << (byte * 8);
		j36_hif_write(w, port, value);
	}
}

/*
 * Returns -EMSGSIZE if the packet was longer than the buffer.  The words are
 * read out either way, because the FIFO has to be drained whatever we do with
 * the bytes: a short read leaves the port misframed for every packet after it.
 */
static int j36_hif_read_port(struct j36_wifi *w, u32 port, u32 target,
			     u8 *data, u32 size, u32 capacity)
{
	const u32 words = ALIGN(size, 4) / 4;
	u32 i;

	j36_hif_configure_transfer(w, target, size);
	for (i = 0; i < words; i++) {
		const u32 value = j36_hif_read(w, port);
		const u32 offset = i * 4;
		u32 byte;

		for (byte = 0; byte < 4 && offset + byte < capacity; byte++)
			data[offset + byte] = (u8)(value >> (byte * 8));
	}
	return size <= capacity ? 0 : -EMSGSIZE;
}

/* ── TX page accounting ──────────────────────────────────────────────────────
 *
 * WTSR0 and WTSR1 are not status words to glance at and drop.  They are six
 * bytes of freed page counts, one per traffic class, and they clear when read --
 * so a read that discards its value is a credit thrown on the floor.
 *
 * MVII learnt this the expensive way.  With no credit held, the fifth command in
 * a row goes into a HIF FIFO that has no room for it; the AHB write never
 * retires and the CPU stops inside a bus transaction, with no exception and no
 * watchdog.  Nothing in this file sends five commands in a row -- the download
 * blocks on a per-chunk ACK -- but the counts are kept from the first write
 * anyway, because the layer that will send them is the next one to be written.
 *
 * nicTxResetResource is the run-time table and nicTxInitResetResource the
 * download one, where only TC0 exists with eight pages.
 */
static const u8 j36_hif_tx_pages_download[J36_HIF_TX_CLASSES] = {
	8, 0, 0, 0, 0, 0,
};
static const u8 j36_hif_tx_pages_runtime[J36_HIF_TX_CLASSES] = {
	1, 20, 1, 1, 4, 1,
};

static void j36_hif_tx_reset(struct j36_wifi *w, const u8 *table)
{
	unsigned int tc;

	for (tc = 0; tc < J36_HIF_TX_CLASSES; tc++) {
		w->hif_stats.tx_free[tc] = table[tc];
		w->hif_stats.tx_max[tc] = table[tc];
	}
}

/* One round of nicTxPollingResource's body: read both status registers as the
 * six freed counts they are, credit each class, clamp against its maximum. */
static void j36_hif_tx_release(struct j36_wifi *w)
{
	const u32 status0 = j36_hif_read(w, J36_HIF_WTSR0);
	const u32 status1 = j36_hif_read(w, J36_HIF_WTSR1);
	const u8 freed[J36_HIF_TX_CLASSES] = {
		(u8)(status0), (u8)(status0 >> 8),
		(u8)(status0 >> 16), (u8)(status0 >> 24),
		(u8)(status1), (u8)(status1 >> 8),
	};
	unsigned int tc;

	for (tc = 0; tc < J36_HIF_TX_CLASSES; tc++) {
		u32 credit = w->hif_stats.tx_free[tc] + freed[tc];

		if (credit > w->hif_stats.tx_max[tc])
			credit = w->hif_stats.tx_max[tc];
		w->hif_stats.tx_credited += freed[tc];
		w->hif_stats.tx_free[tc] = (u8)credit;
	}
}

/* ── ownership and the one-time HIF programming ──────────────────────────────*/

static int j36_hif_acquire_driver_own(struct j36_wifi *w)
{
	const ktime_t deadline =
		ktime_add_us(ktime_get(), J36_HIF_DRIVER_OWN_TIMEOUT_US);

	if (j36_hif_read(w, J36_HIF_WHLPCR) & J36_WHLPCR_IS_DRIVER_OWN) {
		w->driver_own = true;
		return 0;
	}

	j36_hif_write(w, J36_HIF_WHLPCR, J36_WHLPCR_DRIVER_OWN_REQ);
	while (ktime_before(ktime_get(), deadline)) {
		if (j36_hif_read(w, J36_HIF_WHLPCR) & J36_WHLPCR_IS_DRIVER_OWN) {
			w->driver_own = true;
			return 0;
		}
		usleep_range(8, 40);
	}

	w->driver_own = false;
	j36_wifi_fail(w, "hif-driver-own-timeout",
		      "the WLAN HIF would not grant driver ownership");
	return -ETIMEDOUT;
}

/*
 * 0x6592 is this die.  The other two are accepted because the same CONSYS block
 * and the same HIF are on the MT6572 and MT6582, and a chip ID this driver has
 * no business refusing is one it should report rather than reject -- but a read
 * that returns something else entirely is the bus telling us the subsystem is
 * not powered, and that is worth stopping for.
 */
static bool j36_hif_supported_chip(u16 chip_id)
{
	return chip_id == 0x6572 || chip_id == 0x6582 || chip_id == 0x6592;
}

static int j36_hif_configure(struct j36_wifi *w)
{
	const u32 wcir = j36_hif_read(w, J36_HIF_WCIR);
	const u16 chip_id = wcir & J36_WCIR_CHIP_ID_MASK;
	u32 whcr;
	int ret;

	w->hif_stats.chip_id = chip_id;
	w->hif_stats.revision =
		(wcir & J36_WCIR_REVISION_MASK) >> J36_WCIR_REVISION_SHIFT;
	if (!j36_hif_supported_chip(chip_id)) {
		j36_wifi_fail(w, "hif-chip-id-invalid",
			      "the WLAN AHB HIF answered chip ID 0x%04x, which is not one this driver knows",
			      chip_id);
		return -ENODEV;
	}

	ret = j36_hif_acquire_driver_own(w);
	if (ret)
		return ret;

	j36_hif_write(w, J36_HIF_WHLPCR, J36_WHLPCR_INT_EN_CLR);

	/*
	 * Stock's whole HIF programming, in stock's order: nicSDIOInit does one
	 * read-modify-write of WHCR clearing bit 16 and bits [7:4] and nothing
	 * else, and nicInitializeAdapter then writes WHIER.  nicMCRInit and
	 * nicHifInit are both empty on this SOC.  Bits stock preserves are
	 * preserved and the RX length count stays 0.
	 */
	whcr = j36_hif_read(w, J36_HIF_WHCR);
	whcr &= ~(J36_WHCR_RX_ENHANCE_MODE_EN | J36_WHCR_MAX_HIF_RX_LEN_MASK);
	j36_hif_write(w, J36_HIF_WHCR, whcr);

	j36_hif_write(w, J36_HIF_WHIER, J36_WHIER_DEFAULT);

	/* The download table, before anything is credited against it.  The
	 * run-time one is installed where the ready bit is seen, and nowhere
	 * else. */
	j36_hif_tx_reset(w, j36_hif_tx_pages_download);

	w->hif_stats.last_whisr = j36_hif_read(w, J36_HIF_WHISR);
	j36_hif_tx_release(w);
	w->hif_stats.last_wrplr = j36_hif_read(w, J36_HIF_WRPLR);
	w->hif_ready = true;
	return 0;
}

/*
 * Bring the HIF up.  Idempotent: a second call re-takes ownership, which the
 * chip can drop, and does nothing else.
 *
 * The Wi-Fi TX PA rail is raised here and never lowered, because this function
 * is HifAhbProbe() and that is what HifAhbProbe() does first -- hwPowerOn of
 * MT6323_POWER_LDO_VCN33_WIFI, then the switch to hardware mode, and only then
 * the download / WIFI_START / WLAN_READY sequence.  On stock the rail is up for
 * the entire life of the WLAN driver, not just for a moment.
 *
 * MVII believed otherwise for a long time: searching conn_soc for WIFI_PALDO
 * finds only the pair bracketing the RF calibration script, so stage 2 raises
 * the rail for the calibration frame and drops it again.  That reading is right
 * about the WMT layer and wrong about the chip, because the WLAN driver raises
 * the same regulator under a name no WIFI_PALDO search finds.  Both are true and
 * stock does both.  What believing only the first cost: the firmware downloaded
 * and started with its transmit PA unpowered, ran, and reported failure rather
 * than readiness -- WCIR bit 21 never set.
 *
 * A rail that will not come up is logged and not fatal.  A board that cannot
 * raise it has a PMIC fault the WLAN bring-up cannot fix and should not hide,
 * and the firmware's own error path is a better report than a refused bind.
 */
int j36_wifi_hif_bind(struct j36_wifi *w)
{
	if (w->hif_ready)
		return j36_hif_acquire_driver_own(w);

	if (j36_wifi_set_wifi_rail(w, true))
		dev_warn(w->dev,
			 "VCN33_WIFI would not come up; the WLAN transmit PA is unpowered\n");

	return j36_hif_configure(w);
}

/* ── receiving what the boot ROM says ────────────────────────────────────────*/

/* WRPLR carries both ports' pending lengths, RX0 in the low half.  Zero in both
 * is the normal answer and not an error -- it is how a poll spins. */
static bool j36_hif_next_packet(struct j36_wifi *w, u8 *port, u32 *length)
{
	const u32 lengths = j36_hif_read(w, J36_HIF_WRPLR);

	w->hif_stats.last_wrplr = lengths;
	if (lengths & 0xffff) {
		*port = 0;
		*length = lengths & 0xffff;
		return true;
	}
	if (lengths >> 16) {
		*port = 1;
		*length = lengths >> 16;
		return true;
	}
	return false;
}

static int j36_hif_receive(struct j36_wifi *w, u8 port, u32 length)
{
	/* See J36_HIF_RX_HW_APPENDED_LEN: the port holds one DW of hardware
	 * status behind the packet and the transfer has to take it too. */
	const u32 drain = ALIGN(length + J36_HIF_RX_HW_APPENDED_LEN, 4);

	if (port == 0)
		return j36_hif_read_port(w, J36_HIF_WRDR0, J36_HIF_TARGET_RXD0,
					 w->hif_rx, drain, J36_HIF_RX_BUFFER_SIZE);
	return j36_hif_read_port(w, J36_HIF_WRDR1, J36_HIF_TARGET_RXD1,
				 w->hif_rx, drain, J36_HIF_RX_BUFFER_SIZE);
}

static u8 j36_hif_next_sequence(struct j36_wifi *w)
{
	/* Wraps to 1, not to 0: an event carrying sequence 0 is what an
	 * uninitialised field looks like, and the matcher cannot tell that from
	 * a real answer. */
	if (++w->command_sequence == 0)
		w->command_sequence = 1;
	return w->command_sequence;
}

/*
 * Wait for the ROM's plain acknowledgement of a download chunk.
 *
 * The init events are [len16][pad][EID][seq][status]: byte 2 is the event ID and
 * byte 3 the sequence echoed back, which is what makes a stale event from an
 * earlier chunk distinguishable from this one's answer rather than being counted
 * as it.
 */
static int j36_hif_wait_init_ack(struct j36_wifi *w, u8 sequence)
{
	const ktime_t deadline =
		ktime_add_us(ktime_get(), J36_HIF_INIT_ACK_TIMEOUT_US);

	while (ktime_before(ktime_get(), deadline)) {
		u8 port;
		u32 length;

		if (!j36_hif_next_packet(w, &port, &length)) {
			usleep_range(50, 200);
			continue;
		}
		if (j36_hif_receive(w, port, length) || length < 8) {
			w->hif_stats.dropped_packets++;
			continue;
		}
		if (w->hif_rx[2] != J36_INIT_EVENT_CMD_RESULT ||
		    w->hif_rx[3] != sequence) {
			w->hif_stats.dropped_packets++;
			continue;
		}
		if (w->hif_rx[4]) {
			j36_wifi_fail(w, "firmware-download-rejected",
				      "the WLAN boot ROM rejected a download chunk with status %u",
				      w->hif_rx[4]);
			return -EIO;
		}
		j36_hif_tx_release(w);
		return 0;
	}

	j36_wifi_fail(w, "firmware-download-ack-timeout",
		      "no acknowledgement for a download chunk within %u ms",
		      J36_HIF_INIT_ACK_TIMEOUT_US / 1000);
	return -ETIMEDOUT;
}

/*
 * The same wait for an event that carries a body, kept separate on purpose.
 *
 * The ACK waiter above is on the download path -- it is the reason a quarter of
 * a megabyte lands correctly -- and nothing diagnostic is worth destabilising
 * it.  The whole event is handed back, header included, because the layout of
 * an ACCESS_REG body is inferred rather than measured: if a field is off by four
 * bytes the bytes still say so.
 *
 * Returns 0 for the event asked for, 1 when something else arrived and its bytes
 * are in the buffer, and -ETIMEDOUT when nothing did.  The three are genuinely
 * different: a write is acknowledged in a different shape than a read, and
 * reporting that as a timeout once cost an afternoon of believing writes were
 * being dropped when they were landing.
 */
static int j36_hif_wait_init_event(struct j36_wifi *w, u8 event, u8 sequence,
				   u8 *out, u32 max, u32 *out_len)
{
	const ktime_t deadline =
		ktime_add_us(ktime_get(), J36_HIF_INIT_ACK_TIMEOUT_US);
	u8 unmatched[16];
	u32 unmatched_len = 0;

	if (out_len)
		*out_len = 0;

	while (ktime_before(ktime_get(), deadline)) {
		u8 port;
		u32 length;

		if (!j36_hif_next_packet(w, &port, &length)) {
			usleep_range(50, 200);
			continue;
		}
		if (j36_hif_receive(w, port, length) || length < 8) {
			w->hif_stats.dropped_packets++;
			continue;
		}
		if (w->hif_rx[2] != event || w->hif_rx[3] != sequence) {
			if (!unmatched_len) {
				unmatched_len = min_t(u32, length,
						      sizeof(unmatched));
				memcpy(unmatched, w->hif_rx, unmatched_len);
			}
			w->hif_stats.dropped_packets++;
			continue;
		}

		if (out && max) {
			const u32 copy = min(length, max);

			memcpy(out, w->hif_rx, copy);
			if (out_len)
				*out_len = copy;
		}
		j36_hif_tx_release(w);
		return 0;
	}

	if (unmatched_len && out && max) {
		const u32 copy = min(unmatched_len, max);

		memcpy(out, unmatched, copy);
		if (out_len)
			*out_len = copy;
		return 1;
	}
	return -ETIMEDOUT;
}

/* ── the download ────────────────────────────────────────────────────────────*/

/*
 * One chunk, encrypted, acknowledged.
 *
 * No page credit is taken here, deliberately, and this is the one place in the
 * file where that is a decision rather than an oversight.  Stock does gate it --
 * nicTxInitResetResource gives the download TC0 with eight pages -- but this
 * path blocks on the ROM's per-chunk ACK, and one outstanding chunk cannot
 * overrun eight pages.  The credits are still collected: the ACK wait releases
 * into the same table.  Gating a path that demonstrably works would buy only the
 * chance that the ROM does not report freed pages the way the firmware does, and
 * lose the download to it.
 */
static int j36_hif_send_download(struct j36_wifi *w, u32 address,
				 const u8 *data, u32 size)
{
	const u32 packet_size = J36_INIT_DOWNLOAD_HEADER_SIZE + size;
	const u32 transfer = ALIGN(packet_size, 4);
	const u8 sequence = j36_hif_next_sequence(w);
	u8 *packet = w->hif_tx;

	if (transfer > J36_HIF_TX_BUFFER_SIZE) {
		j36_wifi_fail(w, "firmware-chunk-too-large",
			      "a %u byte download chunk does not fit the %u byte HIF buffer",
			      size, J36_HIF_TX_BUFFER_SIZE);
		return -EINVAL;
	}

	memset(packet, 0, transfer);
	j36_put_le16(packet + 0, transfer);
	packet[4] = J36_INIT_CMD_DOWNLOAD_BUF;
	packet[5] = sequence;
	j36_put_le32(packet + 8, address);
	j36_put_le32(packet + 12, size);
	/* crc32_le is the same reflected 0xedb88320 the ROM checks with; the
	 * inversions the kernel leaves to the caller are what make it the
	 * standard CRC-32 rather than a variant. */
	j36_put_le32(packet + 16, crc32_le(~0U, data, size) ^ ~0U);
	j36_put_le32(packet + 20,
		     J36_INIT_DOWNLOAD_ENCRYPTION | J36_INIT_DOWNLOAD_ACK);
	memcpy(packet + J36_INIT_DOWNLOAD_HEADER_SIZE, data, size);

	j36_hif_write_port(w, J36_HIF_WTDR0, J36_HIF_TARGET_TXD0, packet, transfer);
	return j36_hif_wait_init_ack(w, sequence);
}

/* ── reading and writing connectivity memory through the boot ROM ────────────*/

/*
 * INIT_CMD_ACCESS_REG: the ROM answering a read or a write on our behalf.
 *
 * This reaches where nothing else can.  The AP has an aperture onto part of
 * CONSYS SRAM, but the ROM's own globals are below it, and they are where both
 * of the two bytes this stage has to touch live.  Valid only before WIFI_START:
 * the ROM's command handler is gone from the moment that command is dispatched.
 */
static int j36_hif_rom_access_reg(struct j36_wifi *w, bool write, u32 address,
				  u32 value, u8 *out, u32 max, u32 *out_len)
{
	const u8 sequence = j36_hif_next_sequence(w);
	u8 *packet = w->hif_tx;

	/* Ownership can be taken back by the chip between commands, and this is
	 * called in a poll loop; the bind is a single register read when it is
	 * already held. */
	if (j36_wifi_hif_bind(w))
		return -EIO;

	memset(packet, 0, J36_INIT_ACCESS_REG_PACKET_SIZE);
	j36_put_le16(packet + 0, J36_INIT_ACCESS_REG_PACKET_SIZE);
	packet[4] = J36_INIT_CMD_ACCESS_REG;
	packet[5] = sequence;
	j36_put_le32(packet + 8, write ? 1 : 0); /* ucSetQuery + 3 reserved */
	j36_put_le32(packet + 12, address);
	j36_put_le32(packet + 16, write ? value : 0);

	j36_hif_write_port(w, J36_HIF_WTDR0, J36_HIF_TARGET_TXD0, packet,
			   J36_INIT_ACCESS_REG_PACKET_SIZE);
	j36_hif_tx_release(w);

	return j36_hif_wait_init_event(w, J36_INIT_EVENT_ACCESS_REG, sequence,
				       out, max, out_len);
}

/*
 * Read the A-die probe flag back through the ROM.
 *
 * -1 if the ROM did not answer, or answered for a different address: the events
 * have slipped at that point and any word read out of one is a plausible lie.
 * The address echo in the body is what makes that detectable at all.
 */
static int j36_hif_read_adie_flag(struct j36_wifi *w)
{
	u8 event[32];
	u32 event_len = 0;

	if (j36_hif_rom_access_reg(w, false, J36_CONSYS_ROM_ADIE_PROBED, 0,
				   event, sizeof(event), &event_len) ||
	    event_len < 12)
		return -1;
	if (j36_get_le32(event + 4) != (u32)J36_CONSYS_ROM_ADIE_PROBED)
		return -1;
	return j36_get_le32(event + 8) & 0xff;
}

static void j36_hif_write_adie_flag(struct j36_wifi *w, u32 value)
{
	u8 event[32];
	u32 event_len = 0;

	/* A write is acknowledged in a different shape than a read, so a
	 * non-zero return here is the normal answer and not a failure. */
	(void)j36_hif_rom_access_reg(w, true, J36_CONSYS_ROM_ADIE_PROBED, value,
				     event, sizeof(event), &event_len);
}

/*
 * Make the ROM run the MT6625L A-die probe for real.
 *
 * Returns 1 if the probe's own completion flag came up, 0 if it did not, and a
 * negative error if the ROM stopped answering.  Only the first of those leaves
 * the RF front end configured; the caller forges the flag for the other two, so
 * that a failed probe still gets us a firmware that boots, which is strictly
 * better than where this stage was before the probe existed.
 *
 * A flag that is already set is not good news, it is the thing in the way: the
 * probe sequence returns early on exactly this byte.  It reads set whenever an
 * earlier call in this session probed, and -- far more likely on a board that
 * has been running previous builds -- whenever an old unconditional forge left
 * it that way and the CONSYS was never power-cycled since.  Those two cases are
 * the same byte with the same value, so believing it would mean reporting
 * somebody else's success.  Take it back and probe from a known state.
 *
 * Re-probing when the probe genuinely did run is safe: the routine is the
 * boot-time initialisation path, it reads the A-die over SPI and rebuilds its
 * own table from what it reads.
 */
static int j36_hif_run_adie_probe(struct j36_wifi *w)
{
	u8 *packet = w->hif_tx;
	unsigned int i;
	int flag;
	int ret;

	ret = j36_wifi_hif_bind(w);
	if (ret)
		return ret;

	flag = j36_hif_read_adie_flag(w);
	if (flag < 0) {
		w->hif_stats.adie_probe_state = J36_ADIE_PROBE_ROM_SILENT;
		return -EIO;
	}
	if (flag > 0) {
		w->hif_stats.adie_probe_stale = true;
		j36_hif_write_adie_flag(w, 0);
		if (j36_hif_read_adie_flag(w) != 0) {
			/* The write did not take.  The gate stays shut, so the
			 * probe cannot run at all -- do not send a command that
			 * would only be discarded and then poll a byte that was
			 * never going to change. */
			w->hif_stats.adie_probe_state = J36_ADIE_PROBE_STUCK;
			return -EIO;
		}
	}

	memset(packet, 0, J36_INIT_ADIE_PROBE_PACKET_SIZE);
	j36_put_le16(packet + 0, J36_INIT_ADIE_PROBE_PACKET_SIZE);
	packet[4] = J36_INIT_CMD_ADIE_PROBE;
	packet[5] = j36_hif_next_sequence(w);
	/* packet[8] stays 0.  It is the only payload byte the ROM's copy helper
	 * branches on, and 0 takes the shorter of its two arms.  The post that
	 * actually starts the probe is unconditional either way. */

	j36_hif_write_port(w, J36_HIF_WTDR0, J36_HIF_TARGET_TXD0, packet,
			   J36_INIT_ADIE_PROBE_PACKET_SIZE);
	j36_hif_tx_release(w);

	/*
	 * Deliberately not waiting on an init event.  Type 7 is not one of the
	 * commands whose reply shape is known, and the thing worth waiting for is
	 * not an acknowledgement of the command anyway -- it is the flag, which
	 * the probe writes several calls deeper.
	 *
	 * Two seconds at 20 ms, which is the reference's window and not a guess:
	 * wifi_task handles the message asynchronously and the probe's SPI helper
	 * spins up to 32000 iterations per transfer.
	 */
	for (i = 0; i < J36_ADIE_PROBE_POLL_LIMIT; i++) {
		msleep(J36_ADIE_PROBE_POLL_INTERVAL_MS);
		flag = j36_hif_read_adie_flag(w);
		if (flag > 0) {
			w->hif_stats.adie_probe_state = J36_ADIE_PROBE_RAN;
			w->hif_stats.adie_probe_polls = i + 1;
			return 1;
		}
		if (flag < 0) {
			w->hif_stats.adie_probe_state = J36_ADIE_PROBE_ROM_SILENT;
			w->hif_stats.adie_probe_polls = i + 1;
			return -EIO;
		}
	}

	w->hif_stats.adie_probe_state = J36_ADIE_PROBE_TIMEOUT;
	w->hif_stats.adie_probe_polls = J36_ADIE_PROBE_POLL_LIMIT;
	return 0;
}

/* See J36_CONSYS_ROM_HANDOFF_GATE: without this WIFI_START sets a mode nothing
 * ever acts on, and the ROM sits in its receive loop forever with a fully
 * downloaded firmware next to it. */
static void j36_hif_release_rom_handoff(struct j36_wifi *w)
{
	u8 event[32];
	u32 event_len = 0;

	(void)j36_hif_rom_access_reg(w, true, J36_CONSYS_ROM_HANDOFF_GATE,
				     J36_CONSYS_ROM_HANDOFF_RELEASE,
				     event, sizeof(event), &event_len);
}

/* ── WIFI_START and the readiness poll ───────────────────────────────────────*/

/*
 * The answer to WIFI_START is WCIR's ready bit, not an init event, so nothing is
 * waited for here.
 *
 * u4Override at +8 is the half of this command the two references disagree
 * about.  The vendor source builds CFG_OVERRIDE_FW_START_ADDRESS 0 and passes
 * FALSE alongside a literal address of 0, meaning "enter wherever your own
 * header says".  The kernel this device actually shipped takes the other arm --
 * fgEnable TRUE with prRegInfo->u4StartAddress -- so 1 is what the board has
 * been running, and 1 is what goes on the wire.
 */
static void j36_hif_send_start(struct j36_wifi *w, u32 start_address)
{
	u8 *packet = w->hif_tx;
	unsigned int i;

	memset(packet, 0, J36_INIT_START_PACKET_SIZE);
	j36_put_le16(packet + 0, J36_INIT_START_PACKET_SIZE);
	packet[4] = J36_INIT_CMD_WIFI_START;
	packet[5] = j36_hif_next_sequence(w);
	j36_put_le32(packet + 8, 1);
	j36_put_le32(packet + 12, start_address);

	/*
	 * Baseline the device-to-host mailboxes while the boot ROM still owns the
	 * chip.  Whatever they read here is the ROM's leftover, so any later
	 * change is necessarily the downloaded firmware's own writing -- which is
	 * the only evidence available that it executed at all.
	 */
	w->hif_stats.mailbox_at_start[0] = j36_hif_read(w, J36_HIF_D2HRM0R);
	w->hif_stats.mailbox_at_start[1] = j36_hif_read(w, J36_HIF_D2HRM1R);

	/* The control sample, back to back with nothing between the reads.  See
	 * the field comment in the header for what it is a control for. */
	for (i = 0; i < ARRAY_SIZE(w->hif_stats.cpupcr_before); i++)
		w->hif_stats.cpupcr_before[i] = j36_hif_cpupcr(w);

	j36_hif_write_port(w, J36_HIF_WTDR0, J36_HIF_TARGET_TXD0, packet,
			   J36_INIT_START_PACKET_SIZE);
	j36_hif_tx_release(w);
}

/*
 * Keep whatever the peer volunteers between WIFI_START and the ready bit.
 *
 * Stock does not look -- its readiness poll reads WCIR and nothing else.  Stock
 * also gets a firmware that comes up.  If ours refuses the start it can only say
 * so as an init event on RX0, and this is the only window in which that event
 * exists, so keep the first and drain the rest: a backed-up FIFO must not be
 * able to pass for silence.
 */
static void j36_hif_capture_start_event(struct j36_wifi *w)
{
	unsigned int guard;

	for (guard = 0; guard < 8; guard++) {
		u8 port;
		u32 length;
		u32 copy;

		if (!j36_hif_next_packet(w, &port, &length))
			return;
		if (j36_hif_receive(w, port, length)) {
			w->hif_stats.dropped_packets++;
			return;
		}
		if (w->hif_stats.start_event_valid)
			continue;

		copy = min_t(u32, length, sizeof(w->hif_stats.start_event));
		memset(w->hif_stats.start_event, 0,
		       sizeof(w->hif_stats.start_event));
		memcpy(w->hif_stats.start_event, w->hif_rx, copy);
		w->hif_stats.start_event_length = length;
		w->hif_stats.start_event_valid = true;
	}
}

/*
 * Poll WCIR for WLAN_READY, sampling the connectivity program counter alongside.
 *
 * The PC sampling is the whole reason a failure here is diagnosable.  WCIR, the
 * mailboxes and the packet counters all describe what the core has PUBLISHED,
 * and a core that faults publishes nothing -- so all three report the same thing
 * for a firmware that is spinning as for one that never started.  The PC says
 * which: zero changes across the window means the core is stopped, and
 * cpupcr_last is the address it stopped at.
 */
static int j36_hif_wait_ready(struct j36_wifi *w)
{
	const ktime_t deadline =
		ktime_add_us(ktime_get(), J36_HIF_READY_TIMEOUT_US);

	w->hif_stats.cpupcr_samples = 0;
	w->hif_stats.cpupcr_changes = 0;
	w->hif_stats.cpupcr_first = j36_hif_cpupcr(w);
	w->hif_stats.cpupcr_last = w->hif_stats.cpupcr_first;
	w->hif_stats.cpupcr_min = w->hif_stats.cpupcr_first;
	w->hif_stats.cpupcr_max = w->hif_stats.cpupcr_first;

	while (ktime_before(ktime_get(), deadline)) {
		const u32 wcir = j36_hif_read(w, J36_HIF_WCIR);
		const u32 pc = j36_hif_cpupcr(w);

		w->hif_stats.cpupcr_samples++;
		if (pc != w->hif_stats.cpupcr_last)
			w->hif_stats.cpupcr_changes++;
		w->hif_stats.cpupcr_last = pc;
		w->hif_stats.cpupcr_min = min(w->hif_stats.cpupcr_min, pc);
		w->hif_stats.cpupcr_max = max(w->hif_stats.cpupcr_max, pc);
		w->hif_stats.last_wcir = wcir;

		if (wcir & J36_WCIR_WLAN_READY) {
			w->firmware_alive = true;
			/*
			 * The one place the run-time credit table is installed,
			 * and the one place firmware_alive is raised, so no path
			 * can reach a command sender with the download's
			 * eight-page table still in force -- which would hand
			 * TC4 four pages it does not have and put us straight
			 * back on the bus stall.
			 */
			j36_hif_tx_reset(w, j36_hif_tx_pages_runtime);
			return 0;
		}
		j36_hif_capture_start_event(w);
		msleep(10);
	}

	/*
	 * Resample before giving up.  last_wrplr in particular is written by the
	 * packet poll BEFORE the packet it found is consumed, so after a clean
	 * download it still holds the length of the final ACK -- left stale it
	 * reads as "8 bytes pending, never collected", which is a packet that
	 * does not exist and a lead worth an afternoon.
	 */
	w->hif_stats.last_whisr = j36_hif_read(w, J36_HIF_WHISR);
	w->hif_stats.last_wasr = j36_hif_read(w, J36_HIF_WASR);
	w->hif_stats.last_wrplr = j36_hif_read(w, J36_HIF_WRPLR);
	w->hif_stats.last_mailbox[0] = j36_hif_read(w, J36_HIF_D2HRM0R);
	w->hif_stats.last_mailbox[1] = j36_hif_read(w, J36_HIF_D2HRM1R);

	j36_wifi_fail(w, "firmware-ready-timeout",
		      "the WLAN firmware did not assert WLAN_READY within %u ms (WCIR 0x%08x, connectivity PC %u changes in %u samples, last 0x%08x)",
		      J36_HIF_READY_TIMEOUT_US / 1000, w->hif_stats.last_wcir,
		      w->hif_stats.cpupcr_changes, w->hif_stats.cpupcr_samples,
		      w->hif_stats.cpupcr_last);
	return -ETIMEDOUT;
}

/*
 * The real probe first, and the forgery only if it did not happen.
 *
 * The order matters and is the opposite of what it looks like it should be: the
 * probe sequence refuses to run once the flag is set, so forcing the flag first
 * guarantees the RF front end stays at reset values.  The flag is left alone
 * until the ROM has had its chance at it, and the fallback exists only so that a
 * probe that fails still leaves a firmware that boots.
 *
 * Both writes below go through the ROM, and the ROM stops answering the moment
 * WIFI_START is dispatched, so this is the last window in which either is
 * possible at all.
 */
static int j36_hif_start_firmware(struct j36_wifi *w)
{
	if (j36_hif_run_adie_probe(w) <= 0) {
		dev_warn(w->dev,
			 "the A-die probe did not complete (%u); forcing its flag, so the RF front end stays at reset values\n",
			 w->hif_stats.adie_probe_state);
		j36_hif_write_adie_flag(w, 1);
	}

	j36_hif_release_rom_handoff(w);

	j36_hif_send_start(w, w->hif_stats.start_address);
	if (j36_hif_wait_ready(w))
		return -ETIMEDOUT;

	w->hif_stats.last_whisr = j36_hif_read(w, J36_HIF_WHISR);
	j36_hif_tx_release(w);
	return 0;
}

/* ── the entry point ─────────────────────────────────────────────────────────*/

/*
 * Validate and download the stock divided WIFI_RAM_CODE_SOC image, then start it.
 *
 * The container is a 16-byte header -- signature, CRC32, section count -- then
 * one 16-byte descriptor per section, then the ciphertext.  The CRC covers
 * everything from offset 8, which is to say the header from the section count
 * onwards plus the whole body; it is stock's guard against a truncated read from
 * /system and it is worth keeping here, because request_firmware() reading a
 * short file off an SD card is exactly the failure it catches.
 *
 * Section 0's own destination is the entry point, NOT the compile-time constant,
 * even though stock's source passes prRegInfo->u4StartAddress and wlanProbe puts
 * 0x00060000 there and never touches it again.  MVII sent section 0's
 * destination for a long time and scanned and associated; a later change to the
 * constant on the strength of the disassembly stopped WLAN_READY being asserted
 * at all.  0x00060000 is where the WMT stage already put ROMv1_patch_1_0_hdr.bin,
 * and the pre-start program counter reads inside that patch -- so on a stock boot
 * the AP has torn down far more between the patch and the WLAN download than
 * happens here, and "the entry veneer is resident at 0x60000" may simply not be
 * true on this path, while section 0's own base always is.
 */
int j36_wifi_hif_load_firmware(struct j36_wifi *w, const void *data, size_t size)
{
	const u8 *image = data;
	u32 signature, expected_crc, section_count, header_size;
	u32 start_address = 0;
	unsigned int section;
	int ret;

	if (w->firmware_alive)
		return 0;
	if (!image || size < 16) {
		j36_wifi_fail(w, "firmware-missing",
			      "WIFI_RAM_CODE_SOC is missing or too short to be a container");
		return -EINVAL;
	}

	ret = j36_wifi_hif_bind(w);
	if (ret)
		return ret;

	signature = j36_get_le32(image + 0);
	expected_crc = j36_get_le32(image + 4);
	section_count = j36_get_le32(image + 8);
	if (signature != J36_MTK_WIFI_SIGNATURE || !section_count ||
	    section_count > J36_MAX_FIRMWARE_SECTIONS) {
		j36_wifi_fail(w, "firmware-header-invalid",
			      "WIFI_RAM_CODE_SOC signature 0x%08x with %u sections is not a divided image",
			      signature, section_count);
		return -EINVAL;
	}
	header_size = 16 + section_count * 16;
	if (header_size > size) {
		j36_wifi_fail(w, "firmware-header-truncated",
			      "WIFI_RAM_CODE_SOC declares %u sections but is only %zu bytes",
			      section_count, size);
		return -EINVAL;
	}
	if ((crc32_le(~0U, image + 8, size - 8) ^ ~0U) != expected_crc) {
		j36_wifi_fail(w, "firmware-header-crc-mismatch",
			      "WIFI_RAM_CODE_SOC CRC is 0x%08x, the container says 0x%08x",
			      crc32_le(~0U, image + 8, size - 8) ^ ~0U,
			      expected_crc);
		return -EINVAL;
	}

	w->hif_stats.firmware_size = size;
	w->hif_stats.firmware_sections = section_count;
	w->hif_stats.downloaded_bytes = 0;
	w->hif_stats.start_event_valid = false;
	w->hif_stats.start_event_length = 0;
	memset(w->hif_stats.start_event, 0, sizeof(w->hif_stats.start_event));

	dev_info(w->dev,
		 "WIFI_RAM_CODE_SOC: %u sections, %zu bytes, HIF chip 0x%04x rev %u\n",
		 section_count, size, w->hif_stats.chip_id,
		 w->hif_stats.revision);

	for (section = 0; section < section_count; section++) {
		const u8 *entry = image + 16 + section * 16;
		const u32 file_offset = j36_get_le32(entry + 0);
		const u32 section_size = j36_get_le32(entry + 8);
		const u32 destination = j36_get_le32(entry + 12);
		u32 offset;

		if (!section_size || file_offset > size ||
		    section_size > size - file_offset) {
			j36_wifi_fail(w, "firmware-section-range-invalid",
				      "section %u claims %u bytes at offset %u of a %zu byte image",
				      section, section_size, file_offset, size);
			return -EINVAL;
		}
		if (section == 0)
			start_address = destination;

		dev_dbg(w->dev, "firmware section %u: 0x%08x, %u bytes\n",
			section, destination, section_size);

		for (offset = 0; offset < section_size;
		     offset += J36_FIRMWARE_CHUNK_SIZE) {
			const u32 chunk = min(section_size - offset,
					      (u32)J36_FIRMWARE_CHUNK_SIZE);

			ret = j36_hif_send_download(w, destination + offset,
						    image + file_offset + offset,
						    chunk);
			if (ret)
				return ret;
			w->hif_stats.downloaded_bytes += chunk;
			/* A quarter of a megabyte in 2 KiB chunks is around 130
			 * blocking waits; give the CPU up between them so a
			 * board with one core online still schedules. */
			cond_resched();
		}
	}

	w->firmware_loaded = true;
	w->hif_stats.start_address = start_address;
	dev_info(w->dev, "WIFI_RAM_CODE_SOC downloaded: %u bytes, entry 0x%08x\n",
		 w->hif_stats.downloaded_bytes, start_address);

	return j36_hif_start_firmware(w);
}

/*
 * Everything the stage saw, in one line each, whatever the outcome.
 *
 * Printed on success as well as on failure because the numbers that say WHETHER
 * it worked are not the ones that say HOW WELL: a firmware that asserted
 * WLAN_READY with the A-die probe forged is a firmware whose receiver is at
 * reset values, and that reads as a complete success everywhere else.
 */
void j36_wifi_hif_trace(struct j36_wifi *w)
{
	static const char * const adie[] = {
		"not run", "ran", "stuck", "timed out", "ROM went silent",
	};
	const struct j36_hif_stats *s = &w->hif_stats;

	if (!w->hif_ready) {
		dev_info(w->dev, "hif: not bound\n");
		return;
	}

	dev_info(w->dev,
		 "hif: chip 0x%04x rev %u, %s, %u/%u bytes of %u sections down\n",
		 s->chip_id, s->revision,
		 w->driver_own ? "driver-owned" : "NOT driver-owned",
		 s->downloaded_bytes, s->firmware_size, s->firmware_sections);
	dev_info(w->dev,
		 "hif: A-die probe %s%s after %u polls, entry 0x%08x, %s\n",
		 adie[min_t(u8, s->adie_probe_state, ARRAY_SIZE(adie) - 1)],
		 s->adie_probe_stale ? " (a stale flag was cleared first)" : "",
		 s->adie_probe_polls, s->start_address,
		 w->firmware_alive ? "WLAN_READY" : "NO WLAN_READY");
	dev_info(w->dev,
		 "hif: WCIR 0x%08x WHISR 0x%08x WASR 0x%08x WRPLR 0x%08x, %u packets dropped\n",
		 s->last_wcir, s->last_whisr, s->last_wasr, s->last_wrplr,
		 s->dropped_packets);
	dev_info(w->dev,
		 "hif: mailboxes 0x%08x/0x%08x -> 0x%08x/0x%08x\n",
		 s->mailbox_at_start[0], s->mailbox_at_start[1],
		 s->last_mailbox[0], s->last_mailbox[1]);
	dev_info(w->dev,
		 "hif: connectivity PC control 0x%08x 0x%08x 0x%08x 0x%08x\n",
		 s->cpupcr_before[0], s->cpupcr_before[1],
		 s->cpupcr_before[2], s->cpupcr_before[3]);
	dev_info(w->dev,
		 "hif: connectivity PC %u changes in %u samples, 0x%08x..0x%08x, last 0x%08x\n",
		 s->cpupcr_changes, s->cpupcr_samples, s->cpupcr_min,
		 s->cpupcr_max, s->cpupcr_last);
	if (s->start_event_valid)
		dev_info(w->dev,
			 "hif: the peer volunteered %u bytes: %*ph\n",
			 s->start_event_length,
			 (int)sizeof(s->start_event), s->start_event);
	dev_info(w->dev, "hif: TX pages %*ph of %*ph, %u credited\n",
		 J36_HIF_TX_CLASSES, s->tx_free,
		 J36_HIF_TX_CLASSES, s->tx_max, s->tx_credited);
}
