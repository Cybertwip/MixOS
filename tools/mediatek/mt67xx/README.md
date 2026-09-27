# mt67xx minimal LK -- bring-up instrument (v1)

MixOS's Little Kernel replacement for the mt67xx family (today: MT6739 in
the LG K20), derived from the proven j36 tree (`tools/mediatek/firmware`).
**This builds but does not boot anything yet**: v1 is serial hello +
watchdog off + proven-ticking clock + heartbeat park. eMMC, display, keys
and the kernel handoff land as staged LK-BRINGUP steps, each with its own
grounding. Read `LK-BRINGUP.md` before flashing anything; `FACTS.md`
lists every address and what it is waiting on.

## Status

| Part | Status | Grounding |
|---|---|---|
| Entry + vectors + linker | Derived from j36, retargeted | j36 tree (runs on MT6592) |
| UART 115200 | Derived, prior base | mt6735 map + j36 second witness |
| Timer (arch -> GPT4 -> soft) | New, detection-gated | ARM ARM (arch) + mt6735 GPT (GPT4) |
| Watchdog off | One write | mt6735 platform.c |
| Bootmenu + menu UI | Derived, host-tested | j36 headers (UI shared by design) |
| UBOOT-slot wrap | Verbatim j36 script | j36 tree |
| eMMC read | Not present (step 3) | Waits on clock/pinmux facts |
| Display + menu | Not present (step 4) | Waits on panel facts (OS BRINGUP 2) |
| AArch64 handoff | Not present (step 5) | Needs steps 3-4 first |

## Layout

`firmware/Drivers/` (facts header, entry, linker, uart/timer/wdt, lean
main, UI headers), `firmware/scripts/` (slot wrap), `firmware/tests/`
(host UI test), `build.sh` (per-device build), `FACTS.md` (provenance),
`LK-BRINGUP.md` (the playbook).

## Build

`./tools/mediatek/mt67xx/build.sh --device lm-x120` -- or the wired route,
`./build-flashtools.sh --device lg-mt6739` (no gates: building is harmless,
same as the j36). Lands `lk.bin`, `lk.elf`, `FACTS.md`,
`build-info.txt` in `build/mt67xx/<device>/boot/`. `-DMT67XX_DEBUG_UART=N`
rebuilds for UART N when silence says UART0 was wrong.

## Flash (read LK-BRINGUP step 2 first)

Back up the stock LK slot (SP Flash Tool readback or mtkclient) and keep
the stock image where bootrom can reach it. Flash `lk.bin` to the LK/UBOOT
slot per the stock scatter (or unlocked-fastboot via the shared CLI --
see LK-BRINGUP step 2), with UART wired at 115200. Success looks like:

```
[mt67xx-lk] MixOS minimal LK (bring-up) for lm-x120 (mt6739), commit abc1234
[mt67xx-lk] facts in force: uart=0x11002000/115200 membase=0x41e00000 slot=0x200000 timer=arch(hw)
[mt67xx-lk] preloader args: r0=0x... r1=0x... r2=0x... r3=0x...
[mt67xx-lk] NEXT: LK-BRINGUP step 3 (eMMC read). Parking with heartbeat.
[mt67xx-lk] alive 5s
```

Recovery is the bootrom: a bad LK never touches the preloader, so SP
Flash Tool / mtkclient reflashing the stock LK image always un-bricks.

## Derivation ledger

- `lk_menu_ui.h`: byte-identical to the j36's (verify: `cmp`).
- `scripts/create-mtk-lk-image.py`: byte-identical to the j36 wrap script.
- Entry/linker/UART/bootmenu: derived, retargeted, commented at each delta.
- Timer/main: new, following the j36 contracts. No compiler-rt is linked
  (32-bit math only); the j36 `aeabi` shims have nothing to do here.
