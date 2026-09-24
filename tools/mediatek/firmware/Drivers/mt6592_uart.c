#include "mt6592_uart.h"

enum {
    UART_RBR = 0x00,
    UART_THR = 0x00,
    UART_DLL = 0x00,
    UART_IER = 0x04,
    UART_DLH = 0x04,
    UART_FCR = 0x08,
    UART_LCR = 0x0c,
    UART_LSR = 0x14,
};

static volatile uint32_t* uart_reg(uint32_t offset) {
    return (volatile uint32_t*)(uintptr_t)(MT6592_DEBUG_UART_BASE + offset);
}

static void uart_write(uint32_t offset, uint32_t value) {
    *uart_reg(offset) = value;
}

static uint32_t uart_read(uint32_t offset) {
    return *uart_reg(offset);
}

static int g_lossy;
/* Bytes that may be pushed before LSR has to be consulted again. THRE with the
 * FIFO enabled means the transmit FIFO is empty, so seeing it once buys a whole
 * FIFO's worth of writes -- the depth the blocking path throws away by
 * re-waiting for an empty FIFO before every single byte. */
static uint32_t g_tx_room;

/* The tap. See mt6592_uart.h for why it exists and why it sits at the character
 * level. 192 matches the console's own line buffer, so a line that fits there
 * fits here; anything longer is broken rather than truncated, because a WMT
 * trace with its tail cut off is the half that names the failure. */
#define UART_TAP_LINE 192u
static void (*g_tap)(const char* line);
static char g_tap_line[UART_TAP_LINE];
static uint32_t g_tap_len;
static int g_tap_busy;

static void tap_char(char c) {
    void (*sink)(const char*) = g_tap;

    if (!sink || g_tap_busy) return;
    /* The CR belongs to the port, not to the text: putc synthesises it, and a
     * sink that is a USB console does not want it. */
    if (c == '\r') return;
    if (c != '\n' && g_tap_len + 2u < UART_TAP_LINE) {
        g_tap_line[g_tap_len++] = c;
        return;
    }
    if (c != '\n') g_tap_line[g_tap_len++] = c;
    g_tap_line[g_tap_len++] = '\n';
    g_tap_line[g_tap_len] = '\0';
    g_tap_len = 0u;
    g_tap_busy = 1;
    sink(g_tap_line);
    g_tap_busy = 0;
}

void mt6592_uart_set_tap(void (*sink)(const char* line)) {
    g_tap = sink;
    g_tap_len = 0u;
    g_tap_busy = 0;
}

void mt6592_uart_init(void) {
    const uint32_t divisor = MT6592_UART_CLOCK_HZ / (16u * MT6592_UART_BAUD);

    uart_write(UART_IER, 0x00);
    uart_write(UART_LCR, 0x80);
    uart_write(UART_DLL, divisor & 0xffu);
    uart_write(UART_DLH, (divisor >> 8) & 0xffu);
    uart_write(UART_LCR, 0x03);
    uart_write(UART_FCR, 0x47);
    g_tx_room = 0u;
}

void mt6592_uart_set_lossy(int enabled) {
    g_lossy = enabled ? 1 : 0;
    g_tx_room = 0u;
}

void mt6592_uart_putc(char c) {
    /* Before the port, and unconditionally: in lossy mode the hardware below may
     * drop this character, and the tap's destination is the more reliable of the
     * two. What the driver meant to say should not depend on FIFO room. */
    tap_char(c);
    if (c == '\n') {
        mt6592_uart_putc('\r');
    }
    if (g_lossy) {
        if (g_tx_room == 0u) {
            if ((uart_read(UART_LSR) & (1u << 5)) == 0) {
                return; /* still draining; drop rather than stall the frame */
            }
            g_tx_room = MT6592_UART_TX_FIFO_DEPTH;
        }
        --g_tx_room;
    } else {
        while ((uart_read(UART_LSR) & (1u << 5)) == 0) {
        }
    }
    uart_write(UART_THR, (uint32_t)(uint8_t)c);
}

void mt6592_uart_puts(const char* s) {
    if (!s) return;
    while (*s) {
        mt6592_uart_putc(*s++);
    }
}

void mt6592_uart_puts_untapped(const char* s) {
    const int saved = g_tap_busy;

    g_tap_busy = 1;
    mt6592_uart_puts(s);
    g_tap_busy = saved;
}

void mt6592_uart_put_hex32(uint32_t value) {
    static const char digits[] = "0123456789abcdef";
    mt6592_uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        mt6592_uart_putc(digits[(value >> (uint32_t)shift) & 0xfu]);
    }
}

void mt6592_uart_put_hex64(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    mt6592_uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        mt6592_uart_putc(digits[(value >> (uint32_t)shift) & 0xfu]);
    }
}

void mt6592_uart_put_dec(uint64_t value) {
    static const uint64_t powers[] = {
        10000000000000000000ull,
        1000000000000000000ull,
        100000000000000000ull,
        10000000000000000ull,
        1000000000000000ull,
        100000000000000ull,
        10000000000000ull,
        1000000000000ull,
        100000000000ull,
        10000000000ull,
        1000000000ull,
        100000000ull,
        10000000ull,
        1000000ull,
        100000ull,
        10000ull,
        1000ull,
        100ull,
        10ull,
        1ull,
    };
    int started = 0;
    for (uint32_t i = 0; i < sizeof(powers) / sizeof(powers[0]); ++i) {
        uint8_t digit = 0;
        while (value >= powers[i]) {
            value -= powers[i];
            ++digit;
        }
        if (digit != 0 || started || i + 1u == sizeof(powers) / sizeof(powers[0])) {
            started = 1;
            mt6592_uart_putc((char)('0' + digit));
        }
    }
}
