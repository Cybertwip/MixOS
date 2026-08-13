// SPDX-License-Identifier: GPL-2.0
/*
 * J36 Ultra MT6592 WMT: the BTIF link, MediaTek's STP framing, and the two ROM
 * patches the connectivity MCU needs before it has anything patched to run.
 *
 * A mirror of PowerEngine/OS/MVII's mt6592_wifi_wmt.c.  The stock conn_soc stack
 * initialises the connectivity MCU before probing the WLAN AHB HIF; this is the
 * subset of that flow that gets the MCU running a patched image and its radio
 * configured.  Bootstrap starts in BTIF mandatory mode, follows the stock
 * sequence and switches both ends to full STP before the patch transfer, which is
 * where sequence numbers, cumulative ACKs, header checksums and the MediaTek
 * CRC-16 start applying.
 *
 * Every command byte array below is one of MediaTek's own static arrays, copied
 * out of conn_soc/common/core/wmt_ic_soc.c or read out of the shipped kernel's
 * .rodata where the vendor tree is older than the device.  Each one says which.
 *
 *
 * ── ON POLLING, AND ON WHERE THIS MAY SLEEP ──
 *
 * Interrupts stay masked and every byte is polled.  That is not laziness: BTIF's
 * interrupt routing on this SoC is not attested by anything this port has, and
 * bring-up is a strictly sequential exchange with no concurrency for an IRQ to
 * win anything back.
 *
 * Where it may sleep is a real constraint rather than a preference.  BETWEEN
 * frames it sleeps freely -- an unanswered command is a 180 ms retransmission
 * window in which nothing is coming and spinning through it would be theft.  MID
 * frame it must not: the peer is pushing bytes into a small FIFO, sixteen bytes
 * is about thirty-nine microseconds at this line rate, and usleep_range()'s floor
 * is already in that neighbourhood.  So the mid-frame waits are cpu_relax() loops
 * and the between-frame ones are not.
 *
 * The FIFO is not the only thing standing between a preempted read and a lost
 * byte -- btif_init() enables the hardware handshake, which is what stops the far
 * end while our FIFO is full -- and the STP layer's resync-and-retransmit path
 * exists for the times that is not enough.  MVII needed that path on a kernel
 * that could not be preempted at all, so it is not theoretical here.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/sched.h>
#include <linux/string.h>

#include "j36_mt6592_wifi.h"

/*
 * How long the link may stay silent before waiting for it stops being worth the
 * CPU.  Below this the peer is simply answering and the poll loop is the fastest
 * way to catch the reply; above it the peer is not going to answer inside this
 * retransmission window at all.
 */
#define J36_WMT_IDLE_POLL_US		2000

static ktime_t j36_deadline(u32 microseconds)
{
	return ktime_add_us(ktime_get(), microseconds);
}

static bool j36_before(ktime_t deadline)
{
	return ktime_before(ktime_get(), deadline);
}

/*
 * osal_crc16 (conn_soc/common/linux/pub/osal.c:278-289) is the table form of
 * this: crc starts at 0 and steps crc = (crc >> 8) ^ crc16_table[(crc ^ b) &
 * 0xff].  Its table opens 0x0000, 0xC0C1, 0xC181, 0x0140 (osal.c:55-56), which is
 * the reflected 0x8005 polynomial -- CRC-16/ARC, init 0, no final xor.  The
 * bit-at-a-time loop below produces the same value; there is no need for a table
 * at BTIF rates.  crc_ccitt and crc16 in lib/ are the wrong polynomials for this.
 */
static u16 j36_stp_crc16_update(u16 crc, u8 byte)
{
	int bit;

	crc ^= byte;
	for (bit = 0; bit < 8; bit++)
		crc = (crc >> 1) ^ ((crc & 1) ? 0xa001 : 0);
	return crc;
}

static u16 j36_stp_crc16(const u8 *data, u32 size)
{
	u16 crc = 0;
	u32 i;

	for (i = 0; i < size; i++)
		crc = j36_stp_crc16_update(crc, data[i]);
	return crc;
}

void j36_wifi_wmt_trace(struct j36_wifi *w, const char *phase)
{
	const struct j36_stp_stats *s = &w->stats;

	dev_info(w->dev,
		 "%s: frag %u/%u sz %u tx %u rx %u lsr 0x%02x | hdr %u len %u crc %u big %u/%u fw %u ooo %u retx %u ack %u short %u refill %u | evt %u op 0x%02x txus %u/%u | seq %u,%u,%u\n",
		 phase, s->fragment_index, s->fragment_count, s->fragment_size,
		 s->tx_bytes, s->rx_bytes, s->last_btif_lsr,
		 s->header_errors, s->length_errors, s->crc_errors,
		 s->oversize_events, s->last_oversize_size, s->fw_messages,
		 s->out_of_order, s->retransmits, s->acks, s->short_events,
		 s->tx_refills, s->last_event_size, s->last_event_opcode,
		 s->last_tx_us, s->max_tx_us,
		 w->tx_sequence, w->expected_rx_sequence, w->last_ack);
}

/* ── BTIF ────────────────────────────────────────────────────────────────── */

static int j36_btif_clock_on(struct j36_wifi *w)
{
	u32 value;
	int ret;

	writel(J36_PERI_BTIF_GATE, w->pericfg + J36_PERI_PDN0_CLR);
	ret = readl_poll_timeout(w->pericfg + J36_PERI_PDN0_STA, value,
				 !(value & J36_PERI_BTIF_GATE),
				 10, J36_BTIF_CLOCK_TIMEOUT_US);
	if (ret)
		j36_wifi_fail(w, "wmt-btif-clock-timeout",
			      "the BTIF clock did not ungate");
	return ret;
}

static void j36_btif_clear_fifos(struct j36_wifi *w)
{
	writel(J36_BTIF_FIFOCTRL_CLR_TX | J36_BTIF_FIFOCTRL_CLR_RX,
	       w->btif + J36_BTIF_FIFOCTRL);
	writel(0, w->btif + J36_BTIF_FIFOCTRL);
}

static int j36_btif_init(struct j36_wifi *w)
{
	int ret;

	if (w->btif_ready)
		return 0;

	ret = j36_btif_clock_on(w);
	if (ret)
		return ret;

	writel(0, w->btif + J36_BTIF_FAKELCR);
	writel(J36_BTIF_HANDSHAKE_ENABLE, w->btif + J36_BTIF_HANDSHAKE);
	writel(J36_BTIF_TX_THRESHOLD | (1 << 4), w->btif + J36_BTIF_TRI_LVL);
	writel(0, w->btif + J36_BTIF_SLEEP_EN);
	writel(J36_BTIF_DMA_AUTORESET, w->btif + J36_BTIF_DMA_EN);
	writel(0, w->btif + J36_BTIF_IER);
	j36_btif_clear_fifos(w);

	/* Match hal_btif_raise_wak_sig(): low for more than 1/32 kHz, then high. */
	writel(0, w->btif + J36_BTIF_WAK);
	udelay(80);
	writel(1, w->btif + J36_BTIF_WAK);
	usleep_range(1000, 2000);

	w->stats.last_btif_lsr = readl(w->btif + J36_BTIF_LSR);
	w->btif_ready = true;
	dev_info(w->dev, "WMT BTIF mandatory transport ready (LSR 0x%02x)\n",
		 w->stats.last_btif_lsr);
	return 0;
}

static int j36_btif_write_all(struct j36_wifi *w, const u8 *data, u32 size)
{
	const ktime_t start = ktime_get();
	const ktime_t deadline = ktime_add_us(start, J36_BTIF_IO_TIMEOUT_US);
	u32 written = 0;
	u32 elapsed;

	while (written < size && j36_before(deadline)) {
		u32 lsr = readl(w->btif + J36_BTIF_LSR);
		u32 room = 0;
		u32 i;

		w->stats.last_btif_lsr = lsr;
		if (lsr & J36_BTIF_LSR_TEMT) {
			/* Transmitter idle, so the whole FIFO is free.  This is
			 * the one capacity claim here the device has confirmed:
			 * the longest frame the link has carried in a single
			 * burst on TEMT is the fifteen-byte full-STP switch, so
			 * sixteen is within one byte of proven. */
			room = J36_BTIF_TX_FIFO_SIZE;
		} else if (lsr & J36_BTIF_LSR_THRE) {
			/*
			 * One byte, and deliberately not FIFO_SIZE - THRESHOLD.
			 *
			 * THRE says the transmit FIFO has room; how much depends
			 * on where the trigger level landed, and TRI_LVL packs
			 * the transmit and receive levels into two nibbles whose
			 * order this port has never verified against silicon.
			 * Read one way the write in j36_btif_init() is a
			 * transmit trigger of 8 and THRE means eight free; read
			 * the other way it is 1 and THRE means one free -- and
			 * on that reading an eight-byte burst overruns the FIFO
			 * by seven bytes every time, silently, with the driver
			 * counting all eight as sent.
			 *
			 * That is not hypothetical.  Every frame before the
			 * patch download fits the FIFO whole and goes out on
			 * TEMT alone -- the STP query is 11 bytes, the full-mode
			 * switch 15, an ACK 4.  The patch-address command is 26
			 * and is the first frame in the whole sequence that
			 * touches this path at all.  It was also the first one
			 * that failed, by timing out with the peer silent, which
			 * is exactly what a truncated frame looks like from
			 * here.
			 *
			 * One byte per poll is what the stock PIO writer does
			 * and is correct under either reading, since THRE cannot
			 * mean less than one free slot.  It costs an LSR read
			 * per byte and nothing else: the poll runs far faster
			 * than the wire, so the FIFO still sits at the trigger
			 * level and the burst rate is unchanged.
			 */
			room = 1;
			if (written)
				w->stats.tx_refills++;
		}
		if (!room) {
			/* Spin.  Sleeping here stretches one frame's bytes over
			 * milliseconds, which is precisely what makes a peer STP
			 * parser give up mid-packet. */
			cpu_relax();
			continue;
		}
		if (room > size - written)
			room = size - written;
		for (i = 0; i < room; i++)
			writeb(data[written++], w->btif + J36_BTIF_THR);
	}

	w->stats.tx_bytes += written;
	/* Keep the worst frame seen: whether a big fragment takes microseconds or
	 * hundreds of milliseconds decides between "the peer rejected the
	 * content" and "the peer timed out waiting for the rest of it". */
	elapsed = (u32)ktime_to_us(ktime_sub(ktime_get(), start));
	if (elapsed > w->stats.max_tx_us)
		w->stats.max_tx_us = elapsed;
	w->stats.last_tx_us = elapsed;

	if (written != size) {
		j36_wifi_fail(w, "wmt-btif-tx-timeout",
			      "BTIF transmit stalled after %u of %u bytes",
			      written, size);
		return -ETIMEDOUT;
	}
	return 0;
}

/* Mid-frame: the peer is mid-packet and a sleep here is measured against the
 * FIFO, not against the deadline. */
static int j36_btif_read_byte(struct j36_wifi *w, u8 *value, ktime_t deadline)
{
	while (j36_before(deadline)) {
		u32 lsr = readl(w->btif + J36_BTIF_LSR);

		w->stats.last_btif_lsr = lsr;
		if (lsr & J36_BTIF_LSR_DR) {
			*value = readb(w->btif + J36_BTIF_RBR);
			w->stats.rx_bytes++;
			return 0;
		}
		cpu_relax();
	}
	return -ETIMEDOUT;
}

/*
 * The first byte of a frame, and the only read in this driver allowed to sleep.
 *
 * Every read after it is mid-frame.  Before the first byte there is no frame in
 * flight to lose, and that is what makes this read different rather than merely
 * the convenient place to put the sleep.
 *
 * Even here it polls out J36_WMT_IDLE_POLL_US of silence first, so a peer that
 * answers promptly -- which is every healthy exchange -- is caught by the poll
 * loop and pays nothing for any of this.  What the sleep covers is the other
 * case: a 180 ms retransmission window in which no answer is coming.
 */
static int j36_btif_read_byte_between_frames(struct j36_wifi *w, u8 *value,
					     ktime_t deadline)
{
	const ktime_t start = ktime_get();

	while (j36_before(deadline)) {
		u32 lsr = readl(w->btif + J36_BTIF_LSR);

		w->stats.last_btif_lsr = lsr;
		if (lsr & J36_BTIF_LSR_DR) {
			*value = readb(w->btif + J36_BTIF_RBR);
			w->stats.rx_bytes++;
			return 0;
		}
		if (ktime_to_us(ktime_sub(ktime_get(), start)) < J36_WMT_IDLE_POLL_US)
			cpu_relax();
		else
			usleep_range(1000, 2000);
	}
	return -ETIMEDOUT;
}

/* ── STP ─────────────────────────────────────────────────────────────────── */

/*
 * stp_send_ack (stp_core.c:807-846), which is a bare four-byte header and nothing
 * else -- no payload, no CRC, and the sequence field deliberately 0:
 *
 *	mtkstp_header[0] = 0x80 + (0 << 3) + txAck;
 *	mtkstp_header[1] = 0x00;	// disable NAK
 *	mtkstp_header[2] = 0;
 *	mtkstp_header[3] = (h0 + h1 + h2) & 0xff;
 *
 * The zero in header[1] is not a placeholder: fgEnableNak is 0 (stp_core.c:27) and
 * the receiver reads bit 7 as the NAK flag only when it is not.
 */
static int j36_stp_send_ack(struct j36_wifi *w)
{
	u8 header[J36_STP_HEADER_SIZE];

	header[0] = 0x80 | w->last_rx_sequence;
	header[1] = 0;
	header[2] = 0;
	header[3] = header[0] + header[1] + header[2];
	return j36_btif_write_all(w, header, sizeof(header));
}

/*
 * Stock stp_do_tx_timeout() prefixes every retransmission with four 0x7f bytes --
 * osal_memset(&resync[0], 127, 4) followed by the resend loop, stp_core.c:408-428.
 * The peer parser walks MTKSTP_RESYNC1..4 on that pattern ("RESYNC must be 4
 * _continuous_ 0x7f", stp_core.c:1979) and drops whatever partial packet it was
 * mid-way through, which is the only way to recover a link that lost bytes in the
 * middle of a thousand-byte fragment.
 */
static int j36_stp_send_resync(struct j36_wifi *w)
{
	static const u8 pattern[J36_STP_RESYNC_SIZE] = { 0x7f, 0x7f, 0x7f, 0x7f };

	return j36_btif_write_all(w, pattern, sizeof(pattern));
}

static int j36_stp_discard_bytes(struct j36_wifi *w, u32 count, ktime_t deadline)
{
	u32 i;

	for (i = 0; i < count; i++) {
		u8 byte;

		if (j36_btif_read_byte(w, &byte, deadline))
			return -ETIMEDOUT;
	}
	return 0;
}

static int j36_stp_receive_wmt(struct j36_wifi *w, u8 *payload, u32 capacity,
			       u32 *payload_size, ktime_t deadline)
{
	while (j36_before(deadline)) {
		u8 header[J36_STP_HEADER_SIZE];
		u8 crc_low = 0, crc_high = 0;
		u16 computed_crc = 0;
		u32 type, size, i;
		u8 sequence;

		/* Between frames until this byte lands, mid-frame from the next
		 * one on. */
		if (j36_btif_read_byte_between_frames(w, &header[0], deadline))
			return -ETIMEDOUT;
		if ((header[0] & 0xc0) != 0x80) {
			/* Byte hunt.  Resync padding (0x7f), delimiters (0x55)
			 * and debris left over from a mis-framed packet are all
			 * skipped here. */
			continue;
		}
		for (i = 1; i < J36_STP_HEADER_SIZE; i++)
			if (j36_btif_read_byte(w, &header[i], deadline))
				return -ETIMEDOUT;

		type = (header[1] >> 4) & 0x7;
		size = ((u32)(header[1] & 0x0f) << 8) | header[2];
		sequence = (header[0] >> 3) & 0x7;

		/* The stock core never aborts on a malformed packet: it
		 * resynchronises and lets the retransmission timer recover the
		 * exchange.  Returning an error here turned one corrupted byte
		 * into a failed boot. */
		if (size > J36_STP_MAX_PAYLOAD) {
			w->stats.length_errors++;
			continue;
		}

		/*
		 * Firmware log and assert packets bypass the checksum and CRC
		 * checks in MTKSTP_FW_MSG, are never acknowledged, and do not
		 * consume a receive sequence number.  Treating them as ordinary
		 * data packets advanced the expected sequence by one, after
		 * which every real WMT event looked out of order and the patch
		 * download stalled until the command timed out.
		 *
		 * THE TRAILER IS A JUDGEMENT CALL, so it is on the record.  The
		 * two stock parsers disagree about it: the SDIO one discards a
		 * CRC byte here and says "we will discard another CRC on the
		 * outer switch procedure" (stp_core.c:1930-1938), while the
		 * BTIF/UART one -- the parser for the link we are on -- copies
		 * exactly parser.length bytes and returns to MTKSTP_SYNC with no
		 * trailer consumed at all (:2318-2326).  Every packet the sender
		 * builds carries two trailing bytes in both modes, so this reads
		 * them; stock's BTIF path leaves them to its sync hunt instead,
		 * where a CRC byte with bit 7 set can be mistaken for a header.
		 * Neither choice is lossless if the guess is wrong -- ours would
		 * eat two bytes of the following packet -- so the fw counter is
		 * the one to look at first if a WMT event ever goes missing
		 * right after one.
		 */
		if (type == J36_STP_FW_LOG_TASK || type == J36_STP_INFO_TASK) {
			w->stats.fw_messages++;
			if (j36_stp_discard_bytes(w, size + J36_STP_CRC_SIZE, deadline))
				return -ETIMEDOUT;
			continue;
		}

		if (w->stp_full_mode &&
		    (u8)(header[0] + header[1] + header[2]) != header[3]) {
			w->stats.header_errors++;
			continue;
		}

		if (w->stp_full_mode)
			w->last_ack = header[0] & 0x07;

		/* Full-mode acknowledge and negative-acknowledge packets are the
		 * four-byte header and nothing else. */
		if (w->stp_full_mode && !size) {
			w->stats.acks++;
			continue;
		}

		for (i = 0; i < size; i++) {
			u8 byte;

			if (j36_btif_read_byte(w, &byte, deadline))
				return -ETIMEDOUT;
			computed_crc = j36_stp_crc16_update(computed_crc, byte);
			if (i < capacity)
				payload[i] = byte;
		}

		if (j36_btif_read_byte(w, &crc_low, deadline) ||
		    j36_btif_read_byte(w, &crc_high, deadline))
			return -ETIMEDOUT;

		if (w->stp_full_mode) {
			u16 received_crc = crc_low | ((u16)crc_high << 8);

			if (computed_crc != received_crc) {
				/* Neither acknowledge nor advance: the peer
				 * retransmits on its own timer once our ACK
				 * fails to arrive. */
				w->stats.crc_errors++;
				continue;
			}

			/* Full STP has ONE shared receive sequence across every
			 * task, not one per WMT channel.  Patch download can
			 * provoke INFO or other side-band packets before the
			 * five-byte WMT response, and a parser that discards
			 * those before advancing and ACKing leaves the following
			 * WMT event looking out of order -- which is what stalled
			 * the first ROM-patch fragment.  Validate and ACK every
			 * data packet first, then filter by task, exactly like
			 * stp_process_packet does. */
			if (sequence != w->expected_rx_sequence) {
				w->stats.out_of_order++;
				j36_stp_send_ack(w);
				continue;
			}
			w->last_rx_sequence = sequence;
			w->expected_rx_sequence = (sequence + 1) & 0x7;
			if (j36_stp_send_ack(w))
				return -EIO;
		}

		if (type != J36_STP_WMT_TASK)
			continue;

		/*
		 * An event longer than the buffer is TRUNCATED, NOT DROPPED, and
		 * that is a correctness fix rather than a convenience.
		 *
		 * Stock does not frame WMT events as packets at all.  Each task
		 * owns a byte-stream ring and wmt_core_rx(pBuf, bufLen,
		 * &readSize) (wmt_core.c:319-336) lifts up to bufLen bytes out of
		 * it; anything beyond stays queued, which is the whole reason
		 * wmt_core_rx_flush exists.  Its caller for RF calibration asks
		 * for six bytes out of a 256-byte buffer, so the buffer size was
		 * never the limit on the event size.
		 *
		 * The chip answers that one command with the calibration result
		 * table, which is why stock exempts opcode 0x14 from its content
		 * check by name.  On real J36 Ultra silicon that event exceeded
		 * 256 bytes, and dropping it here was a whole failure mode: the
		 * header checksum, the CRC and the sequence have all been
		 * validated and acknowledged by this line, so it would be a
		 * known-good event thrown away for being too informative.  The
		 * caller only ever reads bytes 0, 1 and 4.
		 *
		 * Unlike stock we consume the remainder off the wire above
		 * rather than leaving it queued, so there is no residue for a
		 * later read to trip over and nothing to flush.
		 */
		if (size > capacity) {
			w->stats.oversize_events++;
			w->stats.last_oversize_size = size;
			*payload_size = capacity;
			return 0;
		}
		*payload_size = size;
		return 0;
	}
	return -ETIMEDOUT;
}

/* ── WMT ─────────────────────────────────────────────────────────────────── */

/* A frame the peer never acknowledged was never accepted, so the sequence it
 * carried is still free.  Handing it back matters: the stock parser drops every
 * out-of-order packet, so leaving the counter advanced after one stalled command
 * made every later command unanswerable and the link permanently dead. */
static void j36_wmt_release_tx_sequence(struct j36_wifi *w, u8 sent_sequence)
{
	if (w->stp_full_mode && w->last_ack != sent_sequence)
		w->tx_sequence = sent_sequence;
}

static int j36_wmt_exchange(struct j36_wifi *w, const u8 *command,
			    u32 command_size, u32 minimum_event_size,
			    u32 timeout_us)
{
	const u32 frame_size = J36_STP_HEADER_SIZE + command_size + J36_STP_CRC_SIZE;
	const ktime_t deadline = j36_deadline(timeout_us);
	const u8 sent_sequence = w->tx_sequence;
	u32 attempts = 0;
	u16 crc;

	if (!command_size || command_size > J36_STP_MAX_PAYLOAD ||
	    frame_size > sizeof(w->frame))
		return -EINVAL;

	/*
	 * The STP frame, both modes, from stp_send_data_no_ps (stp_core.c:882-885
	 * mandatory, :928-931 fullset):
	 *
	 *   fullset:	h0 = 0x80 + (txseq << 3) + txack
	 *		h1 = (type << 4) + ((length & 0xf00) >> 8)
	 *		h2 = length & 0xff
	 *		h3 = (h0 + h1 + h2) & 0xff	-- a sum, not a CRC
	 *		payload, then osal_crc16(payload) low byte first
	 *   mandatory:	h0 = 0x80, h1/h2 as above, h3 = 0x00,
	 *		payload, then two zero bytes where the CRC would go
	 *
	 * The trailer is two bytes in either mode, so frame_size does not branch.
	 */
	w->frame[0] = w->stp_full_mode
			      ? (0x80 | (sent_sequence << 3) | w->last_rx_sequence)
			      : 0x80;
	w->frame[1] = (J36_STP_WMT_TASK << 4) | ((command_size >> 8) & 0x0f);
	w->frame[2] = command_size;
	w->frame[3] = w->stp_full_mode
			      ? (u8)(w->frame[0] + w->frame[1] + w->frame[2])
			      : 0;
	memcpy(w->frame + J36_STP_HEADER_SIZE, command, command_size);
	crc = w->stp_full_mode ? j36_stp_crc16(command, command_size) : 0;
	w->frame[J36_STP_HEADER_SIZE + command_size] = crc;
	w->frame[J36_STP_HEADER_SIZE + command_size + 1] = crc >> 8;

	if (j36_btif_write_all(w, w->frame, frame_size))
		return -EIO;

	/* The stock STP core consumes a transmit sequence when the packet is
	 * queued, not when a matching WMT event happens to arrive.  Keep the same
	 * rule so intervening side-band packets cannot leave the two peers using
	 * different sequence numbers.  Retransmissions reuse the queued bytes, so
	 * the sequence is consumed exactly once per command. */
	if (w->stp_full_mode) {
		w->tx_sequence = (sent_sequence + 1) & 0x7;
		w->last_ack = U32_MAX;
	}

	/* Bound every wait by the stock 180 ms retransmission timer and replay
	 * the queued frame after a resync pattern, up to MTKSTP_RETRY_LIMIT
	 * times.  A ROM patch is around a hundred fragments; without this a
	 * single byte lost on the link ends the whole Wi-Fi bring-up. */
	for (;;) {
		ktime_t window = deadline;

		if (w->stp_full_mode) {
			window = j36_deadline(J36_STP_TX_TIMEOUT_US);
			if (ktime_after(window, deadline))
				window = deadline;
		}

		while (j36_before(window)) {
			u32 event_size = 0;

			if (j36_stp_receive_wmt(w, w->event, sizeof(w->event),
						&event_size, window))
				break;
			w->stats.last_event_size = event_size;
			w->stats.last_event_opcode =
				event_size >= 2 ? w->event[1] : 0xffffffff;
			if (event_size < 5 || w->event[0] != 0x02 ||
			    w->event[1] != command[1])
				continue;
			/*
			 * Length is recorded from here on, not enforced.
			 *
			 * A WMT event is 0x02, opcode, length low, length high,
			 * then the payload, and byte four is the status for
			 * every one of them; those five bytes are the entire
			 * contract this function relies on.  The per-command
			 * minimum on top of that is real -- every one was read
			 * out of the stock kernel's own command/event tables --
			 * but it is not load-bearing, and enforcing it would
			 * make a peer that answers one byte short
			 * indistinguishable from a peer that does not answer at
			 * all.  Take the event, count the disagreement, and let
			 * the trace report the size the peer really sends.
			 */
			if (event_size < minimum_event_size)
				w->stats.short_events++;
			if (!w->event[4])
				return 0;
			j36_wifi_fail(w, "wmt-command-status-failed",
				      "WMT opcode 0x%02x returned status %u",
				      command[1], w->event[4]);
			return -EIO;
		}

		if (!w->stp_full_mode || ++attempts > J36_STP_RETRY_LIMIT ||
		    !j36_before(deadline))
			break;
		w->stats.retransmits++;
		/* A whole retransmission window went by with no answer.  Between
		 * frames in both directions here: nothing is in flight inbound,
		 * and the resync pattern has not been sent yet. */
		cond_resched();
		if (j36_stp_send_resync(w) ||
		    j36_btif_write_all(w, w->frame, frame_size)) {
			j36_wmt_release_tx_sequence(w, sent_sequence);
			return -EIO;
		}
	}

	j36_wmt_release_tx_sequence(w, sent_sequence);
	j36_wifi_fail(w, "wmt-command-response-failed",
		      "WMT opcode 0x%02x got no usable answer", command[1]);
	return -ETIMEDOUT;
}

/*
 * WMT opcode 0x08: read-modify-write one connectivity-side register.
 *
 * The frame is `01 08 10 00 | 01 01 00 01 | addr | value | mask` little-endian
 * throughout, and the peer answers with the 8-byte 02 08 04 00 00 00 00 01.  The
 * semantics are new = (old & ~mask) | (value & mask), so clearing a field means
 * value 0 with the field's bits set in the mask -- not value 0 with mask
 * 0xffffffff, which would clear the whole register.
 *
 * The two patch-address commands are this same opcode with the fields baked in;
 * they stay literal byte arrays because that is the form they were read out of the
 * stock kernel in and the form they can still be compared against it in.
 */
static int j36_wmt_write_register(struct j36_wifi *w, u32 address, u32 value,
				  u32 mask)
{
	u8 command[20] = { 0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01 };
	u32 i;

	for (i = 0; i < 4; i++) {
		command[8 + i] = address >> (8 * i);
		command[12 + i] = value >> (8 * i);
		command[16 + i] = mask >> (8 * i);
	}
	return j36_wmt_exchange(w, command, sizeof(command), 8,
				J36_WMT_COMMAND_TIMEOUT_US);
}

/*
 * Power up the connectivity MCU's data local memory before anything is written
 * into it.
 *
 * This is the first thing stock's wmt_ic_soc_pwr_on() does -- before it even asks
 * how many patches there are -- and it is three separate opcode-0x08 commands
 * rather than one, each clearing a different field of 0x80100060: bits [11:8],
 * then [7:4], then bit 3, in that order.  The stock strings name them "power on
 * dlm cmd1/2/3", and the staging is deliberate; a memory power sequence that could
 * be done in one write would have been.
 *
 * Leaving it out is invisible during the download.  Every fragment still
 * acknowledges, because the acknowledgement comes from the MCU's STP parser and
 * not from the memory the payload lands in, and the reset after a partial patch
 * set acknowledges too.  What fails is the reset that COMPLETES the set -- the one
 * where the MCU branches into the patched image -- and it fails by going silent,
 * which is what an MCU executing unpowered memory looks like from the host side.
 */
static int j36_wmt_power_on_dlm(struct j36_wifi *w)
{
	static const u32 masks[] = { 0x00000f00, 0x000000f0, 0x00000008 };
	u32 i;

	for (i = 0; i < ARRAY_SIZE(masks); i++) {
		if (j36_wmt_write_register(w, 0x80100060, 0, masks[i])) {
			j36_wifi_wmt_trace(w, "dlm power-on failed");
			j36_wifi_fail(w, "wmt-dlm-power-on-failed",
				      "could not power on the connectivity MCU patch memory");
			return -EIO;
		}
	}
	return 0;
}

/*
 * The connectivity MCU clock window stock holds open across the whole download.
 *
 * `fast` raises the MCU to 138.67 MHz (stock: "enable set mcu clk" then "set mcu
 * clk to 138.67MH"); the matching call with fast == 0 puts it back to 26 MHz and
 * drops the override again.  Note where the boundaries fall in stock: the window
 * opens before the first patch and closes AFTER the reset that activates the last
 * one, so the patched firmware takes its first instructions at 138.67 MHz and is
 * only slowed down once it is running.
 *
 * Order matters in both directions and is not symmetric.  Going up, the override
 * enable at 0x80000334 comes first so that the divider write at 0x8000010c takes
 * effect; coming down, the divider is restored first and the override dropped
 * last, so the register is never left holding a stale value with the override
 * still active.
 *
 * Failure is reported but not fatal.  The clock is a performance setting for a
 * transfer that has already been observed to complete at the reset default, so
 * refusing to boot the radio over it would trade a working radio for a tidier
 * invariant.
 */
static void j36_wmt_set_mcu_clock(struct j36_wifi *w, bool fast)
{
	int ret;

	if (fast) {
		ret = j36_wmt_write_register(w, 0x80000334, 0x00010000, 0xffffffff);
		if (!ret)
			ret = j36_wmt_write_register(w, 0x8000010c, 0x00844d59,
						     0xffffffff);
	} else {
		ret = j36_wmt_write_register(w, 0x8000010c, 0x00844d00, 0xffffffff);
		if (!ret)
			ret = j36_wmt_write_register(w, 0x80000334, 0x00000000,
						     0xffffffff);
	}
	if (ret) {
		j36_wifi_wmt_trace(w, fast ? "mcu clock raise failed"
					   : "mcu clock restore failed");
		dev_warn(w->dev, "could not %s the connectivity MCU clock\n",
			 fast ? "raise" : "restore");
	}
}

/*
 * Whether this connectivity chip gets the LTE-coexistence preamble before the
 * shared RF calibration.
 *
 * THIS LIST USED TO BE CALLED chip_skips_rf_calibration() AND THAT WAS WRONG.  The
 * reading was that mtk_wcn_soc_patch_dwn() compares the chip id against these six
 * values immediately before issuing the calibration frame and, on a match,
 * branches past it.  The branch is real -- 0xc039c2d4..0xc039c314, and the `sub
 * r1, r1, #448' off 0x6752 that yields 0x6592 is what put MT6592 on the list --
 * but it does not branch past the calibration.  It branches to 0xc039c500, which
 * is a DETOUR: a wmt_core_ctrl(11) query of the platform's LTE-coex
 * configuration, the LTE filter/frequency tables it gates, and the antenna-select
 * frame.  Every one of those paths then falls into 0xc039c318 -- and 0xc039c318 is
 * the calibration block, straight-line, no gate on the frame.
 *
 * So stock sends `01 14 01 00 01' on MT6592 like everything else.  What the list
 * actually decides is what is sent FIRST.
 *
 * That matters because of the measurement that produced the wrong name.  Sending
 * the calibration frame with none of the four preamble frames in front of it
 * really did hang: the peer never answered and the bootstrap gave up ten seconds
 * later with both patches accepted and the reset acknowledged.  That observation
 * stands.  The conclusion drawn from it -- "this part does not implement the
 * command" -- does not, because a command that is ignored when its preamble is
 * missing looks exactly like a command the silicon does not have.
 *
 * The two masked comparisons are the stock ones and pair adjacent ids the way the
 * original does.
 */
static bool j36_chip_needs_lte_coex_preamble(u32 chip_id)
{
	u32 paired = chip_id & ~2u;

	return chip_id == 0x8163 || paired == 0x6580 || chip_id == 0x6752 ||
	       chip_id == 0x6592 || chip_id == 0x0321 || paired == 0x0335;
}

/*
 * A REJECTED HYPOTHESIS, kept because it is a good argument and the next reader
 * will otherwise re-derive it and break the radio again.
 *
 * The other half of stock's gate is wmt_core_ctrl(11)'s buf[0], and the argument
 * ran: buf[0] == 0 means the board has no LTE modem to coexist with, and on that
 * path stock sends none of the four preamble frames.  The J36 Ultra has no modem
 * -- its scatter file lists no MD1IMG, no modem NVRAM, no protocol partition --
 * and one of the four frames, is_lte_project, carries payload 0x01, so we would be
 * asserting "this IS an LTE project" to a chip in a device with no modem.  All
 * true.
 *
 * It is still wrong, and the board says so.  Sending the preamble unconditionally
 * for chip 0x6592 is what MVII did up to commit b5403d48f1, which SCANNED AND
 * ASSOCIATED to open networks.  Gating it off arrived with "Try fix calibration"
 * (0f8d846483), and from there the WLAN firmware has never asserted WLAN_READY.
 * It is the only behavioural difference in the whole WMT layer between that
 * working build and the broken one.
 *
 * The likely reconciliation is that these frames do more than announce a modem:
 * they are the antenna-select and coexistence-filter setup that the RF
 * calibration immediately after them depends on, so skipping them leaves the front
 * end unconfigured whatever buf[0] would have said.  Until someone can read the
 * real buf[0] off this board rather than infer it, the hardware's answer outranks
 * the inference.
 */

/*
 * The four frames stock issues between the MCU-clock restore and the shared RF
 * calibration, transcribed byte for byte from the table at 0xc0b6cfac in this
 * device's own shipped kernel.
 *
 * Stock reaches them through wmt_core_init_script() entries and gates them on
 * wmt_core_ctrl(11), which returns the board's LTE-coexistence configuration.
 * MT6592 is not in the inner chip list that would select the non-default tables,
 * so it gets the defaults, which is what is below.  buf itself is a platform
 * config blob we do not have and cannot synthesise, so the two data-dependent
 * gates are not reproduced: the defaults go out unconditionally and the log says
 * so.
 *
 * The last frame's payload byte is patched at runtime by stock from
 * wmt_core_ctrl(28) -- the TDM request antenna-select count.  Its static value is
 * 0x00 and 0x00 is what we send, because a count we have not measured is not
 * better than the one the table ships with.
 *
 * None of these are fatal.  They configure a coexistence filter for a radio we are
 * not using; a chip that rejects one has still told us more than a bootstrap that
 * refuses to continue.
 *
 * These four are the only WMT frames in this file with no counterpart in the
 * vendor tree -- conn_soc's wmt_ic_soc.c has no LTE tables at all and subcommands
 * 0x11, 0x12, 0x14 and 0x15 of opcode 0x10 do not exist in it.  That revision
 * simply predates them, so the shipped kernel's .rodata is the only ground truth,
 * which is why the offsets above are quoted rather than a file and line.
 */
static void j36_wmt_send_lte_coex_preamble(struct j36_wifi *w)
{
	static const u8 lte_coex_filter[] = {
		0x01, 0x10, 0x45, 0x00, 0x11, 0x00, 0x00, 0x01, 0x00, 0x16,
		0x16, 0x16, 0x16, 0x00, 0x00, 0x00, 0x00, 0x63, 0x63, 0x63,
		0x63, 0x3c, 0x3c, 0x3c, 0x3c, 0x04, 0x04, 0x04, 0x04, 0x01,
		0x01, 0x01, 0x01, 0x0e, 0x0e, 0x0e, 0x0e, 0x0b, 0x0b, 0x0b,
		0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00 };
	static const u8 lte_freq_idx[] = {
		0x01, 0x10, 0x21, 0x00, 0x12, 0xfc, 0x08, 0x15, 0x09, 0x2e,
		0x09, 0x47, 0x09, 0xc4, 0x09, 0xdd, 0x09, 0xf6, 0x09, 0x0f,
		0x0a, 0x14, 0x09, 0x2d, 0x09, 0x46, 0x09, 0x5f, 0x09, 0xdd,
		0x09, 0xf5, 0x09, 0x0d, 0x0a, 0x27, 0x0a };
	static const u8 is_lte_project[] = { 0x01, 0x10, 0x02, 0x00, 0x15, 0x01 };
	static const u8 tdm_antsel[] = { 0x01, 0x10, 0x02, 0x00, 0x14, 0x00 };

	static const struct {
		const u8 *frame;
		u32 size;
		const char *name;
	} script[] = {
		{ lte_coex_filter, sizeof(lte_coex_filter), "lte coex filter spec" },
		{ lte_freq_idx, sizeof(lte_freq_idx), "lte freq idx" },
		{ is_lte_project, sizeof(is_lte_project), "is lte project" },
		{ tdm_antsel, sizeof(tdm_antsel), "tdm req antsel num" },
	};
	u32 i;

	for (i = 0; i < ARRAY_SIZE(script); i++) {
		int ret = j36_wmt_exchange(w, script[i].frame, script[i].size, 5,
					   J36_WMT_COMMAND_TIMEOUT_US);

		dev_info(w->dev, "WMT %s%s\n", ret ? "(no answer) " : "",
			 script[i].name);
	}
}

static int j36_wmt_run_rf_calibration(struct j36_wifi *w)
{
	/* WMT_CORE_START_RF_CALIBRATION_CMD (wmt_ic_soc.c:177), the sole entry of
	 * calibration_table (:482-485).  Its expected event is six bytes, which is
	 * why the exchange below waits for 6 and not the 5 most others run to. */
	static const u8 calibration[] = { 0x01, 0x14, 0x01, 0x00, 0x01 };
	int ret;

	/*
	 * BOTH VCN33 PALDOs, on for the frame and off again.  wmt_ic_soc.c's step
	 * 7 (:762-782) is exactly this bracket and it is the only place in the
	 * whole conn_soc tree that touches WIFI_PALDO:
	 *
	 *	BT_PALDO   := PALDO_ON
	 *	WIFI_PALDO := PALDO_ON
	 *	wmt_core_init_script(calibration_table)
	 *	BT_PALDO   := PALDO_OFF
	 *	WIFI_PALDO := PALDO_OFF
	 *
	 * Calibration measures both front ends, so both have to be powered while
	 * it runs; raising BT only leaves the Wi-Fi front end calibrated against a
	 * rail stock has up and this driver did not.
	 *
	 * Both are dropped on the way out whatever the frame did -- stock ignores
	 * the return values here too, and a rail stuck on is worse than a missing
	 * log line.
	 */
	if (j36_wifi_set_bt_rail(w, true)) {
		j36_wifi_fail(w, "wmt-vcn33-bt-enable-failed",
			      "could not enable VCN33_BT for RF calibration");
		return -EIO;
	}
	if (j36_wifi_set_wifi_rail(w, true)) {
		j36_wifi_set_bt_rail(w, false);
		j36_wifi_fail(w, "wmt-vcn33-wifi-enable-failed",
			      "could not enable VCN33_WIFI for RF calibration");
		return -EIO;
	}
	usleep_range(1000, 2000);

	/* Six bytes have to arrive, but what is in them does not matter.  Every
	 * other init-script entry is memcmp'd against its expected event, and this
	 * one alone is exempted by name -- "workaround RF calibration data EVT, do
	 * not care this EVT" (wmt_core.c:620-624).  The length check above it is
	 * not exempted, so a short read still fails. */
	ret = j36_wmt_exchange(w, calibration, sizeof(calibration), 6,
			       J36_WMT_CALIBRATION_TIMEOUT_US);
	j36_wifi_set_bt_rail(w, false);
	j36_wifi_set_wifi_rail(w, false);
	if (ret) {
		j36_wifi_wmt_trace(w, "rf calibration failed");
		if (w->peer_answered_after_reset)
			j36_wifi_fail(w, "wmt-rf-calibration-failed",
				      "shared RF calibration failed");
		else
			j36_wifi_fail(w, "wmt-mcu-mute-after-patch-reset",
				      "the connectivity MCU stopped answering after the ROM patch reset");
		return ret;
	}
	return 0;
}

/*
 * Everything stock does after the last patch, and nothing it does not.
 *
 * mtk_wcn_soc_sw_init() ends with six conditional steps, and four of them are
 * compiled out on this platform.  The switches are at the top of wmt_ic_soc.c and
 * they are literals, not Kconfig:
 *
 *	CFG_WMT_MULTI_PATCH	   1  -> the two address commands, and a WMT_RESET
 *				      after every patch rather than one at the end
 *	CFG_CHECK_WMT_RESULT	   1  -> every event is compared, not just counted
 *	CFG_SUBSYS_COEX_NEED	   0  -> no BT/WIFI/PTA/MISC coex frames; the single
 *				      COEX_WMT command is the whole of coex
 *	CFG_WMT_CRYSTAL_TIMING_SET 0  -> no crystal trim SET/GET pair
 *	CFG_WMT_SDIO_DRIVING_SET   0  -> no 0x80050050 drive-strength write
 *
 * So the omissions below are the stock configuration rather than gaps, and the
 * order is stock's: calibration, then coex, then the co-clock, then the FM strap.
 * The A-die/PMIC-id WIFI_5G_PALDO branch is skipped for a different reason -- it
 * only fires for an MT6625 A-die, and this part reports 0x6627.
 *
 * There is deliberately NO FUNC_CTRL here.  MVII sent one for a while on the
 * strength of a stock log line -- "wmt wlan func on befor wlan probe" -- read as
 * evidence of a command.  It is not; it is the message printed immediately before
 * a function call.  wmt_func_wifi_on() only calls mtk_wcn_wlan_probe(), and the
 * one function that would have sent the command sits inside `#if 0' under the
 * comment "in soc, wmt turn on wifi directly, no not need operate SDIO".  The
 * frame itself worked -- the chip answered status 0 -- and changed nothing
 * measurable.
 */
static int j36_wmt_finalize(struct j36_wifi *w)
{
	/*
	 * Every frame below is one of MediaTek's own static command arrays,
	 * copied byte for byte out of conn_soc/common/core/wmt_ic_soc.c:
	 *
	 *	query_stp	 WMT_QUERY_STP_CMD		(:100)
	 *	coex		 WMT_COEX_SETTING_CONFIG_CMD	(:121)  -- see below
	 *	co_clock	 WMT_CORE_CO_CLOCK_CMD		(:174)
	 *	fm_strap	 WMT_STRAP_CONF_CMD_FM_COMM	(:149)
	 *	core_dump_level	 WMT_CORE_DUMP_LEVEL_04_CMD	(:171)
	 *
	 * The coex frame is the one that is not a straight copy, and it took the
	 * longest to settle because the initialiser at :121 ends in 0x00 while we
	 * send 0x01.  The initialiser is not what stock transmits: wmt_ic_soc.c:1384
	 * overwrites that byte immediately before the send with
	 * pWmtGenConf->coex_wmt_ant_mode, parsed out of /etc/firmware/WMT_SOC.cfg.
	 * This device's copy says coex_wmt_ant_mode=1, so 0x01 is this board's
	 * antenna mode and the 0x00 in the reference initialiser is only what a
	 * board with no config file would send.
	 *
	 * co_clock is likewise conditional in stock -- :801 sends osc_type_table
	 * only when the same file's co_clock_flag is set.  That flag is 1 here, so
	 * sending it unconditionally is stock's behaviour on this board rather
	 * than an extra frame.
	 */
	static const u8 query_stp[] = { 0x01, 0x04, 0x01, 0x00, 0x04 };
	static const u8 coex[] = { 0x01, 0x10, 0x02, 0x00, 0x01, 0x01 };
	static const u8 co_clock[] = { 0x01, 0x0a, 0x02, 0x00, 0x08, 0x03 };
	static const u8 fm_strap[] = { 0x01, 0x05, 0x02, 0x00, 0x02, 0x02 };
	static const u8 core_dump_level[] = { 0x01, 0x0f, 0x07, 0x00, 0x04, 0x00,
					      0x00, 0x00, 0x00, 0x00, 0x00 };

	/* Is the peer there at all?  The STP capability query is the cheapest
	 * thing that answers that: it reads state, changes nothing, and this link
	 * has already had it answered twice, so a failure here is about the peer
	 * rather than about the command.
	 *
	 * It deliberately does not gate what follows.  Stock never re-queries
	 * after the final reset, so there is no stock behaviour to say the patched
	 * image must still answer it, and returning early would risk refusing a
	 * radio that was about to work.  All it does is decide which name the next
	 * real failure gets -- without it, whichever command happens to come first
	 * gets blamed for a dead MCU, and that has already been two different
	 * faults for one cause. */
	w->peer_answered_after_reset =
		!j36_wmt_exchange(w, query_stp, sizeof(query_stp), 10,
				  J36_WMT_COMMAND_TIMEOUT_US);
	if (!w->peer_answered_after_reset) {
		j36_wifi_wmt_trace(w, "silent after patch reset");
		dev_warn(w->dev,
			 "the connectivity MCU did not answer the STP query after the patch reset\n");
	}

	/* An unread chip id is not a licence to skip the preamble: fall through to
	 * the stock behaviour for a part we cannot identify. */
	if (j36_chip_needs_lte_coex_preamble(w->chip_id))
		j36_wmt_send_lte_coex_preamble(w);

	/* Now the calibration, in the place stock puts it -- after the preamble,
	 * not instead of it.  Deliberately NOT fatal: the bring-up got further
	 * with the frame absent than with it fatal, and the rest of the
	 * configuration below is what the scan path actually needs. */
	if (j36_wmt_run_rf_calibration(w)) {
		w->calibrated = false;
		dev_warn(w->dev,
			 "shared RF calibration unanswered, continuing without it\n");
	} else {
		w->calibrated = true;
		dev_info(w->dev, "shared RF calibration acknowledged\n");
	}

	if (j36_wmt_exchange(w, coex, sizeof(coex), 5, J36_WMT_COMMAND_TIMEOUT_US)) {
		j36_wifi_wmt_trace(w, "coexistence setup failed");
		if (w->peer_answered_after_reset)
			j36_wifi_fail(w, "wmt-coexistence-config-failed",
				      "coexistence setup failed");
		else
			j36_wifi_fail(w, "wmt-mcu-mute-after-patch-reset",
				      "the connectivity MCU stopped answering after the ROM patch reset");
		return -EIO;
	}
	if (j36_wmt_exchange(w, co_clock, sizeof(co_clock), 5,
			     J36_WMT_COMMAND_TIMEOUT_US)) {
		j36_wifi_wmt_trace(w, "co-clock setup failed");
		j36_wifi_fail(w, "wmt-co-clock-config-failed",
			      "co-clock setup failed");
		return -EIO;
	}

	/* FM strap does not gate WLAN; keep the stock command but do not reject
	 * Wi-Fi if an unrelated FM configuration event is absent. */
	j36_wmt_exchange(w, fm_strap, sizeof(fm_strap), 6, J36_WMT_COMMAND_TIMEOUT_US);

	/* The last frame in stock's power-on sequence, sent once the core-dump
	 * level is wanted.  The source names it WMT_CORE_DUMP_LEVEL_04_CMD
	 * (wmt_ic_soc.c:171) -- "to get full dump when f/w assert" -- and it is
	 * the only one of the four dump levels not inside the `#if 0' above it.
	 * All six payload bytes after the level are zero.  Not fatal for the same
	 * reason as the FM strap: it configures a diagnostic, not a radio. */
	j36_wmt_exchange(w, core_dump_level, sizeof(core_dump_level), 5,
			 J36_WMT_COMMAND_TIMEOUT_US);

	w->ready = true;
	w->blocked = NULL;
	dev_info(w->dev, "WMT ROM patches loaded, radio configured%s\n",
		 w->calibrated ? "" : " (RF calibration unanswered)");
	return 0;
}

/*
 * The mandatory-mode STP capability query, on its own.
 *
 * Everything after this point needs a ROM patch image, and for a long time that
 * meant every fault below the patch stage -- an ungated BTIF, a connectivity MCU
 * still in reset, a link dropping bytes -- reported itself as "the ROM patch did
 * not load", because load_patch was the only door in and it opens by doing all of
 * this first.  Splitting the query out separates "the peer is not talking" from
 * "the patch image is wrong", and does it without needing an image to hand.
 */
int j36_wifi_wmt_probe_link(struct j36_wifi *w)
{
	static const u8 query_stp[] = { 0x01, 0x04, 0x01, 0x00, 0x04 };

	if (j36_btif_init(w))
		return -EIO;
	/* Already switched to full STP by an earlier call; the mandatory-mode
	 * query is not valid twice and the peer has plainly answered once. */
	if (w->stp_full_mode)
		return 0;
	if (j36_wmt_exchange(w, query_stp, sizeof(query_stp), 10,
			     J36_WMT_COMMAND_TIMEOUT_US)) {
		j36_wifi_wmt_trace(w, "mandatory stp query failed");
		j36_wifi_fail(w, "wmt-stp-bootstrap-failed",
			      "the bootstrap did not answer the mandatory STP capability query");
		return -EIO;
	}
	dev_info(w->dev, "the connectivity MCU answered the STP capability query\n");
	w->blocked = NULL;
	return 0;
}

/*
 * One complete pass over the patch body at the given fragment size.  Every
 * fragment is a separate WMT command, so the size only has to agree with itself
 * across the pass: the first is flagged first, the last is flagged last, and the
 * peer reassembles from the flags rather than from a count.
 *
 * The frame is stock's, byte for byte.  WMT_PATCH_CMD[] is declared {0x01, 0x01,
 * 0x00, 0x00, 0x00} (wmt_ic_soc.c:103) and the send loop (:1770-1786) patches two
 * of those five in place -- the flag byte, and the two-byte length that is 1 +
 * fragSize.  The answer it waits for is five bytes.  "Last" is tested before
 * "first", so a one-fragment patch is flagged LAST, and that is the order here.
 */
static int j36_wmt_download_patch_body(struct j36_wifi *w, const u8 *body,
				       u32 body_size, u32 fragment_bytes)
{
	const u32 fragment_count = DIV_ROUND_UP(body_size, fragment_bytes);
	u32 fragment;

	w->stats.fragment_count = fragment_count;
	w->stats.fragment_size = fragment_bytes;
	for (fragment = 0; fragment < fragment_count; fragment++) {
		u32 offset = fragment * fragment_bytes;
		u32 fragment_size = min(body_size - offset, fragment_bytes);
		u16 payload_size = fragment_size + 1;

		w->stats.fragment_index = fragment;
		w->command[0] = 0x01;
		w->command[1] = 0x01;
		w->command[2] = payload_size;
		w->command[3] = payload_size >> 8;
		w->command[4] = fragment + 1 == fragment_count
					? J36_WMT_PATCH_LAST
					: (fragment ? J36_WMT_PATCH_MIDDLE
						    : J36_WMT_PATCH_FIRST);
		memcpy(w->command + 5, body + offset, fragment_size);
		if (j36_wmt_exchange(w, w->command, fragment_size + 5, 5,
				     J36_WMT_COMMAND_TIMEOUT_US))
			return -EIO;
		w->stats.patch_bytes += fragment_size;
	}
	return 0;
}

int j36_wifi_wmt_load_patch(struct j36_wifi *w, const void *data, size_t size)
{
	/*
	 * Four more of MediaTek's static arrays, again byte for byte from
	 * conn_soc/common/core/wmt_ic_soc.c:
	 *
	 *	query_stp	WMT_QUERY_STP_CMD	(:100)
	 *	set_stp_full	WMT_SET_STP_CMD		(:147)
	 *	patch_address	WMT_PATCH_ADDRESS_CMD	(:114)
	 *	reset		WMT_RESET_CMD		(:105)
	 *
	 * set_stp_full's four payload bytes 0xdf 0x0e 0x68 0x01 are the capability
	 * word, and they are not a value to be derived -- stock hard-codes the
	 * same four in the array it compares the query answer against (:102).  The
	 * default-capability form at :101 ends 0x11 0x00 0x00 0x00 and is what an
	 * unpatched bootstrap reports; asking for 0xdf 0x0e 0x68 0x01 is asking for
	 * the full set.
	 *
	 * patch_address is sent verbatim -- :1719 transmits sizeof() straight from
	 * the initialiser with nothing patched into it, and the 0x10 0x00 length
	 * halfword accounts for all sixteen payload bytes, so the trailing four
	 * 0xff are payload and not a match wildcard.
	 */
	static const u8 query_stp[] = { 0x01, 0x04, 0x01, 0x00, 0x04 };
	static const u8 set_stp_full[] = { 0x01, 0x04, 0x05, 0x00, 0x03,
					   0xdf, 0x0e, 0x68, 0x01 };
	static const u8 patch_address[] = { 0x01, 0x08, 0x10, 0x00, 0x01, 0x01,
					    0x00, 0x01, 0x3c, 0x02, 0x09, 0x02,
					    0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
					    0xff, 0xff };
	static const u8 reset[] = { 0x01, 0x07, 0x01, 0x00, 0x04 };
	/*
	 * The per-patch destination, and the one value in this whole driver that
	 * the kernel source cannot settle on its own.
	 *
	 * wmt_ic_soc.c:1741 is `memcpy(&WMT_PATCH_P_ADDRESS_CMD[12], addressByte,
	 * 4)', so the four bytes at [12..15] -- the value half of a write to
	 * connectivity register 0x020904c4 -- are whatever addRess[4] holds.  But
	 * addRess does not come from the kernel: wmt_ctrl_get_patch_info() copies
	 * it out of a WMT_PATCH_INFO that userspace filled in through
	 * WMT_IOCTL_SET_PATCH_INFO (wmt_dev.c:1975), and the sender is
	 * /system/bin/6620_launcher, a stripped PIE with no source in the tree.
	 *
	 * What the launcher's own strings do say is that it reads them out of the
	 * file -- "read patch info:0x%02x,0x%02x,0x%02x,0x%02x" -- and the two real
	 * images off this device pin down where.  Their 28-byte headers differ in
	 * exactly one word, at offset 24:
	 *
	 *   ROMv1_patch_1_0_hdr.bin  80776 bytes  ...ALPS 8a00 8a00 | 22 00 06 00
	 *   ROMv1_patch_1_1_hdr.bin  24592 bytes  ...ALPS 8a00 8a00 | 21 00 0e f0
	 *
	 * Byte 24 is already known to be (patch_count << 4) | download_sequence,
	 * and both files agreeing on a count of 2 while disagreeing on the sequence
	 * is what confirms it.  So the address is the remaining three bytes, low
	 * byte zero, which reads 0x00060000 for the big patch and 0xf00e0000 for
	 * the small one -- both page-aligned, and the second inside the CONSYS EMI
	 * window whose firmware-side base is 0xf0000000.  Taking four bytes from 24
	 * would send the descriptor byte as part of the address; starting at 25
	 * would give 0xff000600 and 0x00f00e00, neither of which is an address.
	 * The stock table's own default value, 0x01003f00, has a zero low byte too.
	 *
	 * The small patch is the coredump one -- its body starts with the ASCII
	 * "; coredump start" -- which is why its destination is in the shared EMI
	 * window rather than in code space.
	 */
	u8 part_address[] = { 0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
			      0xc4, 0x04, 0x09, 0x02, 0x00, 0x3f, 0x00, 0x01,
			      0xff, 0xff, 0xff, 0xff };
	const u8 *image = data;
	const u8 *body;
	u32 body_size, bytes_before;
	u8 metadata, patch_count, sequence;

	if (!image || size <= J36_WMT_PATCH_HEADER_SIZE ||
	    memcmp(image + 16, "ALPS", 4)) {
		j36_wifi_fail(w, "wmt-patch-header-invalid",
			      "the ROM patch header is not an ALPS image");
		return -EINVAL;
	}

	metadata = image[J36_WMT_PATCH_METADATA_OFFSET];
	patch_count = metadata >> 4;
	sequence = metadata & 0x0f;
	if (!patch_count || patch_count > 7 || !sequence || sequence > patch_count) {
		j36_wifi_fail(w, "wmt-patch-sequence-invalid",
			      "ROM patch sequence metadata 0x%02x is invalid", metadata);
		return -EINVAL;
	}
	if (w->patch_count && w->patch_count != patch_count) {
		j36_wifi_fail(w, "wmt-patch-count-mismatch",
			      "ROM patch count changed between images (%u then %u)",
			      w->patch_count, patch_count);
		return -EINVAL;
	}
	w->patch_count = patch_count;
	if (w->patch_mask & BIT(sequence - 1)) {
		if (!w->ready && hweight8(w->patch_mask) == w->patch_count)
			return j36_wmt_finalize(w);
		return 0;
	}
	if (sequence != hweight8(w->patch_mask) + 1) {
		j36_wifi_fail(w, "wmt-patch-order-invalid",
			      "ROM patches were supplied out of sequence");
		return -EINVAL;
	}

	if (j36_btif_init(w))
		return -EIO;

	if (!w->patch_mask) {
		if (!w->stp_full_mode) {
			if (j36_wmt_exchange(w, query_stp, sizeof(query_stp), 10,
					     J36_WMT_COMMAND_TIMEOUT_US)) {
				j36_wifi_wmt_trace(w, "mandatory stp query failed");
				j36_wifi_fail(w, "wmt-stp-bootstrap-failed",
					      "the bootstrap did not answer the mandatory STP capability query");
				return -EIO;
			}
			if (j36_wmt_exchange(w, set_stp_full, sizeof(set_stp_full),
					     6, J36_WMT_COMMAND_TIMEOUT_US)) {
				j36_wifi_wmt_trace(w, "full stp switch failed");
				j36_wifi_fail(w, "wmt-stp-full-mode-switch-failed",
					      "could not switch BTIF to full STP mode");
				return -EIO;
			}
			w->stp_full_mode = true;
			w->tx_sequence = 0;
			w->expected_rx_sequence = 0;
			w->last_rx_sequence = 7;
			/* The stock MT6592 path waits 10 ms for the connectivity
			 * MCU to switch mechanisms before the first full-STP
			 * packet. */
			msleep(10);
			dev_info(w->dev, "WMT BTIF full STP mode ready\n");
		}
		if (j36_wmt_exchange(w, query_stp, sizeof(query_stp), 10,
				     J36_WMT_COMMAND_TIMEOUT_US)) {
			j36_wifi_wmt_trace(w, "full stp query failed");
			j36_wifi_fail(w, "wmt-stp-full-mode-query-failed",
				      "the full STP capability query failed");
			return -EIO;
		}
		j36_wifi_wmt_trace(w, "full stp ready");

		/*
		 * Both of these belong to the first patch only, and both are
		 * what stock brackets the whole download with.  The mask test is
		 * what makes them once-per-set: this function is called once per
		 * image, but the sequence stock runs is per power-on.
		 */
		if (j36_wmt_power_on_dlm(w))
			return -EIO;
		j36_wmt_set_mcu_clock(w, true);
	}

	if (j36_wmt_exchange(w, patch_address, sizeof(patch_address), 8,
			     J36_WMT_COMMAND_TIMEOUT_US)) {
		j36_wifi_wmt_trace(w, "patch address failed");
		j36_wifi_fail(w, "wmt-patch-address-failed",
			      "patch-address setup failed");
		return -EIO;
	}

	part_address[12] = 0;
	part_address[13] = image[J36_WMT_PATCH_METADATA_OFFSET + 1];
	part_address[14] = image[J36_WMT_PATCH_METADATA_OFFSET + 2];
	part_address[15] = image[J36_WMT_PATCH_METADATA_OFFSET + 3];
	if (j36_wmt_exchange(w, part_address, sizeof(part_address), 8,
			     J36_WMT_COMMAND_TIMEOUT_US)) {
		j36_wifi_wmt_trace(w, "part-patch address failed");
		j36_wifi_fail(w, "wmt-part-patch-address-failed",
			      "partial-patch address setup failed");
		return -EIO;
	}
	j36_wifi_wmt_trace(w, "patch address ready");

	body = image + J36_WMT_PATCH_HEADER_SIZE;
	body_size = size - J36_WMT_PATCH_HEADER_SIZE;
	bytes_before = w->stats.patch_bytes;

	/* One retry at the same fragment size, not a search over sizes.
	 *
	 * A halving ladder used to live here -- 1000, then 500, 250 and on down --
	 * put in to find out whether frame length was what the link choked on.  It
	 * answered, and the answer was no: 136-byte fragments stalled the same way
	 * the full-size ones did, and 125 after them.  What the ladder costs is
	 * not free either: every rung replays the body from fragment zero, and a
	 * pass that stalls spends two seconds on the fragment it stalls at, so the
	 * full ladder is minutes of a machine doing nothing else.
	 *
	 * A single retry keeps the part that was worth having: a link that lost
	 * one frame gets a second pass, with both address commands re-issued first
	 * to put the peer back at the start of the sequence. */
	if (j36_wmt_download_patch_body(w, body, body_size,
					J36_WMT_PATCH_FRAGMENT_SIZE)) {
		j36_wifi_wmt_trace(w, "patch fragment retry");
		w->stats.patch_bytes = bytes_before;

		if (j36_wmt_exchange(w, patch_address, sizeof(patch_address), 8,
				     J36_WMT_COMMAND_TIMEOUT_US) ||
		    j36_wmt_exchange(w, part_address, sizeof(part_address), 8,
				     J36_WMT_COMMAND_TIMEOUT_US)) {
			j36_wifi_wmt_trace(w, "patch re-address failed");
			j36_wifi_fail(w, "wmt-patch-readdress-failed",
				      "patch-address setup failed after a stalled pass");
			return -EIO;
		}
		if (j36_wmt_download_patch_body(w, body, body_size,
						J36_WMT_PATCH_FRAGMENT_SIZE)) {
			j36_wifi_wmt_trace(w, "patch fragment stalled");
			j36_wifi_fail(w, "wmt-patch-fragment-failed",
				      "ROM patch fragment download failed");
			return -EIO;
		}
	}

	if (j36_wmt_exchange(w, reset, sizeof(reset), 5, J36_WMT_COMMAND_TIMEOUT_US)) {
		j36_wifi_wmt_trace(w, "patch reset failed");
		j36_wifi_fail(w, "wmt-patch-reset-failed",
			      "the reset after a ROM patch failed");
		return -EIO;
	}
	msleep(20);

	w->patch_mask |= BIT(sequence - 1);
	w->blocked = "wmt-more-patches-required";
	dev_info(w->dev, "WMT ROM patch %u of %u loaded\n", sequence, patch_count);
	j36_wifi_wmt_trace(w, "patch link");

	if (hweight8(w->patch_mask) == w->patch_count) {
		/* The set is complete, so the reset above is the one the patched
		 * image booted from.  Stock closes its clock window here, after
		 * that reset and before it configures the radio. */
		j36_wmt_set_mcu_clock(w, false);
		return j36_wmt_finalize(w);
	}
	return 0;
}
