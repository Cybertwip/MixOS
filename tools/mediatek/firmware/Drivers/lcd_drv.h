/*
 * lcd_drv.h — MT6592 LCD / DDP display path (J36 Ultra)
 *
 * Surgical extraction of the MediaTek lcd_drv.c / disp_drv DDP path: display
 * MTCMOS power domain, MMSYS/INFRA clock gates, and the OVL -> RDMA -> COLOR/BLS
 * -> DSI video scanout pipeline. Linux deps removed.
 */
#ifndef MT6592_LCD_DRV_H
#define MT6592_LCD_DRV_H

#include <stdint.h>

/* Power up the display MTCMOS domain and ungate the INFRA/MMSYS/DISP clocks
 * (plus the cold MIPI reference PLL unless MVII_MT6592_WARM_LK). */
void mt6592_lcd_clocks_on(void);

/* Bring up the OVL/RDMA/COLOR/BLS DDP route and the DSI video engine, then
 * commit the DDP mutex so frames scan out continuously.
 *
 *   fb_addr     : ARGB8888 framebuffer physical address (layer 0 source).
 *   solid_color : if non-zero, the OVL outputs this ARGB colour from its ROI
 *                 background with NO DRAM reads (layer 0 stays disabled) — use
 *                 0xFFFFFFFF to paint the whole panel white without touching
 *                 DRAM. If zero, layer 0 reads the framebuffer at fb_addr.
 *
 * Returns 0 on success (the mutex SOF poll is advisory and non-fatal). */
int mt6592_lcd_scanout(uint32_t fb_addr, uint32_t solid_color);

/* Same DDP video path, but layer 0 reads a small RGB888 surface. This is meant
 * for live stage1 first-light tests before DRAM is trained; the surface may
 * live in the payload SRAM. The rest of the panel is filled by bg_color. */
int mt6592_lcd_scanout_rgb888_patch(uint32_t fb_addr, uint32_t width,
                                    uint32_t height, uint32_t pitch,
                                    uint32_t bg_color);

/* J36 Ultra stock LK uses a 640x480 RGB565 overlay surface with 0x500 pitch on
 * layer 3 during the UBOOT/LK handoff. This helper programs that same layer so
 * the LK-slot replacement uses the proven visible scanout path. */
int mt6592_lcd_scanout_lk_rgb565_layer3(uint32_t fb_addr, uint32_t pitch,
                                        uint32_t bg_color);

/* Scan a 640x480 ARGB8888 DRAM framebuffer out on OVL layer 2 — the exact layer
 * the stock J36 Ultra LK uses for its boot framebuffer (lk.bin decompile). */
int mt6592_lcd_scanout_lk_argb8888_layer2(uint32_t fb_addr, uint32_t pitch,
                                          uint32_t bg_color);

/* Read back the live DDP route/state after scanout setup. The per-layer words
 * describe whichever layer the last scanout call configured (word [8]), not
 * layer 0 — the J36 video route runs on layer 2:
 *   [0] MMSYS OVL0_MOUT_EN
 *   [1] MMSYS DISP_OUT_SEL
 *   [2] OVL0_EN
 *   [3] OVL0_SRC_CON
 *   [4] RDMA0_GLOBAL_CON
 *   [5] OVL0_ROI_BGCLR
 *   [6] MUTEX0_MOD
 *   [7] BLS_EN
 *   [8] configured OVL layer index
 *   [9] OVL0_Ln_CON
 *   [10] OVL0_Ln_ADDR
 *   [11] OVL0_Ln_PITCH
 *   [12] OVL0_Ln_SRC_SIZE
 *   [13] OVL0_Ln_RDMA_CTRL
 *   [14] MUTEX0
 *   [15] MUTEX0_SOF
 */
void mt6592_lcd_readback(uint32_t out[16]);

/* Fault-guarded MMIO. A read or write to an address whose block is unclocked or
 * unmapped data-aborts; these arm the stage1 entry abort trampoline first, so the
 * access returns 0 instead of taking the payload down with it. Read stores
 * 0xdab0da7a on fault. Both return 1 on success, 0 if the access faulted.
 *
 * Exported for the live MVIIFlash console's peek/poke, where the whole point is
 * to let a person type an address that might not be there. */
int mt6592_mmio_safe_read32(uint32_t addr, uint32_t *out);
int mt6592_mmio_safe_write32(uint32_t addr, uint32_t value);

/* Clean (writeback) a CPU framebuffer range to PoC so the OVL/RDMA reads see
 * the painted pixels. Posted writeback, no dsb drain (see notes in the .c). */
void mt6592_lcd_clean_fb(uintptr_t start, uintptr_t len);

/* Clean a payload-SRAM framebuffer and drain the writeback before handing it to
 * OVL. Unlike the DRAM helper above, this is safe before MEMPLL training. */
void mt6592_lcd_clean_sram_fb(uintptr_t start, uintptr_t len);

#endif /* MT6592_LCD_DRV_H */
