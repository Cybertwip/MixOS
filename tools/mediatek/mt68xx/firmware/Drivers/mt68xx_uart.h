/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* mt68xx UART: the 16550-compatible MediaTek port, derived from
 * tools/mediatek/mt65xx/firmware/Drivers/mt6592_uart.h. Same register layout
 * (stable MediaTek IP [mt6877][mt6735][mt6592]), same divisor math, same lossy
 * mode. The character tap is gone: its only consumer was the j36 USB
 * console, and this tree has no second console to mirror to.
 */
#pragma once

#include <stdint.h>

#include "mt68xx_facts.h"

void mt68xx_uart_init(void);

/* Lossy mode: drop characters the transmit FIFO has no room for instead
 * of waiting. Off during boot, where a dropped probe line costs a flash
 * cycle to recover; on once the menu spins, where the blocking wait is
 * ~87 us per byte at 115200 -- a hundred-character line is half a 60 Hz
 * frame spent stalling on a port that may have nothing attached. */
void mt68xx_uart_set_lossy(int enabled);

void mt68xx_uart_putc(char c);
void mt68xx_uart_puts(const char* s);
void mt68xx_uart_put_hex32(uint32_t value);
void mt68xx_uart_put_hex64(uint64_t value);
void mt68xx_uart_put_dec(uint64_t value);
