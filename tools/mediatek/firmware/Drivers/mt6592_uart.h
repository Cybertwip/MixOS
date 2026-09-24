#pragma once

#include <stdint.h>

#define MT6592_UART0_BASE 0x11002000u
#define MT6592_UART1_BASE 0x11003000u
#define MT6592_UART2_BASE 0x11004000u
#define MT6592_UART3_BASE 0x11005000u

#ifndef MT6592_DEBUG_UART_BASE
#define MT6592_DEBUG_UART_BASE MT6592_UART0_BASE
#endif

#ifndef MT6592_UART_CLOCK_HZ
#define MT6592_UART_CLOCK_HZ 26000000u
#endif

#ifndef MT6592_UART_BAUD
#define MT6592_UART_BAUD 115200u
#endif

/* Transmit FIFO depth assumed when running lossy. Sixteen is the 16550 figure
 * and the floor for every MediaTek part; guessing low only costs throughput. */
#ifndef MT6592_UART_TX_FIFO_DEPTH
#define MT6592_UART_TX_FIFO_DEPTH 16u
#endif

void mt6592_uart_init(void);

/*
 * Lossy mode: drop characters the transmit FIFO has no room for instead of
 * waiting. Off during boot, where a dropped line of the probe log costs a whole
 * flash cycle to recover. On once the shell is live, where the blocking wait is
 * ~87us per byte at 115200 with the FIFO fully drained between each one -- one
 * hundred-character log line is half a 60Hz frame, spent stalling the CPU on a
 * port that may well have nothing attached to it. Nothing is actually lost from
 * the log read back over `flash -mtk-read-boot-status`: the console ring in
 * mt6592_bootstatus.c still receives every character.
 */
void mt6592_uart_set_lossy(int enabled);

/*
 * ── THE TAP: EVERY UART LINE, ON A BOARD WITH NO SERIAL CABLE ──
 *
 * The MediaTek drivers on this board trace to the UART, because that is what a
 * MediaTek driver does. Nobody working on it has a serial cable; the instrument
 * is a USB console. So the entire WMT bootstrap -- which frame the connectivity
 * MCU answered, which it did not, why the RF calibration is reported as
 * `calibrated=no' -- is written down in full and then thrown away, and the only
 * thing that survives to the console is a one-word summary that cannot name the
 * frame that failed.
 *
 * Install a tap and it survives. Characters on their way to the port are
 * assembled into lines here and handed to the sink, so the console prints the
 * driver's own trace verbatim.
 *
 * Assembled at the character level, not by wrapping puts(), because
 * put_hex32/put_dec push digits straight through putc() and a puts()-level tap
 * would drop every number. Handed over a line at a time, not a character at a
 * time, because the sink is a packet transport: one USB transfer per character
 * would be slower than the 115200 line being mirrored.
 *
 * The sink must not itself trace to the UART -- a recursion guard suppresses the
 * re-entry rather than hanging, so such a sink silently loses its own output.
 * NULL removes the tap.
 */
void mt6592_uart_set_tap(void (*sink)(const char* line));

/*
 * Write to the port with the tap suppressed. For the one caller that is itself
 * the tap's destination: a console whose own output came back round through the
 * mirror would print every line twice.
 */
void mt6592_uart_puts_untapped(const char* s);

void mt6592_uart_putc(char c);
void mt6592_uart_puts(const char* s);
void mt6592_uart_put_hex32(uint32_t value);
void mt6592_uart_put_hex64(uint64_t value);
void mt6592_uart_put_dec(uint64_t value);
