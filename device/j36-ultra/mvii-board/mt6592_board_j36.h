#ifndef MT6592_BOARD_J36_H
#define MT6592_BOARD_J36_H

/*
 * J36 Ultra board facts from Hardware/Virtua/KiCad:
 *   - sheet_12_lcd_ctp: CON901 main LCM connector, U902 GT913 touch.
 *   - sheet_04_swchr_backlight_driver: U901 TPS61161 backlight driver.
 *   - sheet_02_mt6592_baseband: MT6592 DSI TX and LCD sideband balls.
 *   - MT6592 DCT pinmux: GPIO90 mode 1 is DISP_PWM; GPIO157 mode 1 is TDP0.
 *
 * The Reference/J36-ULTRA lk.bin LCM table exposes jd9365_qc_190227_lcm_drv.
 * Its get_params() reports a 640x480 RGB888 DSI video panel with four lanes,
 * PLL_CLOCK=107, VSA/VBP/VACT=12/16/480, and HSA/HBP/HFP/HACT=120/120/120/640.
 * The params struct is zeroed before these stores; no vertical front porch is
 * set by this LK driver.
 */
enum {
    MT6592_J36_PANEL_WIDTH = 640u,
    MT6592_J36_PANEL_HEIGHT = 480u,
    MT6592_J36_PANEL_DSI_PLL_CLOCK_MHZ = 107u,
    MT6592_J36_PANEL_PIXEL_CLOCK_HZ = 32000000u,
    MT6592_J36_PANEL_HFP = 120u,
    MT6592_J36_PANEL_HSYNC = 120u,
    MT6592_J36_PANEL_HBP = 120u,
    /* Stock LK get_params (FUN_81e1c580): VSA=4, VBP=12, VFP=16 (word indices
     * 0x59/0x5a/0x5b). The earlier 12/16/0 mapping was shifted and left the
     * panel unable to lock vertical sync. */
    MT6592_J36_PANEL_VFP = 16u,
    MT6592_J36_PANEL_VSYNC = 4u,
    MT6592_J36_PANEL_VBP = 12u,
    MT6592_J36_PANEL_DSI_LANES = 4u,

    MT6592_J36_DISP_PWM0_BASE = 0x1400a000u,
    MT6592_J36_DISP_PWM_GPIO = 90u,

    /*
     * Indicator LEDs — three plain GPIOs, active low.
     *
     * Found on the hardware with the console's `pin` command, one pad at a time:
     *   pin #48 0 1 0 1   -> green off      pin #48 0 1 0 0 -> green on
     *   pin #49 0 1 0 0   -> blue on
     *   pin #47 0 1 0 1   -> red off
     * i.e. mode 0, output, no pull, and DOUT 0 lights the LED. The cathode side
     * is switched, so the pad sinks the current and 1 is dark.
     *
     * This replaces an ISINK theory that was wrong twice over. The LEDs are not
     * on the MT6323 current sinks -- `ledscan` walks all four ISINK channels with
     * current and duty programmed and the LEDs never change -- and they are not
     * on 6/112/113 either, which were picked only because they were the three
     * pads that boot in mode 0 as outputs (112 turned out to be panel power).
     * Corroborated by the stock OS: extracted/ramdisk/ueventd.rc chowns
     * /sys/devices/platform/leds-mt65xx/leds/{green,red,blue}, and leds-mt65xx is
     * MTK's GPIO/PWM LED driver, not the PMIC sink driver.
     */
    MT6592_J36_LED_RED_GPIO = 47u,
    MT6592_J36_LED_GREEN_GPIO = 48u,
    MT6592_J36_LED_BLUE_GPIO = 49u,
    MT6592_J36_LED_ACTIVE_LOW = 1u,

    /*
     * KEYPAD PADS — from Reference/J36-ULTRA/preloader_sf6592_wet_l.bin.
     *
     * The preloader exports the symbol name in its own log string, so this is not
     * inference about which function was found: "Enter mtk_kpd_gpio_set! " at file
     * offset 0x176d9, referenced by the function at 0x53f8 (Thumb-2; the image is
     * PIC, so string refs are `ldr rN,[pc]` + `add rN,pc` pairs). That function
     * builds two eight-slot arrays, fills them from the helper at 0x53d0, and
     * walks them:
     *
     *   sense set -> mode 1, dir IN,  pull ENABLED, pull UP
     *   strobe set-> mode 1, dir OUT, pull DISABLED
     *
     * The filler writes five entries, each 0x80000000 | pin (bit 31 is MTK's
     * "pin came from the cust header" marker, stripped by the helper at 0x4de4):
     *
     *   strobes: 0x8000004a, 0x8000005c            -> 74, 92
     *   senses : 0x8000004b, 0x800000a7, 0x800000a8 -> 75, 167, 168
     *
     * The sixth pad comes from a second table present byte-identically in the
     * preloader (0x1aac6) and in the stock kernel (0x887154), a {pin, reg, bit}
     * triple list terminated by 00 00:
     *
     *   {74, 0, 2} {75, 1, 2} {92, 0, 6} {93, 0, 10} {167, 1, 6} {168, 1, 10}
     *
     * Two registers holding three four-bit fields each: register 0 carries 74@2,
     * 92@6, 93@10 and register 1 carries 75@2, 167@6, 168@10. (The same file holds
     * three more such arrays with a different shape -- {99..104}, {114..119},
     * {124..129}, each one clk + one cmd + four data -- which are the three MSDC
     * ports, so the shape is what distinguishes them.)
     *
     * THAT EXPLAINS THE SIX-BIT CEILING. Two strobes crossed with three senses is
     * six keys at stride nine -- bits 0,1,2 and 9,10,11, precisely the set that has
     * ever moved on this board. The ceiling is the preloader's pad list.
     *
     * BUT IT DOES NOT LICENCE TAKING PAD 93. The pad table proves only that 93 can
     * carry a keypad row, not that this board wires it as one, and three things say
     * it does not:
     *
     *   - mtk_kpd_gpio_set's strobe list is TWO entries, built by five stores in the
     *     filler at 0x53d0 (one literal 74, then +18, -17, +92, +1). A two-entry
     *     list is a decision, not an omission -- an unmuxed row cannot produce
     *     phantom presses, so leaving 93 out costs the vendor nothing and gains it a
     *     free GPIO.
     *   - the D-pad has been observed working on GPIO 8/20/45/93 (see kpdmon_run in
     *     mvii_debug_console.c, and the GPIO8/RIGHT 1->0 capture quoted at
     *     gpio_value_is_pressed in mt6592_keys.c). Pad 93 is in that list.
     *   - muxing it is what coincided with the D-pad dying. That is the regression
     *     this revision undoes.
     *
     * So there is no third strobe here. Rows 2 and 3 and column 3 -- the shoulder
     * row, MODE/START/SELECT, and RIGHT/X/R1 -- are on pads nothing in Reference/
     * names, and they stay unreachable on the matrix until a pad turns up. That is a
     * missing fact, not a bug to code around: every button in those rows needs a
     * GPIO backing (R2 already has one) or it needs the pad found.
     *
     * NOTHING ELSE IN THE STOCK CHAIN RAISES THE CEILING EITHER. Stock LK contains
     * no call to its own mt_set_gpio_mode (FUN_81e13098 has zero callers) and reads
     * only indices 0 and 1. The stock kernel does not either: its 12 MB image
     * contains no "KROW", "KCOL", "kpd_gpio" or "mtk_kpd_gpio_set" string anywhere,
     * and no gpio-keys driver, so kpd.c takes the pinmux as it finds it.
     */
    /*
     * THE PAD-TO-LINE MAP. MEASURED, AND THREE OF THESE PADS ARRIVE UNMUXED.
     *
     * The five in the preloader's mtk_kpd_gpio_set filler at 0x53d0 are exactly 74, 92, 75,
     * 167 and 168 (literal 74, then +18, -17, +92, +1 -- five stores, five pads, two rows
     * and three columns). That is a FIXUP LIST, not a census of the matrix; the other three
     * lines are 11 (KPROW3), 12 (KPCOL3) and 2 (KPCOL4). The old reading of that filler --
     * "there is no third strobe here", rows 2 and 3 "on pads nothing in Reference/ names" --
     * mistook the list for the whole matrix and is what capped this board at bits
     * {0,1,2,9,10,11}.
     *
     * THE INTERIM READING, THAT THE DWS DEFAULTS LEAVE 11, 12 AND 2 CORRECT AND SO MVII MUST
     * WRITE NOTHING, IS ALSO REFUTED. It followed the working commit's rule ("MVII
     * deliberately has no gpio_set_mode/dir writers here") and it predicted that a fresh
     * boot would show all eight lines in mode 1. A live LK-console dump, MVII's keypad init
     * never having run, showed the opposite:
     *
     *     pad 11  m0 d0 p1 n0      pad 12  m0 d0 p1 n0      pad 2   m0 d0 p1 n0
     *     pad 75  m1 d0 p2 n1      pad 167 m1 d0 p0 n1      pad 168 m1 d0 p0 n1
     *
     * Mode 0, input, pull-DOWN is the DWS park for an unused pin. So the boot chain muxes
     * the preloader's five and parks the other three, and the seven keys on those three --
     * VOL-, VOL+, SELECT, START, MENU (row 3) and R2, A (column 3) -- are dead on any build
     * that does not mux them. mt6592_keys.c's kpd_pads_apply() does, and only for a pad
     * whose mode is actually wrong: the five that work are left exactly as found.
     *
     * WHAT THIS COSTS THE OLD EVIDENCE. The boot-status capture at the working commit
     * 3ecf7d4664bff05900db75782179a7645e188786 -- "face-button mashing lights exactly bits
     * 0, 9..12, 29, 30" -- cannot be reconciled with that commit writing no pads AND the
     * boot chain parking three of them. One of the two is wrong and it does not matter
     * which, because bits 12, 29 and 30 are corroborated independently by the wire pairs
     * (12 and 29/30 are column 3 and row 3, read directly) and by the vendor keymap. Treat
     * that capture as unreliable for provenance and sound for the bits themselves.
     */
    /*
     * THE PINMUX MODE IS PER PAD, AND THIS BOARD USES THREE DIFFERENT ONES.
     *
     * There is no single "KPD mode" on MT6592. Every pad has its own function list, and the
     * keypad happens to land at a different index on each of the three the preloader does
     * not touch. Assuming one mode for all eight is what produced the last wrong build: at
     * mode 1, pad 11 is a STATIC LOW rather than a strobed KPROW3, so one row-3 press drags
     * its whole column low in all eight rows at once (SELECT lit 2, 11, 20, 29, 38, 47, 56,
     * 65), while pads 12 and 2 at mode 1 connect to nothing at all.
     *
     * MEASURED ON HARDWARE by `kpdmode <pad>` in the LK console, which sweeps a pad's mode
     * 0..7 with one button held and reports which scan bits the block sees go low. Verbatim,
     * three runs, one button each:
     *
     *   pad 12, START held:  mode 3 -> "scan low: 3 12 21 30 39 48 57 66"   whole column 3
     *   pad 2,  MENU  held:  mode 6 -> "scan low: 4 13 22 31 40 49 58 67"   whole column 4
     *   pad 11, SELECT held: mode 3 -> "scan low: 29"                       ONE BIT
     *
     * Every other mode on all three pads read "scan low: none" -- eight modes x three pads,
     * one hit each, no ambiguity. The first two are column drags because pad 11 was driven
     * low as a deliberate ground for those runs; the third is the real thing, because with
     * pad 11 muxed at mode 3 the block strobes row 3 properly and SELECT lights bit 29
     * alone, exactly where the vendor keymap puts it. Modes 1 and 2 on pad 11 both produced
     * the whole-column-2 drag, which is the static-low signature and is why "it worked a
     * little" was never a reason to keep mode 1.
     */
    MT6592_J36_KPD_MUX_MODE = 1u,          /* pads 74, 92, 75, 167, 168 -- the preloader's */
    MT6592_J36_KPD_STROBE3_MUX_MODE = 3u,  /* pad 11 -- KPROW3 */
    MT6592_J36_KPD_SENSE3_MUX_MODE = 3u,   /* pad 12 -- KPCOL3 */
    MT6592_J36_KPD_SENSE4_MUX_MODE = 6u,   /* pad 2  -- KPCOL4 */
    MT6592_J36_KPD_PAD_UNMAPPED = 0xffffffffu,
    MT6592_J36_KPD_STROBE0_GPIO = 74u,  /* KPROW0 -- row 0, the shoulders */
    MT6592_J36_KPD_STROBE1_GPIO = 92u,  /* KPROW1 -- row 1, the face buttons */
    /* KPROW2's pad, spent by this board on UP's EINT instead, which is the whole reason
     * matrix row 2 is dead here. NOT a keypad pad on this hardware: it must stay a mode-0
     * GPIO input, and muxing it is what coincided with the D-pad dying before. Named so
     * kpd_pads_apply() can prove on every boot that it is still mode 0, and put it back if
     * it is not.
     *
     * AND IT WILL NOT IDLE HIGH. This pad reads 0 with an internal pull-up armed and
     * verified in the register readback -- `pin 5d 0 0 2 0` then `pin 5d` gives
     * "mode=0 dir=0 pull=2 dout=0 din=0", unchanged whether UP is held or released. Driven
     * instead of pulled, the same pad behaves perfectly: `pin 5d 0 1 0 1` gives din=1
     * released and din=0 held. So UP's switch is an ordinary one to ground and the input
     * buffer is fine; something on the board loads this pad harder than the internal
     * pull-up can fight, and no register makes that resistor bigger. mt6592_keys.c reads it
     * by driving it high for a microsecond per poll -- see key_gpio_read_driven(), which
     * also explains why the short while the button is held is bounded and acceptable. The
     * other three D-pad pads (45, 20, 8) are not affected; the init probe measures which
     * pads need it rather than assuming this one. */
    MT6592_J36_KPD_ROW2_PAD_TAKEN_BY_DPAD_UP = 93u,
    MT6592_J36_KPD_STROBE2_GPIO = MT6592_J36_KPD_PAD_UNMAPPED,
    /* KPROW3 -- row 3: VOL-, VOL+, SELECT, START, MENU. Parked by the boot chain; mode 3. */
    MT6592_J36_KPD_STROBE3_GPIO = 11u,
    MT6592_J36_KPD_SENSE0_GPIO = 75u,  /* KPCOL0 -- VOL- at bit 27 */
    MT6592_J36_KPD_SENSE1_GPIO = 167u, /* KPCOL1 -- VOL+ at bit 28 */
    MT6592_J36_KPD_SENSE2_GPIO = 168u, /* KPCOL2 -- SELECT at bit 29 */
    /* KPCOL3 -- R2 at bit 3, A at bit 12, START at 30. Parked by the boot chain; mode 3. */
    MT6592_J36_KPD_SENSE3_GPIO = 12u,
    /* KPCOL4 -- MENU at bit 31. Parked by the boot chain; mode 6, alone among the eight. */
    MT6592_J36_KPD_SENSE4_GPIO = 2u,

    /* Highest GPIO the hardware decodes. Both the preloader's mt_set_gpio_mode
     * (0x4d90) and stock LK's (FUN_81e12e7c) reject anything above this before
     * touching a register, so the pad space is 0..168, not 0..207. */
    MT6592_J36_GPIO_MAX = 168u,

    /*
     * THREE READ PATHS, and each button belongs to exactly one of them.
     *
     *   WIRE SCAN   a switch between two pads that both idle high. One pad is pulsed
     *               low and the other is read. This is how the J36 wires the buttons
     *               the matrix cannot reach, and it is measured -- see the wire table
     *               further down. A key with a wire pair is read from the scan and
     *               from nothing else, so mt6592_keys_read() cannot double-count it.
     *   KPD MATRIX  kpd_keymap[72], decoded byte-for-byte below, covers all sixteen
     *               face buttons -- but the preloader muxes two strobes against three
     *               senses, so only bits {0,1,2,9,10,11} are ever scanned on this
     *               board. Everything else reads all-ones (not pressed) forever.
     *   PLAIN GPIO  an absolute active-low level, and NOTHING on this board is read this
     *               way. Every entry that was ever here turned out to be a wire pair
     *               misread as a level, including the last holdout: the note that gpio
     *               11 answers both volume keys. Pad 11 is measured to be the node
     *               shared by START, MENU and both VOLs, so its level says only "some
     *               switch on that node closed" and never which. This path is kept for
     *               a future board revision that actually has a switch to ground; do
     *               not add an entry for a pad that appears in the wire table, which is
     *               the double-count the split exists to prevent.
     *
     * Power is the MT6323 PMIC PWRKEY over PWRAP: leave POWER unmapped in all three.
     */
    MT6592_J36_KEY_GPIO_UNMAPPED = 0xffffffffu,
    MT6592_J36_KEY_MATRIX_UNMAPPED = 0xffffffffu,
    MT6592_J36_KEY_ACTIVE_LOW = 1u,

    /* GPIOs whose level is not stable across consecutive reads. Kept for
     * future board revisions; currently empty (the EINT-grade dpad pins are
     * clean). Terminated by MT6592_J36_KEY_GPIO_DEBOUNCE_LIST_END. */
    MT6592_J36_KEY_GPIO_DEBOUNCE_LIST_END = 0xffffffffu,

    /*
     * THIS CONNECTOR USES THREE READ MECHANISMS AT ONCE, and every earlier revision of
     * this block picked one and declared it universal. It is now measured, three times over.
     * The D-pad IS a switch to something low (pull the pad up, low means pressed); the
     * face/shoulder/system keys around pads 11 and 12 are genuinely pad-to-pad and need an
     * active pulse; six keys are real KPD matrix keys. "No button is a switch to ground"
     * was the heading here for a while, and it was wrong for four of them.
     *
     * `kpdwire <pad>` drives one pad LOW with a real output driver, gives every
     * mode-0 input pad a pull-up, and reports every pad that follows it. Run on the
     * device with one button held per report:
     *
     *   kpdwire 4b   (pad 75)    LEFT -> 20   RIGHT -> 8   DOWN -> 45   UP -> 35
     *                            ^^ ALL FOUR OF THESE ARE WRONG. The pads are right, the
     *                            relationship is not: the D-pad pulls its own pad low, so
     *                            each direction was low for its own reasons while pad 75
     *                            happened to be the pad being driven. See the _GPIO block.
     *   kpdwire a7   (pad 167)   R2   -> 12      (and see the A/R2 note below -- suspect)
     *   kpdwire 1e   (pad 30)    A    -> 12
     *   kpdwire c    (pad 12)    START-> 11
     *   kpdwire b    (pad 11)    MENU -> 2    VOL+ -> 167   VOL- -> 75
     *
     * That is all ten kpdwire reports. The eleventh pair came from `kpdmon`'s unmapped-
     * short loop instead, which watches all eleven probe pads on every pulse:
     *
     *   kpdmon  (drive pad 11)   SELECT -> 168
     *
     * The shared nodes:
     *
     *   pad 75  (keypad sense 0) -- VOL- (11)          [the four directions are NOT here]
     *   pad 11  (plain GPIO)     -- START (12), MENU (2), VOL+ (167), VOL- (75),
     *                               SELECT (168)
     *   pad 12  (plain GPIO)     -- A (30), R2 (167), START (11)
     *   pad 167 (keypad sense 1) -- R2 (12), VOL+ (11)
     *   pad 168 (keypad sense 2) -- SELECT (11)
     *
     * AND A AND R2 ARE THE SAME NODE, so far. Third hardware run, verbatim from the user:
     * "A is wired to R2, both trigger the same". What the log shows is why -- pad 12 goes
     * LOW with nothing driven for either button, so A's entry (drive 30, sense 12) reports
     * for both and R2's entry (drive 12, sense 167) reports for neither. One of the two is
     * a switch from pad 12 to something already low; the other may be the pair 30-12, or
     * may be a second switch onto the same node. `kpdmon` decides it now without another
     * guess: when a sense pad is low with nothing driven it drives the pair the other way,
     * HIGH, and prints REAL PAIR or NOT A PAIR while the button is still held.
     *
     * WHAT IDLES HIGH AND WHAT DOES NOT -- and this block used to say "both sides of
     * every switch idle high", which was an artefact of the instrument. `kpdwire` pulls
     * its candidates UP, so naturally both sides read high while it is running. `kpdmon`
     * printed the resting state with nothing armed, and it is the other way round:
     *
     *   2 p1 n0   8 p1 n0   11 p1 n0   12 p1 n0   20 p1 n0   30 p1 n0   35 p1 n0
     *   45 p1 n0  |  75 p2 n1   167 p0 n1        (p1 = pull-down, p2 = up, p0 = none)
     *
     * EVERY button pad idles LOW on a parked pull-down. Only the keypad senses idle high:
     * 75 on the preloader's pull-up, 167 with no internal pull at all, so an external one.
     * A switch here therefore ties a pulled-DOWN pad to a pulled-UP sense line, the two
     * resistors fight, and whether DIN moves depends on which is stronger -- which is why
     * the same button read as a clean edge from one side and as nothing from the other,
     * and why the KPD engine sees these keys as a "column drag" rather than as key reads.
     *
     * The consequence for the scan is not cosmetic. A sense pad that idles LOW cannot
     * report a closure -- it already reads 0 -- so the pull-up the scan applies is what
     * makes the switch observable, and it has to be applied to a muxed pad too. Leaving a
     * parked pull-down in place was tried, on the theory that the pull-down belonged to
     * some other key, and it made A, START and MENU read permanently closed (mask 0x00d0).
     *
     * AND ONE OF THE TEN IS AN ARTEFACT OF THE SAME BLIND SPOT. `kpdwire` never writes a
     * MUXED candidate's pull, so such a pad is read in whatever state its owner leaves it.
     * Pads 30 and 35 are muxed (m1). A muxed pad parked pull-down reads 0 at rest and is
     * dropped as a fixed connection, which is what should have happened to 35; a muxed pad
     * whose owner moves it reads 1 at rest and 0 later, which is indistinguishable from a
     * press. UP -> 35 is that: `kpdmon` could not reproduce it from either end, and pad 35
     * produced three more phantom shorts in the same run. UP IS NOT MEASURED. Nor is any
     * pair involving pad 30 fully trusted, which puts A back in doubt as well.
     *
     * `kpdscan` in the console is the instrument that answers this properly: it forces
     * every pad in its set to mode 0 / input / pull-up whether muxed or not, checks each
     * one actually reads high AND actually drives high and low before letting it report,
     * and scans all 66 pairs of its twelve pads at once. The table below stands until
     * kpdscan contradicts it, entry by entry.
     *
     * Do not try to force this into the 4x4 grid kpd_keymap[72] describes. UP, DOWN,
     * LEFT and RIGHT all share pad 75, which under the keymap would make 75 the row-0
     * strobe and 35/45/20/8 its four columns -- and then A, at row 1 column 1, would
     * have to touch pad 45. It touches 12 and 30. The keymap's geometry describes the
     * KPD block's own matrix, not this board's wiring, and only bits {0,1,2,9,10,11}
     * of it are ever scanned here. The full topology, and the one place it does bite,
     * is worked out at the wire table further down.
     *
     * These pads are read by an active scan now (key_wires_scan() in mt6592_keys.c),
     * so the `_GPIO` absolute entries below are UNMAPPED for every wire-backed key.
     * There is exactly one truth per button.
     *
     * THE TWO OLD D-PAD READINGS, both of which were real measurements, and one of which
     * was right all along:
     *
     *   "8/20/45/93 idle at 0 in mode 0 with pull-DOWN and a DIN sweep moves none of
     *    them" -- true, and it means nothing, because the LK console does not link
     *    mt6592_keys.c (link map: MVIILK.elf.dir is console=1 keys=0) so no pull-up
     *    had ever been applied. It was then read as proof the pads were dead, and the
     *    four D-pad entries were deleted. That was a regression.
     *   "GPIO8/RIGHT: clean 1->0 on press" -- true, from the OS side, with a pull-up
     *    armed. This was then explained away as a switch to pad 75 losing a pull fight.
     *    IT WAS THE ANSWER. The third hardware run reproduced it for all four directions
     *    with nothing driven at all, and the pair reading it was traded for produced
     *    nothing but a short storm. The lesson worth keeping: an absolute level that
     *    reproduces is stronger evidence than a topology that explains it away.
     *
     * WHAT IS STILL MISSING, two things:
     *
     *   A versus R2. Both put pad 12 low, so both report as A. The verdict test in kpdmon
     *     separates them: whichever one survives "drive 30 HIGH and pad 12 follows" is the
     *     pair 30-12, and the other is a switch from pad 12 to something already low.
     *   The matrix permutation -- which of the six reachable bits {0,1,2,9,10,11} belongs
     *     to which of B, X, Y, L1, L2, R1. No matrix bit on this board has ever been
     *     attributed to a named button; the numbering below is the vendor keymap's. All six
     *     fired on the third run, one at a time, so the run that names them is the run that
     *     finishes this file.
     *
     * The D-pad is no longer on this list. It reads as a level (see the _GPIO block) and
     * the four pads are measured; what is not yet known is whether the far side is ground
     * or pad 30, and that does not change the read.
     *
     * The last two closed the oldest loose end in this file. An early note recorded
     * "gpio 11 answers BOTH volume keys -- VOL+ moves gpio 11 alone, VOL- moves gpio 11
     * together with matrix bit 0", and it was never explicable. It is now, exactly:
     * VOL- ties pad 11 to pad 75, which IS keypad sense 0, so closing it drags column 0
     * and with it matrix bit 0. VOL+ ties pad 11 to pad 167, sense 1, whose column the
     * board's keymap slice does not surface -- so that one looked like "gpio 11 alone".
     * Two observations, one mechanism, no residue. It was never a level on pad 11; it
     * was pad 11 being welded to a keypad sense line for as long as the key was held.
     *
     * SELECT IS NOT A MATRIX KEY, and finding that out completed the accounting. It was
     * filed here with B, X, Y, L1, L2 and R1 as one of the seven that "already worked",
     * and it stopped working the moment the wire scan armed pull-ups on the table's pads
     * -- which no matrix key could do, because the strobes are not in the table. What it
     * does instead: it shorts pad 11 to keypad sense 168, and 168's pull-up drags the
     * parked pad 11 UP. Pull pad 11 up as well and nothing moves. That also explains the
     * one bizarre line in the second hardware run, START and MENU flipping in lockstep
     * every time SELECT was pressed: both of them sense pad 11, and SELECT was the thing
     * moving it.
     *
     * So the full census, and it closes exactly -- in THREE read paths, not two. As of the
     * FOURTH hardware run every line of it is a measurement:
     *
     *   6  real KPD matrix keys        L1 = bit 0, L2 = bit 1, R1 = bit 2,
     *                                  X  = bit 9, Y  = bit 10, B = bit 11.
     *                                  Attributed by name, at last -- see the matrix block
     *                                  at the bottom of this file for how. Six keys against
     *                                  the six bits this board can scan: two strobed rows
     *                                  (74, 92) x three muxed senses (75, 167, 168) =
     *                                  bits {0,1,2,9,10,11}. Row 0 is the shoulders, row 1
     *                                  is the face buttons, which is a sane layout and a
     *                                  small piece of corroboration on top of the ordering.
     *   3  absolute levels             DOWN = 45, LEFT = 20, RIGHT = 8. Pull the pad up,
     *                                  low means pressed. Confirmed working by hand.
     *   1  absolute level, SHARED      pad 12 goes low for A and for R2 alike, and driving
     *                                  its supposed partner (pad 30) HIGH while the key is
     *                                  held does not lift it. So pad 12 is a switch to
     *                                  something already low, both keys are on it, and no
     *                                  read of pad 12 can tell them apart. Counts as two
     *                                  buttons and one measurable node.
     *   1  UNIDENTIFIED PAD            UP. Pad 35 was the guess and pad 35 is wrong: it is
     *                                  muxed (m1), it is in kpdmon's "low at rest" list, and
     *                                  it flapped about ten times in thirty seconds with
     *                                  nobody touching the device. `kpdlow` is the command
     *                                  built to find the real one.
     *   5  wire pairs                  START (12--11), MENU (2--11), VOL+ (11--167),
     *                                  VOL- (11--75), SELECT (11--168). All five work.
     *   1  PMIC                        POWER, MT6323 PWRKEY over PWRAP
     *   --
     *   18                             every input on the device
     *
     * Three mechanisms on one connector is not elegant, and it is what the board does. Two
     * of the three were each, at some point, asserted to be the only one.
     *
     * NOTE WHAT LEFT THE WIRE TABLE. It had seven pairs and now has five: A (30--12) and
     * R2 (12--167) are both gone, because A's entry was firing on a passive low and R2's
     * entry never fired at all. Every pair still in the table has both pads idling HIGH,
     * which is the only condition under which a pulse scan means anything. That is now the
     * table's invariant rather than an accident of which guesses survived.
     */
    /*
     * ============================================================================
     * EVERYTHING ABOVE IS SUPERSEDED. THERE ARE TWO READ PATHS, NOT THREE, AND THE
     * WIRE SCAN WAS THIS DRIVER HAND-STROBING A KEYPAD ROW IT HAD UNPLUGGED ITSELF.
     * ============================================================================
     *
     * Commit 3ecf7d4664bff05900db75782179a7645e188786 ("Route dashboard", 2026-07-30) is
     * the last commit at which every button on this device worked. Its keymap was not
     * guessed from hardware runs -- it was read out of the STOCK KERNEL BINARY, from
     * kpd_pdrv_probe at kernel VA 0xc0476730 and kpd_keymap[72] at 0x887778 in the
     * Reference/J36-ULTRA boot.img, cross-referenced against
     * /system/usr/keylayout/Vendor_2454_Product_6500.kl. Two tables, both complete:
     *
     *   SEVEN GPIO/EINT KEYS               FOUR MATRIX ROWS x NINE COLUMNS
     *     GPIO 20 -> kc 10  DPAD_LEFT        row0 (bits  0.. 8): L1 L2 R1 R2  -
     *     GPIO  8 -> kc 30  DPAD_RIGHT       row1 (bits  9..17): X  Y  B  A   -
     *     GPIO 93 -> kc 48  DPAD_UP          row2 (bits 18..26): LT RT UP DN  -
     *     GPIO 45 -> kc 46  DPAD_DOWN        row3 (bits 27..35): V- V+ SEL STA MENU
     *     GPIO  7 -> kc 317 BTN_THUMBL
     *     GPIO 46 -> kc 318 BTN_THUMBR
     *     GPIO  0 -> kc 88  F12 (home)
     *
     * WHAT THIS SESSION'S FOUR HARDWARE RUNS ACTUALLY MEASURED, once read against those
     * two tables instead of against a blank sheet, is the pad-to-line map of the KPD
     * block -- which is not in any artefact in Reference/ and is worth having:
     *
     *   KPROW0 = 74   KPCOL0 =  75      because "VOL- = 11--75"   is row3 x col0 = bit 27
     *   KPROW1 = 92   KPCOL1 = 167      because "VOL+ = 11--167"  is row3 x col1 = bit 28
     *   KPROW2 = 93   KPCOL2 = 168      because "SELECT = 11--168" is row3 x col2 = bit 29
     *   KPROW3 = 11   KPCOL3 =  12      because "START = 12--11"  is row3 x col3 = bit 30
     *                 KPCOL4 =   2      because "MENU = 2--11"    is row3 x col4 = bit 31
     *
     * THE FIVE "WIRE PAIRS" ARE ROW 3 OF THE VENDOR KEYMAP. Not approximately -- exactly,
     * all five keys, in column order, sharing one common node. The wire scan pulsed pad 11
     * to output-low and read pads 75, 167, 168, 12 and 2: that is a row strobe and five
     * column senses, implemented by hand, in software, one key at a time. It worked for
     * the same reason the KPD block works, and the "hub" that five pairs mysteriously
     * shared was KPROW3. Five pairs, five keys, one row. There was never a third
     * mechanism on this connector.
     *
     * A AND R2 ARE ONE COLUMN, NOT ONE NODE. Pad 12 is KPCOL3. The vendor keymap puts R2
     * at row0 x col3 = bit 3 and A at row1 x col3 = bit 12. Two switches on one column
     * line, which is precisely why both pull pad 12 low with nothing driven, why the
     * drive-HIGH verdict test called it "tied low elsewhere" (the other end is a row line
     * the KPD block was holding low), and why no read OF PAD 12 can separate them. The
     * thing that separates them is the row strobe. That is what a matrix is for. The
     * conclusion recorded above -- "two switches, one observable node, no software can
     * tell them apart" -- was true of the instrument and false of the board.
     *
     * WHY EXACTLY SIX BITS SCANNED. kpd_pads_mux() muxed two rows (74, 92) and three
     * columns (75, 167, 168) and forced the rows to be GPIO outputs. Two rows x three
     * columns = bits {0,1,2,9,10,11}, which is bit-for-bit the set that worked. That set
     * was read as "the six bits this board can scan" and it is nothing of the kind: it is
     * the six bits that survived kpd_pads_mux(). The other seven matrix keys were dead
     * because their row (11) or their column (12, 2) had been taken away from the block
     * and handed to the wire scan as GPIOs.
     *
     * SO THE RULE THE WORKING COMMIT STATED IN ONE LINE -- "MVII deliberately has no
     * gpio_set_mode/dir writers here" -- IS RIGHT ABOUT THE SCAN AND WRONG ABOUT BRING-UP,
     * and the boot after this paragraph was written is what drew the line between them.
     * Right about the scan: the MT6592 KPD block strobes all eight rows in hardware and
     * updates MEM1..MEM5 by itself, so the entire steady-state driver is write DEBOUNCE,
     * write EN=1, read five 16-bit words, treat 0 as pressed -- no strobing, no pulsing, no
     * per-scan MODE writes, and pads 2, 11, 12, 75, 167 and 168 are NOT GPIOs. Wrong about
     * bring-up: the boot chain hands over three of those eight pads (11, 12, 2) in the DWS
     * park for an unused pin -- mode 0, input, pull-DOWN -- measured on a fresh boot from
     * the LK console. Somebody has to mux them, and the only somebody left is MVII. So
     * kpd_pads_apply() writes MODE once, at init, before EN, and only for a pad whose mode
     * is wrong. After that the no-writers rule holds absolutely.
     *
     * The distinction matters because both of the wrong versions of this driver got it by
     * generalising: kpd_pads_mux() wrote a fixed list on every boot regardless of state,
     * and kpd_pads_report() wrote nothing regardless of state. "Write only what is
     * measurably wrong" is what neither of them did.
     *
     * ROW 2 NEVER FIRES, AND NOW WE KNOW WHY. The vendor keymap lists the D-pad on row 2
     * (bits 18-21) and multiple live captures showed that row dead with every other row
     * working. KPROW2 is pad 93 -- and pad 93 is what the vendor probe registers as UP's
     * EINT. The board took row 2's strobe pad and wired it to a button instead, so the
     * D-pad moved off the matrix and onto the four EINTs, and the matrix's row 2 lost its
     * strobe. One pad explains both halves of that. It also means pad 93 must stay a
     * mode-0 GPIO input: mux it to the KPD block as KPROW2 and UP dies.
     *
     * THE NUMERIC COINCIDENCE THAT COST THE MOST. A is matrix BIT 12, and A's column is
     * GPIO PAD 12. "Pad 12 goes low when A is pressed" and "bit 12 is A" are the same
     * fact wearing the same number, and reading the first as a level rather than as a
     * column is what turned a correct vendor table into an eight-run detour. Where this
     * file says 12, check which twelve.
     *
     * THE CENSUS, CORRECTED. Still eighteen, still closes exactly, in TWO read paths:
     *
     *   4  EINT levels        UP = 93, DOWN = 45, LEFT = 20, RIGHT = 8. Active low.
     *                         Read DIN, never reprogram. Three were confirmed working by
     *                         hand this session; the fourth comes from the same vendor
     *                         table as those three.
     *  13  KPD matrix bits    L1=0  L2=1  R1=2  R2=3     (row 0, shoulders)
     *                         X =9  Y =10 B =11 A =12    (row 1, face)
     *                         V-=27 V+=28 SEL=29 STA=30 MENU=31  (row 3)
     *   1  PMIC               POWER, MT6323 PWRKEY over PWRAP
     *   --
     *  18                     every input on the device
     *
     * What the four hardware runs were worth, stated fairly: they independently re-derived
     * six of the thirteen matrix bits (L1=0, L2=1, R1=2, X=9, Y=10, B=11) and got all six
     * right, they produced the pad-to-line map above, and they confirmed three of the four
     * EINT levels. What they could not do was notice that the answer was already in the
     * file, in a table taken from the vendor's own binary, and that the reason the other
     * seven keys were dead was in the driver rather than in the board.
     */
    /*
     * THE D-PAD IS NOT A WIRE PAIR. It is an absolute active-low level, which is what the
     * very first measurement in this file said ("GPIO8/RIGHT: clean 1->0 on press") and
     * what two model changes talked me out of. Third hardware run, with every pad armed
     * pull-up and the pulse scan working on eight other keys: the four directions produced
     * NO pair, and instead produced a twenty-five-line short storm -- every pair among the
     * eight button pads at once, which is 28 minus the 3 the table already names. A pad
     * sitting low while eight pulses walk past it reports a short against all seven others.
     * One pad low, seven bogus pairs, four directions, and the storm is fully accounted.
     *
     * So each direction pulls its OWN pad down when pressed, and the far side is something
     * already low -- real ground, or pad 30, which is muxed (m1) and measured n0 at rest
     * even with a pull-up armed on it. Which of the two does not matter for the read: pull
     * the pad up, and low means pressed. It does matter for the map, and `kpdscan` settles
     * it, because it forces pad 30 to mode 0 with a pull-up and takes its owner out of the
     * picture -- if the directions still go low, the far side is ground.
     *
     * These are the pads kpdwire reported as "following pad 75", and that report was the
     * instrument reading a pad that was low for its own reasons. kpdwire cannot tell "this
     * pad followed my driver" from "this pad was already low"; kpdmon now can, because it
     * samples every probe pad with nothing driven before each pulse pass.
     *
     * THREE OF THE FOUR ARE RIGHT AND UP IS NOT. Fourth hardware run: DOWN, LEFT and RIGHT
     * work; pad 35 "pressed" and "released" ten times in thirty seconds with the device
     * sitting untouched on the desk. Pad 35 is not a button pad and the evidence was in the
     * same report all along, in three places at once:
     *
     *   - `drv 35 m1` -- it is MUXED to some peripheral, and its three working siblings
     *     (45, 20, 8) are all m0. A parked GPIO input is what a button pad looks like here.
     *   - "low at rest with nothing driven: pad 30 pad 35" -- it was already low before any
     *     button was touched, which for a level read is the one disqualifying condition.
     *   - "UNMAPPED short: pad 20 -- pad 35" -- pad 35 wandering across a pulse of pad 20,
     *     reported as a short between two buttons that have nothing to do with each other.
     *
     * So pad 35 is UNMAPPED and UP has no pad yet. It was never separately measured: it came
     * in with the other three from one `kpdwire 4b` report, and that report is now known to
     * name pads that merely read low. Three of its four guesses happened to be right, which
     * is a good reminder that a broken instrument is not a wrong instrument -- it is one
     * whose output has to be checked key by key.
     *
     * HOW TO FIND IT: `kpdlow`. UP's pad is almost certainly a mode-0, dir-0 pad parked on
     * the preloader's pull-DOWN, exactly like 45, 20 and 8, and it reads 0 at rest for that
     * reason rather than because anything is pressed -- which is why no passive sweep has
     * ever seen it. `kpdlow` pulls every mode-0 input pad on the SoC UP, drops the ones that
     * will not sit still, and then watches. Hold UP and it names the pad. It changes PULL
     * only -- no mux, no direction, nothing driven -- which is what makes it safe to point
     * at all 169 pads at once, and is the difference between it and `kpdsweep`.
     */
    /*
     * PAD 93, from the vendor EINT table -- GPIO 93 -> keycode 48 -> DPAD_UP -- and from
     * the working commit, where it read UP correctly. Three of that table's four D-pad
     * entries (20, 8, 45) are confirmed by hand on this device, so the fourth is not a
     * guess being promoted, it is the remaining row of a table that has been right about
     * every row anyone has checked.
     *
     * Pad 35 was UP for three revisions of this file, and pad 35 is muxed, idles low and
     * free-runs. It replaced pad 93 on the strength of a `kpdwire 4b` report, and the note
     * that pad 93 "appears to carry nothing" came from a sweep that never touched pad 93:
     * `sweep_is_skipped` excludes 74, 92 and 93 as suspected strobes. An instrument that
     * skips a pad has not cleared that pad.
     *
     * PAD 93 MUST STAY MODE 0. It is KPROW2, which is why matrix row 2 is dead on this
     * board; the board spent that strobe on this button. Anything that muxes it back to
     * the KPD block trades UP for a row of keys that are all mapped elsewhere.
     */
    MT6592_J36_KEY_DPAD_UP_GPIO = 93u,
    MT6592_J36_KEY_DPAD_DOWN_GPIO = 45u,
    MT6592_J36_KEY_DPAD_LEFT_GPIO = 20u,
    MT6592_J36_KEY_DPAD_RIGHT_GPIO = 8u,
    /*
     * A IS A LEVEL ON PAD 12, not the pair 30--12, and this is the one entry in this file
     * that a single line of output settled outright:
     *
     *     kpdmon: A PRESSED   (pads 30-12)
     *     kpdmon:   ^ sense pad low with nothing driven; drive high -> sns=0
     *                                        => NOT A PAIR (sense tied low elsewhere)
     *
     * Pad 12 was already low before pad 30 was pulsed, so the "closure" was pad 30's pulse
     * walking past a pad that was low on its own account -- the same blind spot that gave
     * the D-pad four wrong entries. The verdict test is what a pulse scan cannot do: it
     * drives pad 30 HIGH and asks whether pad 12 follows. A closed switch conducts both
     * ways; pad 12 stayed at 0. And pad 30 is not the problem -- its own line reads
     * `hi=1 lo=0`, so it drives in both directions perfectly well. The switch simply is not
     * to pad 30.
     *
     * AND R2 IS ON THE SAME PAD. "A is wired to R2, both trigger the same" -- and the run
     * shows why in a way no table entry can fix: pad 12 goes low for A and pad 12 goes low
     * for R2, both with nothing driven, both with the same verdict. Two switches, one
     * observable node. So pad 12 is mapped to A and R2 stays unmapped, which is a choice
     * between two wrong answers and the less wrong one: A works and R2 does nothing, rather
     * than R2 silently emitting A. If `kpdlow` finds a second pad that moves for R2 and not
     * for A, R2 gets that pad and this comment gets deleted.
     *
     * ALL OF WHICH IS A DESCRIPTION OF ONE COLUMN LINE. Pad 12 is KPCOL3; A is row1 x col3
     * and R2 is row0 x col3. Both switches really are on pad 12, the verdict test really
     * did read it tied low elsewhere (the far end is a row line), and the conclusion drawn
     * from that -- that nothing can separate them -- is wrong: the row strobe separates
     * them, and the KPD block does it in hardware. A is bit 12 and R2 is bit 3. Pad 12 is
     * not a button pad and is not read as one.
     */
    MT6592_J36_KEY_A_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,
    MT6592_J36_KEY_B_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,
    MT6592_J36_KEY_X_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,
    MT6592_J36_KEY_Y_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,
    MT6592_J36_KEY_L1_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,
    MT6592_J36_KEY_R1_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,
    MT6592_J36_KEY_L2_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,
    /* Was 12, on four clean `kpdhunt` 1->0 cycles, and then unmapped as a misattribution.
     * Pad 12 IS the pad -- the fourth run put R2 on it with nothing driven -- but it is
     * also A's pad, and the same level cannot answer two keys. So R2 stays unmapped and pad
     * 12 belongs to A above. Those four kpdhunt cycles were real all along; what they could
     * not say, and what nothing has said yet, is WHICH of the two buttons made them. */
    MT6592_J36_KEY_R2_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,
    MT6592_J36_KEY_START_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,
    MT6592_J36_KEY_SELECT_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,
    MT6592_J36_KEY_MENU_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,
    MT6592_J36_KEY_VOL_UP_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,
    MT6592_J36_KEY_VOL_DOWN_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,
    MT6592_J36_KEY_POWER_GPIO = MT6592_J36_KEY_GPIO_UNMAPPED,

    /*
     * WIRE PAIRS -- the scan table. One DRIVE pad and one SENSE pad per button.
     *
     * Orientation rule: the DRIVE pad is never a keypad pad. mt6592_keys.c pulses it
     * to output-low for ~30 us and puts it straight back to input-pull-up, and it
     * writes no MODE register outside kpd_pads_mux() -- the discipline that has kept
     * this driver from wedging the board. Sensing a keypad pad is free: GPIO_DIN
     * reports the real pad level even in mux mode 1 (live proof: `kpdmux` printing
     * "75 mode=1 dir=0 pull=2 din=1"), so 75 and 167 are sensed without being touched.
     *
     * A pulse does briefly drag the keypad column its hub sits on -- which is what
     * `kpdmon`'s eight-bits-at-stride-9 capture was: pad 75 pulled low, so every row
     * of column 0 read pressed at once. That cannot latch a phantom press, because
     * KP_DEBOUNCE is 0x400 on the PMIC's 32 kHz clock, ~32 ms of required stability
     * against a 30 us glitch. mt6592_keys_read() also samples the matrix BEFORE it
     * pulses anything, so the honest sample is the one it uses.
     */
    MT6592_J36_KEY_WIRE_UNMAPPED = 0xffffffffu,
    /* The four directions are UNMAPPED here on purpose: they are absolute levels, not
     * pairs, and their pads are in the _GPIO block above. What used to be here -- 20-75,
     * 8-75, 45-75, 35-75, all four "sensing" keypad sense 0 -- was one artefact repeated
     * four times: `kpdwire 4b` drove pad 75 low, and a pad that a held direction had
     * already pulled low read 0 and was recorded as following. Four reports, one blind
     * spot, and the pads named were right for the wrong reason. */
    MT6592_J36_KEY_DPAD_LEFT_DRIVE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_DPAD_LEFT_SENSE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_DPAD_RIGHT_DRIVE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_DPAD_RIGHT_SENSE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_DPAD_DOWN_DRIVE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_DPAD_DOWN_SENSE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_DPAD_UP_DRIVE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_DPAD_UP_SENSE = MT6592_J36_KEY_WIRE_UNMAPPED,
    /* A WAS 30--12 AND IT IS NOT A PAIR. The fourth run drove pad 30 HIGH while A was held
     * and pad 12 did not follow (sns=0), which a closed switch cannot do -- and pad 30's own
     * `hi=1 lo=0` on the same line proves the driver worked. What the old entry was reading
     * is pad 12 going low by itself, which is the pad-12 level in the _GPIO block above.
     *
     * R2 WAS 12--167 AND IT NEVER FIRED ONCE. Not in the second run, not in the third, not
     * in the fourth -- and the fourth is conclusive, because pad 12 is pulsed (START, which
     * shares it, works) and pad 167 senses fine (VOL+, which uses it, works). Both halves of
     * the entry are known good and the entry still reports nothing, so the pair does not
     * exist. It came from a `kpdwire a7` report that named pad 12, and pad 12 is A's pad
     * going low: the same artefact, a third time, in a third place.
     *
     * Leaving them here as UNMAPPED rather than deleting the names keeps the wire table's
     * invariant visible -- every remaining pair has both pads idling HIGH. */
    MT6592_J36_KEY_A_DRIVE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_A_SENSE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_R2_DRIVE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_R2_SENSE = MT6592_J36_KEY_WIRE_UNMAPPED,
    /*
     * THE WHOLE TABLE IS RETIRED, and the reason is at the top of this key section: these
     * five pairs are row 3 of the vendor keymap, and every "pad" in them is a KPD row or
     * column line. Driving pad 11 was strobing KPROW3 by hand; sensing 75, 167, 168, 12
     * and 2 was reading KPCOL0..KPCOL4. The scan worked -- and while it owned pads 11, 12
     * and 2, the KPD block could not scan row 3 or columns 3 and 4, which is exactly the
     * set of keys that stopped working: START, MENU, SELECT, VOL+, VOL-, A and R2.
     *
     * The five keys now come back from MEM1/MEM2 as bits 27, 28, 29, 30 and 31, scanned by
     * the block against a strobe it generates itself. Nothing here is driven, so pads 2,
     * 11 and 12 stay in the mux the preloader gave them.
     *
     * Kept as UNMAPPED names rather than deleted, because mt6592_keys.c derives g_key_wires[]
     * from them: with every entry unmapped the table is empty, key_wire_owned_mask() is 0,
     * and the scan is inert without any code being ripped out. If a future board revision
     * really does put a switch between two GPIOs, the machinery is still here and this
     * comment is the warning that comes with it -- check first whether the two "GPIOs" are
     * a row and a column.
     */
    MT6592_J36_KEY_START_DRIVE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_START_SENSE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_MENU_DRIVE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_MENU_SENSE = MT6592_J36_KEY_WIRE_UNMAPPED,
    /* The volume keys are the only pairs with NO choice of orientation: their other
     * side is a keypad pad (167 = sense 1, 75 = sense 0), and a keypad pad must never
     * be driven, so pad 11 is driven for both. That makes 11 a drive pad in its own
     * pulse and a sense pad in START's and MENU's, which is fine -- the pulses are
     * sequential and every drive pad is put back to input-pull-up before the next one
     * is touched -- but it is what costs this board its ghost-freedom, below. */
    MT6592_J36_KEY_VOL_UP_DRIVE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_VOL_UP_SENSE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_VOL_DOWN_DRIVE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_VOL_DOWN_SENSE = MT6592_J36_KEY_WIRE_UNMAPPED,
    /* SELECT is the eleventh switch and it is NOT a matrix key, which is the finding
     * that closed this connector. It is pad 11 to keypad sense 2 (pad 168) -- reported
     * by `kpdmon` as an unmapped short, 11--168, in the same run in which pressing
     * SELECT flipped START and MENU together. That lockstep is the corroboration, not a
     * separate puzzle: START senses pad 11 and MENU senses pad 11, so anything that
     * moves pad 11 moves both, and while pad 11 was still parked pull-DOWN, closing
     * SELECT dragged it into sense 168's pull-up and both keys followed. It also costs
     * nothing to scan: pad 11 is already pulsed for the two volume keys, so SELECT rides
     * that pass and the poll is still eight pulses. And it adds no ghost -- 168 hangs off
     * pad 11 by this one edge, so the edge is a bridge and no cycle can contain it. */
    /*
     * AND IT IS A MATRIX KEY AFTER ALL -- bit 29, row 3 x col 2, where col 2 is pad 168.
     * Every observation in this paragraph survives that correction and reads better for it:
     * the "unmapped short 11--168" is KPROW3 to KPCOL2, and START and MENU flipping in
     * lockstep whenever SELECT was pressed is three keys on one row line being dragged
     * together by a hand-rolled strobe. The reason SELECT "stopped working the moment the
     * wire scan armed pull-ups" is that the wire scan was taking its row away. */
    MT6592_J36_KEY_SELECT_DRIVE = MT6592_J36_KEY_WIRE_UNMAPPED,
    MT6592_J36_KEY_SELECT_SENSE = MT6592_J36_KEY_WIRE_UNMAPPED,
    /*
     * THE PAIR HALF OF THE CONNECTOR, and it has exactly one ghost. Seven switches over
     * seven pads (the D-pad left this graph when it turned out to be four levels):
     *
     *   pad  switches on it                                       driven?
     *    11  START(12) MENU(2) VOL+(167) VOL-(75) SELECT(168)      yes, for VOLs + SELECT
     *    12  A(30) R2(167) START(11)                               yes, for R2 and START
     *    75  VOL-(11)                                              no -- keypad sense 0
     *   167  R2(12) VOL+(11)                                       no -- keypad sense 1
     *   168  SELECT(11)                                            no -- keypad sense 2
     *     2  MENU(11)                30  A(12)                     yes (one switch each)
     *
     * Seven nodes, seven edges, connected -- so the cyclomatic number is 7-7+1 = 1 and
     * there is EXACTLY ONE cycle, unchanged by everything that has happened to this table:
     * the triangle 11-12-167, START(11-12), R2(12-167), VOL+(11-167). Verified by brute
     * force over every held-key subset (5110 cases, back when the graph had ten keys in
     * it): the only phantoms are the three the triangle predicts. Every other key is on a
     * bridge edge, and a bridge is in no cycle, so it can neither be fabricated nor
     * fabricate anything.
     *
     *   hold START + R2   -> VOL+ reads pressed
     *   hold START + VOL+ -> R2   reads pressed
     *   hold R2    + VOL+ -> START reads pressed
     *
     * TWO OF THREE IS INDISTINGUISHABLE FROM THREE OF THREE, and no cleverness fixes
     * it. Closing two edges of a triangle already makes all three pairwise connections
     * true, and a digital read through a pull-up measures nothing but connectivity --
     * an equivalence relation, which cannot tell you which edges produced it. Driving a
     * second pad high to break the tie would short two strong drivers through a held
     * switch, which is the one thing this file will not do. Series contact resistance
     * does differ (two switches versus one) but by ohms against a ~50k pull-up, which
     * is microvolts, and none of these pads is an AUXADC channel. Diodes would fix it;
     * the board does not have them.
     *
     * SO IT IS A DECODE, AND THE POLICY BELOW IS THE DECODE. When all three light,
     * report START|R2 and drop VOL+ -- maximum likelihood, because START+R2 is a
     * combination a player makes (pause with a trigger held) and the other two require
     * a volume key held during play. The cost, stated plainly: holding VOL+ and then
     * pressing R2 or START reports START|R2, which loses the real VOL+ and fabricates
     * one of the two face buttons. Set MT6592_J36_KEY_GHOST_PREFER_GAMEPLAY to 0 to
     * take the conservative decode instead -- report none of the three, which never
     * fabricates anything and instead makes START not work while R2 is held.
     *
     * The refinement, if this ever actually bites: the triangle can only complete on
     * the SECOND closure, so a poll that saw exactly one of the three lit knows that
     * one is real. Anchoring on it removes the "loses the real VOL+" half of the cost.
     * It needs state in what is currently a stateless level read, so it is not here.
     */
    MT6592_J36_KEY_GHOST_PREFER_GAMEPLAY = 1u,
    /*
     * WHY THESE PADS ARE SAFE TO DRIVE -- and the claim that used to be here was wrong.
     *
     * It said: `kpdwire` only accepts a candidate that is already a mode-0 input, so
     * every pad it has ever reported is one. Only the second half of that is false in a
     * way that matters. kpdwire refuses to WRITE a muxed pad's pull, because that pull
     * belongs to whatever owns the pad -- but it still READS the pad, and still reports
     * it. So a reported pad is a pad that follows the driven node, and nothing more; its
     * resting mux was never recorded for any of them, not just for pad 30.
     *
     * Hardware billed us for that. The first run of the wire scan produced exactly four
     * working keys -- START, MENU, VOL+, VOL- -- whose drive pads are 2, 11 and 12, and
     * exactly six dead ones whose drive pads are 8, 20, 30, 35 and 45. Not a wiring
     * pattern: the scan refused to pulse a pad that was not already mode 0, and those six
     * pads are not. key_wires_scan() now forces mode 0 for the ~30 us of the pulse and
     * puts the mux straight back, which is what `kpdwire` has always done to its named
     * pin -- to 11, 12, 30, 75 and 167 already, without incident. What is still refused
     * is what could actually break something: a keypad pad, and a pad somebody else has
     * already made an output.
     */

    /*
     * MTK keypad matrix map — the stock kernel's own kpd_keymap[72], decoded.
     *
     * WHERE IT COMES FROM. Reference/J36-ULTRA/kernel, MTK header 0x200, gzip
     * payload at file offset 0x4934, inflated with a raw-deflate window; the array
     * is at offset 0x887778 in the inflated image. Its extent is not a guess: the
     * bytes immediately before it are the kpd module's own parameter-name strings
     * ("kpd_pdrv_probe", "kpd.kpd_show_register", "kpd.kpd_show_hw_keycode"), and
     * the bytes immediately after the 72nd entry are two kernel pointers
     * (0xc0477524, 0xc0477ebc). It is the only 72-entry u16 array in the whole
     * 12 MB image whose non-zero values are all drawn from the code set in
     * /system/usr/keylayout/mtk-kpd.kl -- a filter that rejects every identity
     * ramp and every ADC LUT that shape-based scans kept turning up.
     *
     * WHAT IT SAYS, verbatim, at stride nine (index = row*9 + col, which is the
     * geometry FUN_81e0a530 assumes and the reason its PMIC case is `index%9==8`):
     *
     *   row 0:  2  3  4  5 11   ->  UP    DOWN  LEFT  RIGHT  (11 = spare "0")
     *   row 1:  6  7  8  9 23   ->  B     A     Y     X      (23 = spare "I")
     *   row 2: 10 30 48 46 36   ->  L1    L2    R2    R1     (36 = spare "J")
     *   row 3: 32 18 33 34 35   ->  MODE  spare START SELECT (18/35 = "E"/"H")
     *   rows 4..7 and column 4..8: all zero.
     *
     * Names are mtk-kpd.kl's, which is the right file for these codes: kpd.c sets
     * input_dev->name = "mtk-kpd". Volume, Power and MENU(139) appear in that .kl
     * but NOT in this array -- they are not matrix keys on this board.
     *
     * THIS TABLE REPLACES AN INVERTED ONE, and the inversion is the bug behind
     * "the D-pad, A, start/menu and R2 do not work". The previous map read
     *
     *     bits 0,1,2 = L1,L2,R1     bits 9,10,11 = X,Y,B
     *
     * which is the same six live bits relabelled. It was defended on the grounds
     * that bits 0,1,2 and 9,10,11 fired "while the D-pad and the face buttons were
     * being worked" -- but that capture never recorded which button gave which bit,
     * and the comment that used to sit here said so itself, then drew a conclusion
     * from it anyway. Six unattributed bits cannot choose between two layouts. The
     * kernel's own table can, and it says the D-pad is bits 0..3 and A is bit 10.
     * Under the old map, pressing UP reported L1 and pressing A reported Y.
     *
     * WHAT IS REACHABLE, and why that is a mux fact and not a wiring fact. The
     * keypad pad family is three strobes {74, 92, 93} against three senses
     * {75, 167, 168} -- reg 0 and reg 1 of the {pin,reg,bit} table that appears
     * byte-identically at preloader 0x1aac6 and kernel 0x887154. The preloader's
     * mtk_kpd_gpio_set muxes only FIVE of those six. Disassembled (filler at
     * 0x53d0, one literal plus four adds, so there is no ambiguity):
     *
     *     0x8000004a -> 74   strobes: mode 1, dir OUT, pull off
     *     +0x12 = 0x8000005c -> 92
     *     -0x11 = 0x8000004b -> 75   senses:  mode 1, dir IN,  pull UP
     *     +0x5c = 0x800000a7 -> 167
     *     +0x01 = 0x800000a8 -> 168
     *
     * Two strobes crossed with three senses is exactly bits {0,1,2,9,10,11} -- the
     * set that has ever moved on this board. Pad 93 is left as a parked GPIO, and
     * nothing else raises it: stock LK never calls its own mt_set_gpio_mode, and
     * the stock kernel contains no KROW/KCOL/kpd_gpio symbol and no gpio-keys
     * driver at all. So on the stock chain the matrix reaches six keys, full stop.
     *
     * WHAT IS STILL DEAD ON THE MATRIX, stated plainly so it is not rediscovered:
     *
     *   RIGHT (3), X (12)            need a FOURTH SENSE (column 3)
     *   L1 (18), L2 (19), R2 (20)    need a THIRD STROBE (row 2)
     *   R1 (21)                      needs both
     *   MODE (27), START (29)        need a FOURTH STROBE (row 3)
     *   SELECT (30)                  needed both -- and is now off the matrix entirely,
     *                                measured as the wire pair 11--168
     *
     * FIVE OF THOSE ARE WIRE PAIRS -- RIGHT, R2, MODE/MENU, START, SELECT -- so their
     * dead bits cost nothing, and that is why the missing pads stopped mattering:
     * 6 matrix + 11 wire + 1 PMIC = 18, which is every input on the shell. There is no
     * third strobe and no fourth sense left to find, and `kpdpad` should not be pointed at
     * pads looking for one (`kpdsweep`, the range version, has powered this board off
     * twice; see below).
     *
     * WHICH LEAVES X, L1, L2 AND R1 MISNUMBERED RATHER THAN DEAD. Take the census as
     * given: the six matrix keys are B, X, Y, L1, L2 and R1, because every other button on
     * the shell is a wire pair or the PMIC key. Six keys, and exactly six reachable bits
     * {0,1,2,9,10,11}. So those bits belong to those six keys in SOME order -- which means
     * bits 0,1,2 are not UP/DOWN/LEFT and bit 10 is not A, whatever the vendor keymap
     * says, since all four of those are measured wire pairs. The set is arithmetic; the
     * permutation is not, and only the hold-one-button-at-a-time run below can supply it. None of those three pads is in the keypad {pin,reg,bit}
     * family, so all three are pads with ordinary pull control that no artefact in
     * Reference/ names. An earlier revision closed the row-2 gap by drafting pad 93,
     * on the reading that register 0 of that family (74@2, 92@6, 93@10) made 93 the
     * third strobe. The pad table does say 93 CAN carry a row; it does not say this
     * board wires it as one, and the D-pad evidence says it does not -- see the
     * strobe declarations above. The gap is open again, honestly.
     *
     * All ten are mapped below anyway: an unmuxed row or column reads as all-ones,
     * so a claimed-but-unscanned bit reports "not pressed" and costs one comparison
     * per poll. Nothing misbehaves, and the day the pads are found the map is
     * already right. Until then these buttons need GPIO backings, which is the only
     * path that has ever actually delivered a press on this board.
     *
     * `kpdpad <pin> [1=strobe|0=sense]` is the console command that finds the pads:
     * it muxes one candidate into the keypad block with the preloader's recipe,
     * watches all five scan words while you hold the dead button, prints any bit
     * outside the known-live set with the row/column it implies, and restores the
     * pad. A new bit in 18..22 is the third strobe, in 27..31 the fourth strobe, and
     * in {3,12,21,30} the fourth sense. Note that `kpdsweep`, the range version, has
     * powered this board off twice (pads 15 and 16, both now skipped) -- one pad per
     * run is the safe way.
     *
     * Two GPIO-space facts from earlier kpdmon runs:
     *
     *   gpio 11   was recorded as answering BOTH volume keys -- VOL+ moving gpio 11
     *             alone, VOL- moving gpio 11 together with matrix bit 0. RESOLVED, and
     *             it was never a level on pad 11 at all. `kpdwire b` measured VOL+ as
     *             11--167 and VOL- as 11--75, and 75 IS keypad sense 0: closing VOL-
     *             welds pad 11 to the sense-0 line, which drags column 0 and therefore
     *             matrix bit 0, for as long as the key is held. VOL+ welds it to sense 1
     *             instead, a column this board's keymap slice does not surface, which is
     *             why that one looked like "gpio 11 alone". Both volume keys are on the
     *             wire scan now. The bit-0 coincidence is why this note read as "VOL- is
     *             somehow also UP" for so long; it is a column drag, not a key.
     *   gpio 112  is PANEL POWER, not an LED. Driving it low blanks the display.
     *             Do not probe it again.
     *
     * EVERY MATRIX BIT ON THIS BOARD IS NOW ATTRIBUTED BY NAME. Fourth hardware run, in
     * `kpdmon`, with the pull-ups armed: eight buttons pressed one at a time in the order
     * L1, L2, R2, R1, X, Y, A, B produced exactly eight event groups, in order:
     *
     *     bit 0 (row 0 col 0)   <- L1
     *     bit 1 (row 0 col 1)   <- L2
     *     pad 12 low, no pulse  <- R2
     *     bit 2 (row 0 col 2)   <- R1
     *     bit 9 (row 1 col 0)   <- X
     *     bit 10 (row 1 col 1)  <- Y
     *     pad 12 low, no pulse  <- A
     *     bit 11 (row 1 col 2)  <- B
     *
     * Eight presses, eight groups, nothing extra and nothing missing, so the alignment is
     * forced rather than assumed -- and it is self-checking in two independent ways. The two
     * pad-12 groups fall at positions three and seven, which are exactly R2 and A, the two
     * keys that had already been measured onto pad 12 by a completely different mechanism.
     * And the resulting layout is physically sensible: row 0 (strobe 74) is the three
     * shoulder buttons and row 1 (strobe 92) is the three face buttons. Neither of those was
     * an input to the ordering; both fall out of it.
     *
     * THE VENDOR KEYMAP WAS WRONG ABOUT ALL SIX. It said B=9, Y=11, X=12, L1=18, L2=19,
     * R1=21 -- three of them on bits this board cannot scan at all, and the three that were
     * in range were a permutation of the wrong keys. Every one of those numbers is now
     * replaced by a measurement. Keeping them would have made X report as B and B as Y.
     *
     * The paragraph this replaces said the assignments were the vendor's and warned they
     * were weak, because there is a second way for a matrix bit to move here and every
     * recorded bit event at the time fit it: a COLUMN DRAG. That mechanism is real and is
     * why the earlier runs could not be read as attributions -- and it is also why this one
     * can. Sense 0/1/2 are pads 75/167/168 and
     * they are columns 0/1/2, so bit = row*9 + col puts column 0 at bits {0,9}, column 1
     * at {1,10} and column 2 at {2,11}. A wire switch whose far side is a sense pad welds a
     * button pad to that column, and if the button pad is sitting on its PARKED PULL-DOWN
     * the pull-down wins, the column reads low for as long as the key is held, and every
     * row of that column reports pressed. The first hardware run shows exactly this and
     * nothing else: bit 10 (column 1 = pad 167) while VOL+ was pressed, bit 11 (column 2 =
     * pad 168) while SELECT was pressed, and the older note below records bit 0 (column 0 =
     * pad 75) while VOL- was pressed. Three keys, three columns, three bits, all of them
     * wire pairs. The vendor keymap would call those bits A, Y and UP.
     *
     * ARMING THE PULL-UPS IS WHAT MADE THAT RUN A MEASUREMENT, and it is worth keeping the
     * argument because it is the whole reason the six bits above can be believed. With both
     * sides of every switch pulled up there is no longer a path to ground through a closed
     * key, so a held wire key cannot drag its column -- and the runs in which the drags
     * appeared are precisely the runs where key_gpio_arm_pullup() was preserving parked
     * pull-downs instead of overriding them. Fix the arming, and a matrix bit that moves is a
     * matrix key. The fourth run then produced six clean, one-at-a-time bit events with no
     * drags anywhere in it, which is both the attribution and the confirmation that the
     * arming fix works.
     *
     * The linear index is the same numbering the LK handoff scanner, the flash tool
     * and mt6592_keys.c use: KPD_MEM1 bit 0 is matrix bit 0, KPD_MEM2 bit 0 is
     * matrix bit 16, and so on.
     *
     * Polarity is vendor active-low: the scan words idle at all-ones
     * (mem1..4=0xffff, mem5=0xff) and a held key pulls its bit to 0 — the LK
     * decompile (FUN_81e0a530) and the kernel kpd driver both read it that
     * way, so MVII does too (no baseline guessing).
     *
     * WHAT STOCK LK ACTUALLY DOES WITH THIS BLOCK, read straight out of
     * Reference/j36-lk-reverse/lk.full-decompile.c, because "keypad was working" in LK is
     * true and is much weaker than it sounds:
     *
     *   FUN_81e0a530 (line 10938) is the ONLY code in all of LK that touches KPD_BASE:
     *       uVar2 = *(ushort *)((param_1 >> 4) * 4 + 0x10011004);
     *       uVar3 = 1 << (param_1 & 0xf);
     *   One MEM word, linear bit index, active-low, guarded by param_1 < 0x48 (72 keys).
     *   That is bit-for-bit the indexing this header uses, so the numbering above is
     *   confirmed by the bootloader that works.
     *
     *   BUT: if param_1 % 9 == 8 (or param_1 == 8) it does not read the matrix at all --
     *   it calls FUN_81e07dd8, which reads PMIC register 0x142 bit 1. COLUMN 8 OF THE
     *   NINE IS THE PMIC KEY, not a matrix bit. Nothing in this header claims a bit in
     *   column 8, and nothing should.
     *
     *   AND: LK NEVER INITIALISES THE KPD BLOCK. There is no write to DEBOUNCE, SEL or
     *   EN anywhere in the decompile (the only other `10011` hits in the objdump are
     *   `andseq` false positives). LK inherits whatever the preloader left running, which
     *   is why MVII must program KPD itself and why the preloader's pad list is the ceiling.
     *
     *   AND: its only two callers (FUN_81e1c2c8, lines 21911 and 22011) pass indices 0 and
     *   1 and nothing else. LK's "working keypad" is a two-key boot UI -- up and select, in
     *   effect. It never demonstrated a D-pad, a face button or a shoulder button, so it is
     *   no evidence against the three-mechanism model below; it is one data point that the
     *   matrix path and the bit numbering are real.
     */
    /*
     * THIRTEEN BITS, FROM kpd_keymap[72] IN THE STOCK KERNEL, and every one of them backed
     * by a live measurement on this device. bit = row * 9 + col.
     *
     *          col0    col1    col2    col3    col4      strobe pad
     *   row 0  L1 0    L2 1    R1 2    R2 3      -        74
     *   row 1  X  9    Y 10    B 11    A 12      -        92
     *   row 2  (dead: KPROW2 is pad 93, spent on UP's EINT)   93
     *   row 3  V- 27   V+ 28   SEL 29  STA 30  MENU 31    11
     *          pad 75  pad 167 pad 168 pad 12  pad 2
     *
     * TWO INDEPENDENT DERIVATIONS AGREE, which is why this is not a revert-and-hope. The
     * vendor keymap gives all thirteen. The four hardware runs of this session re-derived
     * six of them by press ordering (L1=0, L2=1, R1=2, X=9, Y=10, B=11) and got all six
     * right, and the five "wire pairs" pin down row 3 column by column: VOL- sensed pad 75
     * = col0 = 27, VOL+ pad 167 = col1 = 28, SELECT pad 168 = col2 = 29, START pad 12 =
     * col3 = 30, MENU pad 2 = col4 = 31. The keymap and the pulse scan were describing the
     * same row from two sides. Older boot-status captures add two more spot checks from a
     * third direction: "raw bit 30 = physical START, raw bit 10 = physical Y".
     *
     * BIT 3 (R2) IS THE ONLY ONE OF THE THIRTEEN WITH NO DIRECT MEASUREMENT. Tallying what
     * backs each: bits 0, 1, 2, 9, 10, 11 from this session's press-ordering runs; bits 0,
     * 9, 10, 11, 12, 29, 30 from a live boot-status capture at the working commit ("face
     * button mashing lights exactly bits 0, 9..12, 29, 30"); bits 27, 28, 29, 30, 31 from
     * the wire pairs' column-by-column agreement. Twelve of thirteen, by three independent
     * methods that overlap and do not contradict each other anywhere. Bit 3 rests on the
     * vendor table alone -- and on the behaviour reproduced on every hardware run this
     * session, that R2 and A both pull pad 12 low with nothing driven, which is what two
     * switches sharing column 3 do and is not otherwise explicable.
     *
     * WHAT THE PREVIOUS VERSION OF THIS BLOCK CLAIMED, and the shape of the mistake: "those
     * six bits are every bit this board can scan", "the matrix half of this keymap is
     * closed: no free bits, no unplaced keys, no vendor numbers left in it", and a warning
     * not to re-derive any single line from the vendor keymap because it would "silently
     * undo all six". The six bits were right. The closure was an artefact of kpd_pads_mux()
     * having unplugged one row and two columns, and the warning defended six correct values
     * by ruling out the source that had the other seven.
     */
    /* Row 2, whose strobe pad this board reassigned to UP's EINT. The vendor keymap does
     * list the D-pad here (bits 18-21) and it can never fire. */
    MT6592_J36_KEY_DPAD_UP_MATRIX = MT6592_J36_KEY_MATRIX_UNMAPPED,
    MT6592_J36_KEY_DPAD_DOWN_MATRIX = MT6592_J36_KEY_MATRIX_UNMAPPED,
    MT6592_J36_KEY_DPAD_LEFT_MATRIX = MT6592_J36_KEY_MATRIX_UNMAPPED,
    MT6592_J36_KEY_DPAD_RIGHT_MATRIX = MT6592_J36_KEY_MATRIX_UNMAPPED,
    /* Row 0 (strobe 74), the shoulders. Bits 0/1/2 are also the session's own measurement;
     * bit 3 is R2, on col3 = pad 12, sharing that column with A below. */
    MT6592_J36_KEY_L1_MATRIX = 0u,
    MT6592_J36_KEY_L2_MATRIX = 1u,
    MT6592_J36_KEY_R1_MATRIX = 2u,
    MT6592_J36_KEY_R2_MATRIX = 3u,
    /* Row 1 (strobe 92), the face buttons. Bits 9/10/11 measured; bit 12 is A. Note that
     * the vendor keymap's row-1 order is X, Y, B, A -- B and A are NOT adjacent in the
     * obvious way, and reading this row as "9=B, 10=Y, 11=X" is what an earlier revision
     * did and is why it concluded the vendor table was wrong about all six. */
    MT6592_J36_KEY_X_MATRIX = 9u,
    MT6592_J36_KEY_Y_MATRIX = 10u,
    MT6592_J36_KEY_B_MATRIX = 11u,
    MT6592_J36_KEY_A_MATRIX = 12u,
    /* Row 3 (strobe 11), the five keys that spent this session being read as wire pairs.
     * Nothing about them is special; they are the row whose strobe pad the wire scan was
     * pulsing by hand. */
    MT6592_J36_KEY_VOL_DOWN_MATRIX = 27u,
    MT6592_J36_KEY_VOL_UP_MATRIX = 28u,
    MT6592_J36_KEY_SELECT_MATRIX = 29u,
    MT6592_J36_KEY_START_MATRIX = 30u,
    MT6592_J36_KEY_MENU_MATRIX = 31u,
    MT6592_J36_KEY_POWER_MATRIX = MT6592_J36_KEY_MATRIX_UNMAPPED,

    MT6592_J36_SOFTWARE_FB_ADDR = 0x82700000u,
    MT6592_J36_SOFTWARE_FB_WIDTH = 640u,
    MT6592_J36_SOFTWARE_FB_HEIGHT = 480u,
    MT6592_J36_SOFTWARE_FB_BPP = 32u,
    MT6592_J36_SOFTWARE_FB_PITCH = MT6592_J36_SOFTWARE_FB_WIDTH * 4u,

    /*
     * Values captured from the working stock-LK handoff scanner. This is the
     * display profile MVII should preserve when it starts after LK: 32-bit
     * software pixels at 0x82700000, OVL/RDMA already running, DSI video output
     * in RGB888 packet mode.
     */
    MT6592_J36_LK_HANDOFF_FB_ADDR = 0x82700000u,
    MT6592_J36_LK_HANDOFF_FB_WIDTH = 640u,
    MT6592_J36_LK_HANDOFF_FB_HEIGHT = 480u,
    MT6592_J36_LK_HANDOFF_FB_BPP = 32u,
    MT6592_J36_LK_HANDOFF_FB_PITCH = 2560u,
    MT6592_J36_LK_HANDOFF_DSI_MODE = 0x00000002u,
    MT6592_J36_LK_HANDOFF_DSI_PSCTRL = 0x00030780u,
    MT6592_J36_LK_HANDOFF_DSI_VM_CMD_CON = 0x00000021u,
    MT6592_J36_LK_HANDOFF_OVL_SRC_CON = 0x0000000cu,
    MT6592_J36_LK_HANDOFF_RDMA_GLOBAL_CON = 0x00000101u,
    MT6592_J36_LK_HANDOFF_RDMA_MEM_CON = 0x00000080u,
    MT6592_J36_LK_HANDOFF_MUTEX_MOD = 0x00000488u,
    MT6592_J36_LK_HANDOFF_BLS_EN = 0x00010000u,
};

#define MT6592_J36_PANEL_PART "JD9365 QC 190227"
#define MT6592_J36_PANEL_LK_DRIVER "jd9365_qc_190227_lcm_drv"
#define MT6592_J36_PANEL_LK_ALIAS "J36-ULTRA stock 3.5in 640x480 MIPI"
#define MT6592_J36_PANEL_STOCK_LK_DRIVER MT6592_J36_PANEL_LK_DRIVER
#define MT6592_J36_PANEL_STOCK_LK_ALIAS MT6592_J36_PANEL_LK_ALIAS
#define MT6592_J36_LCM_CONNECTOR "CON901/AYF332535_LCM"
#define MT6592_J36_TOUCH_CONTROLLER "GT913"
#define MT6592_J36_BACKLIGHT_DRIVER "TPS61161"

#define MT6592_J36_DSI_CLK_P_BALL "M3"
#define MT6592_J36_DSI_CLK_N_BALL "M4"
#define MT6592_J36_DSI_D0_P_BALL "M1"
#define MT6592_J36_DSI_D0_N_BALL "M2"
#define MT6592_J36_DSI_D1_P_BALL "P2"
#define MT6592_J36_DSI_D1_N_BALL "N2"
#define MT6592_J36_DSI_D2_P_BALL "P1"
#define MT6592_J36_DSI_D2_N_BALL "R1"
#define MT6592_J36_LCM_RST_BALL "U3"
#define MT6592_J36_DSI_TE_BALL "Y3"
#define MT6592_J36_DISP_PWM_BALL "AD9"
#define MT6592_J36_KCOL0_BALL "AB13"

#endif
