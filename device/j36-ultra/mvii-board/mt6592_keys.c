#include "mt6592_keys.h"

#include <stdint.h>

#include "mt6592_board_j36.h"
#include "mt6592_bootinfo.h"
#include "mt6592_delay.h"
#include "mt6592_pwrap.h"
#include "mt6592_uart.h"

/*
 * J36 Ultra button reader.
 *
 * MVII does not drive the device into BROM on its own anymore -- the BROM and
 * preloader already enter download mode when Power + Volume-Down are held at
 * power-on, so there is no software rescue key here. This driver only reads the
 * handheld buttons as plain controls. The J36 gamepad cluster is exposed by
 * Android's mtk-kpd device with a board-specific Vendor_2454_Product_6500.kl
 * mapping; most buttons are matrix-backed, while optional direct GPIO entries
 * remain available for wiring revisions.
 *
 * Each button's GPIO lives in mt6592_board_j36.h. Buttons left at
 * MT6592_J36_KEY_GPIO_UNMAPPED are skipped and reported as never-pressed, so
 * the build is correct before the full wiring map is filled in.
 */

enum {
    GPIO_BASE = 0x10005000u,
    GPIO_DIR_BASE = 0x0000u,
    GPIO_PULLEN_BASE = 0x0100u,
    GPIO_PULLSEL_BASE = 0x0200u,
    GPIO_DOUT_BASE = 0x0400u,
    GPIO_DIN_BASE = 0x0500u,
    GPIO_VAL_REG_STRIDE = 0x0010u,
    GPIO_VAL_REG_SET = 0x0004u,
    GPIO_VAL_REG_RST = 0x0008u,
    GPIO_PINS_PER_VAL_REG = 16u,
    GPIO_MODE_BASE = 0x0600u,
    GPIO_MODE_STRIDE = 0x0010u,
    GPIO_MODE_BITS = 3u,
    GPIO_MODE_PER_REG = 5u,
    GPIO_MODE_MASK = 0x7u,

    KPD_BASE = 0x10011000u,
    KPD_STA = 0x0000u,
    KPD_MEM1 = 0x0004u,
    KPD_MEM2 = 0x0008u,
    KPD_MEM3 = 0x000cu,
    KPD_MEM4 = 0x0010u,
    KPD_MEM5 = 0x0014u,
    KPD_DEBOUNCE = 0x0018u,
    KPD_SCAN_TIMING = 0x001cu,
    KPD_SEL = 0x0020u,
    KPD_EN = 0x0024u,
    KPD_DEBOUNCE_DEFAULT = 0x00000400u,
    KPD_DEBOUNCE_MASK = 0x00003fffu,
    /* KP_SEL bit 0 is MTK's double-key enable, and clearing it is all the vendor
     * chain ever does to this register. Worth spelling out because a live dump
     * reading `sel=0x0000` looks like an uninitialised scan-column mask and was read
     * that way once: it is not, it is the register in its intended state. */
    KPD_SEL_DOUBLE_KEY = 0x0001u,
    /* MT6323 register 0x0040, bit 0: the keypad's 32 kHz clock gate. The preloader
     * clears it immediately before muxing the pads and enabling the block, and names
     * it in its own log strings -- "kpd read addr: 0x0040: data:0x%x" and "kpd write
     * fail, addr: 0x0040" (file offsets 0x17711 and 0x176f3, referenced from the
     * routine at 0x546c). Nothing downstream re-gates it, so on a normal boot this
     * is already clear; MVII clears it anyway because the cost is one PWRAP
     * round-trip and the failure it prevents -- an enabled scanner that never scans
     * -- looks exactly like broken wiring. */
    KPD_PMIC_CLK_GATE_REG = 0x0040u,
    KPD_PMIC_CLK_GATE_BIT = 0x0001u,
    KPD_NUM_MEMS = 5u,
    KPD_MEM_BITS = 16u,
    KPD_MEM5_VALID_MASK = 0x000000ffu,

    /* Wire-scan settle, in the ~33-cycles-per-microsecond units the AUXADC code in
     * this file already uses: ~30 us. The edge being measured is actively driven low
     * by the SoC, so this is generous -- the slow edge is the release, and that
     * happens on the pull-up after the pad is restored and long before the next poll.
     * Five drive pads at 30 us is 150 us per frame, which is nothing next to the two
     * AUXADC conversions in the same poll. */
    KEY_WIRE_SETTLE_CYCLES = 1000u,
    /* One line per change of the wire mask, not per frame -- a press and a release
     * are two lines, so this is ~12 button events and then silence. Enough to confirm
     * the mapping from a boot-status readback without touching the frame budget. */
    KEY_WIRE_LOG_LIMIT = 24u,
    /* Generous: these lines only fire on real scan-memory changes (physical
     * presses), and they are the primary tool for mapping which matrix bit a
     * given button toggles on this board. */
    KPD_DELTA_LOG_LIMIT = 96u,

    /*
     * J36 analog sticks — ground truth reverse-engineered from the stock
     * kernel's joystick poller (Reference/J36-ULTRA boot.img, kpd driver
     * region; poll worker at kernel VA 0xc0475ed8):
     *
     *   IMM_GetOneChannelValue(12) -> input_report_abs(ABS_X)   left stick X
     *   IMM_GetOneChannelValue(13) -> input_report_abs(ABS_Y)   left stick Y
     *   IMM_GetOneChannelValue(14) -> input_report_abs(ABS_Z)   right stick X
     *   IMM_GetOneChannelValue(15) -> input_report_abs(ABS_RZ)  right stick Y
     *
     * The sticks are on SoC AUXADC channels 12..15, NOT 0/1. Channels 0/1 are
     * unconnected on this board: they float, charge up from the ADC's own
     * sampling (the "warm-up climb" in old captures), settle at a rate-
     * dependent equilibrium, and respond to a finger on the case — which is
     * exactly the ghost behavior earlier bring-up chased. Never read them.
     *
     * input_set_abs_params in the same probe gives the physical range:
     * |raw| spans 800..3900 (center ~2350), and X/Y are registered with a
     * NEGATED range (-3900..-800) — the vendor reports -raw for X/Y, i.e.
     * raw DECREASES toward Android +X (right) and +Y (down).
     */
    AUXADC_BASE = 0x11001000u,
    AUXADC_CON1_SET = 0x0008u,
    AUXADC_CON1_CLR = 0x000cu,
    AUXADC_CON2 = 0x0010u,
    AUXADC_CON2_BUSY = 1u << 0,
    AUXADC_DAT0 = 0x0014u,
    AUXADC_DAT_STRIDE = 0x0004u,
    AUXADC_DAT_READY = 1u << 12,
    AUXADC_DAT_MASK = 0x0fffu,
    /* Vendor IMM_GetOneChannelValue settle: ~25 us after stopping and after
     * restarting the channel (33 legacy cycles/us convention). */
    AUXADC_SETTLE_CYCLES = 850u,
    /* Stick channel map. The mouse cursor follows the LEFT stick, which is the
     * ch14/15 pair on this unit (ch12/13 is the right stick and is not read).
     * The left stick's channel order and polarity are MIRRORED from the right
     * stick — verified on-device from the directional report:
     *   with X=ch14/Y=ch15 (+1/+1), left drove the cursor down, right drove it
     *   up, up drove it right and down drove it left. That means:
     *     ch15 is the HORIZONTAL channel, and its raw value FALLS pushing right
     *     ch14 is the VERTICAL   channel, and its raw value FALLS pushing down
     *   so X must read ch15 and Y must read ch14, both with NEGATIVE sign (see
     *   AUXADC_JOY_*_SIGN) to get +x=right / +y=down. */
    AUXADC_JOY_X = 15u,   /* left-stick horizontal (falls right) */
    AUXADC_JOY_Y = 14u,   /* left-stick vertical   (falls down)  */
    AUXADC_JOY_Z = 12u,   /* right stick (unused by the cursor) */
    AUXADC_JOY_RZ = 13u,
    AUXADC_SCAN_CHANNELS = 16u,
    AUXADC_POLL_LIMIT = 96u,
    /* Fallback center used only until runtime calibration completes; the pot
     * range is 800..3900, so mid-travel is ~2350. */
    AUXADC_JOY_FALLBACK_CENTER = 2350,
    /* Rest-band half-width in raw counts. Half-travel is ~1550 counts, so 150
     * (~10%) absorbs pot centering error and rest noise without eating
     * meaningful travel. */
    AUXADC_JOY_DEADZONE = 150,
    /* Velocity gain applied to (deflection - deadzone): (1550-150)*3 ≈ 4200,
     * clamped to the ±4096 full-scale the rel converter expects, so a full
     * physical tilt maps to full cursor speed. */
    AUXADC_JOY_GAIN = 3,
    /* Channel-pair discovery threshold, in summed raw counts of deflection.
     * Real deflections are hundreds of counts; floating channels move far
     * less once the proper stop/settle/start read sequence is used. */
    AUXADC_JOY_DISCOVERY_THRESHOLD = 400,
    AUXADC_JOY_LOG_THRESHOLD = 300,
    AUXADC_JOY_AXIS_FULL = 4096,
    /* Left stick polarity (ch15=X horizontal, ch14=Y vertical): both channels
     * FALL as the stick moves toward cursor-positive (right / down), so negate
     * both to get +x=right and +y=down. Verified from the on-device directional
     * report (see the channel map above). */
    AUXADC_JOY_X_SIGN = -1,
    AUXADC_JOY_Y_SIGN = -1,
    /* Rest-detect calibration. The AUXADC can overshoot briefly after the
     * clock ungate; joy_plateau_rebase() heals residual mislocks. Keep the
     * flat window long enough to skip a short warm-up plateau, but not so
     * long that stick setup feels multi-second next to instant D-pad. */
    JOY_CAL_MIN_SAMPLES = 4u,
    JOY_CAL_MAX_JITTER = 40,
    /* Timings cut ~75% from the previous 0.3 s / 1.2 s / 0.8 s set. */
    JOY_CAL_MIN_FLAT_US = 75000,     /* ~75 ms flat before first lock */
    JOY_CAL_FORCE_US = 300000,       /* best-effort center by ~0.3 s */
    /* Dead-GPT fallbacks (the 13 MHz APXGPT is started by the preloader/LK
     * and drives engine pacing, so this is a should-never-happen path): use
     * sample counts instead of time. */
    JOY_CAL_GPT_DEAD_SAMPLES = 8u,       /* unchanged ticks over this many samples = dead */
    JOY_CAL_GPT_DEAD_MIN_SAMPLES = 12u,  /* ~0.2 s at 60 Hz when GPT dead */
    JOY_CAL_MAX_ATTEMPTS = 24u,          /* force-lock cap when GPT dead (~0.4 s) */
    /* Post-lock plateau rebase: any stream that stays inside the jitter band
     * for this long can only be a resting stick (a held deflection wobbles
     * more, and even a perfectly-still hold recovers on release), so its
     * average becomes the new center. Heals both the warm-up mislock and slow
     * thermal drift. */
    JOY_REBASE_FLAT_US = 200000,
    JOY_REBASE_MIN_BIAS = 75, /* half the deadzone */
    /* Auto-center only inside a band well below real deflections. The old
     * threshold of 48 was ~80% of the measured full travel (~60 counts), so
     * the recenter logic chased every held/slow stick movement — visible in
     * the boot logs as cx/cy drifting tens of counts between samples — and
     * released sticks then drifted the other way. 10 counts only trims true
     * rest bias (jitter band is ~±7..13), and the slower divisor keeps the
     * center from following brief excursions. */
    JOY_RECENTER_THRESHOLD = 40,
    JOY_RECENTER_DIVISOR = 8,
    /* Discard the first AUXADC scans before locking the rest center: the first
     * conversions after the clock ungate read low, which is what miscalibrated
     * the center and produced the persistent drift. */
    JOY_SEED_SETTLE_SCANS = 1u,

    /* SoC AUXADC peripheral clock gate. AUXADC sits on the peripheral bus, so
     * its clock is in PERICFG PERI_PDN0. Writing to the CLR register only
     * ungates the named bits (per-bit atomic), so this cannot disturb other
     * modules. lcd_drv.c already ungates PERI_PDN0 bits 0..19 the same way; the
     * AUXADC bit is above that window and is never reached on the stage2 boot
     * path, which is why the stick works right after flashing (the download DA
     * left AUXADC clocked) but is dead on a plain reboot. */
    PERICFG_BASE = 0x10003000u,
    PERI_PDN0_CLR = 0x0010u,
    PERI_PDN0_STA = 0x0018u,
    PERI_PDN0_AUXADC_BITS = 0x0ff00000u, /* bits 20..27 cover AUXADC + neighbours */

    /* Per-motion diagnostics are too expensive for the boot-status UART ring
     * during normal runtime: every line stalls the kernel refresh path. Keep
     * one-shot bring-up logs, but leave per-frame traces opt-in for board
     * mapping sessions. */
    INPUT_RUNTIME_TRACE = 0u,
};

typedef struct {
    uint32_t mask; /* MT6592_KEY_* bit */
    uint32_t gpio; /* MT6592 GPIO number, or MT6592_J36_KEY_GPIO_UNMAPPED */
    uint32_t matrix; /* Linear kpd-bits index, or MT6592_J36_KEY_MATRIX_UNMAPPED */
} key_map_t;

static const key_map_t g_key_map[] = {
    {MT6592_KEY_DPAD_UP, MT6592_J36_KEY_DPAD_UP_GPIO, MT6592_J36_KEY_DPAD_UP_MATRIX},
    {MT6592_KEY_DPAD_DOWN, MT6592_J36_KEY_DPAD_DOWN_GPIO, MT6592_J36_KEY_DPAD_DOWN_MATRIX},
    {MT6592_KEY_DPAD_LEFT, MT6592_J36_KEY_DPAD_LEFT_GPIO, MT6592_J36_KEY_DPAD_LEFT_MATRIX},
    {MT6592_KEY_DPAD_RIGHT, MT6592_J36_KEY_DPAD_RIGHT_GPIO, MT6592_J36_KEY_DPAD_RIGHT_MATRIX},
    {MT6592_KEY_A, MT6592_J36_KEY_A_GPIO, MT6592_J36_KEY_A_MATRIX},
    {MT6592_KEY_B, MT6592_J36_KEY_B_GPIO, MT6592_J36_KEY_B_MATRIX},
    {MT6592_KEY_X, MT6592_J36_KEY_X_GPIO, MT6592_J36_KEY_X_MATRIX},
    {MT6592_KEY_Y, MT6592_J36_KEY_Y_GPIO, MT6592_J36_KEY_Y_MATRIX},
    {MT6592_KEY_L1, MT6592_J36_KEY_L1_GPIO, MT6592_J36_KEY_L1_MATRIX},
    {MT6592_KEY_R1, MT6592_J36_KEY_R1_GPIO, MT6592_J36_KEY_R1_MATRIX},
    {MT6592_KEY_L2, MT6592_J36_KEY_L2_GPIO, MT6592_J36_KEY_L2_MATRIX},
    {MT6592_KEY_R2, MT6592_J36_KEY_R2_GPIO, MT6592_J36_KEY_R2_MATRIX},
    {MT6592_KEY_START, MT6592_J36_KEY_START_GPIO, MT6592_J36_KEY_START_MATRIX},
    {MT6592_KEY_SELECT, MT6592_J36_KEY_SELECT_GPIO, MT6592_J36_KEY_SELECT_MATRIX},
    {MT6592_KEY_MENU, MT6592_J36_KEY_MENU_GPIO, MT6592_J36_KEY_MENU_MATRIX},
    {MT6592_KEY_VOL_UP, MT6592_J36_KEY_VOL_UP_GPIO, MT6592_J36_KEY_VOL_UP_MATRIX},
    {MT6592_KEY_VOL_DOWN, MT6592_J36_KEY_VOL_DOWN_GPIO, MT6592_J36_KEY_VOL_DOWN_MATRIX},
    {MT6592_KEY_POWER, MT6592_J36_KEY_POWER_GPIO, MT6592_J36_KEY_POWER_MATRIX},
};

#define KEY_MAP_COUNT (sizeof(g_key_map) / sizeof(g_key_map[0]))

/*
 * Wire pairs: a switch between two pads, both of which this driver pulls up.
 *
 * This is the read path the J36 actually needs, and the reason nothing else worked.
 * Once both sides idle high a pad's level says nothing about these buttons -- pressing
 * one ties two pulled-up nodes together and neither moves -- so the only way to see the
 * closure is to make one side low on purpose and look at the other. Provenance and the
 * measurements are in mt6592_board_j36.h; the orientation rule is that `drive` is never a
 * keypad pad. At rest, before key_gpio_arm_pullup() runs, these pads idle LOW on parked
 * pull-downs; that is measured, and it is why the arming step is mandatory rather than
 * defensive.
 *
 * Grouped by drive pad at scan time: the five surviving switches sit on three distinct
 * drive pads, because VOL+, VOL- and SELECT all share pad 11, so a poll costs three pulses
 * and not five. Entries with an unmapped side are skipped, which is how this table stayed
 * honest while it was being filled in one measurement at a time -- and it is now also how
 * six wrong entries were retracted without being deleted.
 *
 * THE TABLE'S INVARIANT, which cost four hardware runs to state: a pulse scan can only
 * read a switch whose BOTH pads idle high. The four D-pad entries and A were reading pads
 * that go low on their own, which is not a closure and not attributable to any pulse; they
 * are absolute levels now (mt6592_keys_read()'s GPIO half) and unmapped here. R2's entry
 * had both pads good and still never fired in four runs, so its pair does not exist. What
 * is left -- START, MENU, VOL+, VOL-, SELECT -- all work on hardware.
 *
 * Order matters in one respect only: the first entry for a given drive pad owns the
 * pulse and reads every later entry that shares it, so entries sharing a drive pad must
 * not be separated by an unmapped one. They are adjacent here.
 */
typedef struct {
    uint32_t mask;  /* MT6592_KEY_* bit */
    uint32_t drive; /* pad pulsed to output-low; never a keypad pad */
    uint32_t sense; /* pad read while the drive pad is low; 0 means pressed */
} key_wire_t;

static const key_wire_t g_key_wires[] = {
    {MT6592_KEY_DPAD_UP, MT6592_J36_KEY_DPAD_UP_DRIVE, MT6592_J36_KEY_DPAD_UP_SENSE},
    {MT6592_KEY_DPAD_DOWN, MT6592_J36_KEY_DPAD_DOWN_DRIVE, MT6592_J36_KEY_DPAD_DOWN_SENSE},
    {MT6592_KEY_DPAD_LEFT, MT6592_J36_KEY_DPAD_LEFT_DRIVE, MT6592_J36_KEY_DPAD_LEFT_SENSE},
    {MT6592_KEY_DPAD_RIGHT, MT6592_J36_KEY_DPAD_RIGHT_DRIVE, MT6592_J36_KEY_DPAD_RIGHT_SENSE},
    {MT6592_KEY_A, MT6592_J36_KEY_A_DRIVE, MT6592_J36_KEY_A_SENSE},
    {MT6592_KEY_R2, MT6592_J36_KEY_R2_DRIVE, MT6592_J36_KEY_R2_SENSE},
    {MT6592_KEY_START, MT6592_J36_KEY_START_DRIVE, MT6592_J36_KEY_START_SENSE},
    {MT6592_KEY_MENU, MT6592_J36_KEY_MENU_DRIVE, MT6592_J36_KEY_MENU_SENSE},
    {MT6592_KEY_VOL_UP, MT6592_J36_KEY_VOL_UP_DRIVE, MT6592_J36_KEY_VOL_UP_SENSE},
    {MT6592_KEY_VOL_DOWN, MT6592_J36_KEY_VOL_DOWN_DRIVE, MT6592_J36_KEY_VOL_DOWN_SENSE},
    {MT6592_KEY_SELECT, MT6592_J36_KEY_SELECT_DRIVE, MT6592_J36_KEY_SELECT_SENSE},
};

#define KEY_WIRE_COUNT (sizeof(g_key_wires) / sizeof(g_key_wires[0]))

/*
 * The one ghost this board HAD. START (pads 11-12), R2 (12-167) and VOL_UP (11-167) formed
 * a triangle, so holding any two of them shorted all three pads together and the third read
 * pressed. Two-of-three cannot be told from three-of-three by any digital read --
 * connectivity is an equivalence relation and cannot say which edges made it -- so it was a
 * decode, not a bug to fix. The reasoning, the brute-force check that no OTHER key can be
 * ghosted, and the cost of the choice are in mt6592_board_j36.h.
 *
 * R2 is no longer a wire pair, which removes an edge and with it the cycle, so this board
 * now has NO ghost at all. key_wires_resolve_ghost() checks for the edges rather than
 * trusting this comment -- see there.
 */
#define KEY_GHOST_TRIANGLE (MT6592_KEY_START | MT6592_KEY_R2 | MT6592_KEY_VOL_UP)

static uint32_t g_kpd_baseline[5];
static uint32_t g_kpd_baseline_valid;
static uint32_t g_gpio_baseline[MT6592_KEYS_GPIO_DIN_BANKS];
static uint32_t g_gpio_baseline_valid;
static uint32_t g_aux_center[AUXADC_SCAN_CHANNELS];
static uint32_t g_aux_center_valid;
/* The cursor is permanently bound to the LEFT stick, which is AUXADC_JOY_X=15
 * and AUXADC_JOY_Y=14 -- not ch13/ch12, which this comment claimed for a while
 * and which are the RIGHT stick's Z and RZ. There is no runtime channel
 * discovery: the kernel RE nailed the wiring, so the right stick is never read
 * and can never hijack the pointer, and there is no deflection threshold to meet
 * before the stick arms. */
static uint32_t g_joy_x_channel = AUXADC_JOY_X;
static uint32_t g_joy_y_channel = AUXADC_JOY_Y;
static uint32_t g_joy_channels_locked = 1u;

static volatile uint32_t* reg32(uint32_t addr) {
    return (volatile uint32_t*)(uintptr_t)addr;
}

static uint32_t read32(uint32_t addr) {
    return *reg32(addr);
}

static void write32(uint32_t addr, uint32_t value) {
    *reg32(addr) = value;
}

static volatile uint16_t* reg16(uint32_t addr) {
    return (volatile uint16_t*)(uintptr_t)addr;
}

static uint16_t read16(uint32_t addr) {
    return *reg16(addr);
}

static void write16(uint32_t addr, uint16_t value) {
    *reg16(addr) = value;
}

/* NOTE: the pinmux of a *button* pad is still never touched from here. The one
 * thing this file remuxes is the keypad block's own pads, and that is a different
 * claim: those six are named by the preloader's mtk_kpd_gpio_set, they are written
 * with the recipe it uses, and one of them is written only because the preloader
 * skips it. See kpd_pads_mux(). Buttons themselves are read from DIN or from the
 * scan memories, never after a mode write -- with one pull-only exception, the R2
 * pad, which the preloader leaves with no pull at all and which therefore has no
 * defined idle level until this driver gives it one. See key_gpio_arm_pullup(). */
static uint32_t gpio_read_val_bit(uint32_t base, uint32_t pin) {
    uint32_t reg = GPIO_BASE + base + (pin / GPIO_PINS_PER_VAL_REG) * GPIO_VAL_REG_STRIDE;
    return (read32(reg) >> (pin % GPIO_PINS_PER_VAL_REG)) & 1u;
}

/* Write-1-to-SET at +4, write-1-to-RST at +8: one bit, no read, no neighbours
 * disturbed. Every 16-pin GPIO register has this pair (MODE, which does not, is
 * not written from this file). */
static void gpio_write_val_bit(uint32_t base, uint32_t pin, uint32_t on) {
    uint32_t reg = GPIO_BASE + base + (pin / GPIO_PINS_PER_VAL_REG) * GPIO_VAL_REG_STRIDE +
                   (on != 0u ? GPIO_VAL_REG_SET : GPIO_VAL_REG_RST);
    write32(reg, 1u << (pin % GPIO_PINS_PER_VAL_REG));
}

/*
 * Give one button pad an input pull-up: direction in, PULLSEL up, PULLEN on.
 *
 * Applied to EVERY mapped button pad, which it did not used to be -- it was added
 * for R2 alone, on the evidence of `kpdhunt`, which reports through exactly this
 * configuration:
 *
 *     kpdhunt: gpio 12 -> 0 (PRESSED)
 *     kpdhunt: gpio 12 -> 1 (released)
 *
 * four clean cycles. kpdhunt applies mode 0 / in / pull-up and restores afterwards,
 * so those transitions were evidence about the pad *in this configuration* and not
 * about how the preloader leaves it -- and that is the general point, not an R2
 * quirk. Every button pad on this board has the same problem: MVII reads them
 * active-low, a switch to ground has nothing to idle against without a pull-up, and
 * an undefined level is a phantom press waiting to happen. The D-pad pads read 0 at
 * idle in the LK console for exactly this reason, and that reading is what got them
 * wrongly unmapped (see mt6592_board_j36.h). Arming the pull-up removes the whole
 * class of ambiguity: idle 1, pressed 0, on every pad, in every context.
 *
 * PULLSEL before PULLEN, so the resistor is never briefly enabled in the wrong
 * direction. MODE is deliberately not touched here: a pad that needs a mode change to
 * read a button gets it inside key_wires_scan(), for the duration of one pulse.
 *
 * A PARKED PULL-DOWN IS OVERRIDDEN, NOT PRESERVED, and that is worth its own paragraph
 * because the opposite rule was tried and it is what made A, START and MENU read
 * permanently closed on the second hardware run (mask 0x00d0). The reasoning that led to
 * preserving them -- "a pull-down is the far side of a switch that drags a sense line low
 * by itself" -- had the electrical model backwards. EVERY button pad on this connector
 * idles low on a parked pull-down; only the three keypad sense lines idle high. A pad that
 * idles LOW cannot report a closure, because it already reads 0 and has nowhere to move.
 * The pull-up is therefore not a hardening step that a clever pad can opt out of: it is
 * the thing that makes a closure observable at all, and it has to be applied to a muxed
 * pad too. The measured resting levels are in mt6592_board_j36.h.
 */
static void key_gpio_arm_pullup(uint32_t pin) {
    if (pin == MT6592_J36_KEY_GPIO_UNMAPPED) return;
    gpio_write_val_bit(GPIO_DIR_BASE, pin, 0u);      /* input */
    gpio_write_val_bit(GPIO_PULLSEL_BASE, pin, 1u);  /* up */
    gpio_write_val_bit(GPIO_PULLEN_BASE, pin, 1u);
}

/* Put a drive pad back to an input idling where it idled before its pulse. The pulse
 * writes PULLEN=0 to drive and leaves PULLSEL alone, so the caller's saved direction is
 * all that is needed. For a mapped wire pad that saved value is `up`, because arming ran
 * first; keeping the save/restore rather than forcing up means this stays correct if a
 * future caller ever pulses a pad that was not armed. */
static void key_gpio_restore_pull(uint32_t pin, uint32_t pullsel) {
    gpio_write_val_bit(GPIO_PULLSEL_BASE, pin, pullsel);
    gpio_write_val_bit(GPIO_PULLEN_BASE, pin, 1u);
}

/* Read-modify-write of one pad's 3-bit mux field. Five pads per 16-pin-spaced
 * register, so the arithmetic differs from the VAL registers above and there is no
 * atomic SET/RST pair -- MODE is the one GPIO register family that has to be
 * read back. */
static void gpio_set_mode(uint32_t pin, uint32_t mode) {
    uint32_t reg = GPIO_BASE + GPIO_MODE_BASE + (pin / GPIO_MODE_PER_REG) * GPIO_MODE_STRIDE;
    uint32_t shift = (pin % GPIO_MODE_PER_REG) * GPIO_MODE_BITS;
    uint32_t value = read32(reg);

    value &= ~(GPIO_MODE_MASK << shift);
    value |= (mode & GPIO_MODE_MASK) << shift;
    write32(reg, value);
}

/* Read-only counterpart, used by the wire scan to refuse to drive a pad that some
 * peripheral owns. Mode 0 is plain GPIO; anything else means the pad is muxed and
 * something other than this driver is deciding its level. */
static uint32_t gpio_get_mode(uint32_t pin) {
    uint32_t reg = GPIO_BASE + GPIO_MODE_BASE + (pin / GPIO_MODE_PER_REG) * GPIO_MODE_STRIDE;
    uint32_t shift = (pin % GPIO_MODE_PER_REG) * GPIO_MODE_BITS;
    return (read32(reg) >> shift) & GPIO_MODE_MASK;
}

/* Logs the mux/dir/level of all nine keypad pads and re-muxes any whose mode is wrong;
 * defined below, next to mt6592_keys_init(), because it logs. Was kpd_pads_mux(), which
 * wrote a fixed list of six, then kpd_pads_report(), which wrote none. Both were wrong and
 * the comment on the definition says why. */
static void kpd_pads_apply(void);

/* Mirror one-shot diagnostics to the eMMC console ring (visible in
 * `flash -mtk-read-boot-status`) as well as the physical UART. Weak so the
 * flash-payload/slot builds that don't link bootstatus still build. Only ever
 * called from one-shot (logged-once) paths -- never per frame. */
extern void mt6592_bootstatus_log_text(const char* text) __attribute__((weak));

static void joy_log(const char* text) {
    mt6592_uart_puts(text);
    if (&mt6592_bootstatus_log_text) mt6592_bootstatus_log_text(text);
}

static void joy_log_u32(const char* label, uint32_t value) {
    joy_log(label);
    {
        char buf[11];
        buf[0] = '0';
        buf[1] = 'x';
        buf[10] = 0;
        for (int b = 0; b < 8; ++b) {
            uint32_t nib = (value >> (b * 4)) & 0xfu;
            buf[9 - b] = (char)(nib < 10u ? '0' + nib : 'a' + (nib - 10u));
        }
        joy_log(buf);
    }
}

/*
 * Enable the SoC AUXADC peripheral clock. Idempotent. Logs PERI_PDN0 before and
 * after (to the console ring) so the exact AUXADC gate bit can be confirmed
 * from one status readback if the current mask is off.
 */
static void auxadc_clock_enable(void) {
    static int done;
    if (done) return;
    done = 1;

    joy_log_u32("\n  input: PERI_PDN0_STA before=", read32(PERICFG_BASE + PERI_PDN0_STA));
    write32(PERICFG_BASE + PERI_PDN0_CLR, PERI_PDN0_AUXADC_BITS);
    joy_log_u32("\n  input: PERI_PDN0_STA after =", read32(PERICFG_BASE + PERI_PDN0_STA));
    joy_log("\n");
}

static uint32_t kpd_mem_offset(uint32_t index) {
    return KPD_MEM1 + index * 4u;
}

static uint32_t kpd_mem_mask(uint32_t index) {
    return index == (KPD_NUM_MEMS - 1u) ? KPD_MEM5_VALID_MASK : 0xffffu;
}

static uint32_t kpd_read_mem_index(uint32_t index) {
    if (index >= KPD_NUM_MEMS) return 0u;
    return (uint32_t)read16(KPD_BASE + kpd_mem_offset(index)) & kpd_mem_mask(index);
}

static void kpd_read_mem(uint32_t mem[KPD_NUM_MEMS]) {
    for (uint32_t i = 0; i < KPD_NUM_MEMS; ++i) mem[i] = kpd_read_mem_index(i);
}

static void kpd_log_mem(const char* prefix, const uint32_t mem[KPD_NUM_MEMS]) {
    joy_log(prefix);
    joy_log_u32(" mem1=", mem[0]);
    joy_log_u32(" mem2=", mem[1]);
    joy_log_u32(" mem3=", mem[2]);
    joy_log_u32(" mem4=", mem[3]);
    joy_log_u32(" mem5=", mem[4]);
    joy_log("\n");
}

static void kpd_capture_baseline(void) {
    kpd_read_mem(g_kpd_baseline);
    g_kpd_baseline_valid = 1u;
    kpd_log_mem("\n  input: KPD baseline", g_kpd_baseline);
}

static void kpd_log_deltas(void) {
    static uint32_t have_last_delta;
    static uint32_t last_delta[KPD_NUM_MEMS];
    static uint32_t logged;
    uint32_t mem[KPD_NUM_MEMS];

    if (!g_kpd_baseline_valid || logged >= KPD_DELTA_LOG_LIMIT) return;
    kpd_read_mem(mem);
    for (uint32_t i = 0; i < KPD_NUM_MEMS; ++i) {
        uint32_t delta = (mem[i] ^ g_kpd_baseline[i]) & kpd_mem_mask(i);
        uint32_t newly_changed = have_last_delta ? (delta & ~last_delta[i]) : delta;
        last_delta[i] = delta;
        while (newly_changed && logged < KPD_DELTA_LOG_LIMIT) {
            uint32_t bit = 0u;
            while (((newly_changed >> bit) & 1u) == 0u) ++bit;
            joy_log_u32("\n  input: KPD delta bit=", i * KPD_MEM_BITS + bit);
            joy_log_u32(" mem=", i + 1u);
            joy_log_u32(" now=", mem[i]);
            joy_log_u32(" base=", g_kpd_baseline[i]);
            joy_log("\n");
            newly_changed &= ~(1u << bit);
            ++logged;
        }
    }
    have_last_delta = 1u;
}

/*
 * MT6592 keypad scanner. The legacy 0x10010000 address is the MIPITX DSI PHY;
 * the KPD register file is the next 0x1000 page. The vendor KPD driver uses
 * five 16-bit scan-memory words where KPD_MEM1 bit 0 is matrix bit 0 and
 * KPD_MEM5 only exposes bits 0..7. MVII reads absolute vendor polarity -- idle
 * all-ones, a held key pulls its bit low -- see matrix_bit_is_pressed(). It used
 * to compare against an init-time idle baseline instead, and that is what the
 * g_kpd_baseline capture below is left over from: it now feeds the console's
 * delta log only, because a baseline taken at boot inverts any key that happened
 * to be held while it was captured.
 */
/*
 * Ungate the keypad's 32 kHz clock in the PMIC.
 *
 * This is the one step in the vendor's enable sequence that MVII was missing, and
 * it is the step whose absence is invisible from the SoC side: with the clock gated
 * the scan engine sits there with KP_EN set, KP_DEBOUNCE loaded and every scan
 * memory reading its idle all-ones pattern, which is indistinguishable from a
 * correctly configured matrix that nobody is pressing. A live capture from the LK
 * console showed precisely that -- en=1, deb=0x400, mem 0xffff 0xffff 0xffff 0xffff
 * 0x00ff before and after a verified-correct pad mux.
 *
 * Read-modify-write rather than an assignment: 0x0040 gates more than the keypad on
 * MT6323 and the vendor only ever clears this one bit. A PWRAP failure is logged and
 * otherwise ignored -- if the bus is broken the buttons are the least of it, and
 * bailing out here would skip the KP_EN write that at least leaves the block in the
 * state the preloader left it.
 */
static void kpd_clock_ungate(void) {
    uint32_t val = 0u;

    if (mt6592_pwrap_read(KPD_PMIC_CLK_GATE_REG, &val) != 0) {
        joy_log("  input: KPD clock ungate: PWRAP read failed\n");
        return;
    }
    joy_log_u32("  input: KPD pmic 0x40 was=", val);
    if ((val & KPD_PMIC_CLK_GATE_BIT) == 0u) {
        joy_log(" (already ungated)\n");
        return;
    }
    if (mt6592_pwrap_write(KPD_PMIC_CLK_GATE_REG, val & ~KPD_PMIC_CLK_GATE_BIT) != 0) {
        joy_log(" -- PWRAP write failed\n");
        return;
    }
    val = 0u;
    (void)mt6592_pwrap_read(KPD_PMIC_CLK_GATE_REG, &val);
    joy_log_u32(" now=", val);
    joy_log("\n");
}

static void kpd_scanner_enable(void) {
    static int done;
    uint32_t before[KPD_NUM_MEMS];
    uint32_t after[KPD_NUM_MEMS];

    if (done) return;
    done = 1;

    kpd_read_mem(before);
    joy_log_u32("\n  input: KPD base=", KPD_BASE);
    joy_log_u32(" sta=", read16(KPD_BASE + KPD_STA));
    joy_log_u32(" en_before=", read16(KPD_BASE + KPD_EN));
    joy_log_u32(" sel=", read16(KPD_BASE + KPD_SEL));
    joy_log_u32(" deb=", read16(KPD_BASE + KPD_DEBOUNCE));
    joy_log("\n");
    kpd_log_mem("  input: KPD mem before", before);

    kpd_clock_ungate();

    write16(KPD_BASE + KPD_DEBOUNCE, (uint16_t)(KPD_DEBOUNCE_DEFAULT & KPD_DEBOUNCE_MASK));
    /* Double-key off, exactly as the preloader does it at 0x54a8: read KP_SEL,
     * clear bit 0, write it back. Bit 0 is the only bit either of them touches, so
     * this is a clear rather than an assignment -- KP_SEL's upper bits are column
     * enables on parts that populate them and are not ours to zero. */
    write16(KPD_BASE + KPD_SEL, (uint16_t)(read16(KPD_BASE + KPD_SEL) & ~KPD_SEL_DOUBLE_KEY));
    write16(KPD_BASE + KPD_EN, 1u);

    kpd_read_mem(after);
    joy_log_u32("  input: KPD sta_after=", read16(KPD_BASE + KPD_STA));
    joy_log_u32(" en_after=", read16(KPD_BASE + KPD_EN));
    joy_log_u32(" sel_after=", read16(KPD_BASE + KPD_SEL));
    joy_log_u32(" deb_after=", read16(KPD_BASE + KPD_DEBOUNCE));
    joy_log("\n");
    kpd_log_mem("  input: KPD mem after", after);
}

/*
 * One fresh conversion, vendor style (IMM_GetOneChannelValue in
 * mt_auxadc_hal.c): stop the channel, wait for its ready bit to CLEAR, let
 * the input mux settle, restart it, wait for a NEW ready, then read. The old
 * "enable and poll ready" version could return a conversion that started
 * before the mux settled (ready may still be set from a previous cycle),
 * which made the value depend on how often the caller sampled.
 */
static int soc_auxadc_read_channel(uint32_t channel, uint32_t* out_raw) {
    uint32_t dat_addr;
    uint32_t data = 0u;

    if (!out_raw || channel > 15u) return -1;
    dat_addr = AUXADC_BASE + AUXADC_DAT0 + channel * AUXADC_DAT_STRIDE;

    /* Best-effort global idle wait (bounded; proceed regardless). */
    for (uint32_t i = 0; i < AUXADC_POLL_LIMIT; ++i) {
        if ((read32(AUXADC_BASE + AUXADC_CON2) & AUXADC_CON2_BUSY) == 0u) break;
    }

    write32(AUXADC_BASE + AUXADC_CON1_CLR, 1u << channel);
    for (uint32_t i = 0; i < AUXADC_POLL_LIMIT; ++i) {
        if ((read32(dat_addr) & AUXADC_DAT_READY) == 0u) break;
    }
    mt6592_delay_cycles(AUXADC_SETTLE_CYCLES);

    write32(AUXADC_BASE + AUXADC_CON1_SET, 1u << channel);
    mt6592_delay_cycles(AUXADC_SETTLE_CYCLES);
    for (uint32_t i = 0; i < AUXADC_POLL_LIMIT; ++i) {
        data = read32(dat_addr);
        if ((data & AUXADC_DAT_READY) != 0u) {
            write32(AUXADC_BASE + AUXADC_CON1_CLR, 1u << channel);
            *out_raw = data & AUXADC_DAT_MASK;
            return 0;
        }
    }
    write32(AUXADC_BASE + AUXADC_CON1_CLR, 1u << channel);
    return -1;
}

static int32_t abs32(int32_t value) {
    return value < 0 ? -value : value;
}

static int32_t clamp_axis(int32_t value) {
    if (value < -(int32_t)AUXADC_JOY_AXIS_FULL) return -(int32_t)AUXADC_JOY_AXIS_FULL;
    if (value > (int32_t)AUXADC_JOY_AXIS_FULL) return (int32_t)AUXADC_JOY_AXIS_FULL;
    return value;
}

static int32_t approx_vector_magnitude(int32_t x, int32_t y) {
    int32_t ax = abs32(x);
    int32_t ay = abs32(y);
    return ax > ay ? ax + ay / 2 : ay + ax / 2;
}

static void axes_from_raw_pair(uint32_t raw_x, uint32_t raw_y, int32_t center_x, int32_t center_y,
                               int32_t* out_x, int32_t* out_y) {
    int32_t dx = ((int32_t)raw_x - center_x) * (int32_t)AUXADC_JOY_X_SIGN;
    int32_t dy = ((int32_t)raw_y - center_y) * (int32_t)AUXADC_JOY_Y_SIGN;
    int32_t mag = approx_vector_magnitude(dx, dy);
    int32_t scaled;

    if (!out_x || !out_y) return;
    if (mag <= (int32_t)AUXADC_JOY_DEADZONE) {
        *out_x = 0;
        *out_y = 0;
        return;
    }

    /* Radial deadzone: subtract the rest band along the stick vector instead of
     * zeroing axes independently. This keeps diagonals proportional while still
     * making the center much less twitchy. */
    scaled = (mag - (int32_t)AUXADC_JOY_DEADZONE) * (int32_t)AUXADC_JOY_GAIN;
    *out_x = clamp_axis((dx * scaled) / mag);
    *out_y = clamp_axis((dy * scaled) / mag);
}

static uint32_t abs_delta_u32(uint32_t a, uint32_t b) {
    return a > b ? a - b : b - a;
}

static int soc_auxadc_read_scan(uint32_t raw[AUXADC_SCAN_CHANNELS],
                                uint32_t valid[AUXADC_SCAN_CHANNELS]) {
    /* ONLY the two left-stick channels are ever sampled. The right stick
     * (ch14/15) is intentionally never read so it can never be adopted as the
     * cursor, and skipping it halves the per-frame AUXADC cost. */
    static const uint32_t kStickChannels[] = {AUXADC_JOY_X, AUXADC_JOY_Y};
    uint32_t any = 0u;

    for (uint32_t ch = 0; ch < AUXADC_SCAN_CHANNELS; ++ch) {
        valid[ch] = 0u;
        raw[ch] = 0u;
    }
    for (uint32_t i = 0; i < sizeof(kStickChannels) / sizeof(kStickChannels[0]); ++i) {
        uint32_t ch = kStickChannels[i];
        if (soc_auxadc_read_channel(ch, &raw[ch]) == 0) {
            valid[ch] = 1u;
            any = 1u;
        }
    }
    return any ? 0 : -1;
}

static int soc_auxadc_read_active_pair(uint32_t raw[AUXADC_SCAN_CHANNELS],
                                       uint32_t valid[AUXADC_SCAN_CHANNELS]) {
    uint32_t any = 0u;

    for (uint32_t ch = 0; ch < AUXADC_SCAN_CHANNELS; ++ch) {
        valid[ch] = 0u;
        raw[ch] = g_aux_center_valid ? g_aux_center[ch] : AUXADC_JOY_FALLBACK_CENTER;
    }

    if (!g_aux_center_valid || !g_joy_channels_locked) return soc_auxadc_read_scan(raw, valid);

    if (soc_auxadc_read_channel(g_joy_x_channel, &raw[g_joy_x_channel]) == 0) {
        valid[g_joy_x_channel] = 1u;
        any = 1u;
    }
    if (g_joy_y_channel != g_joy_x_channel &&
        soc_auxadc_read_channel(g_joy_y_channel, &raw[g_joy_y_channel]) == 0) {
        valid[g_joy_y_channel] = 1u;
        any = 1u;
    }
    return any ? 0 : -1;
}

static void auxadc_channel_label(char label[8], char kind, uint32_t ch) {
    uint32_t i = 0;
    label[i++] = ' ';
    label[i++] = kind;
    if (ch >= 10u) label[i++] = (char)('0' + ch / 10u);
    label[i++] = (char)('0' + ch % 10u);
    label[i++] = '=';
    label[i] = 0;
}

static void auxadc_log_scan(const char* prefix, const uint32_t raw[AUXADC_SCAN_CHANNELS],
                            const uint32_t valid[AUXADC_SCAN_CHANNELS]) {
    joy_log(prefix);
    for (uint32_t ch = 0; ch < AUXADC_SCAN_CHANNELS; ++ch) {
        char label[8];
        if (!valid[ch]) continue;
        auxadc_channel_label(label, 'c', ch);
        joy_log_u32(label, raw[ch]);
    }
    joy_log("\n");
}

static void auxadc_log_data_regs(const char* prefix) {
    joy_log(prefix);
    for (uint32_t ch = 0; ch < AUXADC_SCAN_CHANNELS; ++ch) {
        char label[8];
        auxadc_channel_label(label, 'd', ch);
        joy_log_u32(label, read32(AUXADC_BASE + AUXADC_DAT0 + ch * AUXADC_DAT_STRIDE));
    }
    joy_log("\n");
}

/*
 * Adopt the rest positions stage1 captured before the splash.
 *
 * The lazy seeding below can only run once the frame pump is calling
 * mt6592_keys_read_axes(), which is seconds into boot with the device already
 * in someone's hands -- a thumb resting on a stick at that moment becomes the
 * locked center and biases every reading afterwards. stage1 samples the same
 * four channels while the box is still painting its first splash and passes
 * them across in the boot handoff; prefer those whenever they are present.
 *
 * Only the four stick channels are carried. Everything else gets the fallback,
 * which is all the non-stick entries were ever used for (delta logging).
 */
static void auxadc_seed_centers_from_handoff(void) {
    uint32_t centers[MT6592_BOOT_JOY_CHANNEL_COUNT];

    if (g_aux_center_valid) return;
    if (!mt6592_bootinfo_joystick_centers(mt6592_bootinfo_active(), centers)) return;

    for (uint32_t ch = 0; ch < AUXADC_SCAN_CHANNELS; ++ch) {
        g_aux_center[ch] = AUXADC_JOY_FALLBACK_CENTER;
    }
    for (uint32_t i = 0; i < MT6592_BOOT_JOY_CHANNEL_COUNT; ++i) {
        uint32_t ch = MT6592_BOOT_JOY_FIRST_CHANNEL + i;
        if (ch < AUXADC_SCAN_CHANNELS) g_aux_center[ch] = centers[i];
    }
    g_aux_center_valid = 1u;

    joy_log("\n  input: J36 stick centers from stage1 pre-splash capture");
    joy_log_u32(" xch=", AUXADC_JOY_X);
    joy_log_u32(" x=", g_aux_center[AUXADC_JOY_X]);
    joy_log_u32(" ych=", AUXADC_JOY_Y);
    joy_log_u32(" y=", g_aux_center[AUXADC_JOY_Y]);
    joy_log("\n");
}

static void auxadc_seed_centers(const uint32_t raw[AUXADC_SCAN_CHANNELS],
                                const uint32_t valid[AUXADC_SCAN_CHANNELS]) {
    static uint32_t settle;
    if (g_aux_center_valid) return;
    for (uint32_t ch = 0; ch < AUXADC_SCAN_CHANNELS; ++ch) {
        g_aux_center[ch] = valid[ch] ? raw[ch] : AUXADC_JOY_FALLBACK_CENTER;
    }
    /* Re-seed across the first few scans so the locked center reflects a settled
     * AUXADC rather than the cold first conversion (which reads low and was the
     * cause of the persistent rest drift). Centers stay populated the whole time
     * so the channel-delta logic sees small deltas, not garbage. */
    if (++settle < JOY_SEED_SETTLE_SCANS) return;
    g_aux_center_valid = 1u;
    auxadc_log_scan("\n  input: J36 AUXADC scan", raw, valid);
}

static uint32_t auxadc_channel_delta(uint32_t ch, const uint32_t raw[AUXADC_SCAN_CHANNELS],
                                     const uint32_t valid[AUXADC_SCAN_CHANNELS]) {
    if (ch >= AUXADC_SCAN_CHANNELS || !valid[ch]) return 0u;
    return abs_delta_u32(raw[ch], g_aux_center[ch]);
}

static void joy_choose_channels(const uint32_t raw[AUXADC_SCAN_CHANNELS],
                                const uint32_t valid[AUXADC_SCAN_CHANNELS],
                                uint32_t* x_channel,
                                uint32_t* y_channel) {
    /* No discovery: the cursor is always the LEFT stick (ch13=X, ch12=Y).
     * This removes the old "deflect ≥threshold to arm" delay and makes it
     * impossible for the right stick to grab the pointer. */
    (void)raw;
    (void)valid;
    *x_channel = AUXADC_JOY_X;
    *y_channel = AUXADC_JOY_Y;
}

static void joy_log_aux_motion(const uint32_t raw[AUXADC_SCAN_CHANNELS],
                               const uint32_t valid[AUXADC_SCAN_CHANNELS],
                               uint32_t x_channel,
                               uint32_t y_channel) {
    static uint32_t logged;
    uint32_t moving = 0u;

    if (logged >= 16u) return;
    for (uint32_t ch = 0; ch < AUXADC_SCAN_CHANNELS; ++ch) {
        if (auxadc_channel_delta(ch, raw, valid) >= (uint32_t)AUXADC_JOY_LOG_THRESHOLD) moving = 1u;
    }
    if (!moving) return;
    ++logged;
    auxadc_log_scan("\n  input: J36 AUXADC delta", raw, valid);
    joy_log_u32("  input: J36 AUXADC selected xch=", x_channel);
    joy_log_u32(" ych=", y_channel);
    joy_log_u32(" xcenter=", g_aux_center[x_channel]);
    joy_log_u32(" ycenter=", g_aux_center[y_channel]);
    joy_log("\n");
}

/* Per-axis center calibration. The stock Android driver exposes synthetic
 * Linux axes, but the bare-metal AUXADC channel order on this board is still
 * being validated. Seed every channel from the first live scan, then bind the
 * cursor to the channel pair that actually deflects. */
static int g_joy_cal_done;
static int32_t g_joy_center_x = AUXADC_JOY_FALLBACK_CENTER;
static int32_t g_joy_center_y = AUXADC_JOY_FALLBACK_CENTER;

/* Rolling flat-window tracker: accumulates consecutive samples while both
 * axes stay inside the JOY_CAL_MAX_JITTER band and restarts from the current
 * sample the moment either axis breaks out. Window duration is measured on
 * the free-running 13 MHz GPT4 (wrap-safe unsigned tick deltas). */
typedef struct {
    uint32_t n;
    uint32_t start_ticks;
    uint32_t last_ticks;
    uint32_t sum_x;
    uint32_t sum_y;
    uint32_t min_x;
    uint32_t max_x;
    uint32_t min_y;
    uint32_t max_y;
} joy_flat_window_t;

static void joy_window_start(joy_flat_window_t* w, uint32_t raw_x, uint32_t raw_y) {
    w->n = 1u;
    w->start_ticks = mt6592_delay_gpt_ticks();
    w->last_ticks = w->start_ticks;
    w->sum_x = raw_x;
    w->sum_y = raw_y;
    w->min_x = raw_x;
    w->max_x = raw_x;
    w->min_y = raw_y;
    w->max_y = raw_y;
}

/* Returns 1 if the sample extended the flat window, 0 if it broke the band
 * (the window restarts from this sample either way). */
static int joy_window_add(joy_flat_window_t* w, uint32_t raw_x, uint32_t raw_y) {
    uint32_t min_x;
    uint32_t max_x;
    uint32_t min_y;
    uint32_t max_y;

    if (w->n == 0u || w->n >= (1u << 20)) { /* n cap guards the u32 sums */
        joy_window_start(w, raw_x, raw_y);
        return 1;
    }
    min_x = raw_x < w->min_x ? raw_x : w->min_x;
    max_x = raw_x > w->max_x ? raw_x : w->max_x;
    min_y = raw_y < w->min_y ? raw_y : w->min_y;
    max_y = raw_y > w->max_y ? raw_y : w->max_y;
    if ((max_x - min_x) > (uint32_t)JOY_CAL_MAX_JITTER ||
        (max_y - min_y) > (uint32_t)JOY_CAL_MAX_JITTER) {
        joy_window_start(w, raw_x, raw_y);
        return 0;
    }
    w->min_x = min_x;
    w->max_x = max_x;
    w->min_y = min_y;
    w->max_y = max_y;
    w->sum_x += raw_x;
    w->sum_y += raw_y;
    w->last_ticks = mt6592_delay_gpt_ticks();
    ++w->n;
    return 1;
}

static uint32_t joy_window_flat_us(const joy_flat_window_t* w) {
    return mt6592_delay_ticks_to_us(w->last_ticks - w->start_ticks);
}

static int joy_window_gpt_dead(const joy_flat_window_t* w) {
    return w->n >= JOY_CAL_GPT_DEAD_SAMPLES && w->last_ticks == w->start_ticks;
}

static void joy_adopt_center(uint32_t avg_x, uint32_t avg_y, uint32_t x_channel, uint32_t y_channel) {
    g_joy_center_x = (int32_t)avg_x;
    g_joy_center_y = (int32_t)avg_y;
    g_aux_center[x_channel] = avg_x;
    g_aux_center[y_channel] = avg_y;
}

static int joy_calibrate(uint32_t raw_x, uint32_t raw_y, uint32_t x_channel, uint32_t y_channel) {
    static joy_flat_window_t w;
    static uint32_t cal_x_channel = 0xffffffffu;
    static uint32_t cal_y_channel = 0xffffffffu;
    static uint32_t attempts;
    static uint32_t cal_start_ticks;
    static uint32_t cal_started;
    uint32_t now = mt6592_delay_gpt_ticks();
    uint32_t gpt_dead;
    uint32_t flat_ok;
    uint32_t forced;

    if (g_joy_cal_done) return 1;
    if (cal_x_channel != x_channel || cal_y_channel != y_channel) {
        cal_x_channel = x_channel;
        cal_y_channel = y_channel;
        attempts = 0u;
        cal_started = 0u;
        w.n = 0u;
    }
    if (!cal_started) {
        cal_started = 1u;
        cal_start_ticks = now;
    }
    ++attempts;
    (void)joy_window_add(&w, raw_x, raw_y);

    /* Lock only after the samples have been flat for real wall time (the
     * warm-up overshoot plateau is transient; the true rest is not). The
     * forced path caps total calibration time so the stick can never stay
     * permanently dead, adopting the current window average best-effort —
     * joy_plateau_rebase() self-heals it afterwards. */
    gpt_dead = (uint32_t)joy_window_gpt_dead(&w) ||
               (attempts >= 64u && (now - cal_start_ticks) == 0u);
    flat_ok = w.n >= JOY_CAL_MIN_SAMPLES &&
              (gpt_dead ? (w.n >= JOY_CAL_GPT_DEAD_MIN_SAMPLES)
                        : (joy_window_flat_us(&w) >= (uint32_t)JOY_CAL_MIN_FLAT_US));
    forced = gpt_dead ? (attempts >= JOY_CAL_MAX_ATTEMPTS)
                      : (mt6592_delay_ticks_to_us(now - cal_start_ticks) >=
                         (uint32_t)JOY_CAL_FORCE_US);
    if (!flat_ok && !forced) return 0;
    if (w.n == 0u) return 0;

    joy_adopt_center(w.sum_x / w.n, w.sum_y / w.n, x_channel, y_channel);
    g_joy_cal_done = 1;
    joy_log(forced && !flat_ok ? "\n  input: J36 stick calibrated (forced) center x="
                               : "\n  input: J36 stick calibrated center x=");
    joy_log_u32("", (uint32_t)g_joy_center_x);
    joy_log_u32(" y=", (uint32_t)g_joy_center_y);
    joy_log_u32(" xch=", x_channel);
    joy_log_u32(" ych=", y_channel);
    joy_log("\n");
    return 1;
}

/* Post-lock self-healing for warm-up mislock / thermal drift.
 *
 * IMPORTANT: a *held* stick is often flatter than rest (tiny jitter). Rebase
 * only when the plateau sits near the *existing* center (inside the deadzone).
 * Otherwise a hold would become the new center → cursor stops, then jumps
 * again when the pot wiggles — the "stops then moves the same way" bug. */
static void joy_plateau_rebase(uint32_t raw_x, uint32_t raw_y, uint32_t x_channel, uint32_t y_channel) {
    static joy_flat_window_t w;
    static uint32_t logged;
    uint32_t avg_x;
    uint32_t avg_y;
    int32_t bias_x;
    int32_t bias_y;

    (void)joy_window_add(&w, raw_x, raw_y);
    if (joy_window_gpt_dead(&w)) {
        if (w.n < JOY_CAL_GPT_DEAD_MIN_SAMPLES * 2u) return;
    } else if (joy_window_flat_us(&w) < (uint32_t)JOY_REBASE_FLAT_US) {
        return;
    }
    avg_x = w.sum_x / w.n;
    avg_y = w.sum_y / w.n;
    w.n = 0u; /* re-arm for the next plateau either way */

    bias_x = abs32((int32_t)avg_x - g_joy_center_x);
    bias_y = abs32((int32_t)avg_y - g_joy_center_y);

    /* Held deflection: plateau is flat but far from center — do not steal it. */
    if (bias_x > (int32_t)AUXADC_JOY_DEADZONE ||
        bias_y > (int32_t)AUXADC_JOY_DEADZONE)
        return;

    /* Already near center — nothing useful to do. */
    if (bias_x <= (int32_t)JOY_REBASE_MIN_BIAS &&
        bias_y <= (int32_t)JOY_REBASE_MIN_BIAS)
        return;

    joy_adopt_center(avg_x, avg_y, x_channel, y_channel);
    if (logged < 8u) {
        ++logged;
        joy_log_u32("\n  input: J36 stick re-centered x=", avg_x);
        joy_log_u32(" y=", avg_y);
        joy_log("\n");
    }
}

/* Tiny rest-noise trim only. Never runs while the stick is past the deadzone
 * so a hold cannot slowly walk the center toward the hold position. */
static void joy_recenter_small_bias(uint32_t raw_x, uint32_t raw_y, uint32_t x_channel, uint32_t y_channel) {
    int32_t dx = (int32_t)raw_x - g_joy_center_x;
    int32_t dy = (int32_t)raw_y - g_joy_center_y;

    if (abs32(dx) >= (int32_t)JOY_RECENTER_THRESHOLD ||
        abs32(dy) >= (int32_t)JOY_RECENTER_THRESHOLD)
        return;
    /* Require both axes quiet (rest), not a one-axis hold near the band edge. */
    if (abs32(dx) > (int32_t)(JOY_RECENTER_THRESHOLD / 2) &&
        abs32(dy) > (int32_t)(JOY_RECENTER_THRESHOLD / 2))
        return;

    g_joy_center_x += dx / (int32_t)JOY_RECENTER_DIVISOR;
    g_aux_center[x_channel] = (uint32_t)g_joy_center_x;
    g_joy_center_y += dy / (int32_t)JOY_RECENTER_DIVISOR;
    g_aux_center[y_channel] = (uint32_t)g_joy_center_y;
}

static void joy_log_raw_motion(uint32_t raw_x, uint32_t raw_y, int32_t dx, int32_t dy) {
    static uint32_t logged;

    if (logged >= 16u) return;
    if (abs32(dx) < 32 && abs32(dy) < 32) return;
    ++logged;
    joy_log_u32("\n  input: J36 stick raw x=", raw_x);
    joy_log_u32(" y=", raw_y);
    joy_log_u32(" cx=", (uint32_t)g_joy_center_x);
    joy_log_u32(" cy=", (uint32_t)g_joy_center_y);
    joy_log_u32(" dx=", (uint32_t)dx);
    joy_log_u32(" dy=", (uint32_t)dy);
    joy_log("\n");
}

/*
 * Stick-activity supply probe.
 *
 * The open question this exists to answer: with no cell fitted and the console
 * running off the charger alone, moving the stick drops the board. It does NOT
 * drop while the shell is hung, which is the one fact that separates the two
 * cases -- whatever kills it needs the frame pump running. That also rules out
 * the purely mechanical reading, because a pot is a fixed-resistance divider
 * and draws the same current wherever the wiper sits.
 *
 * Everything cheap has already been eliminated against the source:
 *
 *   - No software path from a deflection to power-off. performPowerAction() in
 *     Dashboard.cpp is reachable only from handlePowerModalTouch(), which only
 *     the power modal's touch handler calls.
 *   - The stick clicks are EINT GPIO 7 and 46 (mt6592_board_j36.h) and appear
 *     in g_key_map nowhere, so they reach neither the shell nor the KPD matrix
 *     the PMIC's key-reset input watches -- and that input cannot see an analog
 *     AUXADC channel at all.
 *   - No DVFS ramp in the frame loop: mt6592_cpufreq_apply_max() runs once.
 *   - A moved cursor is answered by Dashboard::renderCursorOnly(), not a full
 *     composite (Dashboard.cpp's render() gate, and the comment there recording
 *     that the full-composite version of this was already fixed).
 *
 * What is left is a supply question, and nobody has measured the rail while the
 * stick is moving. So measure it, and do not act on it: a fix built on a
 * hypothesis the evidence has not confirmed is a guess.
 *
 * Surviving the power cut is the whole difficulty. The console ring only
 * reaches eMMC at a commit, and the runtime keeps autoflush off because an MSDC
 * transaction mid-frame stalls the shell. So this commits at REST, never at the
 * deflection:
 *
 *   1. An armed baseline is published once from rest and committed. If the
 *      board dies at the first deflection, the next boot's readback still
 *      carries the rest voltage plus the absence of any episode line, which
 *      says the death lands inside a frame of the deflection.
 *   2. Each episode's droop is accumulated while deflected and committed only
 *      after the stick has been back at rest for JOY_SUPPLY_REST_SAMPLES frames
 *      -- after the board demonstrably survived. The commit therefore can never
 *      be confused with the load step that killed it.
 *
 * Bounded to JOY_SUPPLY_EPISODES reports and permanently inert afterwards, so
 * the shell does not carry a per-frame cost for it forever.
 *
 * Both dependencies are weak: mt6592_keys.c is also linked into images that
 * carry neither a PMIC nor the boot-status ring.
 */
extern int mt6592_pmic_vbat_sample_mv(void) __attribute__((weak));
extern int mt6592_bootstatus_console_flush(void) __attribute__((weak));

enum {
    JOY_SUPPLY_EPISODES = 8u,
    /* Above joy_log_raw_motion's 32 so an episode means a deliberate
     * deflection, not the ±7..13 jitter band or warm-up drift. */
    JOY_SUPPLY_MOVE_THRESHOLD = 48,
    /* Frames of continuous rest that close an episode: at pump rate a fifth of
     * a second, far enough clear of the deflection for the commit to be
     * unambiguous and still short enough to catch the next one. */
    JOY_SUPPLY_REST_SAMPLES = 12u,
    /* At rest the rail is not the interesting part, so pay for a conversion
     * every fourth frame. Deflections sample every frame -- that is where the
     * resolution is needed. */
    JOY_SUPPLY_REST_DIVISOR = 4u,
};

static uint32_t g_supply_episodes;
static uint32_t g_supply_armed;
static uint32_t g_supply_deflected;
static uint32_t g_supply_rest_run;
static uint32_t g_supply_rest_ticks;
static uint32_t g_supply_defl_samples;
static uint32_t g_supply_defl_frames;
static int32_t g_supply_rest_mv;
static int32_t g_supply_rest_min_mv;
static int32_t g_supply_defl_min_mv;
static int32_t g_supply_defl_last_mv;

/* Millivolts are for a human reading a boot-status capture; joy_log_u32's hex
 * is not. */
static void joy_log_dec(const char* label, int32_t value) {
    char buf[12];
    uint32_t i = (uint32_t)sizeof(buf) - 1u;
    uint32_t magnitude = value < 0 ? 0u - (uint32_t)value : (uint32_t)value;

    joy_log(label);
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0u && i != 0u);
    if (value < 0 && i != 0u) buf[--i] = '-';
    joy_log(&buf[i]);
}

static void joy_supply_commit(void) {
    if (&mt6592_bootstatus_console_flush) (void)mt6592_bootstatus_console_flush();
}

static void joy_supply_probe(int32_t dx, int32_t dy) {
    uint32_t moving;
    int mv = -1;

    if (g_supply_episodes >= (uint32_t)JOY_SUPPLY_EPISODES) return;
    if (!&mt6592_pmic_vbat_sample_mv) return;

    moving = (abs32(dx) >= (int32_t)JOY_SUPPLY_MOVE_THRESHOLD ||
              abs32(dy) >= (int32_t)JOY_SUPPLY_MOVE_THRESHOLD)
                 ? 1u
                 : 0u;

    /* -1 means "ask again", never "the rail is low": the sampler is a
     * kick-then-collect pair and the first call of every pair returns it. */
    if (moving || (g_supply_rest_ticks++ % (uint32_t)JOY_SUPPLY_REST_DIVISOR) == 0u) {
        mv = mt6592_pmic_vbat_sample_mv();
    }

    if (moving) {
        g_supply_rest_run = 0u;
        if (!g_supply_deflected) {
            g_supply_deflected = 1u;
            g_supply_defl_min_mv = 0;
            g_supply_defl_last_mv = 0;
            g_supply_defl_samples = 0u;
            g_supply_defl_frames = 0u;
        }
        ++g_supply_defl_frames;
        if (mv > 0) {
            ++g_supply_defl_samples;
            g_supply_defl_last_mv = mv;
            if (g_supply_defl_min_mv == 0 || mv < g_supply_defl_min_mv) g_supply_defl_min_mv = mv;
        }
        return;
    }

    if (mv > 0) {
        g_supply_rest_mv = mv;
        if (g_supply_rest_min_mv == 0 || mv < g_supply_rest_min_mv) g_supply_rest_min_mv = mv;
    }
    if (g_supply_rest_run < (uint32_t)JOY_SUPPLY_REST_SAMPLES) ++g_supply_rest_run;
    if (g_supply_rest_run < (uint32_t)JOY_SUPPLY_REST_SAMPLES) return;

    if (g_supply_deflected) {
        g_supply_deflected = 0u;
        ++g_supply_episodes;
        joy_log("\n  input: J36 stick supply episode");
        joy_log_dec(" n=", (int32_t)g_supply_episodes);
        joy_log_dec(" rest_mv=", g_supply_rest_mv);
        joy_log_dec(" rest_min_mv=", g_supply_rest_min_mv);
        joy_log_dec(" deflect_min_mv=", g_supply_defl_min_mv);
        joy_log_dec(" deflect_last_mv=", g_supply_defl_last_mv);
        joy_log_dec(" deflect_frames=", (int32_t)g_supply_defl_frames);
        joy_log_dec(" deflect_samples=", (int32_t)g_supply_defl_samples);
        joy_log("\n");
        joy_supply_commit();
        return;
    }

    if (!g_supply_armed && g_supply_rest_mv > 0) {
        g_supply_armed = 1u;
        joy_log("\n  input: J36 stick supply probe armed");
        joy_log_dec(" rest_mv=", g_supply_rest_mv);
        joy_log("\n");
        joy_supply_commit();
    }
}

static int key_is_mapped(uint32_t gpio) {
    return gpio != MT6592_J36_KEY_GPIO_UNMAPPED;
}

static int matrix_is_mapped(uint32_t matrix) {
    return matrix != MT6592_J36_KEY_MATRIX_UNMAPPED;
}

static uint32_t gpio_read_din_bank(uint32_t bank) {
    return read32(GPIO_BASE + GPIO_DIN_BASE + bank * GPIO_VAL_REG_STRIDE);
}


static void gpio_capture_baseline(void) {
    for (uint32_t bank = 0; bank < MT6592_KEYS_GPIO_DIN_BANKS; ++bank) {
        g_gpio_baseline[bank] = gpio_read_din_bank(bank);
    }
    g_gpio_baseline_valid = 1u;
    joy_log_u32("\n  input: GPIO baseline g0=", g_gpio_baseline[0]);
    joy_log_u32(" g1=", g_gpio_baseline[1]);
    joy_log_u32(" g2=", g_gpio_baseline[2]);
    joy_log("\n");
}

/* GPIOs whose DIN level is not a clean switch closure (e.g. a pin that tracks
 * a scanning signal and alternates every frame). Must be confirmed stable
 * across multiple input pumps before the level is trusted, otherwise it
 * synthesises a phantom press every other frame. Currently empty: no button
 * is GPIO-backed anymore (the old noisy "DOWN=GPIO35" entry was a KPD scan
 * artifact, not a switch). */
static int gpio_needs_debounce(uint32_t gpio) {
    static const uint32_t kNoisy[] = {
        MT6592_J36_KEY_GPIO_DEBOUNCE_LIST_END,
    };
    for (uint32_t i = 0; kNoisy[i] != MT6592_J36_KEY_GPIO_DEBOUNCE_LIST_END; ++i) {
        if (kNoisy[i] == gpio) return 1;
    }
    return 0;
}

static uint32_t gpio_level_is_pressed(uint32_t value) {
    return MT6592_J36_KEY_ACTIVE_LOW ? (value == 0u) : (value != 0u);
}

static uint32_t gpio_value_is_pressed(uint32_t gpio, uint32_t value) {
    /* The EINT-backed buttons (dpad and friends) idle high through the DWS
     * pulls and short to ground when pressed — absolute active-low level,
     * confirmed by live captures (GPIO8/RIGHT: clean 1->0 on press). */
    (void)gpio;
    return gpio_level_is_pressed(value);
}

static uint32_t noisy_gpio_level_is_pressed(uint32_t gpio, uint32_t value) {
    enum { kStableFrames = 2u };
    static uint32_t active_gpio = MT6592_J36_KEY_GPIO_UNMAPPED;
    static uint32_t last_value;
    static uint32_t stable_value = 1u;
    static uint32_t stable_frames;

    if (active_gpio != gpio) {
        active_gpio = gpio;
        last_value = value;
        stable_value = value;
        stable_frames = 0u;
        return 0u;
    }
    if (value != last_value) {
        last_value = value;
        stable_frames = 0u;
        return 0u;
    }
    if (stable_frames < kStableFrames) {
        ++stable_frames;
        if (stable_frames < kStableFrames) return 0u;
    }
    stable_value = value;
    return gpio_value_is_pressed(gpio, stable_value);
}

/* Spin a bounded number of cycles so two GPIO samples are separated by roughly
 * a microsecond on the MT6592 core -- enough for a noisy pin to settle one way
 * but far too short to matter for the engine loop. */
static void gpio_short_settle(void) {
    for (volatile uint32_t i = 0; i < 40u; ++i) { /* ~1us at ~1GHz effective */ }
}

/*
 * PADS THAT CANNOT IDLE HIGH, AND THE ONLY WAY LEFT TO READ THEM.
 *
 * DPAD_UP's pad, 93, will not rise on an internal pull-up. That is measured, not inferred,
 * and it took the two readings below to separate "the pad is broken" from "the pull is too
 * weak" -- both taken in the LK console on a live board:
 *
 *     pin 5d 0 0 2 0   ->  mode=0 dir=0 pull=2 dout=0 din=0    input, pulled UP, still 0
 *     pin 5d           ->  din=0 held AND din=0 released       the pull-up never wins
 *
 *     pin 5d 0 1 0 1   ->  mode=0 dir=1 pull=0 dout=1
 *     pin 5d           ->  din=1 released, din=0 held          the DRIVER always wins
 *
 * So the switch is an ordinary one to ground -- press it and the pad goes low, exactly like
 * the other three D-pad pads -- and the input buffer works perfectly. What is missing is
 * only something strong enough to hold the pad high while nobody is pressing. Something on
 * this board loads pad 93 harder than the internal pull-up (tens of kilohms) can fight, and
 * no register write can make that resistor bigger.
 *
 * THE FIX IS TO SUPPLY THE HIGH WITH THE OUTPUT DRIVER, FOR A MICROSECOND AT A TIME. Drive
 * the pad high, let it settle, sample DIN while it is still driven, and put it straight back
 * to an input. Released, the driver wins and DIN reads 1. Held, the closed switch wins and
 * DIN reads 0 -- which is the whole point: the driver is deliberately not strong enough to
 * beat a dead short, so the switch is still what decides the reading.
 *
 * YES, THIS SHORTS THE DRIVER TO GROUND WHILE THE BUTTON IS HELD, and that is the honest
 * cost of the approach. It is bounded on purpose: the pad is an output for the ~1 us of
 * gpio_short_settle() plus one register read, once per input poll, so the duty cycle is
 * well under a thousandth even with the button held down continuously. A MediaTek pad's
 * default drive is a few milliamps into a hard short, so the peak is a few milliamps for a
 * microsecond. The alternative was leaving one button on the console permanently dead.
 *
 * WHICH PADS GET THIS IS MEASURED AT INIT, NOT HARDCODED. key_pads_probe_idle() arms every
 * mapped button pad's pull-up, lets it settle, and reads it: a pad that still reads 0 with
 * nothing pressed cannot idle high and is added here. That keeps the rule this whole keypad
 * bring-up converged on -- write only what is measurably wrong -- and it means a board that
 * populates the missing pull-up simply never takes this path. A false positive (a pad marked
 * because the user happened to be holding that button at boot) is harmless: the driven read
 * is correct for a healthy pad too, it just costs the microsecond.
 */
enum { KEY_DRIVEN_PAD_MAX = 8u };
static uint32_t g_driven_pads[KEY_DRIVEN_PAD_MAX];
static uint32_t g_driven_pad_count;

static uint32_t key_pad_is_driven(uint32_t pin) {
    for (uint32_t i = 0; i < g_driven_pad_count; ++i) {
        if (g_driven_pads[i] == pin) return 1u;
    }
    return 0u;
}

static void key_pad_mark_driven(uint32_t pin) {
    if (key_pad_is_driven(pin)) return;
    if (g_driven_pad_count >= KEY_DRIVEN_PAD_MAX) return;
    g_driven_pads[g_driven_pad_count++] = pin;
}

static uint32_t key_gpio_read_driven(uint32_t pin) {
    uint32_t value;

    /* No resistor in the loop: PULLEN off first, so the pull-down this pad is stuck on is
     * not still fighting while the driver ramps. DOUT before DIR, always -- setting the
     * direction first would drive whatever DOUT happened to hold, which on a button pad
     * means a deliberate short to ground for as long as it takes to fix. */
    gpio_write_val_bit(GPIO_PULLEN_BASE, pin, 0u);
    gpio_write_val_bit(GPIO_DOUT_BASE, pin, 1u);
    gpio_write_val_bit(GPIO_DIR_BASE, pin, 1u);
    gpio_short_settle();
    value = gpio_read_val_bit(GPIO_DIN_BASE, pin);
    /* Input again before anything else. Everything after this point is housekeeping; the
     * pad must stop driving the instant the sample is taken. */
    gpio_write_val_bit(GPIO_DIR_BASE, pin, 0u);
    gpio_write_val_bit(GPIO_PULLSEL_BASE, pin, 1u);
    gpio_write_val_bit(GPIO_PULLEN_BASE, pin, 1u);
    return value;
}

static uint32_t key_is_pressed(uint32_t gpio) {
    uint32_t value = key_pad_is_driven(gpio) ? key_gpio_read_driven(gpio)
                                             : gpio_read_val_bit(GPIO_DIN_BASE, gpio);
    if (gpio_needs_debounce(gpio)) {
        gpio_short_settle();
        if (gpio_read_val_bit(GPIO_DIN_BASE, gpio) != value) return 0u;
        return noisy_gpio_level_is_pressed(gpio, value);
    }
    return gpio_value_is_pressed(gpio, value);
}

/* Reverse lookup: MT6592 GPIO number -> MT6592_KEY_* mask, generated from
 * g_key_map[] so there is a single source of truth. Returns 0 if the pin is
 * not wired to any button. Used by the USB input layer's GPIO change probe. */
uint32_t mt6592_keys_gpio_to_mask(uint32_t gpio) {
    for (uint32_t i = 0; i < KEY_MAP_COUNT; ++i) {
        if (g_key_map[i].gpio == gpio) return g_key_map[i].mask;
    }
    return 0u;
}

int mt6592_keys_gpio_din_changed(uint32_t* out_bank, uint32_t* out_previous, uint32_t* out_value) {
    static uint32_t last[MT6592_KEYS_GPIO_DIN_BANKS];
    static uint32_t have_last;
    uint32_t changed_bank = 0xffffffffu;
    uint32_t changed_previous = 0u;
    uint32_t changed_value = 0u;

    for (uint32_t bank = 0; bank < MT6592_KEYS_GPIO_DIN_BANKS; ++bank) {
        uint32_t value = gpio_read_din_bank(bank);
        if (have_last && value != last[bank] && changed_bank == 0xffffffffu) {
            changed_bank = bank;
            changed_previous = last[bank];
            changed_value = value;
        }
        last[bank] = value;
    }
    have_last = 1u;

    if (changed_bank == 0xffffffffu) return 0;
    if (out_bank) *out_bank = changed_bank;
    if (out_previous) *out_previous = changed_previous;
    if (out_value) *out_value = changed_value;
    return 1;
}

static int wire_is_mapped(const key_wire_t* wire) {
    return wire->drive != MT6592_J36_KEY_WIRE_UNMAPPED &&
           wire->sense != MT6592_J36_KEY_WIRE_UNMAPPED;
}

/*
 * Is this pad one the keypad block owns? Nothing in this driver may drive it, pull it or
 * read it as a GPIO -- the block owns all eight. The single exception is kpd_pads_apply(),
 * which runs once at init, before the block is enabled, and only on a pad whose mode is
 * wrong; after that point this guard is absolute.
 *
 * THE LIST WAS SHORT BY THREE, and those three were the whole regression: pads 11 (KPROW3),
 * 12 (KPCOL3) and 2 (KPCOL4) were not here, so the wire scan was free to claim them as
 * drive and sense pads. It claimed them, and row 3 plus columns 3 and 4 stopped scanning.
 * A guard that lists six of the block's eight pads does not protect the block.
 */
static int kpd_pad_is_ours(uint32_t pin) {
    static const uint32_t kPads[] = {
        MT6592_J36_KPD_STROBE0_GPIO, MT6592_J36_KPD_STROBE1_GPIO,
        MT6592_J36_KPD_STROBE2_GPIO, MT6592_J36_KPD_STROBE3_GPIO,
        MT6592_J36_KPD_SENSE0_GPIO,  MT6592_J36_KPD_SENSE1_GPIO,
        MT6592_J36_KPD_SENSE2_GPIO,  MT6592_J36_KPD_SENSE3_GPIO,
        MT6592_J36_KPD_SENSE4_GPIO,
    };
    for (uint32_t i = 0; i < sizeof(kPads) / sizeof(kPads[0]); ++i) {
        if (kPads[i] != MT6592_J36_KPD_PAD_UNMAPPED && kPads[i] == pin) return 1;
    }
    return 0;
}

/* Every key the wire scan is responsible for. mt6592_keys_read() skips the GPIO and
 * matrix readings for these, so a key never has two sources that can disagree -- and
 * in particular a keypad column dragged low by a pulse cannot resurface as a press. */
static uint32_t key_wire_owned_mask(void) {
    static uint32_t cached;
    static uint32_t valid;

    if (!valid) {
        for (uint32_t i = 0; i < KEY_WIRE_COUNT; ++i) {
            if (wire_is_mapped(&g_key_wires[i])) cached |= g_key_wires[i].mask;
        }
        valid = 1u;
    }
    return cached;
}

/*
 * One pass of the wire scan: pulse each drive pad low, read the pads paired with it.
 *
 * The pulse is the whole trick and also the whole risk, so both are bounded:
 *
 *   - DOUT is written before DIR, so the pad never spends a cycle as a high output.
 *     Driving a hub high would fight the pull-ups on every other button on it.
 *   - MODE is written, and is put straight back. It used to be untouchable: a pad that
 *     was not already mode 0 was skipped outright. That rule cost six buttons. On the
 *     first hardware run of this scan, exactly the keys whose drive pad is 8, 20, 30, 35
 *     or 45 were dead and exactly the keys whose drive pad is 2, 11 or 12 worked, which
 *     is not a wiring pattern -- it is this rule. The pad is forced to mode 0 for the
 *     ~30 us of the pulse and restored, which is what `kpdwire` has done to 11, 12, 30,
 *     75 and 167 without incident. A keypad pad and an already-output pad are still
 *     refused, because those are the cases that could actually break something.
 *   - the pad goes back to input with the pull it idles at, before the next drive pad is
 *     touched. That matters for pad 11, which is the drive pad for VOL+, VOL- and SELECT
 *     and the sense pad for START and MENU: each pass restores it, so whichever role comes
 *     second sees it configured. (It used to matter for pad 12 in the same way, when R2
 *     drove it and A sensed it. Neither is a wire pair any more.)
 *   - ~30 us of drive per pad. Long enough for an actively driven falling edge on a
 *     board trace, far too short for KP_DEBOUNCE (0x400 at 32 kHz, ~32 ms of required
 *     stability) to latch the column drag the pulse causes.
 *
 * Ghosting cannot happen with the pairs measured so far, because no two buttons share
 * both pads, and with R2 out of the table there is no longer even a cycle in the graph.
 * If a future pair does collide, the fix is the standard one -- read only the sense pads
 * that belong to the pass, which this already does.
 */
static uint32_t key_wires_scan(void) {
    uint32_t pressed = 0u;

    for (uint32_t i = 0; i < KEY_WIRE_COUNT; ++i) {
        uint32_t drive = g_key_wires[i].drive;
        uint32_t seen = 0u;
        uint32_t save_mode;
        uint32_t save_pullsel;
        uint32_t j;

        if (!wire_is_mapped(&g_key_wires[i])) continue;
        /* One pass per distinct drive pad; later entries sharing it ride along. */
        for (j = 0; j < i; ++j) {
            if (wire_is_mapped(&g_key_wires[j]) && g_key_wires[j].drive == drive) seen = 1u;
        }
        if (seen) continue;
        if (kpd_pad_is_ours(drive) || gpio_read_val_bit(GPIO_DIR_BASE, drive) != 0u) {
            /* Logged once per pad rather than swallowed: a wire-backed key whose drive
             * pad cannot be pulsed is silently dead otherwise, and "the button does
             * nothing" is the one symptom this driver has produced by five different
             * mechanisms already. A keypad pad belongs to the KPD engine; an output
             * belongs to whoever made it one. Neither is ours to borrow. */
            static uint32_t warned;
            if ((warned & g_key_wires[i].mask) == 0u) {
                warned |= g_key_wires[i].mask;
                joy_log_u32("\n  input: wire drive pad unusable, key skipped: pad ", drive);
                joy_log_u32(" mode=", gpio_get_mode(drive));
                joy_log("\n");
            }
            continue;
        }

        save_mode = gpio_get_mode(drive);
        save_pullsel = gpio_read_val_bit(GPIO_PULLSEL_BASE, drive);
        if (save_mode != 0u) gpio_set_mode(drive, 0u);
        gpio_write_val_bit(GPIO_DOUT_BASE, drive, 0u);
        gpio_write_val_bit(GPIO_PULLEN_BASE, drive, 0u);
        gpio_write_val_bit(GPIO_DIR_BASE, drive, 1u); /* now driving low */
        mt6592_delay_cycles(KEY_WIRE_SETTLE_CYCLES);

        for (j = i; j < KEY_WIRE_COUNT; ++j) {
            if (!wire_is_mapped(&g_key_wires[j]) || g_key_wires[j].drive != drive) continue;
            if (gpio_read_val_bit(GPIO_DIN_BASE, g_key_wires[j].sense) == 0u) {
                pressed |= g_key_wires[j].mask;
            }
        }

        gpio_write_val_bit(GPIO_DIR_BASE, drive, 0u); /* input again */
        key_gpio_restore_pull(drive, save_pullsel);
        if (save_mode != 0u) gpio_set_mode(drive, save_mode);
    }
    return pressed;
}

/*
 * Resolve the START/R2/VOL_UP triangle. Nothing to do unless all three read pressed,
 * which -- given they are a triangle -- means at least two of them really are held and
 * the third is a phantom of the other two, with no way to tell which case it is.
 *
 * The decode drops VOL_UP and keeps the two gameplay buttons, so a player pausing with a
 * trigger held gets what they pressed instead of a volume change. Flipping
 * MT6592_J36_KEY_GHOST_PREFER_GAMEPLAY to 0 takes the conservative decode instead:
 * report none of the three, which can never fabricate a press but makes START and R2
 * mutually exclusive. Either way the log line below fires once so we find out whether
 * this ever happens in practice.
 *
 * AS OF THE FOURTH HARDWARE RUN THIS IS DEAD CODE, and it is kept rather than deleted
 * because the reason it is dead could be undone by one table edit. The cycle was
 * 11-12-167 and R2 (12--167) was one of its three edges; R2 is no longer a wire pair, so
 * the wire graph is 6 nodes and 5 edges -- a tree, cyclomatic 5 - 6 + 1 = 0, no cycle, no
 * ghost. The guard below states that as a computation instead of a comment: a cycle needs
 * all three of its edges, so if any triangle member is not a mapped wire pair there is
 * nothing to resolve. Restore R2 as 12--167 and the resolver arms itself again.
 */
static uint32_t key_wires_resolve_ghost(uint32_t pressed) {
    static uint32_t logged;

    if ((key_wire_owned_mask() & KEY_GHOST_TRIANGLE) != KEY_GHOST_TRIANGLE) return pressed;
    if ((pressed & KEY_GHOST_TRIANGLE) != KEY_GHOST_TRIANGLE) return pressed;
    if (!logged) {
        logged = 1u;
        joy_log("\n  input: START/R2/VOL+ triangle lit -- two of three are real\n");
    }
    if (MT6592_J36_KEY_GHOST_PREFER_GAMEPLAY) return pressed & ~(uint32_t)MT6592_KEY_VOL_UP;
    return pressed & ~(uint32_t)KEY_GHOST_TRIANGLE;
}

/* One line per change of the wire mask, capped. This is the readback that confirms
 * the mapping on hardware: press each button once and the boot-status ring names the
 * mask it produced, which is the attribution every earlier capture was missing. */
static void key_wires_log_change(uint32_t mask) {
    static uint32_t logged;
    static uint32_t last;
    static uint32_t have_last;

    if (have_last && mask == last) return;
    last = mask;
    have_last = 1u;
    if (logged >= KEY_WIRE_LOG_LIMIT) return;
    ++logged;
    joy_log_u32("\n  input: wire scan mask=", mask);
    joy_log("\n");
}

static uint32_t matrix_bit_is_pressed(uint32_t matrix) {
    uint32_t index = matrix / KPD_MEM_BITS;
    uint32_t bit = matrix % KPD_MEM_BITS;
    uint32_t mask = 1u << bit;

    if (index >= KPD_NUM_MEMS) return 0u;
    if ((mask & kpd_mem_mask(index)) == 0u) return 0u;
    /* Vendor polarity, same as the stock LK (FUN_81e0a530) and the Android
     * kpd driver: the scan words idle at all-ones and a held key pulls its
     * bit low. Absolute polarity beats the old baseline-XOR approach, which
     * inverted any key that happened to be held while the baseline was
     * captured at boot. */
    return (kpd_read_mem_index(index) & mask) == 0u;
}

/*
 * Report every keypad pad, and mux the ones the boot chain did not.
 *
 * THIS FUNCTION HAS NOW BEEN WRONG IN BOTH DIRECTIONS, so the history is worth keeping.
 *
 * It started as kpd_pads_mux(), which muxed two strobes (74, 92) and three senses (75,
 * 167, 168) and nothing else, because the preloader's mtk_kpd_gpio_set writes exactly
 * those five pads. The same revision handed pads 11, 12 and 2 to a GPIO pulse scan, so
 * the block was left with two rows and three columns -- bits {0,1,2,9,10,11} and nothing
 * else -- and seven buttons went dark: START, MENU, SELECT, VOL+, VOL-, A and R2.
 *
 * The fix for that was to stop writing entirely, on the working commit's rule ("MVII
 * deliberately has no gpio_set_mode/dir writers here") plus the inference that pads 11,
 * 12 and 2 must therefore arrive correct from the DWS defaults. HARDWARE SAID NO. A live
 * console dump on a fresh boot, with MVII's keypad init never having run:
 *
 *     pad 11  m0 d0 p1 n0      pad 12  m0 d0 p1 n0      pad 2   m0 d0 p1 n0
 *     pad 75  m1 d0 p2 n1      pad 167 m1 d0 p0 n1      pad 168 m1 d0 p0 n1
 *
 * Mode 0, input, pull-DOWN is the DWS park for an UNUSED pin, not a keypad line. The boot
 * chain hands over five muxed pads and three parked ones, and the three parked ones are
 * KPROW3, KPCOL3 and KPCOL4 -- exactly row 3 (VOL-, VOL+, SELECT, START, MENU) plus
 * column 3 (R2 at bit 3, A at bit 12). Seven keys, three pads, and they are the same seven
 * the old mux bug killed, which is why one wrong model could stand in for the other.
 *
 * SO IT WRITES AGAIN, AND THE RULE IS NARROWER THAN EITHER OLD ONE: touch a pad only if
 * its mode is wrong. 74, 92, 75, 167 and 168 already work and are left exactly as found,
 * floating sense lines included -- 167 and 168 sit in mode 1 with no pull at all and Y and
 * B read fine through them, so the block supplies what they need and a "corrective" pull
 * here would be a change with no evidence behind it. Only a pad whose mode is wrong is
 * written, and every write is logged with its before and after.
 *
 * AND THEN THE THIRD WRONG TURN: WRITING MODE 1 TO THE THREE PARKED PADS. Knowing a pad is
 * parked is not knowing what to write to it. Mode 1 is the keypad function on the five the
 * preloader names; on 11, 12 and 2 it is something else, and hardware showed exactly what.
 * Pad 11 at mode 1 became a STATIC LOW instead of a strobed KPROW3, so every row-3 press
 * tied its column to ground in ALL eight rows at once -- SELECT lit bits 2, 11, 20, 29, 38,
 * 47, 56 and 65, VOL+ lit the column-1 eight, VOL- the column-0 eight. Pads 12 and 2 at
 * mode 1 lit nothing at all. Row 0 and row 1 kept reporting one clean bit per key the whole
 * time, which is the control: the block and the keymap are fine, the pinmux is not.
 *
 * THE MODES BELOW ARE MEASURED, NOT ASSUMED. `kpdmode <pad>` in the LK console sweeps one
 * pad through all eight modes with a button held and reports what the block sees. Three
 * runs, one hit each, twenty-one clean misses: pad 12 at mode 3 dragged column 3 low
 * (bits 3 12 21 30 39 48 57 66), pad 2 at mode 6 dragged column 4 (4 13 22 31 40 49 58 67),
 * and pad 11 at mode 3 -- the real proof -- put SELECT on BIT 29 ALONE, no drag, exactly
 * where the vendor keymap puts it. So the mode is a per-pad fact and this board needs three
 * different values; MT6592_J36_KPD_MUX_MODE is now only the preloader's five.
 *
 * PAD 93 IS THE SAME RULE POINTING THE OTHER WAY. It is KPROW2's pad, and this board gave
 * it to DPAD_UP's EINT, so its correct mode is 0 and a mode of 1 there means something has
 * traded UP away for a matrix row whose keys this board does not have. The console reports
 * it low at rest, which is either a pull-down (harmless, the pull-up loop in
 * mt6592_keys_init() fixes it) or the block strobing it (not harmless, and this is the
 * write that takes it back).
 */
static void kpd_pads_apply(void) {
    enum { KPD_PAD_DIR_IN = 0u, KPD_PAD_DIR_OUT = 1u };
    static const struct {
        const char* name;
        uint32_t pad;
        uint32_t want_mode; /* per pad: the keypad lands at a different index on each */
        uint32_t dir;   /* rows drive, columns listen; only applied on a mode fix */
        uint32_t pull;  /* 0 = leave the resistors alone, 1 = pull up */
    } kPads[] = {
        {"KPROW0 ", MT6592_J36_KPD_STROBE0_GPIO, MT6592_J36_KPD_MUX_MODE, KPD_PAD_DIR_OUT, 0u},
        {"KPROW1 ", MT6592_J36_KPD_STROBE1_GPIO, MT6592_J36_KPD_MUX_MODE, KPD_PAD_DIR_OUT, 0u},
        {"KPROW3 ", MT6592_J36_KPD_STROBE3_GPIO, MT6592_J36_KPD_STROBE3_MUX_MODE, KPD_PAD_DIR_OUT, 0u},
        {"KPCOL0 ", MT6592_J36_KPD_SENSE0_GPIO, MT6592_J36_KPD_MUX_MODE, KPD_PAD_DIR_IN, 1u},
        {"KPCOL1 ", MT6592_J36_KPD_SENSE1_GPIO, MT6592_J36_KPD_MUX_MODE, KPD_PAD_DIR_IN, 1u},
        {"KPCOL2 ", MT6592_J36_KPD_SENSE2_GPIO, MT6592_J36_KPD_MUX_MODE, KPD_PAD_DIR_IN, 1u},
        {"KPCOL3 ", MT6592_J36_KPD_SENSE3_GPIO, MT6592_J36_KPD_SENSE3_MUX_MODE, KPD_PAD_DIR_IN, 1u},
        {"KPCOL4 ", MT6592_J36_KPD_SENSE4_GPIO, MT6592_J36_KPD_SENSE4_MUX_MODE, KPD_PAD_DIR_IN, 1u},
        /* Not a keypad pad on this board: UP's EINT. Wanted mode is 0, input, pulled up. */
        {"UP/EINT", MT6592_J36_KPD_ROW2_PAD_TAKEN_BY_DPAD_UP, 0u, KPD_PAD_DIR_IN, 1u},
    };
    uint32_t i;
    uint32_t fixed = 0u;

    for (i = 0; i < sizeof(kPads) / sizeof(kPads[0]); ++i) {
        uint32_t mode;

        if (kPads[i].pad == MT6592_J36_KPD_PAD_UNMAPPED) continue;
        mode = gpio_get_mode(kPads[i].pad);
        joy_log("\n  input: ");
        joy_log(kPads[i].name);
        joy_log_u32(" pad=", kPads[i].pad);
        joy_log_u32(" mode=", mode);
        joy_log_u32(" dir=", gpio_read_val_bit(GPIO_DIR_BASE, kPads[i].pad));
        joy_log_u32(" din=", gpio_read_val_bit(GPIO_DIN_BASE, kPads[i].pad));
        if (mode == kPads[i].want_mode) continue;

        /* Mode first, so the pad is never a GPIO output for the width of two stores: a
         * row pad turned output before it is muxed would drive its whole row low, and
         * every key on it would read pressed for as long as that lasted. */
        joy_log_u32(" -> muxing to mode ", kPads[i].want_mode);
        gpio_set_mode(kPads[i].pad, kPads[i].want_mode);
        gpio_write_val_bit(GPIO_DIR_BASE, kPads[i].pad, kPads[i].dir);
        if (kPads[i].pull != 0u) {
            gpio_write_val_bit(GPIO_PULLSEL_BASE, kPads[i].pad, 1u);
            gpio_write_val_bit(GPIO_PULLEN_BASE, kPads[i].pad, 1u);
        } else {
            gpio_write_val_bit(GPIO_PULLEN_BASE, kPads[i].pad, 0u);
        }
        joy_log_u32(", now mode=", gpio_get_mode(kPads[i].pad));
        joy_log_u32(" din=", gpio_read_val_bit(GPIO_DIN_BASE, kPads[i].pad));
        ++fixed;
    }
    joy_log_u32("\n  input: KPD pads checked, pads re-muxed ", fixed);
    joy_log("\n");
}

void mt6592_keys_init(void) {
    /* Ungate the SoC AUXADC clock so the analog sticks read on every boot, not
     * just the first boot after flashing. */
    auxadc_clock_enable();
    /* Before any live sampling: stage1's capture is the only one taken while
     * nobody could plausibly have been touching the sticks. */
    auxadc_seed_centers_from_handoff();
    /* Before the scanner is enabled -- the vendor order is pads, then clock, then EN --
     * and the log shows every pad as the boot chain left it before anything is written. */
    kpd_pads_apply();
    kpd_scanner_enable();

    /* Button pads keep their pinmux -- the four D-pad pads are mode-0 EINTs already and
     * reprogramming a button pad's mode is what wedged the old bring-up -- but every
     * mapped one gets an input pull-up, so "active low" has a defined idle to be low
     * against. See key_gpio_arm_pullup().
     *
     * The mapped set is four pads (93, 45, 20, 8) and not one of them belongs to the
     * keypad block, so the kpd_pad_is_ours() guard below is belt and braces rather than
     * load-bearing -- but it stays, because the last time this loop ran over a list that
     * happened to include a KPD column pad, that column stopped scanning. Pad 93 is the
     * one that needs this most: the LK console reports it low at rest, and a pad parked on
     * a pull-down cannot report a closure because it already reads 0 and has nowhere to
     * move. kpd_pads_apply() has already put it back to mode 0 if something took it. */
    for (uint32_t i = 0; i < KEY_MAP_COUNT; ++i) {
        if (g_key_map[i].gpio == MT6592_J36_KEY_GPIO_UNMAPPED) continue;
        if (kpd_pad_is_ours(g_key_map[i].gpio)) continue;
        key_gpio_arm_pullup(g_key_map[i].gpio);
    }

    /* Then ask each armed pad whether the pull-up actually took, because on pad 93 it does
     * not. A pad still reading 0 with the pull-up on has nowhere to move when its switch
     * closes, so it gets the driven read instead -- see key_gpio_read_driven(). The pull
     * needs a moment against the pad's own capacitance before the answer means anything --
     * a weak pull-up into a few tens of picofarads is a microsecond-scale RC, so the wait
     * below is generous by two orders of magnitude rather than cutting it fine -- and the
     * log line is the record of which pads this board actually needed it for. */
    for (uint32_t i = 0; i < 64u; ++i) gpio_short_settle();
    for (uint32_t i = 0; i < KEY_MAP_COUNT; ++i) {
        uint32_t pin = g_key_map[i].gpio;

        if (pin == MT6592_J36_KEY_GPIO_UNMAPPED) continue;
        if (kpd_pad_is_ours(pin)) continue;
        if (gpio_read_val_bit(GPIO_DIN_BASE, pin) != 0u) continue;
        key_pad_mark_driven(pin);
        joy_log_u32("\n  input: pad ", pin);
        joy_log(" will not idle high on its pull-up -- switching it to a driven read");
    }
    joy_log_u32("\n  input: pads needing a driven read: ", g_driven_pad_count);
    joy_log("\n");

    /* Wire pads get the same treatment, and they need it more: a drive pad's idle
     * state IS its pull-up (that is what the scan restores it to after every pulse),
     * and a sense pad with no pull has no level to fall from. Keypad pads are the one
     * exception and are skipped: the block owns them and MVII writes none of them.
     *
     * THE TABLE IS EMPTY ON THIS BOARD, so this loop and the scan it arms do nothing.
     * All five entries went to WIRE_UNMAPPED when the five keys they read turned out to be
     * row 3 of the matrix; see the wire block in mt6592_board_j36.h. The mask logged below
     * must be 0, and a nonzero mask on this hardware means somebody has re-mapped a pair
     * and should check first whether its two pads are a row and a column. */
    for (uint32_t i = 0; i < KEY_WIRE_COUNT; ++i) {
        if (!wire_is_mapped(&g_key_wires[i])) continue;
        if (!kpd_pad_is_ours(g_key_wires[i].drive)) key_gpio_arm_pullup(g_key_wires[i].drive);
        if (!kpd_pad_is_ours(g_key_wires[i].sense)) key_gpio_arm_pullup(g_key_wires[i].sense);
    }
    joy_log_u32("\n  input: wire-scanned keys mask=", key_wire_owned_mask());
    joy_log("\n");

    gpio_capture_baseline();
    kpd_capture_baseline();
}

uint32_t mt6592_keys_read(void) {
    uint32_t owned = key_wire_owned_mask();
    uint32_t pressed = 0u;
    uint32_t wired;

    if (INPUT_RUNTIME_TRACE) kpd_log_deltas();
    /* Levels and scan memories FIRST, while no drive pad has been pulsed in this
     * poll: a pulse drags the keypad column its hub sits on, so this is the honest
     * sample. Keys the wire scan owns are skipped entirely -- their GPIO level is
     * meaningless (both sides of the switch idle high) and their matrix bit is in a
     * row the preloader never strobed.
     *
     * THIS ORDERING IS NOW LOAD-BEARING RATHER THAN MERELY TIDY, because pad 12 is read
     * two ways in one poll: it is A's level pad and it is START's drive pad. Reading it
     * here, before key_wires_scan() pulls it to output-low, is the difference between
     * "A is pressed" and "A is pressed on every poll forever". Do not move this loop
     * below the scan. */
    for (uint32_t i = 0; i < KEY_MAP_COUNT; ++i) {
        uint32_t gpio = g_key_map[i].gpio;
        uint32_t matrix = g_key_map[i].matrix;
        if ((g_key_map[i].mask & owned) != 0u) continue;
        if (key_is_mapped(gpio) && key_is_pressed(gpio)) pressed |= g_key_map[i].mask;
        if (matrix_is_mapped(matrix) && matrix_bit_is_pressed(matrix)) pressed |= g_key_map[i].mask;
    }

    wired = key_wires_resolve_ghost(key_wires_scan());
    key_wires_log_change(wired);
    return pressed | wired;
}

int mt6592_keys_read_axes(mt6592_keys_axes_t* out) {
    static uint32_t unavailable_logged;
    static uint32_t movement_logged;
    uint32_t raw[AUXADC_SCAN_CHANNELS];
    uint32_t valid[AUXADC_SCAN_CHANNELS];
    uint32_t x_channel;
    uint32_t y_channel;
    uint32_t raw_x;
    uint32_t raw_y;

    if (!out) return -1;
    out->valid = 0;

    if (soc_auxadc_read_active_pair(raw, valid) != 0) {
        if (!unavailable_logged) {
            unavailable_logged = 1u;
            joy_log("\n  input: J36 AUXADC axes unavailable; local stick disabled\n");
            auxadc_log_data_regs("  input: J36 AUXADC data");
        }
        return -1;
    }

    auxadc_seed_centers(raw, valid);
    if (!g_aux_center_valid) {
        /* Still settling the AUXADC; report a valid-but-centered sample so the
         * pump treats the stick as at rest and no calibration locks onto a cold
         * conversion. */
        out->valid = 1;
        out->x = 0;
        out->y = 0;
        out->z = 0;
        out->rz = 0;
        return 0;
    }
    joy_choose_channels(raw, valid, &x_channel, &y_channel);
    if (x_channel != g_joy_x_channel || y_channel != g_joy_y_channel) {
        g_joy_cal_done = 0;
    }
    g_joy_x_channel = x_channel;
    g_joy_y_channel = y_channel;

    raw_x = valid[x_channel] ? raw[x_channel] : g_aux_center[x_channel];
    raw_y = valid[y_channel] ? raw[y_channel] : g_aux_center[y_channel];

    /* auxadc_seed_centers() already discards the cold conversion and records
     * the settled rest position. Do not impose a second 75-300 ms flat-window
     * calibration here: that made the first real stick movement disappear.
     * The existing plateau rebase still corrects warm-up drift afterwards. */
    if (!g_joy_cal_done) {
        joy_adopt_center(g_aux_center[x_channel], g_aux_center[y_channel], x_channel, y_channel);
        g_joy_cal_done = 1;
        joy_log("\n  input: J36 stick center ready x=");
        joy_log_u32("", (uint32_t)g_joy_center_x);
        joy_log_u32(" y=", (uint32_t)g_joy_center_y);
        joy_log_u32(" xch=", x_channel);
        joy_log_u32(" ych=", y_channel);
        joy_log("\n");
    }

    out->valid = 1;
    out->raw_x = raw_x;
    out->raw_y = raw_y;
    out->raw_z = valid[AUXADC_JOY_Z] ? raw[AUXADC_JOY_Z] : AUXADC_JOY_FALLBACK_CENTER;
    out->raw_rz = valid[AUXADC_JOY_RZ] ? raw[AUXADC_JOY_RZ] : AUXADC_JOY_FALLBACK_CENTER;
    out->center_x = g_aux_center[x_channel];
    out->center_y = g_aux_center[y_channel];
    out->center_z = g_aux_center[AUXADC_JOY_Z];
    out->center_rz = g_aux_center[AUXADC_JOY_RZ];
    out->channel_x = x_channel;
    out->channel_y = y_channel;

    if (!joy_calibrate(raw_x, raw_y, x_channel, y_channel)) {
        out->x = 0;
        out->y = 0;
        out->z = 0;
        out->rz = 0;
        return 0;
    }
    joy_plateau_rebase(raw_x, raw_y, x_channel, y_channel);
    out->center_x = (uint32_t)g_joy_center_x;
    out->center_y = (uint32_t)g_joy_center_y;

    if (INPUT_RUNTIME_TRACE) {
        joy_log_aux_motion(raw, valid, x_channel, y_channel);
        joy_log_raw_motion(raw_x, raw_y, (int32_t)raw_x - (int32_t)g_aux_center[x_channel],
                           (int32_t)raw_y - (int32_t)g_aux_center[y_channel]);
    }
    /* Not behind INPUT_RUNTIME_TRACE. That gate is off in every shipped build,
     * and this measures a fault the user hits on a normal device -- a probe
     * nobody can run is not a probe. It bounds and disables itself instead. */
    joy_supply_probe((int32_t)raw_x - g_joy_center_x, (int32_t)raw_y - g_joy_center_y);
    axes_from_raw_pair(raw_x, raw_y, g_joy_center_x, g_joy_center_y, &out->x, &out->y);
    out->z = 0;
    out->rz = 0;
    joy_recenter_small_bias(raw_x, raw_y, x_channel, y_channel);

    if (INPUT_RUNTIME_TRACE && !movement_logged && (out->x != 0 || out->y != 0)) {
        movement_logged = 1u;
        joy_log_u32("\n  input: J36 stick move x=", (uint32_t)out->x);
        joy_log_u32(" y=", (uint32_t)out->y);
        joy_log_u32(" rawx=", raw_x);
        joy_log_u32(" rawy=", raw_y);
        joy_log("\n");
    }

    return 0;
}

void mt6592_keys_snapshot(mt6592_keys_snapshot_t* out) {
    if (!out) return;
    out->mapped_buttons = mt6592_keys_read();
    out->kpd_sta = read16(KPD_BASE + KPD_STA);
    out->kpd_mem1 = kpd_read_mem_index(0u);
    out->kpd_mem2 = kpd_read_mem_index(1u);
    out->kpd_mem3 = kpd_read_mem_index(2u);
    out->kpd_mem4 = kpd_read_mem_index(3u);
    out->kpd_mem5 = kpd_read_mem_index(4u);
    out->kpd_sel = read16(KPD_BASE + KPD_SEL);
    out->kpd_debounce = read16(KPD_BASE + KPD_DEBOUNCE);
    out->kpd_scan_timing = read16(KPD_BASE + KPD_SCAN_TIMING);
    out->kpd_en = read16(KPD_BASE + KPD_EN);
}
