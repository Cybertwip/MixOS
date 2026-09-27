/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* mt67xx LK platform facts: every hardware address this bootloader touches,
 * in the one place, each with its provenance. Nothing here was measured on
 * mt6739 silicon yet -- FACTS.md tracks what each value is waiting on, and
 * the boot banner prints the ones in force so a serial log names its own
 * assumptions.
 *
 * Provenance key:
 *   [mt6735]  reference/lk mt6735 map. Same infra generation (the mt6735 /
 *             mt6737 / mt6739 line shares its APB layout); the strongest
 *             ground short of the stock DTB. MIT-licensed reference, read
 *             only, reimplemented here.
 *   [mt6592]  Second witness: the proven j36 tree runs this value on real
 *             MediaTek hardware (tools/mediatek/firmware).
 *   [todo]    No ground yet. Values marked [todo] are NOT in this header:
 *             their drivers do not exist until LK-BRINGUP lands the fact.
 *
 * Family rule: a second mt67xx SoC adds a row to this file, never a new
 * driver. Drivers read MT67XX_* and know no SoC by name.
 */
#pragma once

#include <stdint.h>

#ifndef MT67XX_SOC
#define MT67XX_SOC 6739
#endif

#if MT67XX_SOC != 6739
#error "mt67xx LK knows only mt6739 today; a second SoC adds a facts row, not a driver"
#endif

/* APB IO window [mt6735] IO_PHYS. */
#define MT67XX_IO_PHYS 0x10000000u

/* The UART block [mt6735] AP_UART0_BASE..3; [mt6592] identical on the j36,
 * so this base has two independent witnesses. */
#define MT67XX_UART0_BASE 0x11002000u
#define MT67XX_UART1_BASE 0x11003000u
#define MT67XX_UART2_BASE 0x11004000u
#define MT67XX_UART3_BASE 0x11005000u

/* Which UART the console talks on. 0 is the MediaTek norm (preloader and
 * stock LK log there) but the K20's wired port is unknown until OS BRINGUP
 * step 2 -- -DMT67XX_DEBUG_UART=1..3 rebuilds for the next candidate, and
 * build-info.txt records which one a boot dir holds. */
#ifndef MT67XX_DEBUG_UART
#define MT67XX_DEBUG_UART 0
#endif

#if MT67XX_DEBUG_UART == 0
#define MT67XX_DEBUG_UART_BASE MT67XX_UART0_BASE
#elif MT67XX_DEBUG_UART == 1
#define MT67XX_DEBUG_UART_BASE MT67XX_UART1_BASE
#elif MT67XX_DEBUG_UART == 2
#define MT67XX_DEBUG_UART_BASE MT67XX_UART2_BASE
#elif MT67XX_DEBUG_UART == 3
#define MT67XX_DEBUG_UART_BASE MT67XX_UART3_BASE
#else
#error "MT67XX_DEBUG_UART wants 0..3"
#endif

/* UART source clock [mt6735] UART_SRC_CLK; [mt6592] same 26 MHz. The init
 * sequence enables no clock gates: like the j36 and the mt6735 reference,
 * this assumes the preloader left the UART clocked, which it must have --
 * it just logged through it. */
#define MT67XX_UART_CLOCK_HZ 26000000u

#ifndef MT67XX_UART_BAUD
#define MT67XX_UART_BAUD 115200u
#endif

/* Transmit FIFO depth assumed when running lossy. Sixteen is the 16550
 * figure and the floor for every MediaTek part; guessing low only costs
 * throughput. ([mt6592] rationale, unchanged.) */
#ifndef MT67XX_UART_TX_FIFO_DEPTH
#define MT67XX_UART_TX_FIFO_DEPTH 16u
#endif

/* General-purpose timer [mt6735] APXGPT_BASE + mt_gpt.h offsets. GPT4 runs
 * the menu clock at 13 MHz sys clock, powered by PERICFG bit 13
 * ([mt6735] mt_gpt.c gpt_power_on). Second choice after the ARM arch
 * timer, which needs no SoC facts at all -- see mt67xx_timer.c. */
#define MT67XX_APXGPT_BASE 0x10004000u
#define MT67XX_GPT4_CON (MT67XX_APXGPT_BASE + 0x40u)
#define MT67XX_GPT4_CLK (MT67XX_APXGPT_BASE + 0x44u)
#define MT67XX_GPT4_DAT (MT67XX_APXGPT_BASE + 0x48u)
#define MT67XX_GPT_SYS_HZ 13000000u
#define MT67XX_GPT_TICKS_PER_US 13u
#define MT67XX_PERICFG_BASE 0x10002000u
#define MT67XX_GPT_PDN_BIT 13u

/* Watchdog [mt6735] platform.c:70-71 writes 0x22000000 to TOPRGU_BASE to
 * disable it. The K20 LK parks with a heartbeat instead of kicking, so a
 * live watchdog would reset-loop the board -- this write is what makes
 * silence mean "UART wrong" rather than "resetting too fast to print". */
#define MT67XX_TOPRGU_BASE 0x10212000u
#define MT67XX_WDT_DISABLE_KEY 0x22000000u

/* Where the preloader loads this image and branches to it [mt6735] target
 * MEMBASE. Preloader-defined per device, so this is the load-bearing prior:
 * LK-BRINGUP step 1 reads it back out of the stock LK image (its vectors
 * resolve to absolute addresses) before anything is flashed. The linker
 * script takes the same value from CMake, so the two cannot disagree. */
#ifndef MT67XX_MEMBASE
#define MT67XX_MEMBASE 0x41E00000u
#endif

/* Image budget inside the UBOOT slot. 512 KiB like the j36: the v1 image
 * is tens of KiB, and the handoff step revisits the ceiling once the
 * kernel/ramdisk/framebuffer map exists. */
#define MT67XX_IMAGE_BUDGET (512u * 1024u)

/* eMMC controller [mt6735] MSDC0_BASE. Declared so the map reads whole;
 * NOTHING reads it yet -- the MSDC driver is LK-BRINGUP step 3, waiting
 * on clock/pinmux facts, and touching the controller before then would be
 * the invented-address kind of bug. */
#define MT67XX_MSDC0_BASE 0x11230000u

/* DRAM base [shared OS prior MTK_DRAM_BASE_PRIOR]. The LK never sizes
 * memory (the preloader trained it); this is printed, not used. */
#define MT67XX_DRAM_BASE_PRIOR 0x40000000u
