/* Copyright (c) 2026 SynChanCyberSecurity LLC. All Rights Reserved. */
/*
 * mt6592_usb_gadget.c — MUSB peripheral transport for the live debug console.
 * See mt6592_usb_gadget.h for why this exists and what is transcribed from the
 * stock LK rather than invented.
 *
 * This file deliberately never drives the DRVVBUS pad, and that is not an
 * oversight to be corrected later. A gadget is the B-device: the host on the
 * other end of the cable is the one sourcing 5 V, and the VBUS this code cares
 * about is an input it waits to see. The MUSB host driver drives the pad because
 * it is the A-device; driving it from here would put the board's own 5 V onto a
 * line a host is already holding up. Read mt6592_board_j36.h beside
 * MT6592_J36_USB_DRVVBUS_GPIO before changing that.
 */

#include "mt6592_usb_gadget.h"

#include "mt6592_bootstatus.h"
#include "mt6592_delay.h"
#include "mt6592_uart.h"

#define BIT(n) (1u << (n))

enum {
    MUSB_BASE = 0x11200000u,
    USB_PHY_BASE = 0x11210800u,

    /* Common block */
    MUSB_FADDR = 0x00u,
    MUSB_POWER = 0x01u,
    MUSB_INTRTX = 0x02u,
    MUSB_INTRRX = 0x04u,
    MUSB_INTRTXE = 0x06u,
    MUSB_INTRRXE = 0x08u,
    MUSB_INTRUSB = 0x0au,
    MUSB_INTRUSBE = 0x0bu,
    MUSB_INDEX = 0x0eu,
    MUSB_DEVCTL = 0x60u,

    /* Indexed by MUSB_INDEX */
    MUSB_TXMAXP = 0x10u,
    MUSB_CSR0 = 0x12u,
    MUSB_TXCSR = 0x12u,
    MUSB_RXMAXP = 0x14u,
    MUSB_RXCSR = 0x16u,
    MUSB_COUNT0 = 0x18u,
    MUSB_RXCOUNT = 0x18u,
    MUSB_TXFIFOSZ = 0x62u,
    MUSB_RXFIFOSZ = 0x63u,
    MUSB_TXFIFOADD = 0x64u,
    MUSB_RXFIFOADD = 0x66u,

    MUSB_FIFO0 = 0x20u,
    MUSB_FIFO_STRIDE = 4u,

    /* POWER */
    POWER_ENSUSPEND = BIT(0),
    POWER_RESET = BIT(3),
    POWER_HSMODE = BIT(4),
    POWER_HSENAB = BIT(5),
    POWER_SOFTCONN = BIT(6),

    /* INTRUSB (peripheral role) */
    INTRUSB_SUSPEND = BIT(0),
    INTRUSB_RESUME = BIT(1),
    INTRUSB_RESET = BIT(2),
    INTRUSB_SOF = BIT(3),
    INTRUSB_CONN = BIT(4),
    INTRUSB_DISCON = BIT(5),

    /* CSR0, peripheral role */
    CSR0_RXPKTRDY = BIT(0),
    CSR0_TXPKTRDY = BIT(1),
    CSR0_SENTSTALL = BIT(2),
    CSR0_DATAEND = BIT(3),
    CSR0_SETUPEND = BIT(4),
    CSR0_SENDSTALL = BIT(5),
    CSR0_SERVICED_RXPKTRDY = BIT(6),
    CSR0_SERVICED_SETUPEND = BIT(7),
    CSR0_FLUSHFIFO = BIT(8),

    /* TXCSR, peripheral role */
    TXCSR_TXPKTRDY = BIT(0),
    TXCSR_FIFONOTEMPTY = BIT(1),
    TXCSR_UNDERRUN = BIT(2),
    TXCSR_FLUSHFIFO = BIT(3),
    TXCSR_SENDSTALL = BIT(4),
    TXCSR_SENTSTALL = BIT(5),
    TXCSR_CLRDATATOG = BIT(6),
    TXCSR_MODE = BIT(13),

    /* RXCSR, peripheral role */
    RXCSR_RXPKTRDY = BIT(0),
    RXCSR_FIFOFULL = BIT(1),
    RXCSR_OVERRUN = BIT(2),
    RXCSR_FLUSHFIFO = BIT(4),
    RXCSR_SENDSTALL = BIT(5),
    RXCSR_SENTSTALL = BIT(6),
    RXCSR_CLRDATATOG = BIT(7),

    EP_IN = 1u,  /* device -> host */
    EP_OUT = 2u, /* host -> device */

    /* Bounded spins. A wedged FIFO must cost milliseconds, not the boot. */
    TX_SPIN_CAP = 400000u,
    EP0_SPIN_CAP = 200000u,
};

/* USB standard requests we answer. */
enum {
    REQ_GET_STATUS = 0x00u,
    REQ_CLEAR_FEATURE = 0x01u,
    REQ_SET_FEATURE = 0x03u,
    REQ_SET_ADDRESS = 0x05u,
    REQ_GET_DESCRIPTOR = 0x06u,
    REQ_GET_CONFIGURATION = 0x08u,
    REQ_SET_CONFIGURATION = 0x09u,
    REQ_GET_INTERFACE = 0x0au,
    REQ_SET_INTERFACE = 0x0bu,

    DESC_DEVICE = 1u,
    DESC_CONFIG = 2u,
    DESC_STRING = 3u,
};

/* ── register helpers ───────────────────────────────────────────────────────── */

static volatile uint8_t* r8(uint32_t off) {
    return (volatile uint8_t*)(uintptr_t)(MUSB_BASE + off);
}
static volatile uint16_t* r16(uint32_t off) {
    return (volatile uint16_t*)(uintptr_t)(MUSB_BASE + off);
}
static uint8_t rd8(uint32_t off) { return *r8(off); }
static void wr8(uint32_t off, uint8_t v) { *r8(off) = v; }
static uint16_t rd16(uint32_t off) { return *r16(off); }
static void wr16(uint32_t off, uint16_t v) { *r16(off) = v; }

static uint8_t phy_rd(uint32_t off) {
    return *(volatile uint8_t*)(uintptr_t)(USB_PHY_BASE + off);
}
static void phy_wr(uint32_t off, uint8_t v) {
    *(volatile uint8_t*)(uintptr_t)(USB_PHY_BASE + off) = v;
}
static void phy_set(uint32_t off, uint8_t bits) { phy_wr(off, (uint8_t)(phy_rd(off) | bits)); }
static void phy_clr(uint32_t off, uint8_t bits) { phy_wr(off, (uint8_t)(phy_rd(off) & ~bits)); }

/* 33 legacy cycles ~= 1 us (mt6592_delay.h). */
static void udelay(uint32_t us) { mt6592_delay_cycles(us * 33u); }

static void glog(const char* text) {
    mt6592_uart_puts(text);
    mt6592_bootstatus_log_text(text);
}

static void glog_hex(const char* label, uint32_t value) {
    glog(label);
    {
        char buf[11];
        buf[0] = '0';
        buf[1] = 'x';
        for (uint32_t i = 0; i < 8u; ++i) {
            const uint32_t nib = (value >> ((7u - i) * 4u)) & 0xfu;
            buf[2u + i] = (char)(nib < 10u ? ('0' + nib) : ('a' + nib - 10u));
        }
        buf[10] = 0;
        glog(buf);
    }
}

/* ── descriptors ────────────────────────────────────────────────────────────── */

static const uint8_t kDeviceDesc[18] = {
    18u, DESC_DEVICE,
    0x00u, 0x02u,                     /* USB 2.00 */
    0xffu, 0xffu, 0xffu,              /* vendor specific: no host OS claims it */
    MT6592_GADGET_PACKET,             /* bMaxPacketSize0 */
    (uint8_t)(MT6592_GADGET_VID & 0xffu), (uint8_t)(MT6592_GADGET_VID >> 8),
    (uint8_t)(MT6592_GADGET_PID & 0xffu), (uint8_t)(MT6592_GADGET_PID >> 8),
    0x00u, 0x01u,                     /* bcdDevice 1.00 */
    1u, 2u, 0u,                       /* iManufacturer, iProduct, iSerial */
    1u,                               /* bNumConfigurations */
};

static const uint8_t kConfigDesc[32] = {
    /* configuration */
    9u, DESC_CONFIG, 32u, 0u, 1u, 1u, 0u, 0x80u, 0xfau,
    /* interface */
    9u, 4u, 0u, 0u, 2u, 0xffu, 0xffu, 0xffu, 0u,
    /* EP1 IN, bulk, 64 */
    7u, 5u, 0x80u | EP_IN, 0x02u, MT6592_GADGET_PACKET, 0u, 0u,
    /* EP2 OUT, bulk, 64 */
    7u, 5u, EP_OUT, 0x02u, MT6592_GADGET_PACKET, 0u, 0u,
};

static const uint8_t kStringLang[4] = {4u, DESC_STRING, 0x09u, 0x04u};
static const uint8_t kStringMfr[10] = {10u, DESC_STRING, 'M', 0, 'V', 0, 'I', 0, 'I', 0};
static const uint8_t kStringProd[26] = {
    26u, DESC_STRING, 'J', 0, '3', 0, '6', 0, ' ', 0, 'C', 0,
    'o', 0, 'n', 0, 's', 0, 'o', 0, 'l', 0, 'e', 0,
};

/* ── state ──────────────────────────────────────────────────────────────────── */

static uint32_t g_inited;
static uint32_t g_configured;
static uint32_t g_pending_address;
static uint32_t g_have_pending_address;

/* Enumeration forensics. "The host never showed up" has several very different
 * causes and they are indistinguishable from the outside: no VBUS/no bus reset
 * means the cable or the pull-up, resets but no SETUP means the host gave up
 * before asking, SETUPs but no SET_CONFIGURATION means it did not like an
 * answer. These three counters separate them, and they are cheap. */
static uint32_t g_reset_count;
static uint32_t g_setup_count;
static uint32_t g_intrusb_seen;
static uint32_t g_reset_raw;
static uint8_t g_intrusb_prev;

/* ── PHY ────────────────────────────────────────────────────────────────────── */

/*
 * Transcription of the stock LK's usb_phy_recover(), FUN_81e09520 at
 * Reference/j36-lk-reverse/lk.full-decompile.c:9866.
 *
 * The one deviation: the original reads eFuse word 0x13 to pick the
 * VRT/TERM_VREF trim for PHY register 0x06, and falls back to the constant 0x68
 * when the eFuse field is blank. We always take that fallback, because the
 * eFuse accessor is a whole driver we do not have and 0x68 is what an untrimmed
 * part of this family runs. Trim affects HS eye margin; this device is full
 * speed only, where it does not matter. The two other eFuse-conditional tweaks
 * (PHY 0x00 bit5 and 0x05 bits 6:4) are skipped for the same reason.
 *
 * Note what the tail does: clear 0x6c bit4, set 0x6c 0x2e, set 0x6d 0x3e. That
 * is MTK's force-DEVICE-mode override -- the exact mirror of
 * musb_phy_force_host() in the MUSB host driver, which sets 0x6c bit4 instead. So
 * this does not merely wake the PHY, it pins the role, and the console does not
 * depend on how the OTG ID pin happens to float on this board.
 */
static void phy_recover(void) {
    phy_clr(0x1du, 0x10u);
    phy_clr(0x6bu, 0x04u); /* force_uart_en = 0 */
    phy_clr(0x6eu, 0x01u); /* RG_UART_EN = 0 */
    phy_clr(0x68u, 0xf4u); /* drop the force_* overrides */
    phy_clr(0x69u, 0x3cu);
    phy_clr(0x6au, 0xbeu);
    phy_clr(0x1au, 0x80u); /* rg_usb20_gpio_ctl = 0 */
    phy_wr(0x06u, (uint8_t)((phy_rd(0x06u) & 0x07u) | 0x68u));
    phy_set(0x1au, 0x10u);
    udelay(800u);
    phy_clr(0x6cu, 0x10u);
    phy_set(0x6cu, 0x2eu);
    phy_set(0x6du, 0x3eu);
}

/* Transcription of FUN_81e093b8 (line 9825), the stock savecurrent()/power-down. */
static void phy_savecurrent(void) {
    phy_clr(0x6bu, 0x04u);
    phy_clr(0x6eu, 0x01u);
    phy_clr(0x1au, 0x80u);
    phy_wr(0x06u, (uint8_t)((phy_rd(0x06u) & 0x07u) | 0x68u));
    phy_clr(0x22u, 0x03u); /* DP/DM 100k pull-downs off */
    phy_clr(0x6au, 0x04u);
    udelay(800u);
    phy_clr(0x6cu, 0x10u);
    phy_set(0x6cu, 0x2eu);
    phy_set(0x6du, 0x3eu);
}

/* ── endpoints ──────────────────────────────────────────────────────────────── */

/* MUSB dynamic FIFO: size field is log2(bytes) - 3, address unit is 8 bytes.
 * Both confirmed against FUN_81e09a1c in the decompile, which computes
 * TXFIFOADD as (addr << 13) >> 16 == addr / 8. EP0 owns bytes 0..63 in
 * hardware, so our two 64-byte windows start at 64 and 128. */
enum {
    FIFOSZ_64 = 3u,
    FIFOADD_EP_IN = 64u / 8u,
    FIFOADD_EP_OUT = 128u / 8u,
};

static void ep_configure(void) {
    /* EP1 IN. MODE=1 marks the endpoint as a transmitter; harmless with split
     * TX/RX FIFOs and required if this core was built with shared ones. */
    wr8(MUSB_INDEX, (uint8_t)EP_IN);
    wr16(MUSB_TXMAXP, MT6592_GADGET_PACKET);
    wr8(MUSB_TXFIFOSZ, FIFOSZ_64);
    wr16(MUSB_TXFIFOADD, FIFOADD_EP_IN);
    wr16(MUSB_TXCSR, (uint16_t)(TXCSR_MODE | TXCSR_FLUSHFIFO | TXCSR_CLRDATATOG));
    wr16(MUSB_TXCSR, (uint16_t)TXCSR_MODE);

    /* EP2 OUT. */
    wr8(MUSB_INDEX, (uint8_t)EP_OUT);
    wr16(MUSB_RXMAXP, MT6592_GADGET_PACKET);
    wr8(MUSB_RXFIFOSZ, FIFOSZ_64);
    wr16(MUSB_RXFIFOADD, FIFOADD_EP_OUT);
    wr16(MUSB_RXCSR, (uint16_t)(RXCSR_FLUSHFIFO | RXCSR_CLRDATATOG));
    wr16(MUSB_RXCSR, 0u);

    wr8(MUSB_INDEX, 0u);
}

/* ── EP0 ────────────────────────────────────────────────────────────────────── */

static void fifo_write(uint32_t ep, const uint8_t* p, uint32_t n) {
    const uint32_t off = MUSB_FIFO0 + ep * MUSB_FIFO_STRIDE;
    while (n--) *r8(off) = *p++;
}

static void fifo_read(uint32_t ep, uint8_t* p, uint32_t n) {
    const uint32_t off = MUSB_FIFO0 + ep * MUSB_FIFO_STRIDE;
    while (n--) *p++ = *r8(off);
}

static void ep0_stall(void) {
    wr8(MUSB_INDEX, 0u);
    wr16(MUSB_CSR0, (uint16_t)(CSR0_SERVICED_RXPKTRDY | CSR0_SENDSTALL));
}

/* Single-packet control IN. Every descriptor this device owns is <= 64 bytes,
 * so there is no multi-packet path to get wrong. */
static void ep0_tx(const uint8_t* data, uint32_t len, uint32_t requested) {
    if (len > requested) len = requested;
    if (len > MT6592_GADGET_PACKET) len = MT6592_GADGET_PACKET;

    wr8(MUSB_INDEX, 0u);
    wr16(MUSB_CSR0, (uint16_t)CSR0_SERVICED_RXPKTRDY);
    fifo_write(0u, data, len);
    wr16(MUSB_CSR0, (uint16_t)(CSR0_TXPKTRDY | CSR0_DATAEND));

    for (uint32_t i = 0; i < EP0_SPIN_CAP; ++i) {
        if ((rd16(MUSB_CSR0) & CSR0_TXPKTRDY) == 0u) break;
    }
}

/* Status stage for a control transfer with no data. */
static void ep0_ack(void) {
    wr8(MUSB_INDEX, 0u);
    wr16(MUSB_CSR0, (uint16_t)(CSR0_SERVICED_RXPKTRDY | CSR0_DATAEND));
    /* Hardware clears DATAEND when the status phase completes; SET_ADDRESS must
     * not take effect before then, which is what g_pending_address is for. */
    for (uint32_t i = 0; i < EP0_SPIN_CAP; ++i) {
        if ((rd16(MUSB_CSR0) & CSR0_DATAEND) == 0u) break;
    }
    if (g_have_pending_address) {
        wr8(MUSB_FADDR, (uint8_t)g_pending_address);
        g_have_pending_address = 0u;
    }
}

static void ep0_handle_setup(const uint8_t* pkt) {
    const uint8_t bmRequestType = pkt[0];
    const uint8_t bRequest = pkt[1];
    const uint16_t wValue = (uint16_t)(pkt[2] | ((uint16_t)pkt[3] << 8));
    const uint16_t wLength = (uint16_t)(pkt[6] | ((uint16_t)pkt[7] << 8));
    const uint32_t to_host = (bmRequestType & 0x80u) != 0u;

    switch (bRequest) {
        case REQ_GET_DESCRIPTOR: {
            const uint8_t type = (uint8_t)(wValue >> 8);
            const uint8_t index = (uint8_t)(wValue & 0xffu);
            if (!to_host) break;
            if (type == DESC_DEVICE) {
                ep0_tx(kDeviceDesc, sizeof(kDeviceDesc), wLength);
                return;
            }
            if (type == DESC_CONFIG) {
                ep0_tx(kConfigDesc, sizeof(kConfigDesc), wLength);
                return;
            }
            if (type == DESC_STRING) {
                if (index == 0u) {
                    ep0_tx(kStringLang, sizeof(kStringLang), wLength);
                    return;
                }
                if (index == 1u) {
                    ep0_tx(kStringMfr, sizeof(kStringMfr), wLength);
                    return;
                }
                if (index == 2u) {
                    ep0_tx(kStringProd, sizeof(kStringProd), wLength);
                    return;
                }
            }
            /* Everything else -- device qualifier, other-speed config, OS
             * descriptors -- is legitimately stalled by a full-speed-only
             * device. */
            break;
        }
        case REQ_SET_ADDRESS:
            if (to_host) break;
            g_pending_address = wValue & 0x7fu;
            g_have_pending_address = 1u;
            ep0_ack();
            return;
        case REQ_SET_CONFIGURATION:
            if (to_host) break;
            if ((wValue & 0xffu) == 1u) {
                ep_configure();
                g_configured = 1u;
                glog("usb: gadget configured\n");
            } else {
                g_configured = 0u;
            }
            ep0_ack();
            return;
        case REQ_GET_CONFIGURATION: {
            const uint8_t cfg = (uint8_t)(g_configured ? 1u : 0u);
            if (!to_host) break;
            ep0_tx(&cfg, 1u, wLength);
            return;
        }
        case REQ_GET_STATUS: {
            static const uint8_t zero[2] = {0u, 0u};
            if (!to_host) break;
            ep0_tx(zero, sizeof(zero), wLength);
            return;
        }
        case REQ_GET_INTERFACE: {
            static const uint8_t alt = 0u;
            if (!to_host) break;
            ep0_tx(&alt, 1u, wLength);
            return;
        }
        case REQ_SET_INTERFACE:
            if (to_host) break;
            ep0_ack();
            return;
        case REQ_CLEAR_FEATURE:
        case REQ_SET_FEATURE:
            if (to_host) break;
            ep0_ack();
            return;
        default:
            break;
    }
    ep0_stall();
}

static void bus_reset(void) {
    g_configured = 0u;
    g_have_pending_address = 0u;
    /* Hardware has already zeroed FADDR. Endpoint sizing survives a reset, but
     * re-arming on SET_CONFIGURATION covers the case where it does not. */
    wr8(MUSB_INDEX, 0u);
}

void mt6592_usb_gadget_poll(void) {
    uint8_t intrusb;
    uint16_t csr0;

    if (!g_inited) return;

    /* Sample once and test every bit we care about from the copy. */
    intrusb = rd8(MUSB_INTRUSB);
    (void)rd16(MUSB_INTRTX);
    (void)rd16(MUSB_INTRRX);
    g_intrusb_seen |= intrusb;

    /* Write the sampled bits back. The board says the read alone does not clear
     * them: with the reset storm now gone (resets=1, one real bus reset) the raw
     * counter still climbs by exactly one per poll, so INTRUSB.RESET stays
     * latched at 1 forever after that single reset even though POWER.RESET reads
     * 0 and the bus is plainly idle. That is write-1-to-clear behaviour, not the
     * read-to-clear the Mentor documentation describes. Writing back exactly what
     * was read is safe either way: a genuinely read-to-clear register ignores the
     * write, and a W1C one clears precisely the bits just consumed and nothing
     * that arrived in between. */
    wr8(MUSB_INTRUSB, intrusb);

    /* Act on the 0->1 edge, not the level. With INTRUSBE now enabled the read
     * above should clear the bit and every assertion is already an edge, so
     * this is belt-and-braces -- but it is belt-and-braces that costs one AND
     * and makes the driver correct whether or not the core latches the way the
     * enable register says it should. The two counters are kept apart on
     * purpose: g_reset_raw counts the bit being seen and g_reset_count counts
     * resets actually acted on, so one heartbeat line says which it was. */
    {
        const uint8_t edges = (uint8_t)(intrusb & ~g_intrusb_prev);
        g_intrusb_prev = intrusb;

        if (intrusb & INTRUSB_RESET) ++g_reset_raw;
        if (edges & INTRUSB_RESET) {
            ++g_reset_count;
            bus_reset();
        }
        if (edges & INTRUSB_DISCON) g_configured = 0u;
    }

    wr8(MUSB_INDEX, 0u);
    csr0 = rd16(MUSB_CSR0);

    /* A premature status stage leaves SETUPEND set; it must be acknowledged or
     * EP0 never accepts another SETUP. */
    if (csr0 & CSR0_SETUPEND) {
        wr16(MUSB_CSR0, (uint16_t)CSR0_SERVICED_SETUPEND);
        csr0 = rd16(MUSB_CSR0);
    }
    if (csr0 & CSR0_SENTSTALL) {
        wr16(MUSB_CSR0, 0u);
        csr0 = rd16(MUSB_CSR0);
    }
    if ((csr0 & CSR0_RXPKTRDY) == 0u) return;

    {
        const uint16_t count = rd16(MUSB_COUNT0);
        uint8_t setup[8];
        if (count != 8u) {
            /* Not a SETUP packet: drain it and move on rather than mistaking
             * the bytes for a request. */
            uint8_t sink;
            for (uint16_t i = 0; i < count; ++i) fifo_read(0u, &sink, 1u);
            wr16(MUSB_CSR0, (uint16_t)CSR0_SERVICED_RXPKTRDY);
            return;
        }
        fifo_read(0u, setup, 8u);
        ++g_setup_count;
        ep0_handle_setup(setup);
    }
}

/* ── bulk ───────────────────────────────────────────────────────────────────── */

int mt6592_usb_gadget_configured(void) {
    return g_inited && g_configured;
}

void mt6592_usb_gadget_log_state(void) {
    if (!g_inited) {
        glog("usb: not initialised\n");
        return;
    }
    glog_hex("usb: POWER=", rd8(MUSB_POWER));
    glog_hex(" DEVCTL=", rd8(MUSB_DEVCTL));
    glog_hex(" FADDR=", rd8(MUSB_FADDR));
    glog_hex(" intrusb_seen=", g_intrusb_seen);
    glog_hex(" resets=", g_reset_count);
    glog_hex(" rawrst=", g_reset_raw);
    glog_hex(" setups=", g_setup_count);
    glog_hex(" cfg=", g_configured);
    glog("\n");
}

int mt6592_usb_gadget_send(const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sent = 0u;

    if (!g_inited || !g_configured) return -1;

    while (sent < len) {
        uint32_t chunk = len - sent;
        uint32_t i;
        if (chunk > MT6592_GADGET_PACKET) chunk = MT6592_GADGET_PACKET;

        /* Wait for the previous packet to leave. EP0 keeps being serviced while
         * we wait: the host may well be mid-control-transfer, and a console that
         * stops answering EP0 while it talks gets dropped off the bus. */
        for (i = 0; i < TX_SPIN_CAP; ++i) {
            wr8(MUSB_INDEX, (uint8_t)EP_IN);
            if ((rd16(MUSB_TXCSR) & TXCSR_TXPKTRDY) == 0u) break;
            if ((i & 0x3ffu) == 0u) mt6592_usb_gadget_poll();
            if (!g_configured) return -1;
        }
        if (i == TX_SPIN_CAP) return -1;

        wr8(MUSB_INDEX, (uint8_t)EP_IN);
        fifo_write(EP_IN, p + sent, chunk);
        wr16(MUSB_TXCSR, (uint16_t)(TXCSR_MODE | TXCSR_TXPKTRDY));
        sent += chunk;
    }

    /* Drain the last packet so a caller that immediately jumps to the kernel
     * does not strand it in the FIFO. */
    for (uint32_t i = 0; i < TX_SPIN_CAP; ++i) {
        wr8(MUSB_INDEX, (uint8_t)EP_IN);
        if ((rd16(MUSB_TXCSR) & TXCSR_TXPKTRDY) == 0u) break;
        if ((i & 0x3ffu) == 0u) mt6592_usb_gadget_poll();
    }
    return (int)sent;
}

int mt6592_usb_gadget_recv(void* data, uint32_t max) {
    uint16_t count;

    if (!g_inited || !g_configured) return -1;

    wr8(MUSB_INDEX, (uint8_t)EP_OUT);
    if ((rd16(MUSB_RXCSR) & RXCSR_RXPKTRDY) == 0u) return 0;

    count = rd16(MUSB_RXCOUNT);
    if (count > max) count = (uint16_t)max;
    fifo_read(EP_OUT, (uint8_t*)data, count);
    /* Clearing RXPKTRDY hands the FIFO back; MODE has no meaning on an RX-only
     * endpoint so the register goes to zero. */
    wr16(MUSB_RXCSR, 0u);
    return (int)count;
}

/* ── bring-up ───────────────────────────────────────────────────────────────── */

int mt6592_usb_gadget_init(void) {
    if (g_inited) return 0;

    /* Ungate the PERI clocks before the first MUSB access. On this SoC an APB
     * read of a gated peripheral does not fault, it hangs the bus, so this is
     * not optional and it has to happen first. Blunt (everything, not just the
     * USB bit) because the exact USB gate bit is unproven on this board and
     * ungating a clock is always safe for access -- the same choice, for the
     * same reason, that the MUSB host driver makes. */
    {
        volatile uint32_t* pdn_sta = (volatile uint32_t*)(uintptr_t)0x10003018u;
        volatile uint32_t* pdn_clr = (volatile uint32_t*)(uintptr_t)0x10003010u;
        glog_hex("usb: PERI_PDN0=", *pdn_sta);
        *pdn_clr = 0xffffffffu;
        glog_hex(" -> ", *pdn_sta);
        glog("\n");
    }

    phy_recover();

    /* Polled, but NOT masked -- and that distinction cost an enumeration.
     *
     * The obvious thing for a driver with no interrupt handler is to zero the
     * enable registers, and that is what this did. It does not work: on this
     * Mentor core the INTRUSB *status* bits are only latch-and-read-to-clear for
     * sources that are enabled. With INTRUSBE = 0 they instead track the raw bus
     * condition, so a read returns the live level and clears nothing.
     *
     * That is not a theory, it is what the board reported. The heartbeat counted
     * bus resets at 20001 / 25001 / 30001 across three 5 s samples -- exactly
     * +5000 per 5 s, one per iteration of a 1 ms poll loop. INTRUSB.RESET was
     * reading set on literally every poll, so bus_reset() ran ~1000 times a
     * second and zeroed g_configured faster than the host could ever set it.
     *
     * Enabling the sources makes the status register behave as read-to-clear.
     * Nothing is wired to the GIC and no CPU interrupts are unmasked here, so
     * this stays fully polled; INTRUSBE only governs latching. SOF is left
     * disabled deliberately -- at 1 kHz it is pure noise to a polled driver and
     * it is the one bit we never act on. */
    wr16(MUSB_INTRTXE, (uint16_t)(BIT(0) | BIT(EP_IN)));
    wr16(MUSB_INTRRXE, (uint16_t)BIT(EP_OUT));
    wr8(MUSB_INTRUSBE,
        (uint8_t)(INTRUSB_SUSPEND | INTRUSB_RESUME | INTRUSB_RESET | INTRUSB_DISCON));
    (void)rd16(MUSB_INTRTX);
    (void)rd16(MUSB_INTRRX);
    (void)rd8(MUSB_INTRUSB);
    g_intrusb_prev = 0u;

    /* Peripheral role: do NOT set DEVCTL.SESSION (that is the host's job) and
     * do NOT set HSENAB -- see the header on why this device is full speed by
     * choice. SOFTCONN is what actually attaches the D+ pull-up and makes the
     * host notice us.
     *
     * The detach comes first, and it is not defensive tidying -- it is the whole
     * reason the host ever looks at us. By the time this runs the port is already
     * enumerated: the BROM answered on it, then the preloader did, and the host
     * still holds that device's address and descriptors. Raising a pull-up that
     * is, as far as the hub is concerned, already raised changes nothing
     * observable, so the host never re-probes and the console is invisible on a
     * bus it is plainly attached to. Dropping D+ for longer than the 2.5 us
     * disconnect-detect window makes the hub report a real disconnect; the
     * re-attach after it is then a new device, and the host reads OUR
     * descriptors. 200 ms because hubs debounce port changes over roughly 100 ms
     * and a detach the hub debounces away is a detach that did not happen. */
    wr8(MUSB_FADDR, 0u);
    wr8(MUSB_POWER, (uint8_t)(rd8(MUSB_POWER) & ~POWER_SOFTCONN));
    udelay(200000u);
    wr8(MUSB_POWER, (uint8_t)((rd8(MUSB_POWER) & ~(POWER_HSENAB | POWER_ENSUSPEND)) |
                              POWER_SOFTCONN));
    wr8(MUSB_INDEX, 0u);

    g_inited = 1u;
    g_configured = 0u;

    glog_hex("usb: gadget attached, DEVCTL=", rd8(MUSB_DEVCTL));
    glog_hex(" POWER=", rd8(MUSB_POWER));
    glog("\n");
    return 0;
}

void mt6592_usb_gadget_shutdown(void) {
    if (!g_inited) return;
    wr8(MUSB_POWER, (uint8_t)(rd8(MUSB_POWER) & ~POWER_SOFTCONN));
    phy_savecurrent();
    g_inited = 0u;
    g_configured = 0u;
    glog("usb: gadget detached\n");
}
