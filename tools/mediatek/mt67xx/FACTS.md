# mt67xx LK facts

Every hardware value the LK builds with, its provenance, and what would
promote it from prior to fact. `build-info.txt` in each boot dir records
the values one image was built with; this file records why they are
believed. Strengths: STRONG (two witnesses or same-generation map),
MEDIUM (one witness or conventional), WEAK (conventional only).

| Fact | Value | Provenance | Strength | Promoted by |
|---|---|---|---|---|
| UART0 base | 0x11002000 | mt6735 AP_UART0_BASE + j36 MT6592 same | STRONG | Serial hello (step 2) |
| UART1-3 bases | 0x11003000/4000/5000 | mt6735 map + j36 same | STRONG | First use, if UART0 silent |
| UART clock | 26 MHz | mt6735 UART_SRC_CLK + j36 same | STRONG | Correct baud on hello |
| GPT base | 0x10004000 | mt6735 APXGPT_BASE | MEDIUM | Timer reports "gpt4" (or unused: arch wins) |
| GPT4 offsets/clock | +0x40/44/48, 13 MHz | mt6735 mt_gpt.h | MEDIUM | Same |
| GPT power bit | PERICFG+0x10 bit 13 | mt6735 mt_gpt.c | MEDIUM | Same |
| TOPRGU base | 0x10212000 | mt6735 map | MEDIUM | No reset loop (step 2) |
| WDT disable word | 0x22000000 | mt6735 platform.c:70-71 | MEDIUM | Same |
| MEMBASE | 0x41E00000 | mt6735 target MEMBASE | MEDIUM | Step 1 reads it from the stock LK |
| UBOOT slot size | 0x200000 | j36 slot size | WEAK | Step 1 reads the stock scatter |
| MSDC0 base | 0x11230000 | mt6735 map | MEDIUM | Step 3 (driver does not exist yet) |
| DRAM base | 0x40000000 | Shared OS prior | MEDIUM | OS BRINGUP step 2 (printed, not used) |
| Console UART index | 0 | MediaTek norm | MEDIUM | Step 2 (`-DMT67XX_DEBUG_UART=N` if silent) |
| Boot-status sector | Same format as j36 | j36 wrap script | FORMAT | Reader lands with bootstatus step |

Not facts yet (no values anywhere, drivers absent by design): eMMC
clock/pinmux, DSI/panel model + init, key input path, PMIC model,
kernel/ramdisk load addresses, LK framebuffer canvas. Each lands with
its LK-BRINGUP step; until then the LK refuses to need them.
