/*
 * dsi_drv.h — MT6592 DSI host + MIPITX D-PHY (J36 Ultra / JD9365)
 *
 * Faithful port of the vendor kernel dsi_drv.c. The public entry points mirror
 * the stock function names and run the genuine register sequences (read-modify-
 * write on the typed register structs), parameterised by a panel-params struct
 * that replaces the kernel's LCM_PARAMS.
 */
#ifndef MT6592_DSI_DRV_H
#define MT6592_DSI_DRV_H

#include <stdint.h>

/* DSI_MODE_CTRL.MODE values (dsi_reg.h DSI_MODE_CTRL enum). */
#define DSI_CMD_MODE            0
#define DSI_SYNC_PULSE_VDO_MODE 1
#define DSI_SYNC_EVENT_VDO_MODE 2
#define DSI_BURST_VDO_MODE      3

/* DSI_PS_Control ps_type (dsi_reg.h DSI_PS_TYPE enum). */
#define PACKED_PS_16BIT_RGB565  0
#define LOOSELY_PS_18BIT_RGB666 1
#define PACKED_PS_24BIT_RGB888  2
#define PACKED_PS_18BIT_RGB666  3

/* ── Genuine dsi_drv.c entry points (stock names) ── */
void DSI_PowerOn(void);          /* enable DSI engine/digital clocks */
void DSI_Reset(void);
void DSI_SetMode(unsigned int mode);
void DSI_PHY_clk_setting(void);  /* MIPITX D-PHY PLL (RMW bitfields) */
void DSI_PHY_TIMCONFIG(void);    /* DSI host D-PHY HS timing */
void DSI_TXRX_Control(void);
void DSI_PS_Control(void);
void DSI_Config_VDO_Timing(void);
void DSI_Set_VM_CMD(void);
void DSI_clk_HS_mode(int enter);
void DSI_EnableClk(void);
void DSI_Start(void);

/* ── DCS panel program (command mode) ── */
int  DSI_dcs_write_short(uint8_t cmd, int has_param, uint8_t param);
int  DSI_dcs_write_long(uint8_t cmd, const uint8_t *params, uint32_t count);
int  mt6592_dsi_panel_init(void);   /* Reference/J36-ULTRA JD9365 init */
int  mt6592_dsi_jd9365_stock_init(void);
int  mt6592_dsi_jd9365_stock_init_range(uint32_t start, uint32_t end);
uint32_t mt6592_dsi_jd9365_last_index(void);
uint32_t mt6592_dsi_jd9365_stock_count(void);
int  mt6592_dsi_jd9365_force_rgb888_video_format(void);
int  mt6592_dsi_generic_lcd30_init(void);
int  mt6592_dsi_fill_rect_rgb888(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                 uint8_t r, uint8_t g, uint8_t b);
int  mt6592_dsi_fill_rect_rgb565(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                 uint16_t color);

/* Read back 12 key MIPITX-PLL + DSI-host registers (live clock-state probe). */
void mt6592_dsi_readback(uint32_t out[12]);

/* How many packets so far left DSI_INTSTA.BUSY set after their START pulse.
 * The transport does not wait on that bit — see dsi_send_cmdq() for why — so
 * this is the only evidence of whether the host is completing transactions.
 * Near zero: healthy. Equal to the packet count: the bit is wedged, which on
 * this board it is, and the panel is being programmed anyway. */
uint32_t mt6592_dsi_busy_stalls(void);

/* DSI built-in self-test: drive a solid colour out of the link without the
 * OVL/RDMA datapath. 1:1 port of Reference/J36-ULTRA/src/lcd.c
 * dsi_selftest_colour(): writes the standard MT6592 DSI BIST_CON/BIST_PATTERN
 * registers and pulses the video-mode start bit. `colour` is 0x00RRGGBB.
 *
 * These BIST offsets (+0xb0 / +0xf0) are NOT used by stock LK; they are the
 * standard MT6592 DSI smoke-test registers noted in the reference port. Use as a
 * datapath-independent "is the panel link alive" diagnostic. */
void mt6592_dsi_bist_solid_color(uint32_t colour);

/* ── High-level bring-up (stock init_dsi order) ──
 * Sequence: mt6592_dsi_video_setup() -> mt6592_dsi_panel_init() (DCS in LP) ->
 * mt6592_dsi_video_start() -> DDP route (lcd_drv) -> mt6592_dsi_video_kick(). */
void mt6592_dsi_phy_pll_on(void);   /* DSI_PHY_clk_setting + DSI_PHY_TIMCONFIG */
void mt6592_dsi_video_setup(void);  /* PLL+Init+TXRX+TIMCONFIG+PS+VDO timing+VM_CMD, leaves LP */
void mt6592_dsi_video_start(void);  /* DSI_SetMode(video) + clock-lane HS + DSI_Start, before dsi_config_ddp */
void mt6592_dsi_video_kick(void);   /* DSI_Start again once the DDP path is assembled */

#endif /* MT6592_DSI_DRV_H */
