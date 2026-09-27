# mt67xx LK bring-up playbook

Companion to `device/lg-k20/BRINGUP.md` (the OS side): several steps share
facts, and each step below names its OS twin. Work top to bottom; each step
names the exact files it fills. The standing rule: a driver that would need
an ungrounded address does not exist yet -- silence and precise breadcrumbs
beat invented registers in a bootloader.

## Prerequisites

An LM-X120 with a stock LK backup (SP Flash Tool readback or mtkclient)
reachable from bootrom, UART wired at 115200, and the recovery path tested
BEFORE the first flash (read back the slot you are about to write and
compare hashes). Without those, stop: nothing below is testable and every
flash is a brick risk. Same gate as the OS tree: `LG_K20_BRINGUP_ACK=1`.

## Step 0 -- toolchain proof (host only, no phone)

`./build-flashtools.sh --device lg-mt6739` (with the ACK) and the host UI
test (`cc ... tools/mt67xx/firmware/tests/test-lk-ui.c`). Success is a boot
dir with `lk.bin`, `lk.elf`, `FACTS.md`, `build-info.txt`, and PASS. This
proves the derivation compiles and wraps; it proves nothing about the
phone.

## Step 1 -- stock LK + scatter (host only, no flashing)

From the stock firmware (OFP/BRINGUP step 1 route on the OS side): read the
LK/UBOOT slot's offset + size from the scatter (fills `MT67XX_LK_SLOT_SIZE`,
replacing the 2 MiB prior), and disassemble the stock LK's entry -- its
`ldr =` literals resolve to absolute addresses, which reads back the real
MEMBASE (fills `MT67XX_MEMBASE`, replacing the mt6735 prior). Record both
in `FACTS.md` with the image + offset as provenance.

Fill: `firmware/CMakeLists.txt` default, `Drivers/mt67xx_facts.h` comment,
`FACTS.md` rows to STRONG.

## Step 2 -- first light (this v1 image)

Flash the step-0 `lk.bin` to the LK slot. Expect the README's serial log:
hello, facts, preloader args, heartbeat. Diagnose by the table in
`mt67xx_lk_main.c`'s header comment (heartbeat = proceed; reset loop = WDT
prior wrong; silence = UART index/MEMBASE prior wrong; exception = the PC
names the liar). Iterate UART index via `-DMT67XX_DEBUG_UART=1..3` before
doubting MEMBASE (UART has two witnesses; MEMBASE has one).

Fill on success: `FACTS.md` (UART + WDT rows to STRONG), OS `BRINGUP.md`
(console UART -- the OS twin gets its UART for free).

## Step 3 -- eMMC read

Grounds: MSDC0 base (already a MEDIUM prior) + clock gate + pinmux from the
stock DTB (OS BRINGUP step 2). Write `mt67xx_msdc.c` (minimal: init + read
sectors, derived from the j36 `mt6592_msdc.c` structure, NOT its clock
facts), call it from the NEXT(step 3) marker in main, print the boot.img
magic. Failure parks with the stage named -- a dead controller must still
talk.

## Step 4 -- display + menu

Grounds: panel model + init sequence + DSI facts from OS BRINGUP step 2/3.
Write the DSI core + panel driver, light the LK framebuffer canvas (address
lands here -- the linker map grows its second region), run the
`lk_bootmenu` window over `lk_menu_ui.h` (both shipped + tested already;
this step only connects key input + pixels). Key path TBD by the PMIC facts
(volume keys via PMIC EINT is the prior, not a promise).

## Step 5 -- handoff

Grounds: steps 3-4 (a kernel in memory, a menu that picked it). Write the
AArch32->AArch64 switch + DTB handoff where entry's comment reserves it,
parse boot.img (magic, sizes, cmdline), jump. The `lk_bootmenu` sniff
(step 4's tag) decides MixOS-vs-stock payload handling. Success is the
OS rescue shell talking -- the OS tree takes it from there.

## Step 6 -- promote

When a full boot works: assets slot + boot-status reader (j36 shape),
release policy (second variant only if a second boot policy exists),
`--device` promotion notes in `build-flashtools.sh`, and this file into
history (what each fact turned out to be, with sources).
