# mt68xx LK facts

Every hardware value the LK builds with, its provenance, and what would
promote it from prior to fact. `build-info.txt` in each boot dir records
the values one image was built with; this file records why they are
believed. Strengths: STRONG (two witnesses or same-generation map),
MEDIUM (one witness or conventional), WEAK (conventional only).

| Fact | Value | Provenance | Strength | Promoted by |
|---|---|---|---|---|
| UART0/1 bases | 0x11002000/3000 | mt6877 map + mt6735 + j36 (3 witnesses) | STRONG | Serial hello (step 2) |
| UART2/3 bases | 0x11004000/5000 | mt6735 map + j36 same (no mt6877 twin) | MEDIUM | First use, if UART0 silent |
| UART clock | 26 MHz | mt6735 + j36; same UART IP on Dimensity | STRONG | Correct baud on hello |
| GPT base + offsets | 0x10004000... | mt6735 only; infra moved (cf PWRAP) | WEAK | Step 2c (COMPILED OUT until then) |
| TOPRGU base | 0x10212000 | mt6735 only; RGU moved before | WEAK | Step 2b (COMPILED OUT until then) |
| WDT disable word | 0x22000000 | mt6735 platform.c:70-71 | WEAK | Step 2b reads it from the stock LK |
| MEMBASE | 0x41E00000 | mt6735 target MEMBASE (older line) | WEAK | Step 1 reads it from the stock LK |
| UBOOT slot size | 0x200000 | j36 slot size | WEAK | Step 1 reads the stock scatter |
| MSDC0 base | 0x11230000 | mt6877 map + mt6735 twin | STRONG | Step 3 (driver does not exist yet) |
| DRAM base | 0x40000000 | Shared OS prior | MEDIUM | OS BRINGUP step 2 (printed, not used) |
| Console UART index | 0 | MediaTek norm | MEDIUM | Step 2 (`-DMT68XX_DEBUG_UART=N` if silent) |
| PWRAP (reference) | 0x10026000 on mt6877 | mt6877 map (NOT mt6735's 0x10001000) | INFO | Proof the infra moved; nothing uses it yet |
| Boot-status sector | Same format as j36 | j36 wrap script | FORMAT | Reader lands with bootstatus step |

Not facts yet (no values anywhere, drivers absent by design): eMMC
clock/pinmux, DSI/panel model + init, key input path (VOL keys are GPIO
EINT on mt6877 -- mechanism prior, bases TBD), PMIC model,
kernel/ramdisk load addresses, LK framebuffer canvas. Each lands with
its LK-BRINGUP step; until then the LK refuses to need them.
