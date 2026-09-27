/*
 * mt6592_wifi_wmt.c — minimal MT6592 WMT bootstrap over the integrated BTIF.
 *
 * The stock conn_soc stack initializes the connectivity MCU before probing the
 * WLAN AHB HIF. MVII only needs the discovery subset of that flow, but it must
 * still apply the two ROMv1 patches or the WLAN firmware has nothing patched to
 * run on. Shared RF calibration is NOT part of that subset on this chip; see
 * chip_skips_rf_calibration() for what the stock driver actually does.
 *
 * Bootstrap starts in BTIF mandatory mode, then follows the stock sequence and
 * switches both ends to full STP before patch transfer. Full mode uses sequence
 * numbers, cumulative ACKs, header checksums, and the MediaTek CRC-16; polling
 * keeps the implementation independent of scheduler and GIC routing.
 */

#include "mt6592_wifi_wmt.h"

#include "mt6592_bootstatus.h"
#include "mt6592_delay.h"
#include "mt6592_pmic.h"
#include "mt6592_timer.h"
#include "mt6592_uart.h"
#include "mt6592_wifi_sdio.h"

#include <stdint.h>

extern void minos_machine64_io_yield(void) __attribute__((weak));

/* Non-zero when the CPU was actually handed back, so a caller can fall through
 * to its own wait when this build has no scheduler linked in. */
static uint32_t g_wmt_yields;

static int wmt_cooperative_yield(void)
{
    if (!(&minos_machine64_io_yield))
    {
        return 0;
    }
    ++g_wmt_yields;
    minos_machine64_io_yield();
    return 1;
}

#define BIT(n) (1u << (n))

enum
{
    PERICFG_BASE   = 0x10003000u,
    PERI_PDN0_CLR  = PERICFG_BASE + 0x0010u,
    PERI_PDN0_STA  = PERICFG_BASE + 0x0018u,
    PERI_BTIF_GATE = BIT(20),

    BTIF_BASE      = 0x1100c000u,
    BTIF_RBR       = 0x0000u,
    BTIF_THR       = 0x0000u,
    BTIF_IER       = 0x0004u,
    BTIF_FIFOCTRL  = 0x0008u,
    BTIF_FAKELCR   = 0x000cu,
    BTIF_LSR       = 0x0014u,
    BTIF_SLEEP_EN  = 0x0048u,
    BTIF_DMA_EN    = 0x004cu,
    BTIF_TRI_LVL   = 0x0060u,
    BTIF_WAK       = 0x0064u,
    BTIF_HANDSHAKE = 0x006cu,

    BTIF_LSR_DR           = BIT(0),
    BTIF_LSR_THRE         = BIT(5),
    BTIF_LSR_TEMT         = BIT(6),
    BTIF_FIFOCTRL_CLR_RX  = BIT(1),
    BTIF_FIFOCTRL_CLR_TX  = BIT(2),
    BTIF_DMA_AUTORESET    = BIT(2),
    BTIF_HANDSHAKE_ENABLE = BIT(0),
    BTIF_TX_FIFO_SIZE     = 16u,
    BTIF_TX_THRESHOLD     = 8u,

    STP_HEADER_SIZE = 4u,
    STP_CRC_SIZE    = 2u,
    /* conn_soc/common/include/stp_exp.h:31-34 names all three and caps the
     * field: WMT_TASK_INDX 4, STP_TASK_INDX 5, INFO_TASK_INDX 6, and
     * MTKSTP_MAX_TASK_NUM 7 -- which is why the parser below masks the type
     * nibble with 7 rather than 15, exactly as stock does with 0x70 >> 4
     * (stp_core.c:2089). */
    STP_WMT_TASK    = 4u,
    /* Task 5 (firmware assert/coredump) and task 6 (runtime firmware log) are
     * parsed by MTKSTP_FW_MSG in the stock core: no header checksum check, no
     * CRC check, no ACK, and no receive-sequence consumption. The dispatch is
     * ahead of the checksum test, at the top of MTKSTP_CHECKSUM
     * (stp_core.c:2193-2196), so a firmware message is taken on the strength of
     * its type byte alone -- which is the point, since the chip sends these
     * while it is dying. */
    STP_FW_LOG_TASK = 5u,
    STP_INFO_TASK   = 6u,
    STP_MAX_PAYLOAD = 2048u,

    /* conn_soc/common/core/include/stp_core.h:76-77: MTKSTP_TX_TIMEOUT is 180,
     * in ms, and MTKSTP_RETRY_LIMIT is 10. */
    STP_TX_TIMEOUT_US = 180000u,
    STP_RETRY_LIMIT   = 10u,
    STP_RESYNC_SIZE   = 4u,

    /* struct WMT_PATCH (conn_soc/common/core/include/wmt_core.h:322-328) is
     * ucDateTime[16] + ucPLat[4] + u2HwVer + u2SwVer + u4PatchVer = 28 bytes,
     * and mtk_wcn_soc_patch_dwn steps past exactly that before the first
     * fragment (wmt_ic_soc.c:1702-1703). */
    WMT_PATCH_HEADER_SIZE   = 28u,
    /* DEFAULT_PATCH_FRAG_SIZE (wmt_ic_soc.c:40), and the only size this sends
     * now. The fall-back floor that used to sit next to it is gone with the
     * ladder it belonged to; see the retry in mt6592_wifi_wmt_load_patch(). */
    WMT_PATCH_FRAGMENT_SIZE = 1000u,
    /* WMT_PATCH_FRAG_1ST/MID/LAST, wmt_ic_soc.c:41-43. */
    WMT_PATCH_FIRST         = 1u,
    WMT_PATCH_MIDDLE        = 2u,
    WMT_PATCH_LAST          = 3u,

    WMT_FRAME_BUFFER_SIZE = 1100u,
    WMT_EVENT_BUFFER_SIZE = 256u,

    BTIF_CLOCK_TIMEOUT_US      = 100000u,
    BTIF_IO_TIMEOUT_US         = 2000000u,
    WMT_COMMAND_TIMEOUT_US     = 2000000u,
    WMT_CALIBRATION_TIMEOUT_US = 10000000u,

    /* How long the link may stay silent before waiting for it stops being worth
     * the CPU. Below this the peer is simply answering and the poll loop is the
     * fastest way to catch the reply; above it the peer is not going to answer
     * inside this retransmission window at all, and every microsecond spent
     * spinning is a microsecond the rest of the machine does not get.
     *
     * That distinction is the whole of the freeze. This driver runs on the
     * firmware worker thread and the kernel is cooperative, so a context that
     * does not hand the CPU back does not lose it: one unanswered command is two
     * seconds in which the compositor does not composite and no guest gets a
     * turn, and bring-up issues a dozen of them before it gives up. */
    WMT_IDLE_YIELD_US = 2000u,
};

static mt6592_wifi_wmt_state g_state = {
    .status  = "MediaTek WMT ROM patches not loaded",
    .blocked = "mt6592-wifi:wmt-patches-not-loaded",
};

static uint8_t g_stp_frame[WMT_FRAME_BUFFER_SIZE];
static uint8_t g_wmt_command[WMT_FRAME_BUFFER_SIZE];
static uint8_t g_wmt_event[WMT_EVENT_BUFFER_SIZE];
/*
 * The three sequence counters, with stock's opening values (stp_core.c:327-331):
 *
 *     stp_core_ctx.sequence.txseq        = 0;
 *     stp_core_ctx.sequence.txack        = 7;
 *     stp_core_ctx.sequence.expected_rxseq = 0;
 *
 * The 7 is not a sentinel for "nothing received yet" that we chose -- it is
 * stock's own opening ack, and it is what the first frame we send carries in
 * its low three bits. Fields are three bits wide (MTKSTP_SEQ_SIZE 8,
 * stp_core.h:72), which is the & 7 everywhere below.
 */
static int g_stp_full_mode;
static uint8_t g_stp_tx_sequence;
static uint8_t g_stp_expected_rx_sequence;
static uint8_t g_stp_last_rx_sequence = 7u;

/* Link diagnostics. A ROM patch is ~100 fragments of 1011 bytes each, so a
 * single dropped byte anywhere in the transfer used to end the boot with no
 * indication of where it happened. */
static uint32_t g_stp_header_errors;
static uint32_t g_stp_length_errors;
static uint32_t g_stp_crc_errors;
static uint32_t g_stp_oversize_events;
static uint32_t g_stp_last_oversize_size;
static uint32_t g_stp_fw_messages;
static uint32_t g_stp_out_of_order;
static uint32_t g_stp_retransmits;
/* Header-only full-mode packets, and the acknowledge field of the last packet
 * the peer sent. That field is the only evidence the link layer gives that a
 * long frame arrived intact, so it separates "the peer rejected the frame" from
 * "the peer took the frame but produced no WMT event". */
static uint32_t g_stp_acks;
static uint32_t g_stp_last_ack = 0xffffffffu;
static uint32_t g_last_fragment_index;
static uint32_t g_last_fragment_count;
static uint32_t g_last_fragment_size;
/* The last WMT event the peer produced, and how many of them were the answer to
 * the command in flight but shorter than the caller said to expect.
 *
 * The minimums themselves are no longer in question. Every command this driver
 * sends and every size it expects was read back out of the stock kernel's own
 * tables, which sit in .rodata as command/event pairs -- ROMv1 patch address and
 * part address answer with the 8-byte 02 08 04 00 00 00 00 01, a patch fragment
 * with the 5-byte 02 01 01 00 00, the STP query with a 10-byte 02 04 06 ...,
 * reset, co-clock and antenna with 5, calibration and FM strap with 6 -- and all
 * nine agree with what is passed here. The command bytes agree too, down to the
 * antenna mode and co-clock flag that the stock driver patches in from
 * WMT_SOC.cfg.
 *
 * So these counters are a tripwire rather than an investigation. A minimum that
 * is one byte too large would not degrade anything, it would fail the command
 * outright and look exactly like a dead peer, so it is worth knowing for certain
 * that it never happens rather than inferring it from a table match. */
/* The size and opcode of the last event live in g_state rather than here, so a
 * caller that only gets a pass/fail out of the bootstrap can still tell a silent
 * peer from a talkative one answering the wrong thing. */
static uint32_t g_stp_short_events;
/* Frames that did not fit the transmit FIFO in one burst and had to be topped up
 * against THRE. Zero for the whole of bring-up until the first patch-address
 * command, which is precisely why it is worth counting: it separates "the frame
 * went out whole and the peer rejected it" from "the frame was assembled across
 * FIFO loads", and those two have never yet been told apart on this link. */
static uint32_t g_btif_tx_refills;

static uint32_t read32(uint32_t address)
{
    return *(volatile uint32_t*)(uintptr_t)address;
}

static void write32(uint32_t address, uint32_t value)
{
    *(volatile uint32_t*)(uintptr_t)address = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint8_t read8(uint32_t address)
{
    return *(volatile uint8_t*)(uintptr_t)address;
}

static void write8(uint32_t address, uint8_t value)
{
    *(volatile uint8_t*)(uintptr_t)address = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void delay_us(uint32_t microseconds)
{
    mt6592_delay_cycles(microseconds * (uint32_t)MT6592_DELAY_LEGACY_CYCLES_PER_US);
}

static void copy_bytes(uint8_t* dst, const uint8_t* src, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i)
    {
        dst[i] = src[i];
    }
}

/*
 * osal_crc16 (conn_soc/common/linux/pub/osal.c:278-289) is the table form of
 * this: crc starts at 0 and steps crc = (crc >> 8) ^ crc16_table[(crc ^ b) &
 * 0xff]. Its table opens 0x0000, 0xC0C1, 0xC181, 0x0140 (osal.c:55-56), which
 * is the reflected 0x8005 polynomial -- CRC-16/ARC, init 0, no final xor. The
 * byte-at-a-time loop below produces the same value one bit at a time; there is
 * no room here for a 512-byte table and no need for one at BTIF rates.
 */
static uint16_t stp_crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= byte;
    for (uint32_t bit = 0; bit < 8u; ++bit)
    {
        crc = (uint16_t)((crc >> 1) ^ ((crc & 1u) ? 0xa001u : 0u));
    }
    return crc;
}

static uint16_t stp_crc16(const uint8_t* data, uint32_t size)
{
    uint16_t crc = 0u;
    for (uint32_t i = 0; i < size; ++i)
    {
        crc = stp_crc16_update(crc, data[i]);
    }
    return crc;
}

static void set_failure(const char* status, const char* blocked)
{
    const int changed = g_state.blocked != blocked;
    g_state.status    = status;
    g_state.blocked   = blocked;
    if (changed)
    {
        mt6592_uart_puts("  wifi: ");
        mt6592_uart_puts(status ? status : "driver failure");
        mt6592_uart_puts(" [");
        mt6592_uart_puts(blocked ? blocked : "unknown");
        mt6592_uart_puts("]\n");
        mt6592_bootstatus_log_text("wifi: ");
        mt6592_bootstatus_log_text(status ? status : "driver failure");
        mt6592_bootstatus_log_text(" [");
        mt6592_bootstatus_log_text(blocked ? blocked : "unknown");
        mt6592_bootstatus_log_text("]\n");
    }
}

static void bootstatus_hex32(uint32_t value)
{
    static const char hex[] = "0123456789abcdef";
    char out[11];
    out[0] = '0';
    out[1] = 'x';
    for (uint32_t i = 0; i < 8u; ++i)
    {
        out[2u + i] = hex[(value >> ((7u - i) * 4u)) & 0x0fu];
    }
    out[10] = '\0';
    mt6592_bootstatus_log_text(out);
}

/* Emit the link counters on both sinks so a failed boot identifies the exact
 * fragment and the reason the STP link stopped making progress. */
static void trace_stp_state(const char* phase)
{
    static const struct
    {
        const char* label;
        const uint32_t* value;
    } fields[] = {
        {" frag=", &g_last_fragment_index},  {" of=", &g_last_fragment_count},
        {" fsz=", &g_last_fragment_size},    {" tx=", &g_state.tx_bytes},
        {" rx=", &g_state.rx_bytes},         {" lsr=", &g_state.last_btif_lsr},
        {" hdr_err=", &g_stp_header_errors}, {" len_err=", &g_stp_length_errors},
        {" crc_err=", &g_stp_crc_errors},    {" big=", &g_stp_oversize_events},
        {" bigsz=", &g_stp_last_oversize_size},
        {" fwmsg=", &g_stp_fw_messages},     {" ooo=", &g_stp_out_of_order},
        {" retx=", &g_stp_retransmits},      {" acks=", &g_stp_acks},
        {" txus=", &g_state.last_tx_us},     {" maxtxus=", &g_state.max_tx_us},
        {" evt=", &g_state.last_event_size}, {" evop=", &g_state.last_event_opcode},
        {" short=", &g_stp_short_events},    {" yield=", &g_wmt_yields},
        {" refill=", &g_btif_tx_refills},
    };

    mt6592_uart_puts("  wifi: ");
    mt6592_uart_puts(phase);
    mt6592_bootstatus_log_text("wifi: ");
    mt6592_bootstatus_log_text(phase);
    for (uint32_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
    {
        mt6592_uart_puts(fields[i].label);
        mt6592_uart_put_hex32(*fields[i].value);
        mt6592_bootstatus_log_text(fields[i].label);
        bootstatus_hex32(*fields[i].value);
    }
    mt6592_uart_puts(" seq=");
    mt6592_uart_put_hex32((uint32_t)g_stp_tx_sequence);
    mt6592_uart_puts(",");
    mt6592_uart_put_hex32((uint32_t)g_stp_expected_rx_sequence);
    mt6592_uart_puts(",");
    mt6592_uart_put_hex32(g_stp_last_ack);
    mt6592_uart_puts("\n");
    mt6592_bootstatus_log_text(" seq=");
    bootstatus_hex32((uint32_t)g_stp_tx_sequence);
    mt6592_bootstatus_log_text(",");
    bootstatus_hex32((uint32_t)g_stp_expected_rx_sequence);
    mt6592_bootstatus_log_text(",");
    bootstatus_hex32(g_stp_last_ack);
    mt6592_bootstatus_log_text("\n");
}

static int btif_clock_on(void)
{
    const uint64_t deadline = mt6592_timer_microseconds() + BTIF_CLOCK_TIMEOUT_US;

    write32(PERI_PDN0_CLR, PERI_BTIF_GATE);
    while (mt6592_timer_microseconds() < deadline)
    {
        if ((read32(PERI_PDN0_STA) & PERI_BTIF_GATE) == 0u)
        {
            return 0;
        }
    }
    set_failure("MediaTek BTIF clock did not ungate", "mt6592-wifi:wmt-btif-clock-timeout");
    return -1;
}

static void btif_clear_fifos(void)
{
    write32(BTIF_BASE + BTIF_FIFOCTRL, BTIF_FIFOCTRL_CLR_TX | BTIF_FIFOCTRL_CLR_RX);
    write32(BTIF_BASE + BTIF_FIFOCTRL, 0u);
}

static int btif_init(void)
{
    if (g_state.btif_ready)
    {
        return 0;
    }
    if (btif_clock_on() != 0)
    {
        return -1;
    }

    write32(BTIF_BASE + BTIF_FAKELCR, 0u);
    write32(BTIF_BASE + BTIF_HANDSHAKE, BTIF_HANDSHAKE_ENABLE);
    write32(BTIF_BASE + BTIF_TRI_LVL, BTIF_TX_THRESHOLD | (1u << 4));
    write32(BTIF_BASE + BTIF_SLEEP_EN, 0u);
    write32(BTIF_BASE + BTIF_DMA_EN, BTIF_DMA_AUTORESET);
    write32(BTIF_BASE + BTIF_IER, 0u); /* bounded polling; no GIC dependency */
    btif_clear_fifos();

    /* Match hal_btif_raise_wak_sig(): low for >1/32 kHz, then high. */
    write32(BTIF_BASE + BTIF_WAK, 0u);
    delay_us(80u);
    write32(BTIF_BASE + BTIF_WAK, 1u);
    delay_us(1000u);

    g_state.last_btif_lsr = read32(BTIF_BASE + BTIF_LSR);
    g_state.btif_ready    = 1;
    g_state.status        = "MediaTek WMT BTIF mandatory transport ready";
    g_state.blocked       = 0;
    mt6592_uart_puts("  wifi: WMT BTIF mandatory transport ready\n");
    return 0;
}

static int btif_write_all(const uint8_t* data, uint32_t size)
{
    const uint64_t start    = mt6592_timer_microseconds();
    const uint64_t deadline = start + BTIF_IO_TIMEOUT_US;
    uint32_t written        = 0u;

    while (written < size && mt6592_timer_microseconds() < deadline)
    {
        const uint32_t lsr    = read32(BTIF_BASE + BTIF_LSR);
        uint32_t room         = 0u;
        g_state.last_btif_lsr = lsr;
        if (lsr & BTIF_LSR_TEMT)
        {
            /* Transmitter idle, so the whole FIFO is free. This is the one
             * capacity claim in here that the device has already confirmed: the
             * longest frame the link has ever carried successfully is the
             * fifteen-byte full-STP switch, written in a single burst on TEMT,
             * so the FIFO is at least fifteen deep and a sixteen-byte burst is
             * within one byte of proven. */
            room = BTIF_TX_FIFO_SIZE;
        }
        else if (lsr & BTIF_LSR_THRE)
        {
            /* One byte, and deliberately not BTIF_TX_FIFO_SIZE - THRESHOLD.
             *
             * THRE says the transmit FIFO has room; how much room depends on
             * where the trigger level actually landed, and BTIF_TRI_LVL packs
             * the transmit and receive levels into two nibbles whose order this
             * driver has never verified against the hardware. btif_init() writes
             * BTIF_TX_THRESHOLD | (1 << 4). Read one way that is a transmit
             * trigger of 8 and THRE means eight free; read the other way it is a
             * transmit trigger of 1 and THRE means one free -- and on that
             * reading the old eight-byte burst overran the FIFO by seven bytes
             * every time, silently, with the driver counting all eight as sent.
             *
             * Which matters exactly here. Every WMT command in bring-up before
             * the patch download builds a frame that fits the FIFO whole and
             * goes out on TEMT alone: the STP query is 11 bytes, the full-mode
             * switch 15, an ACK 4. The patch-address command is 26 -- 4 header,
             * 20 payload, 2 CRC -- and is the first frame in the entire sequence
             * that has to touch the THRE path at all. It is also the first one
             * that fails, and it fails by timing out with the peer silent, which
             * is what a truncated frame looks like from this side.
             *
             * One byte per poll is what the stock PIO writer does and is correct
             * under every reading, since THRE cannot mean less than one free
             * slot. It costs an LSR read per byte and nothing else: the poll runs
             * far faster than the 3.25 Mbit wire, so the FIFO still sits at the
             * trigger level and the burst rate is unchanged. */
            room = 1u;
            if (written != 0u)
            {
                ++g_btif_tx_refills;
            }
        }
        if (room == 0u)
        {
            /* Poll rather than sleep. delay_us() runs a 64-read Strongly-Ordered
             * probe of the GPT before it waits at all, so a 10 us request costs
             * far more than 10 us and a 1011-byte fragment pays it ~60 times;
             * stretching one frame's bytes over that long is exactly what would
             * make a peer STP parser give up mid-packet. Stock's PIO loop spins
             * on the same LSR bits with no sleep. */
            continue;
        }
        if (room > size - written)
        {
            room = size - written;
        }
        for (uint32_t i = 0; i < room; ++i)
        {
            write8(BTIF_BASE + BTIF_THR, data[written++]);
        }
    }

    g_state.tx_bytes += written;
    /* Keep the worst frame seen: whether a big fragment takes microseconds or
     * hundreds of milliseconds decides between "the peer rejected the content"
     * and "the peer timed out waiting for the rest of it". */
    {
        const uint32_t elapsed = (uint32_t)(mt6592_timer_microseconds() - start);
        if (elapsed > g_state.max_tx_us)
        {
            g_state.max_tx_us = elapsed;
        }
        g_state.last_tx_us = elapsed;
    }
    if (written != size)
    {
        set_failure("MediaTek WMT BTIF transmit timed out", "mt6592-wifi:wmt-btif-tx-timeout");
        return -1;
    }
    return 0;
}

static int btif_read_byte(uint8_t* value, uint64_t deadline)
{
    while (mt6592_timer_microseconds() < deadline)
    {
        const uint32_t lsr    = read32(BTIF_BASE + BTIF_LSR);
        g_state.last_btif_lsr = lsr;
        if (lsr & BTIF_LSR_DR)
        {
            *value = read8(BTIF_BASE + BTIF_RBR);
            ++g_state.rx_bytes;
            return 0;
        }
        delay_us(10u);
    }
    return -1;
}

/* The first byte of a frame, and the only read in this driver allowed to hand
 * the CPU away.
 *
 * Every read after it is mid-frame: the peer is pushing the rest of a packet
 * into a sixteen-byte FIFO, and a cooperative yield is not a pause but however
 * long the compositor and every runnable guest take -- milliseconds, against a
 * FIFO that holds sixteen bytes. Before the first byte there is no frame in
 * flight to lose, and that is what makes this read different rather than merely
 * the convenient place to put a yield.
 *
 * Even here it waits out WMT_IDLE_YIELD_US of silence first, so a peer that
 * answers promptly -- which is every healthy exchange -- is still caught by the
 * poll loop and pays nothing for any of this. What the yield covers is the other
 * case: a retransmission window in which no answer is coming, where the choice
 * is between spinning for 180 ms and letting the machine run. */
static int btif_read_byte_between_frames(uint8_t* value, uint64_t deadline)
{
    const uint64_t start = mt6592_timer_microseconds();
    uint64_t now         = start;

    while (now < deadline)
    {
        const uint32_t lsr    = read32(BTIF_BASE + BTIF_LSR);
        g_state.last_btif_lsr = lsr;
        if (lsr & BTIF_LSR_DR)
        {
            *value = read8(BTIF_BASE + BTIF_RBR);
            ++g_state.rx_bytes;
            return 0;
        }
        /* delay_us() is the fallback and not the alternative: a build with no
         * scheduler linked in must still pace the poll rather than spin flat
         * out on the LSR. */
        if (now - start < (uint64_t)WMT_IDLE_YIELD_US || !wmt_cooperative_yield())
        {
            delay_us(10u);
        }
        now = mt6592_timer_microseconds();
    }
    return -1;
}

/*
 * stp_send_ack (stp_core.c:807-846), which is a bare four-byte header and
 * nothing else -- no payload, no CRC, and the sequence field deliberately 0:
 *
 *     mtkstp_header[0] = 0x80 + (0 << 3) + txAck;
 *     mtkstp_header[1] = 0x00;    // disable NAK
 *     mtkstp_header[2] = 0;
 *     mtkstp_header[3] = (h0 + h1 + h2) & 0xff;
 *     (*sys_if_tx)(&mtkstp_header[0], MTKSTP_HEADER_SIZE, &ret);
 *
 * The zero in header[1] is not a placeholder: fgEnableNak is 0 (stp_core.c:27)
 * and the receiver reads bit 7 as the NAK flag only when it is not.
 */
static int stp_send_ack(void)
{
    uint8_t header[STP_HEADER_SIZE];
    header[0] = (uint8_t)(0x80u | g_stp_last_rx_sequence);
    header[1] = 0u;
    header[2] = 0u;
    header[3] = (uint8_t)(header[0] + header[1] + header[2]);
    return btif_write_all(header, sizeof(header));
}

/* Stock stp_do_tx_timeout() prefixes every retransmission with four 0x7f bytes
 * -- osal_memset(&resync[0], 127, 4) followed by the resend loop, stp_core.c:
 * 408-428. The peer parser walks MTKSTP_RESYNC1..4 on that pattern ("RESYNC
 * must be 4 _continuous_ 0x7f", stp_core.c:1979) and drops whatever partial
 * packet it was mid-way through, which is the only way to recover a link that
 * lost bytes in the middle of a 1011-byte fragment. */
static int stp_send_resync(void)
{
    static const uint8_t pattern[STP_RESYNC_SIZE] = {0x7fu, 0x7fu, 0x7fu, 0x7fu};
    return btif_write_all(pattern, sizeof(pattern));
}

static int stp_discard_bytes(uint32_t count, uint64_t deadline)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        uint8_t byte;
        if (btif_read_byte(&byte, deadline) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int stp_receive_wmt(uint8_t* payload, uint32_t capacity, uint32_t* payload_size, uint64_t deadline)
{
    while (mt6592_timer_microseconds() < deadline)
    {
        uint8_t header[STP_HEADER_SIZE];
        /* Between frames until this byte lands, mid-frame from the next one on. */
        if (btif_read_byte_between_frames(&header[0], deadline) != 0)
        {
            return -1;
        }
        if ((header[0] & 0xc0u) != 0x80u)
        {
            /* Byte hunt. Resync padding (0x7f), delimiters (0x55) and debris
             * left over from a mis-framed packet are all skipped here. */
            continue;
        }
        for (uint32_t i = 1; i < STP_HEADER_SIZE; ++i)
        {
            if (btif_read_byte(&header[i], deadline) != 0)
            {
                return -1;
            }
        }

        const uint32_t type    = (header[1] >> 4) & 0x7u;
        const uint32_t size    = (((uint32_t)header[1] & 0x0fu) << 8) | header[2];
        const uint8_t sequence = (header[0] >> 3) & 0x7u;

        /* The stock core never aborts on a malformed packet: it resynchronises
         * and lets the retransmission timer recover the exchange. Returning an
         * error here turned one corrupted byte into a failed boot. */
        if (size > STP_MAX_PAYLOAD)
        {
            ++g_stp_length_errors;
            continue;
        }

        /* Firmware log and assert packets bypass the checksum and CRC checks in
         * MTKSTP_FW_MSG, are never acknowledged, and do not consume a receive
         * sequence number. Treating them as ordinary data packets advanced the
         * expected sequence by one, after which every real WMT event looked out
         * of order and the patch download stalled until the command timed out. */
        /*
         * THE TRAILER IS A JUDGEMENT CALL, so it is on the record. The two
         * stock parsers disagree with each other about it: the SDIO one
         * discards a CRC byte here and says "we will discard another CRC on the
         * outer switch procedure" (stp_core.c:1930-1938), while the BTIF/UART
         * one -- the parser for the link we are on -- copies exactly
         * parser.length bytes and returns straight to MTKSTP_SYNC with no
         * trailer consumed at all (stp_core.c:2318-2326). Every packet the
         * sender builds carries two trailing bytes in both modes, so this reads
         * them; stock's BTIF path leaves them to its sync hunt instead, where a
         * CRC byte with bit 7 set can be mistaken for the next header. Neither
         * choice is lossless if the guess is wrong -- ours would eat two bytes
         * of the following packet -- so g_stp_fw_messages is the counter to
         * look at first if a WMT event ever goes missing right after one.
         */
        if (type == STP_FW_LOG_TASK || type == STP_INFO_TASK)
        {
            ++g_stp_fw_messages;
            if (stp_discard_bytes(size + STP_CRC_SIZE, deadline) != 0)
            {
                return -1;
            }
            continue;
        }

        if (g_stp_full_mode && (uint8_t)(header[0] + header[1] + header[2]) != header[3])
        {
            ++g_stp_header_errors;
            continue;
        }

        if (g_stp_full_mode)
        {
            g_stp_last_ack = (uint32_t)(header[0] & 0x07u);
        }

        /* Full-mode acknowledge and negative-acknowledge packets contain only
         * the four-byte header; the payload-carrying flow resumes below. */
        if (g_stp_full_mode && size == 0u)
        {
            ++g_stp_acks;
            continue;
        }

        uint16_t computed_crc = 0u;
        for (uint32_t i = 0; i < size; ++i)
        {
            uint8_t byte;
            if (btif_read_byte(&byte, deadline) != 0)
            {
                return -1;
            }
            computed_crc = stp_crc16_update(computed_crc, byte);
            if (i < capacity)
            {
                payload[i] = byte;
            }
        }

        uint8_t crc_low  = 0u;
        uint8_t crc_high = 0u;
        if (btif_read_byte(&crc_low, deadline) != 0 || btif_read_byte(&crc_high, deadline) != 0)
        {
            return -1;
        }
        if (g_stp_full_mode)
        {
            const uint16_t received_crc = (uint16_t)((uint16_t)crc_low | ((uint16_t)crc_high << 8));
            if (computed_crc != received_crc)
            {
                /* Neither acknowledge nor advance: the peer retransmits on its
                 * own timer once our ACK fails to arrive. */
                ++g_stp_crc_errors;
                continue;
            }

            /* Full STP has one shared receive sequence across every task, not
             * one sequence per WMT channel. Patch download can provoke INFO or
             * other side-band packets before the five-byte WMT response. The
             * old parser discarded those packets before advancing/ACKing the
             * sequence, so the following WMT event looked out of order and the
             * first ROM-patch fragment timed out. Validate and ACK every data
             * packet first, then filter by task exactly like stp_process_packet
             * in the stock conn_soc stack. */
            if (sequence != g_stp_expected_rx_sequence)
            {
                ++g_stp_out_of_order;
                (void)stp_send_ack();
                continue;
            }
            g_stp_last_rx_sequence     = sequence;
            g_stp_expected_rx_sequence = (uint8_t)((sequence + 1u) & 0x7u);
            if (stp_send_ack() != 0)
            {
                return -1;
            }
        }

        if (type != STP_WMT_TASK)
        {
            continue;
        }
        /*
         * An event longer than the buffer is TRUNCATED, NOT DROPPED, and that is
         * a correctness fix rather than a convenience.
         *
         * Stock does not frame WMT events as packets at all. Each task owns a
         * byte-stream ring, and wmt_core_rx(pBuf, bufLen, &readSize)
         * (wmt_core.c:319-336) lifts up to bufLen bytes out of it and reports
         * how many it got; anything beyond that stays queued, which is the whole
         * reason wmt_core_rx_flush (wmt_core.c:338-352) has to exist. So the
         * stock caller for RF calibration asks for six bytes
         *
         *     WMT_CORE_START_RF_CALIBRATION_EVT[] = {0x2,0x14,0x02,0x00,0x00,0x01}
         *     (wmt_ic_soc.c:178)
         *     iRet = wmt_core_rx(evtBuf, script[i].evtSz, &u4Res);
         *     if (iRet || (u4Res != script[i].evtSz)) break;
         *     if (0x14 != evtBuf[1]) // workaround RF calibration data EVT,
         *         ... memcmp ...     // do not care this EVT
         *     (wmt_core.c:613-635)
         *
         * and is structurally incapable of noticing that more arrived. Its
         * evtBuf is UCHAR[256] -- the same 256 we use -- yet the read length is
         * six, so the buffer size was never the limit on the event size.
         *
         * The chip answers this one command with the calibration result table,
         * which is why stock exempts opcode 0x14 from its content check by name.
         * On real J36 Ultra silicon that event exceeded 256 bytes, and dropping
         * it here was the whole failure: the header checksum, the payload CRC16
         * and the sequence have all been validated and acknowledged by the time
         * we get to this line, so this is a known-good event being thrown away
         * for being too informative. wmt_exchange only ever reads bytes 0, 1 and
         * 4 of it, all of which are present in the first capacity bytes that
         * :707 has already copied.
         *
         * Unlike stock we consume the remainder off the wire above instead of
         * leaving it queued, so there is no residue for a later read to trip
         * over and nothing to flush. Record the wire length -- a truncation is
         * still worth seeing in the trace, just not worth failing the boot over.
         */
        if (size > capacity)
        {
            ++g_stp_oversize_events;
            g_stp_last_oversize_size = size;
            *payload_size = capacity;
            return 0;
        }
        *payload_size = size;
        return 0;
    }
    return -1;
}

/* A frame the peer never acknowledged was never accepted, so the sequence it
 * carried is still free. Handing it back matters: the stock parser drops every
 * out-of-order packet, so leaving the counter advanced after one stalled
 * command made every later command unanswerable and the link permanently dead. */
static void wmt_release_tx_sequence(uint8_t sent_sequence)
{
    if (g_stp_full_mode && g_stp_last_ack != (uint32_t)sent_sequence)
    {
        g_stp_tx_sequence = sent_sequence;
    }
}

static int wmt_exchange(const uint8_t* command, uint32_t command_size, uint32_t minimum_event_size, uint32_t timeout_us)
{
    const uint32_t frame_size   = STP_HEADER_SIZE + command_size + STP_CRC_SIZE;
    const uint64_t deadline     = mt6592_timer_microseconds() + timeout_us;
    const uint8_t sent_sequence = g_stp_tx_sequence;

    if (command_size == 0u || command_size > STP_MAX_PAYLOAD || frame_size > sizeof(g_stp_frame))
    {
        return -1;
    }

    /* One guaranteed hand-back per command, taken here rather than left to the
     * receive path. The idle yield in btif_read_byte_between_frames() only fires
     * once the link has gone quiet for a while, so a run of commands the peer
     * answers immediately -- the whole of a healthy patch download, a hundred
     * fragments of it -- would otherwise never reach a yield point at all and
     * would hold the CPU for the entire transfer. Between two commands is always
     * between frames, so this is free of the mid-frame hazard. */
    (void)wmt_cooperative_yield();

    /*
     * The STP frame, both modes, from stp_send_data_no_ps
     * (conn_soc/common/core/stp_core.c:882-885 mandatory, 928-931 fullset):
     *
     *   fullset:    h0 = 0x80 + (txseq << 3) + txack
     *               h1 = (type << 4) + ((length & 0xf00) >> 8)
     *               h2 = length & 0xff
     *               h3 = (h0 + h1 + h2) & 0xff        -- a sum, not a CRC
     *               payload, then osal_crc16(payload) low byte first
     *   mandatory:  h0 = 0x80, h1/h2 as above, h3 = 0x00,
     *               payload, then two zero bytes where the CRC would go
     *
     * -- which is what the two ternaries below are. The trailer is two bytes in
     * either mode, so frame_size does not branch.
     */
    g_stp_frame[0] = g_stp_full_mode ? (uint8_t)(0x80u | (sent_sequence << 3) | g_stp_last_rx_sequence) : 0x80u;
    g_stp_frame[1] = (uint8_t)((STP_WMT_TASK << 4) | ((command_size >> 8) & 0x0fu));
    g_stp_frame[2] = (uint8_t)command_size;
    g_stp_frame[3] = g_stp_full_mode ? (uint8_t)(g_stp_frame[0] + g_stp_frame[1] + g_stp_frame[2]) : 0u;
    copy_bytes(g_stp_frame + STP_HEADER_SIZE, command, command_size);
    const uint16_t crc                               = g_stp_full_mode ? stp_crc16(command, command_size) : 0u;
    g_stp_frame[STP_HEADER_SIZE + command_size]      = (uint8_t)crc;
    g_stp_frame[STP_HEADER_SIZE + command_size + 1u] = (uint8_t)(crc >> 8);

    if (btif_write_all(g_stp_frame, frame_size) != 0)
    {
        return -1;
    }
    /* The stock STP core consumes a transmit sequence when the packet is
     * queued, not when a matching WMT event happens to arrive. Keep the same
     * rule so intervening side-band packets cannot leave the two peers using
     * different sequence numbers. Retransmissions reuse the queued bytes, so
     * the sequence is consumed exactly once per command. */
    if (g_stp_full_mode)
    {
        g_stp_tx_sequence = (uint8_t)((sent_sequence + 1u) & 0x7u);
        g_stp_last_ack    = 0xffffffffu;
    }

    /* Bound every wait by the stock 180 ms retransmission timer and replay the
     * queued frame after a resync pattern, up to MTKSTP_RETRY_LIMIT times. A
     * ROM patch is around a hundred 1011-byte fragments; without this a single
     * byte lost on the BTIF link ended the whole Wi-Fi bring-up. */
    for (uint32_t attempts = 0;;)
    {
        uint64_t window = deadline;
        if (g_stp_full_mode)
        {
            window = mt6592_timer_microseconds() + STP_TX_TIMEOUT_US;
            if (window > deadline)
            {
                window = deadline;
            }
        }

        while (mt6592_timer_microseconds() < window)
        {
            uint32_t event_size = 0u;
            if (stp_receive_wmt(g_wmt_event, sizeof(g_wmt_event), &event_size, window) != 0)
            {
                break;
            }
            g_state.last_event_size   = event_size;
            g_state.last_event_opcode = event_size >= 2u ? (uint32_t)g_wmt_event[1] : 0xffffffffu;
            if (event_size < 5u || g_wmt_event[0] != 0x02u || g_wmt_event[1] != command[1])
            {
                continue;
            }
            /* Length is recorded from here on, not enforced.
             *
             * A WMT event is 0x02, opcode, length low, length high, then the
             * payload, and byte four is the status for every one of them; those
             * five bytes are the entire contract this function relies on. The
             * per-command minimum on top of that is real -- it was checked
             * against the stock kernel's own command/event tables, see
             * g_stp_short_events -- but it is not load-bearing, and enforcing it
             * would make a peer that answers one byte short indistinguishable
             * from a peer that does not answer at all. Take the event, count the
             * disagreement, and let the trace report the size the peer really
             * sends if the two ever part company. */
            if (event_size < minimum_event_size)
            {
                ++g_stp_short_events;
            }
            if (g_wmt_event[4] == 0u)
            {
                return 0;
            }
            set_failure("MediaTek WMT command returned a failure status", "mt6592-wifi:wmt-command-status-failed");
            return -1;
        }

        if (!g_stp_full_mode || ++attempts > STP_RETRY_LIMIT || mt6592_timer_microseconds() >= deadline)
        {
            break;
        }
        ++g_stp_retransmits;
        /* A whole retransmission window went by with no answer, so the machine
         * has just been starved for up to 180 ms and is about to be starved for
         * another. Between frames in both directions here: nothing is in flight
         * inbound, and the resync pattern has not been sent yet. */
        (void)wmt_cooperative_yield();
        if (stp_send_resync() != 0 || btif_write_all(g_stp_frame, frame_size) != 0)
        {
            wmt_release_tx_sequence(sent_sequence);
            return -1;
        }
    }

    wmt_release_tx_sequence(sent_sequence);
    set_failure("MediaTek WMT command response timed out or failed", "mt6592-wifi:wmt-command-response-failed");
    return -1;
}

/*
 * One complete pass over the patch body at the given fragment size. Every
 * fragment is a separate WMT command, so the size only has to agree with itself
 * across the pass: the first fragment is flagged first, the last is flagged
 * last, and the peer reassembles from the flags rather than from a count.
 *
 * The frame is stock's, byte for byte -- WMT_PATCH_CMD[] is declared
 * {0x01, 0x01, 0x00, 0x00, 0x00} (wmt_ic_soc.c:103) and the send loop
 * (wmt_ic_soc.c:1770-1786) patches two of those five in place:
 *
 *     WMT_PATCH_CMD[4] = last ? WMT_PATCH_FRAG_LAST
 *                             : (fragSeq == 0 ? WMT_PATCH_FRAG_1ST : ..._MID);
 *     cmdLen = 1 + fragSize;             // the flag byte plus the fragment
 *     osal_memcpy(&WMT_PATCH_CMD[2], &cmdLen, 2);
 *
 * and the answer it waits for is WMT_PATCH_EVT[] = {0x02, 0x01, 0x01, 0x00,
 * 0x00} (wmt_ic_soc.c:104), five bytes -- which is the minimum passed below.
 * "Last" is tested before "first", so a one-fragment patch is flagged LAST,
 * and that is the order here too.
 */
static int wmt_download_patch_body(const uint8_t* body, uint32_t body_size, uint32_t fragment_bytes)
{
    const uint32_t fragment_count = (body_size + fragment_bytes - 1u) / fragment_bytes;

    g_last_fragment_count = fragment_count;
    g_last_fragment_size  = fragment_bytes;
    for (uint32_t fragment = 0; fragment < fragment_count; ++fragment)
    {
        const uint32_t offset  = fragment * fragment_bytes;
        g_last_fragment_index  = fragment;
        uint32_t fragment_size = body_size - offset;
        if (fragment_size > fragment_bytes)
        {
            fragment_size = fragment_bytes;
        }

        const uint16_t command_payload_size = (uint16_t)(fragment_size + 1u);
        g_wmt_command[0]                    = 0x01u;
        g_wmt_command[1]                    = 0x01u;
        g_wmt_command[2]                    = (uint8_t)command_payload_size;
        g_wmt_command[3]                    = (uint8_t)(command_payload_size >> 8);
        g_wmt_command[4] =
            fragment + 1u == fragment_count ? WMT_PATCH_LAST : (fragment == 0u ? WMT_PATCH_FIRST : WMT_PATCH_MIDDLE);
        copy_bytes(g_wmt_command + 5u, body + offset, fragment_size);
        if (wmt_exchange(g_wmt_command, fragment_size + 5u, 5u, WMT_COMMAND_TIMEOUT_US) != 0)
        {
            return -1;
        }
        g_state.patch_bytes += fragment_size;
        /* No yield here any more: wmt_exchange() takes one per command, which is
         * the same rate this was and covers the address commands too. */
        if ((fragment & 0x0fu) == 0u)
        {
            mt6592_pmic_charger_service();
        }
    }
    return 0;
}

static int patch_bit_count(uint8_t value)
{
    int count = 0;
    while (value)
    {
        count += value & 1u;
        value >>= 1;
    }
    return count;
}

/*
 * WMT opcode 0x08: read-modify-write one connectivity-side register.
 *
 * The frame is `01 08 10 00 | 01 01 00 01 | addr | value | mask` little-endian
 * throughout, and the peer answers with the 8-byte 02 08 04 00 00 00 00 01. The
 * semantics are new = (old & ~mask) | (value & mask), so clearing a field means
 * value 0 with the field's bits set in the mask -- not value 0 with mask
 * 0xffffffff, which would clear the whole register.
 *
 * The two patch-address commands are this same opcode with the fields baked in;
 * they are left as literal byte arrays because that is the form they were read
 * out of the stock kernel in and the form they can still be compared against it
 * in.
 */
static int wmt_write_register(uint32_t address, uint32_t value, uint32_t mask)
{
    uint8_t command[20] = {0x01u, 0x08u, 0x10u, 0x00u, 0x01u, 0x01u, 0x00u, 0x01u};
    uint32_t i;

    for (i = 0u; i < 4u; ++i)
    {
        command[8u + i]  = (uint8_t)(address >> (8u * i));
        command[12u + i] = (uint8_t)(value >> (8u * i));
        command[16u + i] = (uint8_t)(mask >> (8u * i));
    }
    return wmt_exchange(command, sizeof(command), 8u, WMT_COMMAND_TIMEOUT_US);
}

/*
 * Power up the connectivity MCU's data local memory before anything is written
 * into it.
 *
 * This is the first thing stock's wmt_ic_soc_pwr_on() does -- before it even
 * asks how many patches there are -- and it is three separate opcode-0x08
 * commands rather than one, each clearing a different field of 0x80100060:
 * bits [11:8], then [7:4], then bit 3, in that order. The stock strings name
 * them "power on dlm cmd1/2/3", and the staging is deliberate; a memory power
 * sequence that could be done in one write would have been.
 *
 * Leaving it out is invisible during the download. Every fragment still
 * acknowledges, because the acknowledgement comes from the MCU's STP parser and
 * not from the memory the payload lands in, and the reset that follows a partial
 * patch set acknowledges too. What fails is the reset that completes the set --
 * the one where the MCU actually branches into the patched image -- and it fails
 * by going silent, which is what an MCU executing unpowered memory looks like
 * from the host side.
 */
static int wmt_power_on_dlm(void)
{
    static const uint32_t masks[] = {0x00000f00u, 0x000000f0u, 0x00000008u};
    uint32_t i;

    for (i = 0u; i < sizeof(masks) / sizeof(masks[0]); ++i)
    {
        if (wmt_write_register(0x80100060u, 0u, masks[i]) != 0)
        {
            trace_stp_state("dlm power-on failed");
            set_failure("MediaTek WMT could not power on the connectivity MCU patch memory",
                        "mt6592-wifi:wmt-dlm-power-on-failed");
            return -1;
        }
    }
    return 0;
}

/*
 * The connectivity MCU clock window stock holds open across the whole download.
 *
 * `fast` raises the MCU to 138.67 MHz (stock: "enable set mcu clk" then "set mcu
 * clk to 138.67MH"); the matching call with fast == 0 puts it back to 26 MHz and
 * drops the override again. Note where the boundaries fall in stock: the window
 * opens before the first patch and closes *after* the reset that activates the
 * last one, so the patched firmware takes its first instructions at 138.67 MHz
 * and is only slowed down once it is running.
 *
 * Order matters in both directions and is not symmetric. Going up, the override
 * enable at 0x80000334 comes first so that the divider write at 0x8000010c takes
 * effect; coming down, the divider is restored first and the override dropped
 * last, so the register is never left holding a stale value with the override
 * still active.
 *
 * Failure here is reported but not fatal. The clock is a performance setting for
 * a transfer that has already been observed to complete at the reset default, so
 * refusing to boot the radio over it would trade a working radio for a tidier
 * invariant.
 */
static void wmt_set_mcu_clock(int fast)
{
    int rc;

    if (fast)
    {
        rc = wmt_write_register(0x80000334u, 0x00010000u, 0xffffffffu);
        if (rc == 0) rc = wmt_write_register(0x8000010cu, 0x00844d59u, 0xffffffffu);
    }
    else
    {
        rc = wmt_write_register(0x8000010cu, 0x00844d00u, 0xffffffffu);
        if (rc == 0) rc = wmt_write_register(0x80000334u, 0x00000000u, 0xffffffffu);
    }
    if (rc != 0)
    {
        trace_stp_state(fast ? "mcu clock raise failed" : "mcu clock restore failed");
        mt6592_uart_puts(fast ? "  wifi: WMT could not raise the connectivity MCU clock\n"
                              : "  wifi: WMT could not restore the connectivity MCU clock\n");
    }
}

/*
 * Whether this connectivity chip gets the LTE-coexistence preamble before the
 * shared RF calibration.
 *
 * THIS LIST USED TO BE CALLED chip_skips_rf_calibration() AND THAT WAS WRONG.
 * The reading was that mtk_wcn_soc_patch_dwn() compares the chip id against
 * these six values immediately before issuing the calibration frame and, on a
 * match, branches past it. The branch is real -- 0xc039c2d4..0xc039c314, and the
 * `sub r1, r1, #448` off 0x6752 that yields 0x6592 is what put MT6592 on the
 * list -- but it does not branch past the calibration. It branches to
 * 0xc039c500, which is a DETOUR: a wmt_core_ctrl(11) query of the platform's
 * LTE-coex configuration, the LTE filter/frequency tables it gates, and the
 * antenna-select frame. Every one of those paths then falls into 0xc039c318 --
 * `bne 0xc039c318` at 0xc039c598 for a chip that is not 0x6582/0x6592, `beq
 * 0xc039c318` at 0xc039c5f0 once the antsel frame is acknowledged -- and
 * 0xc039c318 is the calibration block, straight-line, no gate on the frame.
 *
 * So stock sends `01 14 01 00 01` on MT6592 like everything else. What the list
 * actually decides is what is sent FIRST. Chips off the list calibrate
 * immediately; chips on it configure LTE coexistence and the antenna select
 * first, and only then calibrate.
 *
 * That matters because of the measurement that produced the wrong name. Sending
 * the calibration frame here really did hang: the peer never answered and the
 * bootstrap gave up ten seconds later with both patches accepted and the reset
 * acknowledged. That observation stands. The conclusion drawn from it -- "this
 * part does not implement the command" -- does not, because the frame was being
 * sent out of order, with none of the four frames stock puts in front of it.
 * A command that is ignored when its preamble is missing looks exactly like a
 * command the silicon does not have.
 *
 * The two masked comparisons are the stock ones and pair adjacent ids
 * (0x6580/0x6582 and 0x0335/0x0337) the way the original does.
 */
static int chip_needs_lte_coex_preamble(uint32_t chip_id)
{
    const uint32_t paired = chip_id & ~2u;

    return chip_id == 0x8163u || paired == 0x6580u || chip_id == 0x6752u || chip_id == 0x6592u ||
           chip_id == 0x0321u || paired == 0x0335u;
}

/*
 * A REJECTED HYPOTHESIS, kept because it is a good argument and the next reader
 * will otherwise re-derive it and break the radio again.
 *
 * The other half of stock's gate is wmt_core_ctrl(11)'s buf[0], and the argument
 * ran: buf[0] == 0 means the board has no LTE modem to coexist with, and on that
 * path stock sends none of the four preamble frames. The J36 Ultra has no modem
 * -- MT6592_Android_scatter.txt lists PRELOADER, MBR, EBR1, PRO_INFO, NVRAM,
 * PROTECT_F, PROTECT_S, SECCFG, UBOOT, BOOTIMG, RECOVERY, SEC_RO, MISC, LOGO,
 * EBR2, FRP, EXPDB, ANDROID, CACHE, USRDATA, BMTPOOL and stops; no MD1IMG, no
 * modem NVRAM, no protocol partition, and the build is full_sf6592_wet_l. One of
 * the four frames, is_lte_project, carries payload 0x01, so we were asserting
 * "this IS an LTE project" to a chip in a device with no modem. All true.
 *
 * It is still wrong, and the board says so. Sending the preamble unconditionally
 * for chip 0x6592 is what this driver did up to commit b5403d48f1, which scanned
 * and associated to open networks. Gating it off is the change that arrived with
 * "Try fix calibration" (0f8d846483), and from there the WLAN firmware has never
 * asserted WLAN_READY. It is the only behavioural difference in the whole WMT
 * layer between that working build and now -- the frame tables are identical,
 * and the STP oversize path that also changed has never fired (big=0 in every
 * trace).
 *
 * The likely reconciliation is that these frames do more than announce a modem:
 * they are the antenna-select and coexistence-filter setup that the RF
 * calibration immediately after them depends on, so skipping them leaves the
 * front end unconfigured whatever buf[0] would have said. Until someone can read
 * the real buf[0] off this board rather than infer it, the hardware's answer
 * outranks the inference.
 */

/*
 * The four frames stock issues between the MCU-clock restore and the shared RF
 * calibration, transcribed byte for byte from the table at 0xc0b6cfac.
 *
 * Stock reaches them through wmt_core_init_script() entries at 0xc0b6d0b0,
 * 0xc0b6d0c4, 0xc0b6d0d8 and 0xc0b6d0ec, and gates them on a host-side query,
 * wmt_core_ctrl(11), that returns the board's LTE-coexistence configuration:
 *
 *   - buf[0] == 0  -> none of this is sent at all, and the coex frame later is
 *                     skipped too
 *   - buf[4] != 0  -> the two big tables are skipped, "is LTE project" is not
 *   - chip in {0x6580, 0x6752, 0x8163, 0x0321, 0x0335} -> the NON-default
 *                     tables at 0xc0b6d088/0xc0b6d09c are sent instead
 *
 * MT6592 is not in that inner list, so it gets the default tables, which is
 * what is below. buf itself is a platform config blob we do not have and cannot
 * synthesise, so the two data-dependent gates are simply not reproduced: we
 * send the defaults unconditionally and say so in the log. If the board turns
 * out to have no LTE modem at all then stock sends nothing here and this is
 * four extra frames -- which is a thing worth knowing, and the chip's own
 * status byte will say it.
 *
 * The last frame's payload byte is patched at runtime by stock from
 * wmt_core_ctrl(28) -- the TDM request antenna-select count -- into
 * 0xc0b6d0f1. Its static value is 0x00 and 0x00 is what we send, because a
 * count we have not measured is not better than the one the table ships with.
 *
 * None of these are fatal. They configure a coexistence filter for a radio we
 * are not using; a chip that rejects one has still told us more than a
 * bootstrap that refuses to continue.
 *
 * These four are the only WMT frames in this file with no counterpart in the
 * vendor tree: conn_soc/common/core/wmt_ic_soc.c has no LTE tables at all, and
 * its whole coex section is one COEX_WMT command plus four more behind
 * CFG_SUBSYS_COEX_NEED (0). That revision simply predates them -- subcommands
 * 0x11, 0x12, 0x14 and 0x15 of opcode 0x10 do not exist in it. So the shipped
 * kernel's .rodata is the only ground truth here, which is exactly the case the
 * shipped-kernel-wins rule is for, and it is why the offsets above are quoted
 * rather than a file and line.
 */
static void wmt_send_lte_coex_preamble(void)
{
    static const uint8_t lte_coex_filter[] = {
        0x01u, 0x10u, 0x45u, 0x00u, 0x11u, 0x00u, 0x00u, 0x01u, 0x00u, 0x16u, 0x16u, 0x16u, 0x16u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x63u, 0x63u, 0x63u, 0x63u, 0x3cu, 0x3cu, 0x3cu, 0x3cu, 0x04u,
        0x04u, 0x04u, 0x04u, 0x01u, 0x01u, 0x01u, 0x01u, 0x0eu, 0x0eu, 0x0eu, 0x0eu, 0x0bu, 0x0bu,
        0x0bu, 0x0bu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    static const uint8_t lte_freq_idx[] = {
        0x01u, 0x10u, 0x21u, 0x00u, 0x12u, 0xfcu, 0x08u, 0x15u, 0x09u, 0x2eu, 0x09u, 0x47u, 0x09u,
        0xc4u, 0x09u, 0xddu, 0x09u, 0xf6u, 0x09u, 0x0fu, 0x0au, 0x14u, 0x09u, 0x2du, 0x09u, 0x46u,
        0x09u, 0x5fu, 0x09u, 0xddu, 0x09u, 0xf5u, 0x09u, 0x0du, 0x0au, 0x27u, 0x0au};
    static const uint8_t is_lte_project[] = {0x01u, 0x10u, 0x02u, 0x00u, 0x15u, 0x01u};
    static const uint8_t tdm_antsel[]     = {0x01u, 0x10u, 0x02u, 0x00u, 0x14u, 0x00u};

    static const struct
    {
        const uint8_t* frame;
        uint32_t       size;
        const char*    name;
    } script[] = {
        {lte_coex_filter, (uint32_t)sizeof(lte_coex_filter), "lte coex filter spec"},
        {lte_freq_idx, (uint32_t)sizeof(lte_freq_idx), "lte freq idx"},
        {is_lte_project, (uint32_t)sizeof(is_lte_project), "is lte project"},
        {tdm_antsel, (uint32_t)sizeof(tdm_antsel), "tdm req antsel num"},
    };

    for (uint32_t i = 0u; i < (uint32_t)(sizeof(script) / sizeof(script[0])); ++i)
    {
        const int rc = wmt_exchange(script[i].frame, script[i].size, 5u, WMT_COMMAND_TIMEOUT_US);

        mt6592_uart_puts(rc == 0 ? "  wifi: WMT " : "  wifi: WMT (no answer) ");
        mt6592_uart_puts(script[i].name);
        mt6592_uart_puts("\n");
    }
}

/*
 * Whether the connectivity MCU was still answering when the patch set was
 * activated. Set once per bootstrap by wmt_finalize().
 *
 * This exists because of how the failure it names presents. Everything after the
 * final patch reset is a configuration command, and if the MCU is not running
 * then the first of those commands is what times out and gets blamed -- so the
 * reported fault moves whenever the first command changes. It has already been
 * "shared RF calibration failed" and then "coexistence setup failed" for the same
 * dead MCU, purely because a chip-id gate was added in front of one of them.
 * Distinguishing the two costs one read-only command and stops the diagnosis
 * from depending on the order of the ones that follow.
 */
static int g_peer_answered_after_reset = 1;

static int wmt_run_rf_calibration(void)
{
    /* WMT_CORE_START_RF_CALIBRATION_CMD (wmt_ic_soc.c:177), the sole entry of
     * calibration_table (:482-485). Its expected event is
     * WMT_CORE_START_RF_CALIBRATION_EVT (:178) = {0x02,0x14,0x02,0x00,0x00,0x01},
     * six bytes -- which is why the exchange below waits for 6 and not the 5 that
     * most of the other events run to. */
    static const uint8_t calibration[] = {0x01u, 0x14u, 0x01u, 0x00u, 0x01u};
    int rc;

    /*
     * BOTH VCN33 PALDOs, on for the frame and off again. wmt_ic_soc.c's step 7
     * (:762-782) is exactly this bracket and it is the only place in the whole
     * conn_soc tree that touches WIFI_PALDO:
     *
     *     BT_PALDO   := PALDO_ON
     *     WIFI_PALDO := PALDO_ON
     *     wmt_core_init_script(calibration_table)
     *     BT_PALDO   := PALDO_OFF
     *     WIFI_PALDO := PALDO_OFF
     *
     * Calibration measures both front ends, so both have to be powered while it
     * runs; an earlier revision raised BT only and left WiFi to whatever the
     * power-on stage had done, which meant the WiFi front end was calibrated
     * against a rail that stock has up and this driver did not.
     *
     * Both are dropped on the way out whatever the frame did -- stock ignores the
     * return values here too, and a rail stuck on is worse than a missing log line.
     */
    if (mt6592_wifi_sdio_set_bt_rail(1) != 0)
    {
        set_failure("MediaTek WMT could not enable VCN33_BT for RF calibration",
                    "mt6592-wifi:wmt-vcn33-bt-enable-failed");
        return -1;
    }
    if (mt6592_wifi_sdio_set_wifi_rail(1) != 0)
    {
        (void)mt6592_wifi_sdio_set_bt_rail(0);
        set_failure("MediaTek WMT could not enable VCN33_WIFI for RF calibration",
                    "mt6592-wifi:wmt-vcn33-wifi-enable-failed");
        return -1;
    }
    delay_us(1000u);
    /* Six bytes have to arrive, but what is in them does not matter. Every other
     * init-script entry is memcmp'd against its expected event, and this one
     * alone is exempted by name:
     *
     *     if (0x14 != evtBuf[1])  // workaround RF calibration data EVT,
     *                             // do not care this EVT
     *         if (osal_memcmp(evtBuf, script[i].evt, script[i].evtSz) != 0) ...
     *
     * (wmt_core_init_script, wmt_core.c:620-624, under CFG_CHECK_WMT_RESULT.)
     * The length check above it is not exempted, so a short read still fails.
     * That is the same shape as the exchange here -- wait the full timeout for
     * six bytes, do not inspect them -- and it is the reason a rejected
     * calibration is worth a log line rather than a refused radio. */
    rc = wmt_exchange(calibration, sizeof(calibration), 6u, WMT_CALIBRATION_TIMEOUT_US);
    (void)mt6592_wifi_sdio_set_bt_rail(0);
    (void)mt6592_wifi_sdio_set_wifi_rail(0);
    if (rc != 0)
    {
        trace_stp_state("rf calibration failed");
        if (g_peer_answered_after_reset)
        {
            set_failure("MediaTek WMT shared RF calibration failed", "mt6592-wifi:wmt-rf-calibration-failed");
        }
        else
        {
            set_failure("MediaTek WMT connectivity MCU stopped answering after the ROM patch reset",
                        "mt6592-wifi:wmt-mcu-mute-after-patch-reset");
        }
        return -1;
    }
    return 0;
}

/*
 * WMT opcode 0x06 -- FUNC_CTRL. Turn one connectivity subsystem on or off.
 *
 * This driver has never sent it, and stock cannot bring Wi-Fi up without it.
 * Every other WMT command stock issues is a static array in one table in the
 * kernel image (0xc0b6cfac..0xc0b6d168: the STP queries, the three power-on DLM
 * writes, the four MCU-clock writes, the reset, the LTE-coex filters, the RF
 * calibration, the co-clock and the FM strap) and this driver reproduces all of
 * them. FUNC_CTRL is not in that table because stock builds it on the stack --
 * which is exactly why reproducing the table missed it.
 *
 * Its code is still in the image: wmt_core_func_ctrl_cmd(), with its own STP
 * exchange and its own diagnostics, one of which names the calling convention:
 *
 *   "[WMT-FUNC][E]%s(%d):wmt-func: wmt_core_func_ctrl_cmd(bt_on) failed(%d)"
 *   "[WMT-FUNC][I]%s:WMT-FUNC: wmt wlan func on befor wlan probe"
 *
 * The second is the ordering, stated by stock about itself: the WLAN function is
 * turned on BEFORE the wlan driver probes, and the probe is what downloads
 * WIFI_RAM_CODE_SOC and sends WIFI_START. We do the download and the start with
 * the subsystem never enabled.
 *
 * That fits the failure exactly. The download agent answers, decrypts, writes
 * and reports no error -- it is reached over the AP-side HIF and needs nothing
 * from WMT. WIFI_START then hands off to a core that is still held off, so the
 * agent goes quiet (it has handed over) and WLAN_READY never comes: wcir sits at
 * 0x00106592 for the full six seconds with POR_INDICATOR still set, and neither
 * mailbox is ever written. Six seconds of a CPU that was never started looks
 * identical to six seconds of a CPU that started and crashed, and that is the
 * whole reason this took so long to see.
 *
 * The frame is the ordinary WMT one -- 0x01, opcode, length, payload -- with a
 * two-byte payload of {subsystem, on}. wmt_exchange already checks the echoed
 * opcode and the status byte at index 4, which is the whole contract.
 *
 * Source now says the same, field by field. WMT_PKT is {eType, eOpCode,
 * u2SduLen (little endian), aucParam[]} with WMT_HDR_LEN 4
 * (conn_soc/common/core/include/wmt_core.h:279-284, :59), PKT_TYPE_CMD is 1 and
 * PKT_TYPE_EVENT 2 (:288-290), OPCODE_FUNC_CTRL is 6 (:301), and
 * wmt_core_func_ctrl_cmd (wmt_core.c:373-384) fills exactly this:
 *
 *     rWmtPktCmd.eType     = PKT_TYPE_CMD;
 *     rWmtPktCmd.eOpCode   = OPCODE_FUNC_CTRL;
 *     rWmtPktCmd.aucParam[0] = type;      // the driver type
 *     rWmtPktCmd.aucParam[1] = fgEn ? 1 : 0;
 *     rWmtPktCmd.u2SduLen  = WMT_FLAG_LEN + WMT_FUNC_CTRL_PARAM_LEN;  // 2
 *     u4WmtEventPduLen     = WMT_HDR_LEN + WMT_STS_LEN;               // 5
 *
 * -- the 5 being the minimum passed below, and the three checks that follow it
 * there (event type, echoed opcode, status byte 0) being the three
 * wmt_exchange makes. The subsystem numbers in mt6592_wifi_wmt.h are
 * WMTDRV_TYPE_* verbatim: BT 0, FM 1, GPS 2, WIFI 3, WMT 4
 * (conn_soc/common/include/wmt_exp.h:69-73).
 *
 * FUNC_CTRL is also the only opcode this driver sends that is built from that
 * struct at all. ENUM_OPCODE stops at OPCODE_INT = 8 (wmt_core.h:294-305), and
 * the frames wmt_finalize() sends carry 0x0a, 0x0f, 0x10 and 0x14 -- opcodes
 * that are not in it and never pass through it. Those go out through
 * wmt_core_init_script (wmt_core.c:592-635), which writes the array bytes to the
 * link and compares the reply to a second array, so the enum is not the
 * authority on which opcodes exist; the init tables are.
 */
static int wmt_func_ctrl(uint8_t subsystem, uint8_t on)
{
    uint8_t command[6] = {0x01u, 0x06u, 0x02u, 0x00u, 0x00u, 0x00u};

    command[4] = subsystem;
    command[5] = on;
    return wmt_exchange(command, sizeof(command), 5u, WMT_COMMAND_TIMEOUT_US);
}

int mt6592_wifi_wmt_func_ctrl(uint8_t subsystem, int on)
{
    int rc;

    if (!g_state.btif_ready)
    {
        set_failure("MediaTek WMT link is not up", "mt6592-wifi:wmt-link-down");
        return -1;
    }
    rc = wmt_func_ctrl(subsystem, on ? 1u : 0u);
    /*
     * Record it. Without this the console printed "wifi: accepted" and the state
     * dump three lines below printed `wlanfunc=no` in the same breath, because
     * the only writer of the flag was wmt_finalize clearing it. Two instruments
     * disagreeing about one measured event is worse than either alone: it is
     * what made a successful FUNC_CTRL look like a failed one for a whole
     * session. The flag means what its comment says -- did the MCU accept
     * FUNC_CTRL(wifi, on) -- so only subsystem 3 touches it.
     */
    if (subsystem == (uint8_t)WMT_SUBSYSTEM_WIFI)
    {
        g_state.wifi_function_on = (rc == 0 && on) ? 1 : 0;
    }
    return rc;
}

/*
 * Send an arbitrary WMT frame and hand the event back verbatim.
 *
 * Every WMT question so far has cost a reflash, because the only commands this
 * driver could send were the ones compiled into it. That was tolerable while the
 * question was "does the documented sequence work"; it is not tolerable now that
 * the questions are one-offs read out of the stock kernel's own tables. The
 * A-die chip-id read is the case in point -- opfunc_adie_lpbk_test sends the
 * eight bytes 01 13 04 00 02 04 24 00 and prints "read A die chipid CMD fail" if
 * the peer will not answer -- and there is no reason a command that specific
 * should need its own C function.
 *
 * The event is copied out whatever the outcome, including on a non-zero status,
 * because on this path the failure detail IS the answer. wmt_exchange returns
 * -1 for "the peer said no" and for "the peer said nothing", and only the bytes
 * distinguish them.
 */
int mt6592_wifi_wmt_raw_command(const uint8_t* command, uint32_t command_size, uint8_t* event_out,
                                uint32_t event_max, uint32_t* event_len)
{
    int rc;
    uint32_t n;

    if (event_len != 0)
    {
        *event_len = 0u;
    }
    if (command == 0 || command_size < 2u)
    {
        return -1;
    }
    if (!g_state.btif_ready)
    {
        set_failure("MediaTek WMT link is not up", "mt6592-wifi:wmt-link-down");
        return -1;
    }

    g_state.last_event_size = 0u;
    rc                      = wmt_exchange(command, command_size, 5u, WMT_COMMAND_TIMEOUT_US);

    n = g_state.last_event_size;
    if (n > sizeof(g_wmt_event))
    {
        n = (uint32_t)sizeof(g_wmt_event);
    }
    if (event_out != 0 && event_max != 0u)
    {
        if (n > event_max)
        {
            n = event_max;
        }
        copy_bytes(event_out, g_wmt_event, n);
    }
    if (event_len != 0)
    {
        *event_len = n;
    }
    return rc;
}

/*
 * Everything stock does after the last patch, and nothing it does not.
 *
 * mtk_wcn_soc_sw_init() ends with six conditional steps, and four of them are
 * compiled out on this platform. The switches are at the top of
 * conn_soc/common/core/wmt_ic_soc.c and they are literals, not Kconfig:
 *
 *     CFG_WMT_MULTI_PATCH        1   -> the two address commands, and a WMT_RESET
 *                                      after every patch rather than one at the end
 *     CFG_CHECK_WMT_RESULT       1   -> every event is compared, not just counted
 *     CFG_SUBSYS_COEX_NEED       0   -> no BT/WIFI/PTA/MISC coex frames; the single
 *                                      COEX_WMT command is the whole of coex
 *     CFG_WMT_CRYSTAL_TIMING_SET 0   -> no crystal trim SET/GET pair
 *     CFG_WMT_SDIO_DRIVING_SET   0   -> no 0x80050050 drive-strength write
 *
 * So the omissions below are the stock configuration rather than gaps, and the
 * order is stock's: calibration, then coex, then the co-clock, then the FM strap.
 * The A-die/PMIC-id WIFI_5G_PALDO branch is skipped for a different reason -- it
 * only fires for an MT6625 A-die, and this part reports 0x6627.
 */
static int wmt_finalize(void)
{
    /*
     * Every frame below is one of MediaTek's own static command arrays, copied
     * byte for byte out of conn_soc/common/core/wmt_ic_soc.c:
     *
     *     query_stp        WMT_QUERY_STP_CMD          (:100)
     *     coex             WMT_COEX_SETTING_CONFIG_CMD(:121)  -- see below
     *     co_clock         WMT_CORE_CO_CLOCK_CMD      (:174)
     *     fm_strap         WMT_STRAP_CONF_CMD_FM_COMM (:149)
     *     core_dump_level  WMT_CORE_DUMP_LEVEL_04_CMD (:171)
     *
     * The coex frame is the one that is not a straight copy, and it took the
     * longest to settle because the initialiser at :121 ends in 0x00 while we
     * send 0x01. The initialiser is not what stock transmits: wmt_ic_soc.c:1384
     * overwrites that byte immediately before the send --
     *
     *     coex_table[COEX_WMT].cmd[5] = pWmtGenConf->coex_wmt_ant_mode;
     *
     * -- with the value parsed out of /etc/firmware/WMT_SOC.cfg (the field list
     * is wmt_conf.c:114, the struct member wmt_core.h:156). The J36's own copy of
     * that file says `coex_wmt_ant_mode=1`, which is quoted in full at
     * mt6592_wifi_sdio.h:44-58 for the co_clock_flag that sits four lines below
     * it. So 0x01 is this device's antenna mode, and the 0x00 in the reference
     * initialiser is only what a board with no config file would send.
     *
     * co_clock is likewise conditional in stock -- wmt_ic_soc.c:801 sends
     * osc_type_table only when mtk_wcn_soc_co_clock_get() reads WMT_CO_CLOCK_EN,
     * which is the same file's co_clock_flag. That flag is 1 here
     * (MT6592_WIFI_CO_CLOCK_FLAG), so sending it unconditionally is stock's
     * behaviour on this board rather than an extra frame.
     */
    static const uint8_t query_stp[] = {0x01u, 0x04u, 0x01u, 0x00u, 0x04u};
    static const uint8_t coex[]      = {0x01u, 0x10u, 0x02u, 0x00u, 0x01u, 0x01u};
    static const uint8_t co_clock[]  = {0x01u, 0x0au, 0x02u, 0x00u, 0x08u, 0x03u};
    static const uint8_t fm_strap[]  = {0x01u, 0x05u, 0x02u, 0x00u, 0x02u, 0x02u};
    static const uint8_t core_dump_level[] = {0x01u, 0x0fu, 0x07u, 0x00u, 0x04u, 0x00u,
                                              0x00u, 0x00u, 0x00u, 0x00u, 0x00u};

    const uint32_t chip_id = mt6592_wifi_sdio_get_state()->chip_id;

    /* Is the peer there at all? The STP capability query is the cheapest thing
     * that answers that: it reads state, changes nothing, and this driver has
     * already had it answered twice on this link, so a failure here is about the
     * peer rather than about the command.
     *
     * It deliberately does not gate what follows. Stock never re-queries after
     * the final reset, so there is no stock behaviour to say the patched image
     * must still answer it -- returning early on a failed probe would risk
     * refusing a radio that was about to work. All it does is decide which name
     * the next real failure gets. */
    g_peer_answered_after_reset = wmt_exchange(query_stp, sizeof(query_stp), 10u, WMT_COMMAND_TIMEOUT_US) == 0;
    if (!g_peer_answered_after_reset)
    {
        trace_stp_state("silent after patch reset");
        mt6592_uart_puts("  wifi: WMT connectivity MCU did not answer the STP query after the patch reset\n");
    }

    /* An unread chip id is not a licence to skip the preamble: fall through to
     * the stock behaviour for a part we cannot identify. */
    if (chip_needs_lte_coex_preamble(chip_id))
    {
        wmt_send_lte_coex_preamble();
    }

    /* Now the calibration, in the place stock puts it -- after the preamble, not
     * instead of it. It is deliberately NOT fatal here.
     *
     * The last time this driver sent this frame it hung for ten seconds and the
     * bootstrap gave up, which is how it came to be skipped on a wrong reading
     * of the chip-id branch above. The preamble is the one thing that has
     * changed since, so this run is the experiment: if the four frames in front
     * of it are what the peer was waiting for, it answers now. If it still does
     * not, that is worth exactly one log line and not a refused radio -- the
     * bring-up got further with the frame absent than with it fatal, and the
     * rest of the configuration below is what the scan path actually needs. */
    if (wmt_run_rf_calibration() != 0)
    {
        g_state.calibrated = 0;
        mt6592_uart_puts("  wifi: WMT shared RF calibration unanswered, continuing without it\n");
    }
    else
    {
        g_state.calibrated = 1;
        mt6592_uart_puts("  wifi: WMT shared RF calibration acknowledged\n");
    }

    if (wmt_exchange(coex, sizeof(coex), 5u, WMT_COMMAND_TIMEOUT_US) != 0)
    {
        trace_stp_state("coexistence setup failed");
        if (g_peer_answered_after_reset)
        {
            set_failure("MediaTek WMT coexistence setup failed", "mt6592-wifi:wmt-coexistence-config-failed");
        }
        else
        {
            set_failure("MediaTek WMT connectivity MCU stopped answering after the ROM patch reset",
                        "mt6592-wifi:wmt-mcu-mute-after-patch-reset");
        }
        return -1;
    }
    if (wmt_exchange(co_clock, sizeof(co_clock), 5u, WMT_COMMAND_TIMEOUT_US) != 0)
    {
        trace_stp_state("co-clock setup failed");
        set_failure("MediaTek WMT co-clock setup failed", "mt6592-wifi:wmt-co-clock-config-failed");
        return -1;
    }
    /* FM strap does not gate WLAN; retain the stock command but do not reject
     * Wi-Fi if an unrelated FM configuration event is absent. */
    (void)wmt_exchange(fm_strap, sizeof(fm_strap), 6u, WMT_COMMAND_TIMEOUT_US);

    /* The last frame in stock's power-on sequence, from the table entry at
     * 0xc0b6d168, sent at 0xc039c7ac once the call at 0xc03a9354 says core dumps
     * are wanted. The source names it WMT_CORE_DUMP_LEVEL_04_CMD
     * (wmt_ic_soc.c:171) -- "to get full dump when f/w assert" -- and it is the
     * only one of the four dump levels not inside the `#if 0` above it, the
     * other three being the 01/02/03 variants at :164-169. All six payload bytes
     * after the level are zero, which is the level this build wants too, so there
     * is nothing to compute. Not fatal for the same reason as the FM strap: it
     * configures a diagnostic, not a radio. */
    (void)wmt_exchange(core_dump_level, sizeof(core_dump_level), 5u, WMT_COMMAND_TIMEOUT_US);

    /*
     * No FUNC_CTRL here. I sent one for a while on the strength of a stock log
     * line -- "wmt wlan func on befor wlan probe" -- read as evidence of a
     * command. It is not; it is the message printed immediately before a
     * function call. MediaTek's own source for this part settles it:
     * wmt_func_wifi_on() in conn_soc/common/core/wmt_func.c only calls
     * mtk_wcn_wlan_probe(), and the one function that would have sent the
     * command, wmt_func_wifi_ctrl(), sits inside `#if 0` under the comment "in
     * soc, wmt turn on wifi directly, no not need operate SDIO".
     * wmt_core_func_ctrl_cmd() is called in that tree for BT, FM and GPS, and
     * never for WLAN.
     *
     * The frame itself was right -- the chip answered status 0 and reported
     * wlanfunc=yes -- and WIFI_START hung exactly as before, so it changed
     * nothing measurable. A command stock never sends, that turns a subsystem
     * on, and that fixes nothing, is not worth carrying. `wifi func <type>
     * [0|1]` stays as a console probe.
     */
    g_state.wifi_function_on = 0;

    /* `calibrated` is now set by the calibration attempt above and left alone
     * here, so the flag says what happened rather than what the bootstrap would
     * like to have happened. It used to be forced to 1 on the reasoning that a
     * part carrying its calibration in the ROM patch is already configured and a
     * 0 would read as a half-finished bootstrap. That reasoning depended on the
     * chip-id branch meaning "skips calibration", and it does not. */
    g_state.ready   = 1;
    g_state.status  = g_state.calibrated ? "MediaTek WMT ROM patches and shared RF calibration ready"
                                         : "MediaTek WMT ROM patches ready, RF calibration unanswered";
    g_state.blocked = 0;
    mt6592_uart_puts("  wifi: WMT ROM patches loaded, radio configured\n");
    return 0;
}

/*
 * The mandatory-mode STP capability query, on its own.
 *
 * Everything the bootstrap does after this point needs a ROM patch image, and
 * for a long time that meant every fault below the patch stage -- an ungated
 * BTIF, a connectivity MCU still in reset, a link dropping bytes -- reported
 * itself as "the ROM patch did not load", because load_patch was the only door
 * in and it opens by doing all of this first. Splitting the query out separates
 * "the peer is not talking" from "the patch image is wrong", and does it without
 * needing a patch image to hand, which matters because the images live in the
 * stock /system partition and the bring-up code that most needs this answer runs
 * long before anything can read them.
 */
int mt6592_wifi_wmt_probe_link(void)
{
    static const uint8_t query_stp[] = {0x01u, 0x04u, 0x01u, 0x00u, 0x04u};

    if (btif_init() != 0)
    {
        return -1;
    }
    /* Already switched to full STP by an earlier call; the mandatory-mode query
     * is not valid twice and the peer has plainly answered once already. */
    if (g_stp_full_mode)
    {
        return 0;
    }
    if (wmt_exchange(query_stp, sizeof(query_stp), 10u, WMT_COMMAND_TIMEOUT_US) != 0)
    {
        trace_stp_state("mandatory stp query failed");
        set_failure("MediaTek WMT bootstrap did not answer the mandatory STP capability query",
                    "mt6592-wifi:wmt-stp-bootstrap-failed");
        return -1;
    }
    g_state.status  = "MediaTek WMT connectivity MCU answered the STP capability query";
    g_state.blocked = 0;
    return 0;
}

int mt6592_wifi_wmt_load_patch(const void* data, uint32_t size)
{
    /*
     * Four more of MediaTek's static arrays, again byte for byte from
     * conn_soc/common/core/wmt_ic_soc.c:
     *
     *     query_stp      WMT_QUERY_STP_CMD       (:100)
     *     set_stp_full   WMT_SET_STP_CMD         (:147)
     *     patch_address  WMT_PATCH_ADDRESS_CMD   (:114)
     *     reset          WMT_RESET_CMD           (:105)
     *
     * set_stp_full's four payload bytes 0xdf 0x0e 0x68 0x01 are the capability
     * word, and they are not a value to be derived -- stock hard-codes the same
     * four in the array it compares the query answer against,
     * WMT_QUERY_STP_EVT (:102). The default-capability form at :101 ends
     * 0x11 0x00 0x00 0x00 and is what an unpatched bootstrap reports; asking for
     * 0xdf 0x0e 0x68 0x01 is asking for the full set.
     *
     * patch_address is sent verbatim -- wmt_ic_soc.c:1719 transmits
     * sizeof(WMT_PATCH_ADDRESS_CMD) straight from the initialiser with nothing
     * patched into it, and the 0x10 0x00 length halfword accounts for all
     * sixteen payload bytes, so the trailing 0xff 0xff 0xff 0xff are payload and
     * not a match wildcard. The partial-patch twin at :116,
     * WMT_PATCH_P_ADDRESS_CMD, is the one with the runtime hole: :1741 overwrites
     * its [12..15] before sending. That is the array our part_address[] mirrors,
     * and the four bytes it gets are the one thing in this file the reference
     * tree cannot confirm -- stock does not derive them, it is handed them by
     * ioctl from the userspace loader (wmt_dev.c:1975 WMT_IOCTL_SET_PATCH_INFO ->
     * wmt_lib.h:119 addRess[4] -> wmt_ctrl.c:553), and that loader is not in this
     * tree. We take them from the image header instead.
     */
    static const uint8_t query_stp[]     = {0x01u, 0x04u, 0x01u, 0x00u, 0x04u};
    static const uint8_t set_stp_full[]  = {0x01u, 0x04u, 0x05u, 0x00u, 0x03u, 0xdfu, 0x0eu, 0x68u, 0x01u};
    static const uint8_t patch_address[] = {0x01u, 0x08u, 0x10u, 0x00u, 0x01u, 0x01u, 0x00u, 0x01u, 0x3cu, 0x02u,
                                            0x09u, 0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0xffu, 0xffu, 0xffu, 0xffu};
    static const uint8_t reset[]         = {0x01u, 0x07u, 0x01u, 0x00u, 0x04u};
    const uint8_t* image                 = (const uint8_t*)data;

    if (!image || size <= WMT_PATCH_HEADER_SIZE || image[16] != 'A' || image[17] != 'L' || image[18] != 'P' ||
        image[19] != 'S')
    {
        set_failure("MediaTek WMT ROM patch header is invalid", "mt6592-wifi:wmt-patch-header-invalid");
        return -1;
    }

    const uint8_t metadata    = image[24];
    const uint8_t patch_count = metadata >> 4;
    const uint8_t sequence    = metadata & 0x0fu;
    if (patch_count == 0u || patch_count > 7u || sequence == 0u || sequence > patch_count)
    {
        set_failure("MediaTek WMT ROM patch sequence metadata is invalid", "mt6592-wifi:wmt-patch-sequence-invalid");
        return -1;
    }
    if (g_state.patch_count != 0u && g_state.patch_count != patch_count)
    {
        set_failure("MediaTek WMT ROM patch count changed between images", "mt6592-wifi:wmt-patch-count-mismatch");
        return -1;
    }
    g_state.patch_count = patch_count;
    if (g_state.patch_mask & BIT(sequence - 1u))
    {
        if (!g_state.ready && patch_bit_count(g_state.patch_mask) == g_state.patch_count)
        {
            return wmt_finalize();
        }
        return 0;
    }
    if (sequence != (uint8_t)(patch_bit_count(g_state.patch_mask) + 1))
    {
        set_failure("MediaTek WMT ROM patches were supplied out of sequence", "mt6592-wifi:wmt-patch-order-invalid");
        return -1;
    }

    if (btif_init() != 0)
    {
        return -1;
    }
    if (g_state.patch_mask == 0u)
    {
        if (!g_stp_full_mode)
        {
            if (wmt_exchange(query_stp, sizeof(query_stp), 10u, WMT_COMMAND_TIMEOUT_US) != 0)
            {
                trace_stp_state("mandatory stp query failed");
                set_failure("MediaTek WMT bootstrap did not answer the mandatory STP capability query",
                            "mt6592-wifi:wmt-stp-bootstrap-failed");
                return -1;
            }
            if (wmt_exchange(set_stp_full, sizeof(set_stp_full), 6u, WMT_COMMAND_TIMEOUT_US) != 0)
            {
                trace_stp_state("full stp switch failed");
                set_failure("MediaTek WMT could not switch BTIF to full STP mode",
                            "mt6592-wifi:wmt-stp-full-mode-switch-failed");
                return -1;
            }
            g_stp_full_mode            = 1;
            g_stp_tx_sequence          = 0u;
            g_stp_expected_rx_sequence = 0u;
            g_stp_last_rx_sequence     = 7u;
            /* The stock MT6592 path waits 10 ms for the connectivity MCU to
             * switch mechanisms before emitting the first full-STP packet. */
            delay_us(10000u);
            mt6592_bootstatus_log_text("wifi: WMT BTIF full STP mode ready\n");
        }
        if (wmt_exchange(query_stp, sizeof(query_stp), 10u, WMT_COMMAND_TIMEOUT_US) != 0)
        {
            trace_stp_state("full stp query failed");
            set_failure("MediaTek WMT full STP capability query failed", "mt6592-wifi:wmt-stp-full-mode-query-failed");
            return -1;
        }
        trace_stp_state("full stp ready");

        /*
         * Both of these belong to the first patch only, and both are what stock
         * brackets the whole download with -- see wmt_power_on_dlm() for why the
         * memory power-up is not optional, and wmt_set_mcu_clock() for where the
         * clock window closes. The mask test is what makes them once-per-set:
         * this function is called once per image, but the sequence stock runs is
         * per power-on.
         */
        if (wmt_power_on_dlm() != 0)
        {
            return -1;
        }
        wmt_set_mcu_clock(1);
    }

    if (wmt_exchange(patch_address, sizeof(patch_address), 8u, WMT_COMMAND_TIMEOUT_US) != 0)
    {
        trace_stp_state("patch address failed");
        set_failure("MediaTek WMT patch-address setup failed", "mt6592-wifi:wmt-patch-address-failed");
        return -1;
    }

    /*
     * The per-patch destination, and the one value in this whole driver that the
     * kernel source cannot settle on its own.
     *
     * wmt_ic_soc.c:1741 is `memcpy(&WMT_PATCH_P_ADDRESS_CMD[12], addressByte, 4)`,
     * so the four bytes at [12..15] -- the value half of a write to connectivity
     * register 0x020904c4 -- are whatever `addRess[4]` holds. But addRess does not
     * come from the kernel: wmt_ctrl_get_patch_info() copies it out of a
     * WMT_PATCH_INFO that userspace filled in through WMT_IOCTL_SET_PATCH_INFO
     * (wmt_dev.c:1975), and the sender is /system/bin/6620_launcher, which is a
     * stripped PIE with no source in the vendor tree.
     *
     * What the launcher's own strings do say is that it reads them out of the file
     * -- "read patch info:0x%02x,0x%02x,0x%02x,0x%02x" -- and the two real images
     * off this device pin down where. Their 28-byte headers differ in exactly one
     * word, at offset 24:
     *
     *   ROMv1_patch_1_0_hdr.bin  80776 bytes  ...ALPS 8a00 8a00 | 22 00 06 00
     *   ROMv1_patch_1_1_hdr.bin  24592 bytes  ...ALPS 8a00 8a00 | 21 00 0e f0
     *
     * Byte 24 is already known to be (patch_count << 4) | download_sequence, and
     * both files agreeing on a count of 2 while disagreeing on the sequence is
     * what confirms it. So the address is the remaining three bytes, low byte
     * zero, which reads 0x00060000 for the big patch and 0xf00e0000 for the small
     * one -- both page-aligned, and the second inside the CONSYS EMI window whose
     * firmware-side base is 0xf0000000. Taking four bytes starting at 24 instead
     * would send the descriptor byte as part of the address; starting at 25 would
     * give 0xff000600 and 0x00f00e00, neither of which is an address. The stock
     * table's own default value, 0x01003f00, has a zero low byte too.
     *
     * The small patch is the coredump one -- its body starts with the ASCII
     * "; coredump start" -- which is why its destination is in the shared EMI
     * window rather than in code space.
     */
    uint8_t part_address[sizeof(patch_address)] = {0x01u, 0x08u, 0x10u, 0x00u, 0x01u, 0x01u, 0x00u,
                                                   0x01u, 0xc4u, 0x04u, 0x09u, 0x02u, 0x00u, 0x3fu,
                                                   0x00u, 0x01u, 0xffu, 0xffu, 0xffu, 0xffu};
    part_address[12]                            = 0u;
    part_address[13]                            = image[25];
    part_address[14]                            = image[26];
    part_address[15]                            = image[27];
    if (wmt_exchange(part_address, sizeof(part_address), 8u, WMT_COMMAND_TIMEOUT_US) != 0)
    {
        trace_stp_state("part-patch address failed");
        set_failure("MediaTek WMT partial-patch address setup failed", "mt6592-wifi:wmt-part-patch-address-failed");
        return -1;
    }
    trace_stp_state("patch address ready");

    const uint8_t* body         = image + WMT_PATCH_HEADER_SIZE;
    const uint32_t body_size    = size - WMT_PATCH_HEADER_SIZE;
    const uint32_t bytes_before = g_state.patch_bytes;

    /* One retry at the same fragment size, not a search over sizes.
     *
     * A halving ladder used to live here -- 1000, then 500, 250 and on down to
     * 15 -- put in to find out whether frame length was what the link choked on.
     * It answered, and the answer was no: 136-byte fragments stalled the same
     * way the full-size ones did, and 125 after them. What the ladder costs is
     * not free either. Every rung replays the body from fragment zero, so the
     * last rungs are thousands of round trips, and a pass that stalls spends two
     * seconds on the fragment it stalls at before giving up -- the full ladder
     * is minutes of a machine that is doing nothing else, which is most of what
     * the scan freeze actually was.
     *
     * A single retry keeps the part that was worth having: a link that lost one
     * frame gets a second pass, with both address commands re-issued first to
     * put the peer back at the start of the sequence. */
    if (wmt_download_patch_body(body, body_size, WMT_PATCH_FRAGMENT_SIZE) != 0)
    {
        trace_stp_state("patch fragment retry");
        g_state.patch_bytes = bytes_before;

        /* The abandoned pass left the peer part-way through a patch sequence, so
         * re-issue both address commands before starting the body over. */
        if (wmt_exchange(patch_address, sizeof(patch_address), 8u, WMT_COMMAND_TIMEOUT_US) != 0 ||
            wmt_exchange(part_address, sizeof(part_address), 8u, WMT_COMMAND_TIMEOUT_US) != 0)
        {
            trace_stp_state("patch re-address failed");
            set_failure("MediaTek WMT patch-address setup failed after a stalled pass",
                        "mt6592-wifi:wmt-patch-readdress-failed");
            return -1;
        }
        if (wmt_download_patch_body(body, body_size, WMT_PATCH_FRAGMENT_SIZE) != 0)
        {
            trace_stp_state("patch fragment stalled");
            set_failure("MediaTek WMT ROM patch fragment download failed", "mt6592-wifi:wmt-patch-fragment-failed");
            return -1;
        }
    }

    if (wmt_exchange(reset, sizeof(reset), 5u, WMT_COMMAND_TIMEOUT_US) != 0)
    {
        trace_stp_state("patch reset failed");
        set_failure("MediaTek WMT reset after ROM patch failed", "mt6592-wifi:wmt-patch-reset-failed");
        return -1;
    }
    delay_us(20000u);

    g_state.patch_mask |= (uint8_t)BIT(sequence - 1u);
    g_state.status  = "MediaTek WMT ROM patch loaded; waiting for remaining patch";
    g_state.blocked = "mt6592-wifi:wmt-more-patches-required";
    mt6592_uart_puts("  wifi: WMT ROM patch sequence=");
    mt6592_uart_put_hex32(sequence);
    mt6592_uart_puts(" loaded\n");
    trace_stp_state("patch link");

    if (patch_bit_count(g_state.patch_mask) == g_state.patch_count)
    {
        /* The set is complete, so the reset above is the one the patched image
         * booted from. Stock closes its clock window here, after that reset and
         * before it configures the radio. */
        wmt_set_mcu_clock(0);
        return wmt_finalize();
    }
    return 0;
}

const mt6592_wifi_wmt_state* mt6592_wifi_wmt_get_state(void)
{
    return &g_state;
}
