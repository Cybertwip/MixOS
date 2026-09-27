/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* mt68xx UART implementation. Derived from mt6592_uart.c: identical init
 * (DLAB divisor off the 26 MHz source clock, 8N1, FIFOs on) and identical
 * blocking/lossy transmit. Divisor rounding matches the mt6735 reference
 * (round-to-nearest on the remainder) rather than truncating.
 */
#include "mt68xx_uart.h"

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
    return (volatile uint32_t*)(uintptr_t)(MT68XX_DEBUG_UART_BASE + offset);
}

static void uart_write(uint32_t offset, uint32_t value) {
    *uart_reg(offset) = value;
}

static uint32_t uart_read(uint32_t offset) {
    return *uart_reg(offset);
}

static int g_lossy;
/* Bytes that may be pushed before LSR has to be consulted again. THRE with
 * the FIFO enabled means the transmit FIFO is empty, so seeing it once buys
 * a whole FIFO's worth of writes. */
static uint32_t g_tx_room;

void mt68xx_uart_init(void) {
    /* Round-to-nearest like the mt6735 reference: the truncation j36 uses
     * is 0.2% off at 26 MHz / 115200 and nobody noticed, but the correct
     * divisor is free. */
    const uint32_t step = 16u * MT68XX_UART_BAUD;
    uint32_t divisor = MT68XX_UART_CLOCK_HZ / step;

    if (MT68XX_UART_CLOCK_HZ % step >= step / 2u) ++divisor;
    uart_write(UART_IER, 0x00);
    uart_write(UART_LCR, 0x80);
    uart_write(UART_DLL, divisor & 0xffu);
    uart_write(UART_DLH, (divisor >> 8) & 0xffu);
    uart_write(UART_LCR, 0x03);
    uart_write(UART_FCR, 0x47);
    g_tx_room = 0u;
}

void mt68xx_uart_set_lossy(int enabled) {
    g_lossy = enabled ? 1 : 0;
    g_tx_room = 0u;
}

void mt68xx_uart_putc(char c) {
    if (c == '\n') {
        mt68xx_uart_putc('\r');
    }
    if (g_lossy) {
        if (g_tx_room == 0u) {
            if ((uart_read(UART_LSR) & (1u << 5)) == 0) {
                return; /* still draining; drop rather than stall the frame */
            }
            g_tx_room = MT68XX_UART_TX_FIFO_DEPTH;
        }
        --g_tx_room;
    } else {
        while ((uart_read(UART_LSR) & (1u << 5)) == 0) {
        }
    }
    uart_write(UART_THR, (uint32_t)(uint8_t)c);
}

void mt68xx_uart_puts(const char* s) {
    if (!s) return;
    while (*s) {
        mt68xx_uart_putc(*s++);
    }
}

void mt68xx_uart_put_hex32(uint32_t value) {
    static const char digits[] = "0123456789abcdef";
    mt68xx_uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        mt68xx_uart_putc(digits[(value >> (uint32_t)shift) & 0xfu]);
    }
}

void mt68xx_uart_put_hex64(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    mt68xx_uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        mt68xx_uart_putc(digits[(value >> (uint32_t)shift) & 0xfu]);
    }
}

void mt68xx_uart_put_dec(uint64_t value) {
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
            mt68xx_uart_putc((char)('0' + digit));
        }
    }
}
