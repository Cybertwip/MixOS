# mt67xx LK facts

Every hardware value the LK builds with, its provenance, and what would
promote it from prior to fact. `build-info.txt` in each boot dir records
the values one image was built with; this file records why they are
believed. Strengths: STRONG (two witnesses or same-generation map),
MEDIUM (one witness or conventional), WEAK (conventional only), OUT
(compiled out for that SoC until the grounding step).

| Fact | Value | Provenance | 6739 | 6765 | Promoted by |
|---|---|---|---|---|---|
| UART0/1 bases | 0x11002000/3000 | mt6735 + mt6755 + j36 MT6592 same | STRONG | STRONG | Serial hello (step 2) |
| UART2/3 bases | 0x11004000/5000 | mt6735 map + j36 same (no mt6755 twin) | STRONG | MEDIUM | First use, if UART0 silent |
| UART clock | 26 MHz | mt6735 + mt6755 dummy26m + j36 same | STRONG | STRONG | Correct baud on hello |
| GPT base + offsets | 0x10004000... | mt6735 only (mt6797 moved it) | MEDIUM | OUT | 6739: timer says "gpt4"; 6765: step 2c |
| TOPRGU base | 0x10212000 | mt6735 only (RGU moved before) | MEDIUM | OUT | 6739: no reset loop; 6765: step 2b |
| WDT disable word | 0x22000000 | mt6735 platform.c:70-71 | MEDIUM | OUT | Same |
| MEMBASE | 0x41E00000 | mt6735 target MEMBASE | MEDIUM | WEAK | Step 1 reads it from the stock LK |
| UBOOT slot size | 0x200000 | j36 slot size | WEAK | WEAK | Step 1 reads the stock scatter |
| MSDC0 base | 0x11230000 | mt6735 + mt6797 + mt6877 agree | STRONG | STRONG | Step 3 (driver does not exist yet) |
| DRAM base | 0x40000000 | Shared OS prior | MEDIUM | MEDIUM | OS BRINGUP step 2 (printed, not used) |
| Console UART index | 0 | MediaTek norm | MEDIUM | MEDIUM | Step 2 (`-DMT67XX_DEBUG_UART=N` if silent) |
| Boot-status sector | Same format as j36 | j36 wrap script | FORMAT | FORMAT | Reader lands with bootstatus step |

Not facts yet (no values anywhere, drivers absent by design): eMMC
clock/pinmux, DSI/panel model + init, key input path, PMIC model
([6739]/[6765] both suspect mt6357 -- confirm from stock DTB),
kernel/ramdisk load addresses, LK framebuffer canvas. Each lands with
its LK-BRINGUP step; until then the LK refuses to need them.
