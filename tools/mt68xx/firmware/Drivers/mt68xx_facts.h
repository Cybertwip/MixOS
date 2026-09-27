/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* mt68xx LK platform facts: every hardware address this bootloader touches,
 * in the one place, each with its provenance. Nothing here was measured on
 * mt6833 silicon yet -- FACTS.md tracks what each value is waiting on, and
 * the boot banner prints the ones in force so a serial log names its own
 * assumptions.
 *
 * Provenance key:
 *   [mt6877]  Same-generation Dimensity map: device/oppo-mt6877/board/
 *             mt6877_addrs.h, parsed from the Dimensity 900 stock DTB. The
 *             Dimensity 810 (mt6833) shares the 2021 infra generation, so a
 *             base that matches here is the strongest ground short of the
 *             CPH2381 DTB itself.
 *   [mt6735]  Older-line witness: reference/lk mt6735 map (MIT, read only,
 *             reimplemented here). Weaker on Dimensity -- the infra moved
 *             between the lines (compare PWRAP below) -- so every [mt6735]
 *             value without an [mt6877] twin is WEAK here.
 *   [mt6592]  Oldest witness: the proven j36 tree runs this value on real
 *             MediaTek hardware (tools/mediatek/firmware).
 *   [conv]    MTK-conventional: stable across every line so far, but no
 *             Dimensity witness. Weakest; compiled OUT, never trusted.
 *   [todo]    No ground yet. Values marked [todo] are NOT in this header:
 *             their drivers do not exist until LK-BRINGUP lands the fact.
 *
 * Family rule: a second mt68xx SoC adds a row to this file, never a new
 * driver. Drivers read MT68XX_* and know no SoC by name.
 */
#pragma once

#include <stdint.h>

#ifndef MT68XX_SOC
#define MT68XX_SOC 6833
#endif

#if MT68XX_SOC != 6833
#error "mt68xx LK knows only mt6833 today; a second SoC adds a facts row, not a driver"
#endif

/* APB IO window [mt6735] IO_PHYS, corroborated [mt6877] (PWRAP, GPIO and
 * KP all live in 0x1000xxxx on Dimensity too). */
#define MT68XX_IO_PHYS 0x10000000u

/* The UART block [mt6877] UART0/1 + [mt6735] + [mt6592]: three witnesses
 * for 0/1, two for 2/3 (the mt6877 header has no UART2/3). Same UART IP
 * across all three ("mediatek,mt6577-uart" per the mt6877 header), so the
 * register layout is not in doubt either. */
#define MT68XX_UART0_BASE 0x11002000u
#define MT68XX_UART1_BASE 0x11003000u
#define MT68XX_UART2_BASE 0x11004000u
#define MT68XX_UART3_BASE 0x11005000u

/* Which UART the console talks on. 0 is the MediaTek norm (preloader and
 * stock LK log there) but the A77's wired port is unknown until OS BRINGUP
 * step 2 -- -DMT68XX_DEBUG_UART=1..3 rebuilds for the next candidate, and
 * build-info.txt records which one a boot dir holds. */
#ifndef MT68XX_DEBUG_UART
#define MT68XX_DEBUG_UART 0
#endif

#if MT68XX_DEBUG_UART == 0
#define MT68XX_DEBUG_UART_BASE MT68XX_UART0_BASE
#elif MT68XX_DEBUG_UART == 1
#define MT68XX_DEBUG_UART_BASE MT68XX_UART1_BASE
#elif MT68XX_DEBUG_UART == 2
#define MT68XX_DEBUG_UART_BASE MT68XX_UART2_BASE
#elif MT68XX_DEBUG_UART == 3
#define MT68XX_DEBUG_UART_BASE MT68XX_UART3_BASE
#else
#error "MT68XX_DEBUG_UART wants 0..3"
#endif

/* UART source clock [mt6735] UART_SRC_CLK; [mt6592] same 26 MHz; same
 * UART IP on Dimensity per the mt6877 header. The init sequence enables no
 * clock gates: like the j36 and the mt6735 reference, this assumes the
 * preloader left the UART clocked, which it must have -- it just logged
 * through it. */
#define MT68XX_UART_CLOCK_HZ 26000000u

#ifndef MT68XX_UART_BAUD
#define MT68XX_UART_BAUD 115200u
#endif

/* Transmit FIFO depth assumed when running lossy. Sixteen is the 16550
 * figure and the floor for every MediaTek part; guessing low only costs
 * throughput. ([mt6592] rationale, unchanged.) */
#ifndef MT68XX_UART_TX_FIFO_DEPTH
#define MT68XX_UART_TX_FIFO_DEPTH 16u
#endif

/* General-purpose timer: UNGROUNDED on Dimensity, compiled OUT. The
 * values below are the [mt6735] map kept so the header reads whole, but
 * the infra moved between the lines -- [mt6877] puts PWRAP at 0x10026000
 * against mt6735's 0x10001000, so its PERICFG power bit cannot be trusted
 * either, and a power-bit write to the wrong address is clock chaos, not a
 * clean miss. HAS_GPT 0 drops the whole attempt; the ARM arch timer (no
 * SoC facts) carries the clock, software is the fallback. LK-BRINGUP step
 * 2c grounds GPT (stock LK init or the DTB timer node) and flips this. */
#define MT68XX_HAS_GPT 0
#define MT68XX_APXGPT_BASE 0x10004000u
#define MT68XX_GPT4_CON (MT68XX_APXGPT_BASE + 0x40u)
#define MT68XX_GPT4_CLK (MT68XX_APXGPT_BASE + 0x44u)
#define MT68XX_GPT4_DAT (MT68XX_APXGPT_BASE + 0x48u)
#define MT68XX_GPT_SYS_HZ 13000000u
#define MT68XX_GPT_TICKS_PER_US 13u
#define MT68XX_PERICFG_BASE 0x10002000u
#define MT68XX_GPT_PDN_BIT 13u

/* Watchdog: UNGROUNDED on Dimensity, compiled OUT. The TOPRGU base and
 * the 0x22000000 disable word are the [mt6735] pair, but TOPRGU already
 * moved once between mt6592 (0x10007000) and mt6735 (0x10212000), so on
 * Dimensity this write is a guess at a reset-adjacent register -- worse
 * than the diagnosable reset loop it avoids. HAS_WDT 0 is a no-op; expect
 * hello-then-reset until LK-BRINGUP step 2b reads the real base out of
 * the stock LK (it writes this same word somewhere) and flips this. */
#define MT68XX_TOPRGU_BASE 0x10212000u
#define MT68XX_WDT_DISABLE_KEY 0x22000000u
#define MT68XX_HAS_WDT 0

/* Where the preloader loads this image and branches to it: [mt6735] target
 * MEMBASE as a WEAK older-line prior (Dimensity DRAM layout differs, and
 * nothing corroborates this address on the 2021 infra). Preloader-defined
 * per device, so this is the load-bearing prior: LK-BRINGUP step 1 reads
 * it back out of the stock LK image (its vectors resolve to absolute
 * addresses) before anything is flashed. The linker script takes the same
 * value from CMake, so the two cannot disagree. */
#ifndef MT68XX_MEMBASE
#define MT68XX_MEMBASE 0x41E00000u
#endif

/* Image budget inside the UBOOT slot. 512 KiB like the j36: the v1 image
 * is tens of KiB, and the handoff step revisits the ceiling once the
 * kernel/ramdisk/framebuffer map exists. */
#define MT68XX_IMAGE_BUDGET (512u * 1024u)

/* eMMC controller [mt6877] MSDC0_BASE + [mt6735] twin: same-generation
 * witness, so this base is STRONG already. Declared so the map reads
 * whole; NOTHING reads it yet -- the MSDC driver is LK-BRINGUP step 3,
 * waiting on clock/pinmux facts, and touching the controller before then
 * would be the invented-address kind of bug. */
#define MT68XX_MSDC0_BASE 0x11230000u

/* DRAM base [shared OS prior MTK_DRAM_BASE_PRIOR]. The LK never sizes
 * memory (the preloader trained it); this is printed, not used. */
#define MT68XX_DRAM_BASE_PRIOR 0x40000000u
