/* Copyright (c) 2026 SynChanCyberSecurity LLC. All Rights Reserved. */
/* See mvii_debug_console.h for the protocol and the safety argument. */

#include "mvii_debug_console.h"

#include "backlight.h"
#include "mt6592_bootstatus.h"
#include "mt6592_dbgflag.h"
#include "mt6592_delay.h"
#include "mt6592_disp_hw.h"
#include "mt6592_led.h"
#include "mt6592_msdc.h"
#include "mt6592_msdc_sd.h"
#include "mt6592_pmic.h"
#include "mt6592_pwrap.h"
#include "mt6592_timer.h"
#include "mt6592_uart.h"
#include "mt6592_usb_gadget.h"
#include "mt6592_wifi_hif.h"
#include "mt6592_wifi_sdio.h"
#include "mt6592_wifi_wmt.h"
#include "mvii_fat.h"
#include "mvii_gpu_offload.h"

/* ── Hardware windows the console pokes by name ──
 *
 * These are duplicated here rather than pulled from the drivers that own them
 * because the console's job is to look at hardware whose driver may be *wrong*.
 * A dump command that shares its offsets with the code under test can only ever
 * confirm that code's assumptions. */
enum {
    KPD_BASE = 0x10011000u, /* matrix keypad: STA, MEM1..MEM5, DEB, SEL, EN */
    SOC_AUXADC_BASE = 0x11001000u,
    SOC_AUXADC_CON1_SET = 0x0008u,
    SOC_AUXADC_CON1_CLR = 0x000cu,
    SOC_AUXADC_CON2 = 0x0010u,
    SOC_AUXADC_DAT0 = 0x0014u,
    SOC_AUXADC_DAT_READY = 1u << 12,
    SOC_AUXADC_DAT_MASK = 0x0fffu,

    /* MT6323 blocks worth naming. The ISINK block is the one the LED driver
     * bets on and the one the board may not actually use. */
    PMIC_TOP_CKPDN0 = 0x0102u,
    PMIC_TOP_CKPDN2 = 0x010eu,
    PMIC_ISINK_BASE = 0x0330u,
    PMIC_ISINK_EN_CTRL = 0x0356u,
};

/* ── Output ──
 *
 * Everything the console says goes to three places: the USB pipe (what the
 * operator sees live), the UART (free, and the only channel that survives a USB
 * fault), and the eMMC console ring (what survives a *session* fault). The ring
 * is why a console that never enumerates is still worth entering. */

static char g_line[192];
static uint32_t g_line_len;

/* mvii_lk_main.c. Installed below so that a CPU exception taken inside a
 * command reports itself to the host that issued the command, instead of only
 * to a UART nobody has a cable for and an eMMC ring readable one boot later. */
extern void mvii_lk_set_log_sink(void (*sink)(const char* text));

/*
 * The sink itself: straight to the endpoint, deliberately bypassing the line
 * buffer.
 *
 * It runs from the abort handler, where g_line may be half-filled by whatever
 * was printing when the fault hit; going through put_str would interleave the
 * post-mortem with a fragment and could re-enter flush_line. The gadget is fully
 * polled and allocation-free, so calling it from an exception context is no
 * different from calling it anywhere else.
 */
static void console_log_sink(const char* text) {
    uint32_t n = 0u;

    while (text[n] != '\0') ++n;
    if (n != 0u) (void)mt6592_usb_gadget_send(text, n);
}

/*
 * The UART tap's destination: the driver trace, verbatim, on the console.
 *
 * Every driver on this board traces to the UART and nobody here has a cable for
 * it, so the whole WMT bootstrap -- which frame the connectivity MCU answered
 * and which it did not -- was written and thrown away, leaving `calibrated=no'
 * with no way to attribute it. With this installed the console prints it.
 *
 * It goes to the endpoint and the eMMC ring directly, NOT through put_str, for
 * the same reason console_log_sink does: the tap fires from inside whatever was
 * printing, so g_line may be half-filled, and re-entering flush_line would both
 * interleave the trace with a fragment and recurse back into the UART.
 */
static void console_uart_tap(const char* line) {
    uint32_t n = 0u;

    while (line[n] != '\0') ++n;
    if (n == 0u) return;
    mt6592_bootstatus_log_text(line);
    (void)mt6592_usb_gadget_send(line, n);
}

/*
 * The endpoint copy of a finished console line.
 *
 * Compiled out of the release bootloader, and this is the only reason it is a
 * function at all. That image links exactly one thing from this file --
 * mvii_battery_boot_policy(), which is boot path and not debug -- and
 * everything it prints goes through flush_line(), so this single call was what
 * kept the MUSB peripheral gadget in an image with no console to drive it. The
 * gadget is never initialised there (mvii_debug_console_run() is the only
 * caller of mt6592_usb_gadget_init(), and the release LK does not call it), so
 * the send already returned -1 without touching a register. Removing it makes
 * the gadget absent rather than merely inert, which is the difference between
 * a bootloader that declines to expose a debug transport and one that does not
 * carry the code for it.
 */
static void console_line_to_endpoint(const char* text, uint32_t len) {
#ifdef MVII_MT6592_LK_RELEASE
    (void)text;
    (void)len;
#else
    (void)mt6592_usb_gadget_send(text, len);
#endif
}

static void flush_line(void) {
    if (g_line_len == 0u) return;
    g_line[g_line_len] = '\0';
    /* Untapped: this line is already on its way to the endpoint two statements
     * below, and a console that mirrors itself prints everything twice. */
    mt6592_uart_puts_untapped(g_line);
    mt6592_bootstatus_log_text(g_line);
    console_line_to_endpoint(g_line, g_line_len);
    g_line_len = 0u;
}

static void put_char(char c) {
    if (g_line_len + 2u >= sizeof(g_line)) flush_line();
    g_line[g_line_len++] = c;
    if (c == '\n') flush_line();
}

static void put_str(const char* s) {
    while (*s != '\0') put_char(*s++);
}

static void put_hex(uint32_t v, uint32_t digits) {
    static const char kHex[] = "0123456789abcdef";
    uint32_t i;
    put_str("0x");
    for (i = digits; i-- > 0u;) put_char(kHex[(v >> (i * 4u)) & 0xfu]);
}

static void put_dec(int32_t v) {
    char buf[12];
    uint32_t n = 0u;
    uint32_t u;
    if (v < 0) {
        put_char('-');
        u = (uint32_t)(-v);
    } else {
        u = (uint32_t)v;
    }
    do {
        buf[n++] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u != 0u && n < sizeof(buf));
    while (n-- > 0u) put_char(buf[n]);
}

static void kv_hex(const char* label, uint32_t v, uint32_t digits) {
    put_str(label);
    put_hex(v, digits);
}

static void kv_dec(const char* label, int32_t v) {
    put_str(label);
    put_dec(v);
}

/* ── Timing ──
 * The GPT-backed delay is the only clock here; the console has no scheduler and
 * wants none. */

/* Keep the endpoint alive across a wait. Compiled out of the release
 * bootloader for the same reason console_line_to_endpoint() is: delay_ms() is
 * reachable from mvii_battery_boot_policy(), which is the one function that
 * image links out of this file, and this call was what dragged the peripheral
 * gadget in behind it. Nothing there ever initialises the gadget, so the poll
 * was reading an unpowered block's registers for no reason. */
static void console_gadget_service(void) {
#ifndef MVII_MT6592_LK_RELEASE
    mt6592_usb_gadget_poll();
#endif
}

static void delay_ms(uint32_t ms) {
    while (ms-- > 0u) {
        mt6592_delay_cycles(1000u * 33u);
        console_gadget_service();
    }
}

/* ── Parsing ──
 *
 * Numbers are hex by default with an optional 0x, because every interesting
 * value on this board is a register or a register address. A leading '#' asks
 * for decimal, which the two commands that want a human number (`bl`, `led`)
 * do not need since 0..100 parses the same either way below 10 -- so `bl 64`
 * means 100%. Documented in `help` so it cannot surprise anyone twice. */

static const char* skip_spaces(const char* p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

static int digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse one number, advance *pp past it. Returns 0 if a number was there. */
static int parse_u32(const char** pp, uint32_t* out) {
    const char* p = skip_spaces(*pp);
    uint32_t base = 16u;
    uint32_t value = 0u;
    int any = 0;
    int d;

    if (*p == '#') {
        base = 10u;
        ++p;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    for (;;) {
        d = digit_val(*p);
        if (d < 0 || (uint32_t)d >= base) break;
        value = value * base + (uint32_t)d;
        ++p;
        any = 1;
    }
    if (any == 0) return -1;
    *pp = p;
    *out = value;
    return 0;
}

static uint32_t parse_u32_default(const char** pp, uint32_t fallback) {
    uint32_t v;
    if (parse_u32(pp, &v) != 0) return fallback;
    return v;
}

/* Does `line` start with `word` followed by a separator or end-of-line? */
static const char* match_word(const char* line, const char* word) {
    while (*word != '\0') {
        if (*line != *word) return 0;
        ++line;
        ++word;
    }
    if (*line != '\0' && *line != ' ' && *line != '\t') return 0;
    return line;
}

/* Exact, case-sensitive equality. SSIDs differ by case and this is used to pick
 * which access point to join, so folding case here would silently join the
 * wrong one on a street with both. */
static int same_text(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/* ── The bridge magic, and the BROM probe it exists to survive ──
 *
 * WHY THERE IS A MAGIC AT ALL
 * ---------------------------
 * This port is 0x0e8d/0x4d56 and the board in BROM is 0x0e8d/0x2000-ish: same
 * vendor, one cable, and a host tool that has to tell them apart before it can
 * say anything useful. VID:PID does that when the host bothers to look, but the
 * failure that motivated this was a host that did NOT -- `flash` without -dbg
 * takes the first 0x0e8d device it finds, which is the live console whenever one
 * is up, and then drives the BROM handshake into it. So the two ends now name
 * themselves in-band as well:
 *
 *   device -> host   MVIIDBG1 magic=4d564931 ...      (on every session open)
 *   host   -> device !MVIIDBG1                        (the host's hello/resync)
 *
 * The device line is what lets a host that opened the wrong port find out
 * immediately, from the first bytes, instead of from a three-second handshake
 * timeout. The host line is a resync: it clears whatever half-typed rubbish is
 * in the command buffer and reprints the prompt, so a bridge that was left
 * mid-line by a dropped cable comes back usable without a reboot.
 *
 * Neither is mandatory in either direction. A host that never sends its magic
 * still gets a working console -- the exchange is there to make a MISIDENTIFIED
 * link diagnosable, and demanding it would only convert one confusing failure
 * into a different one.
 *
 * WHY THE BROM PROBE IS SWALLOWED RATHER THAN REJECTED
 * ----------------------------------------------------
 * The BROM handshake is four bytes sent one at a time, A0 0A 50 05, each
 * awaiting its echo (5F F5 AF FA), with the host restarting from A0 on any
 * mismatch -- see the host's handshake() in mtk_serial.go. Landing on this
 * console it does two things, and the second is the damaging one:
 *
 *   1. A0, 50 and 05 are bytes no operator can type, so they accumulate in the
 *      command buffer as garbage.
 *   2. 0A *is* a newline. Under the plain line reader every retry therefore
 *      submitted a line, so a three-second probe fired a burst of commands made
 *      of whatever fragments happened to be buffered around it.
 *
 * So the four bytes are consumed by a state machine ahead of the line reader.
 * A0 always starts or restarts it and is always swallowed; while it is running
 * the next expected byte is swallowed too, which is what keeps that 0A from
 * ending a line. Anything else drops through untouched.
 *
 * We deliberately do NOT answer with the echo. Answering would make this device
 * claim to be a BROM, and the host would then try to download an agent into a
 * booted board. Staying silent costs the probing host its handshake timeout and
 * nothing else, and it is the correct answer: we are not a BROM.
 */
#define CONSOLE_MAGIC_TAG "MVIIDBG1"
#define CONSOLE_HOST_MAGIC "!" CONSOLE_MAGIC_TAG

static uint32_t g_brom_stage;  /* bytes of A0 0A 50 05 matched so far */
static uint32_t g_brom_probes; /* complete probes swallowed, for the state dump */

/* Nonzero if this byte belongs to a BROM handshake attempt and must not reach
 * the line reader. */
static int console_swallow_brom_byte(uint8_t c) {
    static const uint8_t kBromStart[4] = {0xa0u, 0x0au, 0x50u, 0x05u};

    if (c == kBromStart[0]) {
        /* Restart, wherever we were. The host resets to A0 on every mismatch,
         * so a fresh A0 is far more likely to be a new attempt than to be the
         * middle of the old one. */
        g_brom_stage = 1u;
        return 1;
    }
    if (g_brom_stage == 0u) return 0;
    if (c != kBromStart[g_brom_stage]) {
        g_brom_stage = 0u;
        return 0;
    }
    if (++g_brom_stage >= 4u) {
        g_brom_stage = 0u;
        ++g_brom_probes;
    }
    return 1;
}

static void console_put_magic(void) {
    put_str(CONSOLE_MAGIC_TAG " magic=4d564931 pid=4d56 proto=1");
    kv_hex(" bromprobes=", g_brom_probes, 4u);
    put_char('\n');
}

/* Does the host's hello appear ANYWHERE in this line? Substring rather than
 * prefix, because the line it has to be found in is the one glued to whatever
 * fragment a dropped cable left behind -- see the call site. */
static int console_find_host_magic(const char* line) {
    for (; *line != '\0'; ++line) {
        const char* a = line;
        const char* b = CONSOLE_HOST_MAGIC;
        while (*b != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == '\0') return 1;
    }
    return 0;
}

/* ── Commands ── */

static void cmd_help(void) {
    put_str("commands (numbers are HEX unless prefixed with #):\n");
    put_str("  bat                  battery: THE WINDOW VERDICT first -- presence measured in\n");
    put_str("                       133 ms with no register written and no reboot risked --\n");
    put_str("                       then charger, mV, percent, raw regs, then what the SD card\n");
    put_str("                       holds. Read `fresh=' before anything else: it counts\n");
    put_str("                       conversions that differed from the one before, and 1 means\n");
    put_str("                       the converter handed back the same latch 128 times and the\n");
    put_str("                       verdict is worthless. The card section below is HISTORY and\n");
    put_str("                       no longer overrides the live measurement -- that it did is\n");
    put_str("                       what made this report NO CELL with a pack in the holder.\n");
    put_str("  bat base [set|<mV>]  THE CALIBRATION THAT RETIRED THE PROBE, and it is already\n");
    put_str("                       DONE: base 19185, delta +18, threshold 6 counts are baked\n");
    put_str("                       into mt6592_pmic.h and the board boots on them. VBAT is\n");
    put_str("                       VSYS, so with a cable in and an EMPTY holder the port alone\n");
    put_str("                       holds that node at one repeatable voltage. A regulator\n");
    put_str("                       sources and CANNOT SINK, so nothing but a cell can push the\n");
    put_str("                       node above that -- which makes `above' proof and `on it'\n");
    put_str("                       merely uninformative. `set' re-takes it for THIS board --\n");
    put_str("                       HOLDER EMPTY, or you record the pack and every reading\n");
    put_str("                       after says NO CELL. Plain `bat base' shows stored, live,\n");
    put_str("                       and THE DRIFT BETWEEN THEM.\n");
    put_str("  bat delta [set|<mV>] THE OTHER HALF: how far a pack moves the node, which sets\n");
    put_str("                       the threshold at a third of itself. `set' with the PACK IN\n");
    put_str("                       and the cable still in. Negative is legal and means\n");
    put_str("                       charging. MEASURED, and the second run is why the rule is\n");
    put_str("                       no longer a simple threshold: one session had pack-in at\n");
    put_str("                       base+21..+26 against holder-empty base-2..+4, but the next\n");
    put_str("                       had a pack that was fitted the whole time read 19183 (ON\n");
    put_str("                       the baseline) and climb to 19214 over a minute. The states\n");
    put_str("                       OVERLAP, so the highest reading of the session decides and\n");
    put_str("                       later low ones cannot retract it. Plain `bat delta' prints\n");
    put_str("                       the pair in the form they get pasted into the source -- IN\n");
    put_str("                       COUNTS, because a mV is 4.55 counts and the full-scale\n");
    put_str("                       constant is separately ~70 mV wrong, so a number re-entered\n");
    put_str("                       in mV comes back different from the one measured. `bat\n");
    put_str("                       delta 0' clears it. Order: `bat base set' EMPTY, then this.\n");
    put_str("  bat cal              BATTERY CALIBRATION MODE, for the one reading the window\n");
    put_str("                       cannot settle -- a pack sitting exactly on the baseline.\n");
    put_str("                       Cuts the charger and runs on the holder until VBAT falls\n");
    put_str("                       2 mV below the baseline: only a source can do that. THE\n");
    put_str("                       ONE COMMAND LEFT THAT CAN COST A REBOOT, and only worth\n");
    put_str("                       running when something is believed to be fitted.\n");
    put_str("  bat cal ladder [up]  THE OLD CALIBRATION: find WHICH WRITE STOPS THE BOARD.\n");
    put_str("                       Walks the CV setpoint (CHR_CON3) down its ladder and ends\n");
    put_str("                       on stock's own 300 ms charger-off (CHR_CON0 CSDAC_EN +\n");
    put_str("                       CHR_EN), putting each rung ON THE SD CARD BEFORE writing\n");
    put_str("                       it -- so the step that kills the board is on the medium\n");
    put_str("                       when the board stops. A sweep can never report this: the\n");
    put_str("                       fatal rung never gets to print, so all that survives is\n");
    put_str("                       the rung before it. PACK OUT it is meant to die; power\n");
    put_str("                       cycle and `bat' names the register, value and mV. That\n");
    put_str("                       point is where the charger lets go of VSYS, so it is\n");
    put_str("                       where the system must draw from the cell or stop -- which\n");
    put_str("                       is why the register that crashes the board is the one\n");
    put_str("                       that says whether a battery is there. PACK IN it runs to\n");
    put_str("                       the bottom, charger-off included: nothing else can hold\n");
    put_str("                       the rail across that, so it reports CELL FITTED. `up'\n");
    put_str("                       ladders the other way instead, for the OVP edge.\n");
    put_str("  bat probe [#ms]      THE CALIBRATED RUNG, walked the way it was measured:\n");
    put_str("                       rungs 0..4 in order, then rung 5, each one READ BACK\n");
    put_str("                       OUT OF CHR_CON3 before it counts. CONFIRMED BOTH WAYS --\n");
    put_str("                       pack out the board goes down on rung 5, pack in it walks\n");
    put_str("                       through. The read-back is not a nicety: without it an\n");
    put_str("                       ignored write reported CELL FITTED with an empty holder.\n");
    put_str("                       Takes the rung 3 times, 50 ms apart, and declares only if\n");
    put_str("                       every one survives -- pack out the FIRST probe after the\n");
    put_str("                       cell came out survived and the second powered the board\n");
    put_str("                       off. Costs a reboot whenever the answer is `no cell'.\n");
    put_str("  bat auto             THE BOOT POLICY, and the answer to `no reboot when\n");
    put_str("                       the cell is unfitted'. If the card holds a verdict it\n");
    put_str("                       is applied to the driver and NOT ONE REGISTER IS\n");
    put_str("                       WRITTEN -- no rung, no risk, no reboot, on this boot or\n");
    put_str("                       any after. If it does not, the probe is taken ONCE:\n");
    put_str("                       cell in it survives and records `fitted'; holder empty\n");
    put_str("                       the board goes down and THE NEXT BOOT reads the ARMED\n");
    put_str("                       breadcrumb, records `absent', and never probes again.\n");
    put_str("                       One reboot per battery change, none after it. This is\n");
    put_str("                       what LK boot should call. Six instruments have now\n");
    put_str("                       failed to sense presence live on this board; the crash\n");
    put_str("                       is the only thing that has ever separated the states,\n");
    put_str("                       so the win is spending it once instead of every time.\n");
    put_str("  bat forget           clear THE SESSION'S PEAK WINDOW READING and the sticky\n");
    put_str("                       card verdict. RUN THIS WHEN THE PACK COMES OUT: presence\n");
    put_str("                       latches on the highest reading seen, because a pack that\n");
    put_str("                       sits level with the port is indistinguishable from an\n");
    put_str("                       empty holder and only the peak is proof -- so nothing\n");
    put_str("                       short of this can notice a pack leaving. That is the one\n");
    put_str("                       failure mode the design has, and it is deliberate.\n");
    put_str("  bat track [#reps]    DOES THE RAIL FOLLOW THE SETPOINT? VBAT IS VSYS, so\n");
    put_str("                       with no cell the charger is the only thing on the node\n");
    put_str("                       and stepping CHR_CON3 down rungs 0..4 (4200 -> 4150 mV)\n");
    put_str("                       MUST drag the rail with it. With a cell fitted the node\n");
    put_str("                       is the cell, above every rung, and a charger cannot\n");
    put_str("                       sink -- so it cannot move. THE SLOPE ANSWERS, NOT THE\n");
    put_str("                       LEVEL, which is why this outlives `cell in and cell out\n");
    put_str("                       read within 2 mV': that compared absolute readings,\n");
    put_str("                       hostage to divider, full-scale and offset, where a\n");
    put_str("                       differential against a stimulus we control is immune to\n");
    put_str("                       all three. Carries its own control pass. Rung 5, the\n");
    put_str("                       fatal one, is never written: it CANNOT power the board\n");
    put_str("                       off. Flat in BOTH states means the ADC is the fault.\n");
    put_str("  bat baton [#secs]    the part's dedicated connector detect, CHR_CON7 bit 12.\n");
    put_str("                       MEASURED DEAD ON THIS BOARD 2026-08-09: stuck at 0\n");
    put_str("                       (`cell fitted') with the holder empty, on bit 12 and on\n");
    put_str("                       the 0142 bit 5 mirror alike, across a real connector\n");
    put_str("                       change -- an unpopulated input on a pull. It was worth\n");
    put_str("                       asking because the driver had never once READ it: the\n");
    put_str("                       rung verdict returned two lines above the sample, so\n");
    put_str("                       `undet=-1 seen=0' meant NEVER ASKED, not dead. Kept so\n");
    put_str("                       the negative result can be re-checked on another board.\n");
    put_str("  bat rung [#n] [#t]   the same rung as `bat probe' held too briefly to die in\n");
    put_str("                       -- n reads of CHR_CON2 (8, ~125 us), t times (8), each\n");
    put_str("                       with its own control. NO VERDICT, and that is a result:\n");
    put_str("                       cs and cv were each proposed as the discriminator and\n");
    put_str("                       each was killed by running the other battery state. cv\n");
    put_str("                       asserted 8/8 pack OUT in 2 of 8 trials against 2 of 4\n");
    put_str("                       pack IN -- the same rate. The control is the tell: with\n");
    put_str("                       nothing changed, ctl cs went 3,1,0,3,3,8,6,8. These bits\n");
    put_str("                       chatter near the window length, so a train is an aliased\n");
    put_str("                       sample of a square wave in BOTH states. Kept so the\n");
    put_str("                       evidence for that can be re-run in one line.\n");
    put_str("  bat point [r] [v]    show, or set by hand, the point the rung commands use.\n");
    put_str("  bat scan             kick every plausible PMIC AUXADC channel. Run it twice,\n");
    put_str("                       cell in and cell out -- but note this has been done and\n");
    put_str("                       every channel read within 2 mV either way.\n");
    put_str("  adcscan [a] [b]      sweep PMIC regs a..b, print non-zero\n");
    put_str("  chg                  charger input path: CHRDET, enables, WDT\n");
    put_str("  socadc [ch]          SoC AUXADC (12..15 are the sticks)\n");
    put_str("  pmicr <a> [n]        read n PMIC regs from a\n");
    put_str("  pmicw <a> <v>        write a PMIC reg\n");
    put_str("  peek <a> [n]         read n SoC words from a\n");
    put_str("  strings <a> [n] [m] [h]  scan n bytes from a for ASCII runs >= m chars,\n");
    put_str("                       stopping after h hits (0 = no cap). `strings 83100000`\n");
    put_str("                       sweeps the whole CONSYS EMI window for the WLAN\n");
    put_str("                       firmware's own log text. Runs longer than 96 chars\n");
    put_str("                       continue on the next line instead of being cut.\n");
    put_str("  poke <a> <v>         write a SoC word\n");
    put_str("  sweep <a> [n] [sz]   one checksum per page over n pages; diff two of these to\n");
    put_str("                       find what changed in a window `peek` is too small for.\n");
    put_str("                       0x180e0000..0x180effff HANGS THE BOARD and is skipped\n");
    put_str("  find <w> [a] [n]     which addresses hold the word <w>? pairs with `wifi mark`\n");
    put_str("  wifi log [n]         WHAT DOES THE FIRMWARE SAY IT IS DOING? it keeps a plain\n");
    put_str("                       English log in EMI at 83163010, 36 bytes a record. This\n");
    put_str("                       reads the whole thing; `wifi join' already prints its own\n");
    put_str("                       delta, so you do not have to diff two of these by hand.\n");
    put_str("                       Every question left about association is a question about\n");
    put_str("                       what the firmware decided, and this is where it says it\n");
    put_str("  wifi core            DID THE WLAN FIRMWARE RUN? reads the connectivity coredump\n");
    put_str("                       window, which the driver zeroes before every download\n");
    put_str("  wifi mark <a> [s] [n]  DOES A DOWNLOAD ACK MEAN THE WRITE LANDED? has the WLAN\n");
    put_str("                       boot ROM write s,s+1,.. at <a>, then `find` it. Try an\n");
    put_str("                       address the ROM cannot own (deadb000): if that ACKs, the\n");
    put_str("                       status byte proves nothing and 90% of the firmware may\n");
    put_str("                       never be landing. Before WIFI_START only\n");
    put_str("  wifi rd <a>          DID SECTIONS 0 AND 1 EVER LAND? reads CONSYS memory through\n");
    put_str("                       the boot ROM, which reaches the MCU-internal addresses no\n");
    put_str("                       AP window does. `wifi rd 6a000` is the WIFI_START entry\n");
    put_str("                       point; zero there ends the bring-up. Check the controls\n");
    put_str("                       first: 1f0000 must match peek 18080000, f0020000 must\n");
    put_str("                       match peek 83120000. Before WIFI_START only\n");
    put_str("  wifi dump <a> [n]    READ THE FIRMWARE BACK IN THE CLEAR: n words from <a> via\n");
    put_str("                       the ROM, which decrypted them on the way in. We have no key\n");
    put_str("                       for the image on the card; the chip does.\n");
    put_str("                       `wifi dump 6a000 100` is the WIFI_START entry code\n");
    put_str("  wifi dl [decoy]      download the image untouched, entry deferred. `decoy` edits\n");
    put_str("                       the nine partly-zero blocks in the image first, which is\n");
    put_str("                       MEASURABLY WORSE: 96 changed bytes turned all 2291 blocks that\n");
    put_str("                       had always read back zero into high-entropy garbage, in EMI by\n");
    put_str("                       plain load and in SRAM through the ROM alike. Kept only\n");
    put_str("                       because that result is the strongest fact we have about the\n");
    put_str("                       ROM's decryptor and it is worth being able to reproduce\n");
    put_str("  wifi verify          DID ALL FOUR SECTIONS LAND DECRYPTED? every block whose\n");
    put_str("                       ciphertext is E(sixteen zeros) must read back as zero and\n");
    put_str("                       every other block must not -- no key needed. Checks EMI by\n");
    put_str("                       load and connectivity SRAM through the ROM, so it reaches\n");
    put_str("                       section 1 at 0209f800, the one region never yet read back.\n");
    put_str("                       Run between `wifi dl` and `wifi go`\n");
    put_str("  wifi patch [raw]     the standalone version of what `wifi dl` now does inline. It\n");
    put_str("                       proved the decoy defeats the skip and, in the same run, that a\n");
    put_str("                       lone 16-byte packet decrypts differently every time -- so keep\n");
    put_str("                       it as a probe, not as the fix. `raw` re-sends verbatim\n");
    put_str("  wifi func <s> [0|1]  WMT FUNC_CTRL: turn a connectivity subsystem on. 3 is WLAN.\n");
    put_str("                       AFTER `wifi dl`, before `wifi go` -- the bootstrap that\n");
    put_str("                       powers CONSYS runs inside `wifi dl`, so sent any earlier\n");
    put_str("                       this is refused with `domain=no`. Measured NOT to be the\n");
    put_str("                       trigger for the ROM's A-die probe: the chip answers\n");
    put_str("                       `accepted` and the probe flag at 02090614 stays zero\n");
    put_str("  wifi adie            READ THE A-DIE CHIP ID over WMT, using stock's own frame\n");
    put_str("                       from opfunc_adie_lpbk_test. Answers 0x6627 on this board,\n");
    put_str("                       so the MT6625L is alive and reachable even though the\n");
    put_str("                       ROM never probes it. Needs the WMT link, so after `wifi dl`\n");
    put_str("  wifi wmt <b> <b>...  send ANY WMT frame verbatim and print the event. Bytes go\n");
    put_str("                       on the wire as typed, so a command found in the stock\n");
    put_str("                       kernel's tables costs a paste instead of a rebuild\n");
    put_str("  wifi wr <a> <v>      the write half, undecrypted (unlike `wifi mark`)\n");
    put_str("  wifi err             ask the boot ROM if it has an error queued. Every chunk\n");
    put_str("                       ACKed, but the ACK covers the packet, not the copy\n");
    put_str("  isink                dump the MT6323 ISINK block + clock gates\n");
    put_str("  led <r> <g> <b>      the three LEDs via the driver (0/1 each)\n");
    put_str("  ledscan              walk pads #47/#48/#49 raw, 2s each\n");
    put_str("  kpd                  dump the scan memories once and name the low bits; hold a\n");
    put_str("                       key while running it to see whether the block sees that key\n");
    put_str("  kpdmon [#secs]       LIVE BUTTONS BY NAME (wire scan) + all 80 matrix bits\n");
    put_str("  kpdmap               guided one-button-at-a-time map, screen-cued\n");
    put_str("  kpdon                the vendor enable seq: PMIC clock gate, SEL, EN\n");
    put_str("  kpdmux               re-apply all 8 KPD lines (74 92 11 / 75 167 168 12 2)\n");
    put_str("  kpdmode <pad> [#s]   sweep a pad's pinmux 0..7 holding one button, and say\n");
    put_str("                       which mode the KPD block can see it on\n");
    put_str("  pinhunt <pin> [#s]   same for ONE pad -- the safe form on unknown pads\n");
    put_str("  pin <n> [m [d [p [o]]]]  read/set one pad: mux, dir, pull, dout\n");
    put_str("  gpiodump             every pin's mux/dir/pull/level, raw\n");
    put_str("  gpioout <pin> <0|1>  drive a pad already in mode 0 as an output\n");
    put_str("  gpu                  BRING THE MALI-450 UP AND SAY WHETHER IT WORKS. Powers the\n"
            "                       MFG domain, ungates its clocks, disables the four per-core\n"
            "                       MMUs and runs the GP/PP shader self-tests, then prints one\n"
            "                       yes/no per capability -- each is its own test and each can\n"
            "                       fail alone. The step-by-step trace goes to the UART and the\n"
            "                       boot-status ring. Reachable here and nowhere else, so this\n"
            "                       command is what puts the driver in the image at all\n");
    put_str("  wifi [on|own|link]   CONSYS: registers vs. what the driver believes\n");
    put_str("  wifi fw              stage the MediaTek patches + WIFI_RAM_CODE_SOC off the card\n");
    put_str("                       and start it. The A-die probe flag the firmware ASSERTs on\n");
    put_str("                       (CONSYS 02090614, rlm_phy.c #4209) is now armed here, in\n");
    put_str("                       the last window before WIFI_START in which the ROM answers\n");
    put_str("  wifi probe           run the MT6625L A-die probe (between `wifi dl` and `go`).\n");
    put_str("  wifi domain          send the regulatory channel table stock sends before it ever\n");
    put_str("                       scans (CID 19, twice) and that we never have. Run it between\n");
    put_str("                       two `wifi scan`s: an empty channel list in the firmware would\n");
    put_str("                       explain a scan that completes and finds nothing.\n");
    put_str("  wifi scan [p|a]      2.4 GHz scan, PASSIVE by default (needs `wifi fw` first).\n");
    put_str("                       Passive listens for beacons and never keys the transmitter;\n");
    put_str("                       `active' also sends probes, and is what took the board off\n");
    put_str("                       the bus twice. Same command, same HIF path, so the pair\n");
    put_str("                       says whether the fault is the command or the RF. Kicks the\n");
    put_str("                       charger and arms the WDT first, and reports HIF state once\n");
    put_str("                       a second so a fault names the second it hit\n");
    put_str("  wifi list            reprint the last scan without rescanning\n");
    put_str("  wifi stats           IS THE RECEIVER HEARING ANYTHING? the firmware's own\n"
            "                       twelve MAC counters, CID 130, which stock reads through\n"
            "                       wlanoidQueryStatistics. rx_fcs_errors is the one that\n"
            "                       matters: an FCS error is a frame that was demodulated to\n"
            "                       the end and then failed its checksum, so a radio that is\n"
            "                       unpowered or unclocked cannot count one. It does NOT prove\n"
            "                       a real transmitter is being heard -- noise tripping preamble\n"
            "                       detect counts the same. Zero with rx_fragments zero too\n"
            "                       means nothing is arriving to decode. Run after `wifi scan`,\n"
            "                       which now prints it automatically\n");
    put_str("  wifi mcr <a> [v]     read (or write then read back) a firmware register, CID\n"
            "                       194. THE ONLY REGISTER WINDOW THAT SURVIVES WIFI_START --\n"
            "                       `wifi rd'/`wifi dump' go through the boot ROM and stop\n"
            "                       working at the handoff, which is where the scan problem\n"
            "                       lives. Reads are safe; a write pokes a live radio at an\n"
            "                       address nothing validates\n");
    put_str("  wifi pass <secret>   passphrase for the next join (blank clears it)\n");
    put_str("  wifi join <idx|ssid> associate with an entry from `wifi list`, and poll it\n");
    put_str("  wifi step <idx|ssid> send the association commands ONE AT A TIME, polling a\n");
    put_str("                       second between each, so the one the firmware stops\n");
    put_str("                       surviving can be named instead of guessed at\n");
    put_str("  wifi drop            abandon a running association so scanning works again.\n");
    put_str("                       `join` does this for itself when it gives up; this is for\n");
    put_str("                       an attempt started by anything that does not\n");
    put_str("  wifi start <addr>    arm the next `wifi fw' with WIFI_START override=1 and this\n");
    put_str("                       entry point; 0 = the 0x60000 the J36 kernel passes\n");
    put_str("  wifi start image     arm it with override=0/address=0 instead -- what the vendor\n");
    put_str("                       source builds, and the one combination never yet tried.\n");
    put_str("                       Neither form sends anything: run `wifi fw' after it\n");
    put_str("  wifi pc [n] [us]     sample the connectivity MCU's program counter at\n");
    put_str("                       0x18070160, n times, us apart (16 x 100us default).\n");
    put_str("                       This is stock's only post-mortem instrument -- its\n");
    put_str("                       stp_dbg_poll_cpupcr is wmt_plat_read_cpupcr in a loop,\n");
    put_str("                       and wmt_plat_read_dmaregs beside it is a stub. Needs no\n");
    put_str("                       HIF, no ownership and no download, so it reads before\n");
    put_str("                       `wifi dl` and after the boot ROM has stopped answering.\n");
    put_str("                       changes=0 means that core is stopped and `min` is where.\n");
    put_str("  wifi trace [rd <a>|go] [gap-us]  WHY DOES THE ROM NOT JUMP? burst-trace that PC\n");
    put_str("                       speed from the instruction after the command hits WTDR0.\n");
    put_str("                       `rd` is ACCESS_REG, which the ROM ANSWERS -- run it first,\n");
    put_str("                       it is the control. `go` is WIFI_START, which it services\n");
    put_str("                       and then declines to dispatch. Diff them: what is unique\n");
    put_str("                       to `go` is the dispatch path, and where it ends is the\n");
    put_str("                       decision not to jump to 0006a000\n");
    put_str("  sd [emmc|at <lba>]   probe the microSD card and mount its FAT volume\n");
    put_str("  ls [path]            list a directory on the mounted volume\n");
    put_str("  bl <#pct>            backlight duty\n");
    put_str("  panel                run the LK panel bring-up\n");
    put_str("  fill <argb>          fill the framebuffer\n");
    put_str("  flag [debug|boot|brom]  what the NEXT boot does. `debug` (the default, and what\n");
    put_str("                       this command always did) re-arms the one-shot eMMC console\n");
    put_str("                       flag; `boot` clears it so the board boots the kernel; `brom`\n");
    put_str("                       arms the BROM USBDL latch and resets NOW, coming back in\n");
    put_str("                       download mode with no LK in the way. Host side:\n");
    put_str("                       ./flash -device <vcom> -flag debug|boot|brom\n");
    put_str("  bootlog [bytes]      print what the PREVIOUS image left in the eMMC console\n");
    put_str("                       ring (2000 hex bytes of tail by default). Crash the OS,\n");
    put_str("                       power back in here, and run this FIRST: LK writes the same\n");
    put_str("                       ring from byte 0 on every boot, and so does every line\n");
    put_str("                       this console prints, so the OS's log is being eaten from\n");
    put_str("                       the front while you type. Its tail is the safe part. The\n");
    put_str("                       boot-status record cannot help at all -- LK overwrites the\n");
    put_str("                       whole of it every single boot.\n");
    put_str("  magic                name this end of the bridge: the tag the host matches on,\n");
    put_str("                       and the count of BROM handshakes swallowed. `!MVIIDBG1` is\n");
    put_str("                       the same thing from the other direction -- the host's hello,\n");
    put_str("                       which also RESYNCS the line reader, so a bridge left\n");
    put_str("                       mid-command by a yanked cable is recovered without a reboot.\n");
    put_str("  boot                 leave the console and boot the kernel\n");
    put_str("  reboot               reset the SoC now\n");
}

static void dump_pmic_range(uint32_t start, uint32_t end, int only_nonzero) {
    uint32_t a;
    uint32_t v;
    uint32_t shown = 0u;

    /* MT6323 registers are 16 bits on a 2-byte stride; an odd address is not a
     * register and pwrap will not thank you for asking. */
    for (a = start & ~1u; a <= end; a += 2u) {
        if (mt6592_pwrap_read(a, &v) != MT6592_PWRAP_OK) {
            kv_hex("  ", a, 4u);
            put_str(" = <pwrap error>\n");
            continue;
        }
        if (only_nonzero != 0 && v == 0u) continue;
        kv_hex("  ", a, 4u);
        kv_hex(" = ", v, 4u);
        put_char('\n');
        ++shown;
        if (shown >= 256u) {
            put_str("  ... truncated at 256 lines\n");
            return;
        }
    }
    if (only_nonzero != 0 && shown == 0u) put_str("  (all zero)\n");
}

static const char* bat_status_name(int status) {
    switch (status) {
        case MT6592_PMIC_STATUS_CHARGING: return "CHARGING";
        case MT6592_PMIC_STATUS_DISCHARGING: return "DISCHARGING";
        case MT6592_PMIC_STATUS_NOT_CHARGING: return "NOT_CHARGING";
        case MT6592_PMIC_STATUS_FULL: return "FULL";
        default: return "UNKNOWN";
    }
}

/* BC1.2's verdict. UNKNOWN with a cable in is not a failure — the classifier runs
 * on dwells off the plug edge and takes up to about 700 ms to answer. */
static const char* usb_type_name(int type) {
    switch (type) {
        case MT6592_PMIC_USB_TYPE_SDP: return "SDP (USB host)";
        case MT6592_PMIC_USB_TYPE_CDP: return "CDP (charging host)";
        case MT6592_PMIC_USB_TYPE_DCP: return "DCP (wall charger)";
        case MT6592_PMIC_USB_TYPE_NONSTANDARD: return "NONSTANDARD";
        case MT6592_PMIC_USB_TYPE_APPLE_BRICK_ID: return "APPLE_BRICK_ID";
        default: return "UNKNOWN";
    }
}

static void bat_status(void) {
    mt6592_pmic_battery_t bat;
    uint32_t v;
    int rc;

    /*
     * NO BURST, AND NOW NO WAIT EITHER. This used to open with 128 CS_DET samples
     * 2 ms apart -- 256 ms per invocation -- to fill a presence window whose duty
     * was printed further down, and the duty was measured not to separate the two
     * battery states it was meant to separate. Both are gone.
     *
     * One blocking sample instead, which is what this command is allowed to do:
     * the gauge is a state machine and the console is explicitly not the frame
     * path, so a few milliseconds here buys a fresh median on both sides of the
     * sense shunt rather than whatever the last engine turn happened to leave.
     */
    {
        int primed = 0;
        (void)mt6592_pmic_sample_blocking(MT6592_PSY_CAPACITY, &primed);
    }

    rc = mt6592_pmic_battery_read(&bat);
    kv_dec("bat: read rc=", rc);
    kv_dec(" charger=", bat.charger_online);
    put_str(" (1 cable, 0 none, -1 CHRDET never read)");
    /* WHAT the cable is, which CHRDET cannot say: a comparator on CHRIN is equally
     * true of a laptop port and a 2 A brick. This is BC1.2's answer, and the limit
     * is what the board's cust_charging.h grants that answer. */
    put_str("\r\n     usb_type=");
    put_str(usb_type_name(bat.usb_type));
    kv_dec(" input_limit_ma=", bat.input_limit_ma);
    kv_dec(" plug_events=", (int)mt6592_pmic_charger_generation());
    /* The two voltages, side by side and never conflated. `mv` is BATSNS, the
     * cell node; `chg_mv` is VCHR, the cable. They used to be one number, which
     * is why plugging in appeared to overwrite the battery: it did not, there
     * was simply nowhere else for the charger's voltage to be reported. */
    kv_dec(" mv=", bat.battery_mv);
    kv_dec(" chg_mv=", bat.charger_mv);
    /* SAY WHEN THAT NUMBER IS THE TOP CODE -- but it should no longer be one. The
     * eternal `1799 SATURATED' this printed was channel 8 / 0x0722, which reads
     * 0xffff (READY plus a railed 0x7fff) on this board forever. VCHR is channel 5
     * / 0x071a, which is what stock's battery_meter cmd 7 converts, and it moves.
     * The divider is stock's too now (369/39), so chg_mv is CABLE millivolts and
     * full scale is 17046 -- a number no charger reaches. If this still says
     * SATURATED, the channel is wrong again or the pin is faulted; it is not a
     * units problem any more. */
    if (bat.charger_mv >= 17000) put_str(" (SATURATED: railed, not a voltage)");
    put_str(" status=");
    put_str(bat_status_name(bat.status));
    kv_dec(" pct=", bat.battery_percent);
    /*
     * WHERE DID THAT NUMBER COME FROM? Not from the voltage on this line, and that
     * is deliberate. It is stock's oam_d_5: SEEDED from the PMIC's wakeup OCV latch
     * -- a conversion the hardware took before the charger path existed, the only
     * reading on this board that is about the cell rather than the rail -- and then
     * slewed one point per thirty seconds toward the live curve lookup, in the
     * direction the shunt says charge is flowing.
     *
     * So `pct' will NOT agree with `mv' walked through the table, and if it did
     * that would be the bug: with a cable in, the charger holds this node at its CV
     * setpoint whether the holder contains a full pack or nothing at all (the two
     * differ by about seven millivolts), and a lookup on it reads full either way.
     * That is where the eternal 99% came from. The next line shows both halves --
     * the seed the level rests on, and the lookup it is creeping toward -- so a
     * disagreement here can be attributed instead of guessed at.
     */
    if (bat.charger_online != 1) {
        put_str(" (unplugged: the load current is leaving the cell, so the live"
                " lookup is a real OCV and the level is tracking it down)");
    } else if (bat.status == MT6592_PMIC_STATUS_FULL) {
        put_str(" (six sets at/under 150 mA past 4050 mV: the pack is full)");
    } else if (bat.battery_percent >= 99) {
        put_str(" (99: the ladder's cap -- stock keeps 99 until charging says full)");
    } else {
        put_str(" (cable in: the level is the wakeup seed creeping toward the rail's"
                " lookup, NOT the rail's lookup itself)");
    }
    /* rc != 0 does not mean the fields are meaningless -- the driver fills them on
     * every path -- it means CHRDET has not been read even once and the charger
     * column is a default rather than a measurement. */
    if (rc != 0) put_str(" (charger state not determined: pwrap down, or too early)");
    put_char('\n');

    /* The two halves of the number above, so it can be argued with. */
    {
        const int seed_mv = mt6592_pmic_hw_ocv_mv();
        const int target = mt6592_pmic_soc_target_percent();

        put_str("bat: seed=");
        if (seed_mv > 0) {
            put_dec(seed_mv);
            put_str("mV (wakeup OCV latch, AUXADC_ADC8, taken before the charger)");
        } else {
            put_str("NONE (latch unreadable or implausible; the level was seeded from"
                    " the live rail and is optimistic by however much the charger is"
                    " holding it up)");
        }
        kv_dec("  target=", target);
        put_str("% (live lookup: where the level is slewing, 1%/30s)");
        put_char('\n');
    }

    /*
     * NO `level=' LINE ANY MORE, and its absence is the point.
     *
     * There used to be a `level_trusted' bit here, published so that indicators
     * could decide whether the percentage was attributable to a cell. It answered
     * a real question -- a FULL PACK and an EMPTY HOLDER are the same node to
     * within seven millivolts, so at the setpoint the number cannot be attributed
     * -- and every consumer of it turned it into a claim about whether a battery
     * EXISTS, which is a different question and an unanswerable one. Both LK's
     * park and the LED shipped bugs from it, in opposite directions.
     *
     * The percentage above says what it says. Whether the node is a cell or the
     * port is not knowable on this board (see the top of mt6592_pmic.h), and this
     * screen no longer offers a bit that pretends otherwise. `I now=' below is the
     * closest thing to an answer that is actually a measurement.
     */
    put_str("bat: icon=");
    switch (mt6592_pmic_supply_icon(&bat)) {
        case MT6592_PMIC_ICON_BATTERY: put_str("BATTERY"); break;
        case MT6592_PMIC_ICON_BATTERY_CABLE: put_str("BATTERY+CABLE"); break;
        default: put_str("?"); break;
    }
    kv_dec("  ocv=", bat.ocv_mv);
    put_str("mV");
    kv_dec(" I=", bat.current_ma);
    put_str(bat.current_valid ? "mA (measured)" : "mA (NOT measured: no median yet)");
    put_char('\n');

    /*
     * THE SENSE PAIR. Every other number on this screen is a voltage on the
     * battery net, and the charger holds that net at its CV setpoint with or
     * without a cell -- which is why none of them has ever settled presence.
     * This is the first quantity here that is a DIFFERENCE, and a difference
     * across a sense resistor cannot be non-zero unless current is flowing.
     *
     * Both numbers are a MEDIAN OF FIVE conversions -- stock's own
     * PMIC_AUXADC_STOCK_TIMES -- not one conversion each. One shot per channel,
     * which is what this printed before the driver kept rings, is the difference
     * of two ~19200-count readings with nothing rejecting an outlier, so a single
     * stray conversion WAS the answer.
     *
     * BOTH UNITS, because they are not equally fine. Stock's finished answer is
     * 1000 * dmV / 68 mOhm computed from samples already TRUNCATED to whole
     * millivolts, so its resolution is 1 mV / 68 mOhm = 14.7 mA and every delta
     * this board has shown (0..7 counts) rounds to zero in it. The mA here is the
     * same division done in counts -- ~3.2 mA per count -- which is arithmetic,
     * not a recalibration. The counts remain the instrument.
     */
    {
        int bat_raw = 0, vsen_raw = 0, delta = 0, ma = 0;
        if (mt6592_pmic_sense_pair(&bat_raw, &vsen_raw, &delta, &ma) == 0) {
            kv_dec("bat: sense batsns(ch7)=", bat_raw);
            kv_dec(" vsen(ch6)=", vsen_raw);
            kv_dec(" delta=", delta);
            kv_dec(" counts (signed ", vsen_raw - bat_raw);
            put_str(")");
            kv_dec(" = ", ma);
            put_str(" mA (median of 5 per channel; R=68 mOhm, offset 0)\n");
            put_str("  (1 count = 0.2197 mV = 3.2 mA; stock's own divide cannot resolve\n");
            put_str("   below 14 mA, so read the counts. delta= is stock's, clamped at zero\n");
            put_str("   because stock only ever asks how much is going IN; the signed figure\n");
            put_str("   beside it is the same subtraction with the sign kept, and that is what\n");
            put_str("   the gauge corrects with now.\n");
            put_str("   THE MEASUREMENT TO TAKE, cable OUT: the board is running off the cell,\n");
            put_str("   so its own load current is leaving through this shunt and the signed\n");
            put_str("   figure MUST be negative -- a few hundred mA is 60..100 counts. If it\n");
            put_str("   reads zero there, the two taps are not across the cell's path and no\n");
            put_str("   IR correction on this port can work; say so and it gets rethought.)\n");
        } else {
            put_str("bat: sense pair unavailable (pwrap down, or a channel never became ready)\n");
        }
    }

    /*
     * AND WHAT THE GAUGE DID WITH IT. The pair above is one fresh reading; this is
     * the same current as the DRIVER's own running state, and the verdict the top
     * of the percentage is gated on.
     *
     * THE PEAK AND THE RUN COUNTER ARE NO LONGER PRINTED, because they are no
     * longer published. They were three separate accessors -- charge_current,
     * charge_current_ma, charge_full_state -- and each one was a second opinion
     * about a number that already appears above. The latch is still stock's rule
     * (BAT_TopOffModeAction: six consecutive sample sets at or under
     * CHARGING_FULL_CURRENT, cust +0x98 = 150 mA, and only after the pack has
     * passed V_CC2TOPOFF_THRES, cust +0x90 = 4050 mV; cleared under
     * RECHARGING_VOLTAGE, cust +0x94 = 4110 mV) -- it is simply internal, and the
     * ONE way to observe it is status=FULL on the line above. That is deliberate:
     * a debug screen that can show a run of 4/6 invites reading the counter
     * instead of the verdict, and the counter is not the contract.
     *
     * One sample used to be enough for the latch, which is why the top of the
     * charge flickered: the divide resolves ~3.2 mA per count and stock's own
     * truncation resolves 14.7, so a pack genuinely sitting near 150 mA lands on
     * either side of the line from one set to the next. Six in a row cannot.
     *
     * Read it against the percentage above: 100% is reachable with a cable in only
     * when this line says the charger has let go.
     *
     * AND IT IS SIGNED AND IS LIVE WITH NO CABLE. Current used to be sampled only
     * while a supply was attached, on the reasoning that there is no charge current
     * to measure otherwise -- true, and the wrong quantity. What the shunt carries
     * is the CELL's current, and with the cable out that is the board's own load
     * leaving the pack, which is exactly the correction the unplugged percentage was
     * missing. A negative reading is therefore the normal state on battery.
     */
    {
        put_str("bat: I now=");
        if (bat.current_valid) {
            put_dec(bat.current_ma);
            put_str("mA (signed: negative = leaving the pack)");
        } else {
            put_str("not measured (both sense channels want five conversions first)");
        }
        put_str(" latch=");
        put_str(bat.status == MT6592_PMIC_STATUS_FULL
                    ? "FULL (six sets at/under 150 mA past 4050 mV: 100% is earned)"
                    : (bat.charger_online == 1
                           ? "not full (still delivering: the top is held at 99%)"
                           : "n/a (no cable: there is nothing to terminate)"));
        put_char('\n');
    }

    /*
     * ── THE STEP THAT USED TO BE MISSING, SHOWING ITS WORKING ──
     *
     * This board reported pct=100 while the line above measured 926 mA going INTO
     * the pack. Both numbers came from the same driver and only one of them could
     * be right: a cell taking most of an amp is not full.
     *
     * The cause was the input, not the curve. BATSNS reads a TERMINAL voltage --
     * the cell's own EMF plus whatever the charge current develops across the
     * pack's internal resistance -- and it was being looked up in a table indexed
     * by OPEN-CIRCUIT voltage. Under charge those differ by R*I, which at 157 mOhm
     * and 926 mA is 145 mV. Near the CV setpoint that is only three points, which
     * is why the error hid; at 4000 mV terminal the same current is worth
     * thirty-nine, and the sign is always the same, so the number could only ever
     * climb with a cable in. It is also why the old hand-drawn curve had to put
     * 100% at the CV setpoint to reach 100% at all.
     *
     * The table is the vendor's now (77 entries lifted out of the stock kernel at
     * 0xc0b8294c, with the matching resistance table at 0xc0b832ec), and so is the
     * conversion (mtk_imp_tracking @ 0xc061126c). This line is the conversion:
     * the terminal voltage in, the current taken out of it, the pack resistance
     * used, and the open-circuit voltage the percentage above was actually read
     * from. If pct is ever surprising, it is one of these three that says why.
     */
    {
        int ocv = 0, r_mohm = 0, used_ma = 0;
        if (mt6592_pmic_gauge_ocv(bat.battery_mv, &ocv, &r_mohm, &used_ma) == 0) {
            kv_dec("bat: gauge vterm=", bat.battery_mv);
            kv_dec("mV - I=", used_ma);
            kv_dec("mA x R=", r_mohm);
            kv_dec("mOhm -> ocv=", ocv);
            put_str("mV");
            if (used_ma == 0)
                put_str(" (no current measured: OCV is the terminal reading, which is"
                        " right at rest and is the old bug with a cable in)");
            put_char('\n');
        }
    }

    /*
     * EVERY PRESENCE INDICATOR THAT USED TO PRINT HERE IS GONE, and the list of
     * what was tried is the reason the driver no longer has an API for it.
     *
     * A cs/cv/cc/edge per-mille line, fed by 128 CS_DET samples taken at the top
     * of this command (256 ms on every `bat`). It decided nothing: measured
     * 2026-08-09, three consecutive reads with the board untouched between them,
     * cs 500, 484, 421 and edge 375, 500, 500. The spread inside ONE battery state
     * is wider than any difference between the two states it was meant to
     * separate -- the same way the edge-rate hunt died (CS_DET 227 vs 669 in one
     * state).
     *
     * A `rung' probe, which was a write rather than a sample: CHR_CON3 = 27 (CV
     * code 4137 mV) is fatal with nothing in the holder and shrugged off with a
     * cell in it. It works, once, and its failure mode is a dead board -- so it
     * cannot be on a boot path, and a test the firmware may not run is not a
     * presence API.
     *
     * BATON, whose `seen' mask was the honest form of the question: bit 0 set
     * means the pin has read 0 this session, bit 1 that it read 1, so seen=3 means
     * it MOVED, which no float and no stuck input can fake. It never reached 3 on
     * this board across every connector change tried, including with stock's own
     * arming sequence (CHR_CON7 bit 2, then bit 0, then read bit 12).
     *
     * What is left is the voltage, and the voltage cannot answer it: a full pack
     * reads 4177 mV and an empty holder 4178 mV, because there is no power-path
     * FET on this board and VBAT *is* VSYS. So presence is undecidable here, the
     * header offers no property for it, and this screen offers no bit that
     * pretends otherwise. See the top of mt6592_pmic.h.
     */

    /* The registers the driver's own reading is built from, printed raw. If the
     * numbers above are wrong, the answer is in these. This used to print 0x0754
     * as "rqst0", which is an address nothing on this PMIC listens to — the
     * decompile puts the request at 0x0758 and the strobe at 0x076e, so those
     * are what get printed now. A dump that names the wrong register is worse
     * than no dump: it reads as corroboration. */
    if (mt6592_pwrap_read(0x0758u, &v) == MT6592_PWRAP_OK) kv_hex("bat: rqst0(0758)=", v, 4u);
    if (mt6592_pwrap_read(0x076eu, &v) == MT6592_PWRAP_OK) kv_hex(" con(076e)=", v, 4u);
    if (mt6592_pwrap_read(0x0714u, &v) == MT6592_PWRAP_OK) kv_hex(" adc0(0714)=", v, 4u);
    put_char('\n');
}

/*
 * Charger input path, printed raw.
 *
 * The board has two USB receptacles and only one of them charges. That is a
 * question about wiring, and the register that answers it is CHR_CON0[5] —
 * CHRDET, the VCDT comparator sitting on the PMIC's CHRIN pin. It is a real
 * analogue comparator, not a software guess, so:
 *
 *   plug into the DC port and CHRDET goes 1  -> the port reaches CHRIN and the
 *      problem is downstream: the charger is detected but not armed, or armed
 *      into a current limit low enough to look like nothing.
 *   plug into the DC port and CHRDET stays 0 -> the port does not reach CHRIN
 *      at all. No amount of register writing charges from it, because the
 *      silicon never sees the supply. Either it is a data-only receptacle or
 *      its VBUS goes somewhere else on the board.
 *
 * CHRDET ALONE CANNOT ANSWER IT FROM THIS CONSOLE, which is a flaw in the first
 * version of this command and not a property of the board. The console is a USB
 * gadget: for any of this to be readable a host cable is already in one of the
 * two ports, so VBUS is present, so CHRDET reads 1 no matter which port is being
 * asked about. Three captures that all say CHRDET=1 say nothing.
 *
 * To get a real answer, hold the transport still and vary only the port under
 * test: keep the console cable in the OTG port for the whole session, then run
 * this with the DC port empty and again with a supply in the DC port. What
 * matters is whether ANY of these values move. The AUXADC data window is dumped
 * for the same reason -- one of those registers is VCHR, the charger input
 * voltage, and a second supply appearing on CHRIN has to move it. If nothing at
 * all changes when 5 V is applied to the DC port, that port's VBUS does not reach
 * the PMIC and the fault is upstream of every register here.
 *
 * The rest tells "input absent" apart from "input present but not charging":
 * CHR_EN and CSDAC_EN are the two enables, CON4 is the current-sense threshold,
 * CON13 is the watchdog that stops a charge about a second after it starts if
 * nobody kicks it, and CON7's RGS_BATON_UNDET is the cell-presence comparator (a
 * charger with no cell to charge draws nothing and looks equally dead).
 */
/* Stock's CV table lives with the `bat cal' machinery far below; this is the one
 * thing cmd_chg() needs out of it, so it borrows the decoder rather than a copy of
 * the table. A second copy of a 32-entry vendor table is a second copy that goes
 * stale. */
static int32_t batcal_cv_mv_of(uint32_t code);

static void cmd_chg(void) {
    static const struct { uint32_t addr; const char* name; } regs[] = {
        { 0x0000u, "CHR_CON0  " }, { 0x0004u, "CHR_CON2  " },
        { 0x0008u, "CHR_CON4  " }, { 0x000eu, "CHR_CON7  " },
        { 0x001au, "CHR_CON13 " }, { 0x002eu, "CHR_CON23 " },
    };
    uint32_t v;
    uint32_t i;

    for (i = 0u; i < (uint32_t)(sizeof(regs) / sizeof(regs[0])); ++i) {
        put_str("chg: ");
        put_str(regs[i].name);
        kv_hex(" ", regs[i].addr, 4u);
        if (mt6592_pwrap_read(regs[i].addr, &v) != MT6592_PWRAP_OK) {
            put_str(" = <pwrap error>\n");
            continue;
        }
        kv_hex(" = ", v, 4u);
        put_char('\n');
    }

    if (mt6592_pwrap_read(0x0000u, &v) == MT6592_PWRAP_OK) {
        put_str("chg: CHRDET=");
        put_dec((v & (1u << 5)) != 0u ? 1 : 0);
        put_str(" (VBUS on CHRIN -- PINNED AT 1 WHILE THIS CONSOLE CABLE IS IN)  CHR_EN=");
        put_dec((v & (1u << 4)) != 0u ? 1 : 0);
        put_str(" CSDAC_EN=");
        put_dec((v & (1u << 3)) != 0u ? 1 : 0);
        put_char('\n');
    }
    if (mt6592_pwrap_read(0x000eu, &v) == MT6592_PWRAP_OK) {
        put_str("chg: BATON_UNDET=");
        put_dec((v & (1u << 12)) != 0u ? 1 : 0);
        put_str(" (1 = no cell detected -- STUCK AT 0 on this board, both states)\n");
    }
    {
        uint32_t ones  = 0u;
        uint32_t edges = 0u;
        uint32_t taken = 0u;

        /* these two survive across calls of the enclosing function */
        static uint32_t prev_v = 0u;
        static int      first  = 1;

        put_str("chg: CON2 full-register change trace\n");

        if (mt6592_pwrap_read(0x0004u, &v) != MT6592_PWRAP_OK) {
            put_str("chg: read failed\n");
        } else {
            if (!first) {
                /* second and later calls → compare */
                uint32_t changed = prev_v ^ v;
                if (changed) {
                    unsigned b;
                    for (b = 0u; b < 32u; ++b) {
                        if (changed & (1u << b)) {
                            put_char('b');
                            put_char('i');
                            put_char('t');
                            put_char(' ');
                            if (b >= 10u) {
                                put_char((char)('0' + (b / 10u)));
                                put_char((char)('0' + (b % 10u)));
                            } else {
                                put_char((char)('0' + b));
                            }
                            put_char(' ');
                        }
                    }
                    put_char('\n');
                    ++edges;
                } else {
                    put_str("no bits changed\n");
                }
            } else {
                put_str("baseline stored\n");
            }

            ones  = __builtin_popcount(v);
            taken = 1u;

            /* always update the static state */
            prev_v = v;
            first  = 0;
        }

        kv_dec("... n=", (int32_t)taken);
        kv_dec(" ones=", (int32_t)ones);
        kv_dec(" edges=", (int32_t)edges);
        put_char('\n');
    }

    /* 0x142 is CHRSTATUS: bit 1 is PWRKEY_DEB and bit 2 HOMEKEY_DEB. Nothing to do
     * with the battery -- printed only so the constant 0x0056 stays on the record and
     * the BATON_RO dead end is not re-derived. See the enum in mt6592_pmic.c. */
    if (mt6592_pwrap_read(0x0142u, &v) == MT6592_PWRAP_OK) {
        kv_hex("chg: CHRSTATUS 0x0142 = ", v, 4u);
        put_str("  bit1=");
        put_dec((v & (1u << 1)) != 0u ? 1 : 0);
        put_str(" PWRKEY_DEB (NOT a battery bit)  bit5=");
        put_dec((v & (1u << 5)) != 0u ? 1 : 0);
        put_char('\n');
    }

    /*
     * The charger FSM's own account of itself, all seven comparators.
     *
     * CHRDET says a supply is on the pin; LDO_DET and the VCDT pair say whether
     * the FSM accepted it as chargeable; the CON2 trio says what the loop is
     * doing about it.
     *
     * CS_DET WAS ONCE READ AS PRESENCE -- a charger with no cell to charge moves
     * no current, whatever voltage sits on the node -- and it does not survive
     * measurement: three consecutive reads with the board untouched gave duties of
     * 500, 484 and 421 per mille in ONE state. It is printed as what the loop is
     * asserting, and nothing infers a battery from it.
     */
    {
        const uint32_t f = mt6592_pmic_charger_flags();

        if ((f & MT6592_PMIC_CHG_VALID) == 0u) {
            put_str("chg: comparators <pwrap error>\n");
        } else {
            put_str("chg: ldo=");
            put_dec((f & MT6592_PMIC_CHG_LDO_DET) ? 1 : 0);
            put_str(" chrdet=");
            put_dec((f & MT6592_PMIC_CHG_CHRDET) ? 1 : 0);
            put_str(" vcdt_lv=");
            put_dec((f & MT6592_PMIC_CHG_VCDT_LV_DET) ? 1 : 0);
            put_str(" vcdt_hv=");
            put_dec((f & MT6592_PMIC_CHG_VCDT_HV_DET) ? 1 : 0);
            put_str(" cs=");
            put_dec((f & MT6592_PMIC_CHG_CS_DET) ? 1 : 0);
            put_str(" cv=");
            put_dec((f & MT6592_PMIC_CHG_VBAT_CV_DET) ? 1 : 0);
            put_str(" cc=");
            put_dec((f & MT6592_PMIC_CHG_VBAT_CC_DET) ? 1 : 0);
            put_char('\n');
        }

        /*
         * THE CV TARGET, READ BACK OFF THE HARDWARE, AND THE ONE LINE THAT SAYS
         * WHETHER THIS BOARD CAN CHARGE AT ALL.
         *
         * "It never charges" was this register. CHR_CON3[4:0] powers on at 29 here,
         * which stock's own table calls 4162 mV, and the pack sits at 4183 -- so the
         * CV loop was satisfied before it started and the charger sourced nothing.
         * Nothing in the driver had ever written it. The charger service now asserts
         * code 0 (4200 mV) whenever a cable is in, and this is where to check that it
         * landed: a `cv target=' that is not 0/4200 with a cable attached and no sweep
         * running means the write is being ignored, and no amount of CSDAC or CS_VTH
         * tuning above will make a difference until it is not.
         */
        {
            const int cvc = mt6592_pmic_charger_cv_code();
            put_str("chg: cv target=");
            if (cvc < 0) {
                put_str("<pwrap error>");
            } else {
                put_dec((int32_t)cvc);
                kv_dec(" = ", batcal_cv_mv_of((uint32_t)cvc));
                put_str(" mV");
                if (cvc != 0) put_str("  (NOT the 4200 mV the service asserts)");
            }
            put_char('\n');
        }
    }

    /*
     * VCHR, channel 5, through the driver -- not a bit walk any more.
     *
     * This said "channel 8, 0x0722" for a long time, on the reading that stock
     * routes channel 8's rdy/out accessors to that address. It does -- but channel
     * 8 is not VCHR. battery_meter's cmd 7, the one whose whole job is the charger
     * voltage, is `mov r0,#5' at 0xc0513a6c, and channel 5's accessors (0xc0504be4
     * / 0xc0504c28) read 0x071a. Channel 8 reads 0xffff here forever, which is
     * where the permanent `chg_mv=1799 SATURATED' came from: a railed converter on
     * an input nothing drives.
     *
     * The CHR_CON16 mux is not part of this any more either. All four ADCIN_*_EN
     * setters have ZERO callers in the stock kernel; stock reads eight channels
     * without opening one, and this driver's arming write was the odd one out.
     *
     * BATSNS is printed beside it because the point of this command is the
     * DIFFERENCE: two supplies, two numbers, and if they track each other the
     * cell node is being held by the charger rather than measured.
     */
    {
        int chg_uv = 0, bat_uv = 0;
        const int have_chg = (mt6592_pmic_sample_blocking(MT6592_PSY_INPUT_VOLTAGE_NOW,
                                                          &chg_uv) == 0);
        const int have_bat = (mt6592_pmic_sample_blocking(MT6592_PSY_VOLTAGE_NOW,
                                                          &bat_uv) == 0);
        const int bat_raw = mt6592_pmic_vbat_sample_raw();

        put_str("chg: VCHR(071a) ch5 mv=");
        if (have_chg) put_dec(chg_uv / 1000); else put_str("<not ready>");
        put_str("   BATSNS(0714) mv=");
        if (have_bat) put_dec(bat_uv / 1000); else put_str("<not ready>");
        kv_dec(" raw=", bat_raw);
        put_char('\n');
        put_str("chg: VCHR mv is CABLE millivolts: pin = raw*1800>>15, cable = pin\n");
        put_str("chg: * 369/39 (R_CHARGER_1 330 + R_CHARGER_2 39, both cust-data).\n");
        put_str("chg: That is stock's own two-step, so this is calibrated, not scaled\n");
        put_str("chg: by a placeholder. Full scale at the cable is 17046 mV.\n");
        /* NO VCHR RAW COLUMN ANY MORE. It was there to prove the channel moves at
         * all, back when the address was in doubt; the millivolts are a strictly
         * increasing function of that count, so they prove exactly the same thing
         * and the driver no longer has to publish a second, unscaled accessor to
         * say it. BATSNS keeps its count because the sense pair above is quoted in
         * counts and this is the number to check it against. */
    }

    put_str("chg: keep the console cable in OTG the whole time; vary only DC.\n");
    put_str("chg: run once with DC empty, once with DC fed. If the VCHR raw count\n");
    put_str("chg: does not move, the DC port's VBUS never reaches the PMIC.\n");
}

/*
 * The one measurement this whole console was built to get: which AUXADC request
 * bit actually converts BATSNS, and what the conversion scales to.
 *
 * The register addresses are no longer guesses. The stock LK's generated MT6323
 * accessors are in the decompile, and they fix two of the three unknowns:
 * 0x0714 is a data register with [15] ready and [14:0] value (FUN_81e08730 /
 * FUN_81e0875c read exactly those fields), its siblings run 0x0716..0x0722 on a
 * 2-byte stride, and the only write anywhere in 0x0750..0x075f is a single-bit
 * field write to 0x0758 -- so 0x0758 is RQST0. mt6592_pmic.c has been corrected
 * to match.
 *
 * The third unknown is now answered too, and by the same decompile rather than
 * by this scan: FUN_81e07f8c sets 0x0758 bit 4 and then strobes 0x076e bit 7
 * low and high, and FUN_81e07d78 is a shift-by-`shift` field write, so bit 4 is
 * the start and the strobe is what launches the conversion. This scan predates
 * that reading and did not strobe at all, which is why it never found a ready
 * bit to point at -- with no edge on 0x076e, every one of the eight rows was
 * going to come back not-ready regardless of which one was correct.
 *
 * AND THE THIRD UNKNOWN WAS ANSWERED AGAIN, DIFFERENTLY, AND THAT IS WHY THIS
 * COMMAND CHANGED. The paragraph above has 0x0758 as the request register and
 * 0x076e bit 7 as a general strobe. Both halves of that are wrong, and VCHR is
 * the proof: the charger voltage converts on 0x076e bit EIGHT, into 0x0722, and
 * stock's PMIC_IMM_GetOneChannelValue builds its edge as `lsl r6, r6, r5` with
 * r5 the channel number -- one request bit per channel, strobed low then high on
 * the channel's OWN bit. Bit 7 is channel 7's request, not a strobe. 0x0758
 * bit 4 is RG_VBUF_EN, the input buffer, which stock sets once per conversion
 * regardless of channel.
 *
 * So the previous walk asked eight questions on a register that answers none of
 * them, and its column of zeros -- read at the time as "this part only converts
 * BATSNS, there is no current-sense channel, there is nothing to integrate" --
 * was a null result about the wrong register. That conclusion is load-bearing:
 * it is the reason battery presence was left to a comparator duty that measures
 * at 437-718 permille whether or not a cell is fitted. It has to be retested
 * before it is believed, and this is the retest.
 *
 * Each channel is now requested on its own bit, strobed the way stock strobes
 * it, and every data register is printed with the ready bit broken out and the
 * value converted at both plausible widths. Channel 8 is the control: it is
 * known to work, so a run where 8 reads RDY and the others do not is a real
 * measurement rather than a broken sequence.
 *
 * WHAT TO DO WITH IT: run it with the battery fitted and again with it out. Any
 * channel whose value MOVES between the two is a presence signal, and if the one
 * that moves is the current-sense pair (VBAT against ISENSE across the sense
 * resistor) it is also the charge-current measurement this driver has never had.
 */
static void cmd_batscan(void) {
    enum {
        N_DATA = 8u, N_CHANNELS = 9u, VBUF = 0x0758u, VBUF_EN = 0x0010u,
        DATA0 = 0x0714u, ADC_CON = 0x076eu, ADC_CON_FIELD = 0x01ffu,
        /* bits 8,10,11,12. This constant was written as 0x1500, which is bits
         * 8, 10 and 12 -- VSEN_EN, bit 11, was missing, and it is the one this
         * whole command exists to open. The two runs done with it wrong are
         * still useful: they are the control for what a DISCONNECTED input
         * reads, and the answer is "a plausible voltage 162 counts away from
         * BATSNS", which is exactly what an open input does and exactly what
         * this file warns about everywhere else. */
        CHR_CON16 = 0x0020u, CHR_CON16_MUXES = 0x1d00u
    };
    uint32_t bit;
    uint32_t v;

    /* A gated AUXADC never converts no matter what is requested, and a silent
     * non-conversion looks exactly like a wrong request bit. Print the clock
     * gates first so that possibility can be ruled in or out. */
    put_str("batscan: PMIC clock gates (a gated AUXADC cannot convert)\n");
    dump_pmic_range(PMIC_TOP_CKPDN0, PMIC_TOP_CKPDN0 + 4u, 0);
    dump_pmic_range(PMIC_TOP_CKPDN2, PMIC_TOP_CKPDN2 + 2u, 0);
    put_str("batscan: AUXADC control window 0750..075e\n");
    dump_pmic_range(0x0750u, 0x075eu, 0);

    /*
     * Open every AUXADC input mux before walking anything. This is the other
     * half of the VCHR lesson: channel 8 was requested correctly for a long time
     * and still read zero, because CHR_CON16 bit 12 left the converter looking at
     * a disconnected pin. A channel walk run against closed muxes would repeat
     * that mistake eight more times and produce another column of zeros to draw
     * another wrong conclusion from.
     *
     * The four bits are stock's own, one pmic_config_interface(0x20,v,1,shift)
     * each: 8 VSEN_MUX_EN, 10 VBAT_EN, 11 VSEN_EN, 12 VCHR_EN. All four are high
     * impedance measurement paths and stock sets them every boot.
     *
     * VSEN is the one to watch. A "voltage sense" input distinct from VBAT, on a
     * charger block, is the sense-resistor tap -- and VBAT minus VSEN across a
     * known milliohm shunt is charge current, which is the number the user says
     * this board has never produced.
     */
    if (mt6592_pwrap_read(CHR_CON16, &v) == MT6592_PWRAP_OK) {
        kv_hex("batscan: CHR_CON16 before=", v, 4u);
        (void)mt6592_pwrap_write(CHR_CON16, v | (uint32_t)CHR_CON16_MUXES);
        if (mt6592_pwrap_read(CHR_CON16, &v) == MT6592_PWRAP_OK) {
            kv_hex(" after=", v, 4u);
        }
        put_str(" (muxes 8/10/11/12 = VSEN_MUX/VBAT/VSEN/VCHR)\n");
    }

    for (bit = 0u; bit < N_CHANNELS; ++bit) {
        uint32_t i;
        put_str("\nbatscan: AUXADC_CON(076e) channel ");
        put_dec((int32_t)bit);
        if (bit == 8u) put_str("  <- VCHR, the known-good control");
        put_char('\n');

        /* RG_VBUF_EN, the input buffer. Stock sets it once per conversion and it
         * is not per-channel, so it is set here rather than inside the edge. */
        if (mt6592_pwrap_read(VBUF, &v) == MT6592_PWRAP_OK) {
            (void)mt6592_pwrap_write(VBUF, v | (uint32_t)VBUF_EN);
        }

        /* The conversion is launched by a low-then-high edge on THIS channel's
         * own bit of 0x076e -- stock's `lsl r6, r6, r5` with r5 the channel.
         * Read-modify-write both halves so the other eight channels' request
         * bits are left exactly as they were. */
        if (mt6592_pwrap_read(ADC_CON, &v) != MT6592_PWRAP_OK) {
            put_str("  <pwrap read failed>\n");
            continue;
        }
        v &= ADC_CON_FIELD;
        if (mt6592_pwrap_write(ADC_CON, v & ~(1u << bit)) != MT6592_PWRAP_OK) {
            put_str("  <pwrap write failed>\n");
            continue;
        }
        if (mt6592_pwrap_read(ADC_CON, &v) != MT6592_PWRAP_OK) {
            put_str("  <pwrap read failed>\n");
            continue;
        }
        v &= ADC_CON_FIELD;
        (void)mt6592_pwrap_write(ADC_CON, v | (1u << bit));
        delay_ms(20u);
        for (i = 0u; i < N_DATA; ++i) {
            uint32_t addr = DATA0 + i * 2u;
            uint32_t raw12;
            uint32_t raw15;
            if (mt6592_pwrap_read(addr, &v) != MT6592_PWRAP_OK) {
                kv_hex("  ", addr, 4u);
                put_str(" = <pwrap error>\n");
                continue;
            }
            /* Only the rows that answered are worth reading. A screen of
             * "0x0716 = 0x0000 --- mv12=0 mv15=0" nine times over is what
             * produced the wrong conclusion the first time; suppressing it
             * makes a channel that DOES convert impossible to miss. */
            if (v == 0u) continue;
            kv_hex("  ", addr, 4u);
            kv_hex(" = ", v, 4u);
            put_str((v & 0x8000u) != 0u ? " RDY " : " --- ");
            raw12 = v & 0x0fffu;
            raw15 = v & 0x7fffu;
            /* Two full scales, because the two known channels do not share one:
             * BATSNS is 1.8 V x 4 = 7200 mV through the on-die divider, VCHR is
             * 1.8 V at the pin. Both columns are printed for every row so the
             * scale is chosen from the number rather than assumed. */
            kv_dec("mv15/7200=", (int32_t)((raw15 * 7200u) >> 15));
            kv_dec(" mv15/1800=", (int32_t)((raw15 * 1800u) >> 15));
            kv_dec(" mv12/7200=", (int32_t)((raw12 * 7200u) >> 12));
            put_char('\n');
        }
    }
    put_str("\nbatscan: zero rows are suppressed; every row printed is a channel\n");
    put_str("batscan: that answered. The map this walk found is now confirmed against\n");
    put_str("batscan: the stock kernel's own dispatch tables: ch5 VCHR 0x071a (x1800),\n");
    put_str("batscan: ch6 I_SENSE 0x0716 (x7200), ch7 BAT_SENSE 0x0714 (x7200), ch8\n");
    put_str("batscan: 0x0722 (x1800) -- and ch8 reads 0xffff on this board, railed, on\n");
    put_str("batscan: an input nothing drives. It is not the charger.\n");
    put_str("batscan: RUN IT TWICE, cell fitted and cell removed. A row that reads\n");
    put_str("batscan: 3300..4200 on the 7200 scale only when the cell is in is the\n");
    put_str("batscan: presence signal BATON never gave. A row that tracks VBAT with\n");
    put_str("batscan: a small offset is VSEN, and VBAT-VSEN is the charge current.\n");
}

/* One AUXADC conversion. Same sequence the OS-side joystick poller uses: clear
 * then set the channel bit in CON1, wait for CON2 to go idle, read DATn. Returns
 * the raw DATn word, READY bit included. */
static uint32_t soc_adc_read(uint32_t ch) {
    uint32_t spins = 0u;
    mtk_write32(SOC_AUXADC_BASE + SOC_AUXADC_CON1_CLR, 1u << ch);
    mtk_write32(SOC_AUXADC_BASE + SOC_AUXADC_CON1_SET, 1u << ch);
    while ((mtk_read32(SOC_AUXADC_BASE + SOC_AUXADC_CON2) & 1u) != 0u && spins < 20000u) ++spins;
    mt6592_delay_cycles(850u);
    return mtk_read32(SOC_AUXADC_BASE + SOC_AUXADC_DAT0 + ch * 4u);
}

static void cmd_soc_adc(const char* args) {
    uint32_t only;
    int have_only = parse_u32(&args, &only) == 0;
    uint32_t ch;

    for (ch = 0u; ch < 16u; ++ch) {
        uint32_t dat;
        if (have_only != 0 && ch != only) continue;
        dat = soc_adc_read(ch);
        kv_dec("socadc ch", (int32_t)ch);
        kv_hex(" raw=", dat, 4u);
        kv_dec(" val=", (int32_t)(dat & SOC_AUXADC_DAT_MASK));
        put_str((dat & SOC_AUXADC_DAT_READY) != 0u ? " ready" : " NOTREADY");
        put_char('\n');
    }
}

/* Both defined further down, next to the keypad commands that are their main
 * users; keyhunt lives up here with the other ADC/PMIC readers. */
static void dump_kpd(void);
static void read_kpd_mems(uint32_t* mems);

static void cmd_pmicr(const char* args) {
    uint32_t a;
    uint32_t n;
    if (parse_u32(&args, &a) != 0) {
        put_str("usage: pmicr <addr> [count]\n");
        return;
    }
    n = parse_u32_default(&args, 1u);
    if (n > 128u) n = 128u;
    dump_pmic_range(a, a + (n - 1u) * 2u, 0);
}

static void cmd_pmicw(const char* args) {
    uint32_t a;
    uint32_t v;
    uint32_t rb;
    if (parse_u32(&args, &a) != 0 || parse_u32(&args, &v) != 0) {
        put_str("usage: pmicw <addr> <value>\n");
        return;
    }
    kv_hex("pmicw ", a, 4u);
    kv_hex(" <= ", v, 4u);
    if (mt6592_pwrap_write(a, v) != MT6592_PWRAP_OK) {
        put_str("  FAILED\n");
        return;
    }
    /* Read back: on a PMIC, "the write returned OK" and "the bit stuck" are
     * different facts, and the interesting registers are full of bits that do
     * not stick. */
    if (mt6592_pwrap_read(a, &rb) == MT6592_PWRAP_OK) kv_hex("  readback=", rb, 4u);
    put_char('\n');
}

static void cmd_peek(const char* args) {
    uint32_t a;
    uint32_t n;
    uint32_t i;
    if (parse_u32(&args, &a) != 0) {
        put_str("usage: peek <addr> [words]\n");
        return;
    }
    n = parse_u32_default(&args, 1u);
    /* Same ceiling as `wifi dump`. Once WIFI_START hands the connectivity core
     * to the WLAN firmware the boot ROM's ACCESS_REG channel is gone, so `peek`
     * is the only remaining way to read what the firmware is doing -- and the
     * part of it that the AP can reach is the EMI window (CONSYS 0xf0000000 ==
     * AP 0x83100000, 1 MiB), where sections 2 and 3 live. 64 words at a time is
     * not a usable window on 0x2ebc0 bytes of code. */
    if (n > 0x800u) n = 0x800u;
    a &= ~3u;
    for (i = 0u; i < n; ++i) {
        if ((i & 3u) == 0u) {
            if (i != 0u) put_char('\n');
            kv_hex("  ", a + i * 4u, 8u);
            put_char(':');
        }
        kv_hex(" ", mtk_read32(a + i * 4u), 8u);
    }
    put_char('\n');
}

/*
 * Scan AP-readable memory for printable ASCII and print only what it finds.
 *
 * This exists to read the connectivity firmware's own log. That firmware has a
 * printf -- 0xf0044020 in section 2, reached from every one of the 2104 string
 * references in section 3 -- and it prints the one line that would end this
 * investigation outright: "Ready bit assert!" at wifi/mgmt/hem.c, immediately
 * after it sets the bit the AP is waiting 5.13 s for and never sees. Everything
 * it logs before giving up is a description of why, written by the people who
 * wrote the failure.
 *
 * The catch is that the firmware's log has no route to us. Stock collects it as
 * HIF event packets, which requires the firmware to be ready, which is the thing
 * that is not happening. What we can reach is EMI: CONSYS 0xf0000000 == AP
 * 0x83100000, one mebibyte, and unlike the boot ROM's ACCESS_REG channel it
 * survives WIFI_START. If any of that text is buffered in EMI it is already
 * sitting there after a failed `wifi go`.
 *
 * Sweeping a mebibyte with `peek` is 128 commands of 0x800 words and a wall of
 * hex to read by eye. This is the same sweep with the filter moved onto the
 * device: it reports hits, not contents. A negative result is worth as much as a
 * positive one -- it says the log is not in EMI and rules the approach out in one
 * command instead of a hundred and twenty-eight.
 */
static void cmd_strings(const char* args) {
    uint32_t a;
    uint32_t n;
    uint32_t minlen;
    uint32_t i;
    uint32_t hits = 0u;
    uint32_t run_start = 0u;
    uint32_t run_len = 0u;
    uint32_t maxhits;
    /*
     * Non-zero once a run has been flushed because it filled the buffer, so the
     * remainder is printed even if it is shorter than minlen. Without it the
     * tail of a long run is silently dropped, and this command's first real
     * result was exactly that: the firmware's ASSERT line came out as
     * "... #4209 - H, 0x0," with the rest -- "0x0, 0x0, rc=*" -- missing, which
     * had to be recovered by hand out of a second copy with `peek`.
     */
    int continued = 0;
    char run[97];
    if (parse_u32(&args, &a) != 0) {
        put_str("usage: strings <addr> [bytes] [minlen] [maxhits]   (default 100000, 8, 200)\n");
        return;
    }
    n = parse_u32_default(&args, 0x100000u);
    minlen = parse_u32_default(&args, 8u);
    maxhits = parse_u32_default(&args, 200u);
    if (minlen < 4u) minlen = 4u;
    if (minlen > 64u) minlen = 64u;
    a &= ~3u;
    n &= ~3u;

    kv_hex("strings ", a, 8u);
    kv_hex("..", a + n, 8u);
    kv_dec(" min=", (int32_t)minlen);
    put_char('\n');

    for (i = 0u; i < n; i += 4u) {
        const uint32_t w = mtk_read32(a + i);
        uint32_t b;
        /* Data on this part is little-endian, so byte k of the word is bits
         * 8k..8k+7 -- the same unpacking /tmp/mkdump.py does on a `peek` log. */
        for (b = 0u; b < 4u; ++b) {
            const uint8_t c = (uint8_t)((w >> (b * 8u)) & 0xffu);
            if (c == 0x09u || c == 0x0au || (c >= 0x20u && c < 0x7fu)) {
                if (run_len == 0u) run_start = a + i + b;
                /* Newlines inside a run would break the one-hit-per-line shape,
                 * and the firmware's format strings all end in one. */
                run[run_len] = (c == 0x0au || c == 0x09u) ? ' ' : (char)c;
                ++run_len;
                if (run_len == (uint32_t)(sizeof run - 1u)) {
                    /* Full: print what we have and carry the same run onto the
                     * next line under its own address, rather than discarding
                     * everything past here. */
                    run[run_len] = '\0';
                    kv_hex("  ", run_start, 8u);
                    put_str("  ");
                    put_str(run);
                    put_char('\n');
                    ++hits;
                    if (maxhits != 0u && hits >= maxhits) {
                        kv_dec("strings: stopped at ", (int32_t)hits);
                        kv_hex(" hits, resume from ", a + i, 8u);
                        put_str(" (pass a 4th argument to raise the cap, 0 for none)\n");
                        return;
                    }
                    run_len = 0u;
                    continued = 1;
                }
            } else {
                if (run_len >= minlen || (continued && run_len != 0u)) {
                    run[run_len] = '\0';
                    kv_hex("  ", run_start, 8u);
                    put_str("  ");
                    put_str(run);
                    put_char('\n');
                    ++hits;
                    if (maxhits != 0u && hits >= maxhits) {
                        kv_dec("strings: stopped at ", (int32_t)hits);
                        kv_hex(" hits, resume from ", a + i, 8u);
                        put_str(" (pass a 4th argument to raise the cap, 0 for none)\n");
                        return;
                    }
                }
                run_len = 0u;
                continued = 0;
            }
        }
    }
    if (run_len >= minlen || (continued && run_len != 0u)) {
        run[run_len] = '\0';
        kv_hex("  ", run_start, 8u);
        put_str("  ");
        put_str(run);
        put_char('\n');
        ++hits;
    }
    kv_dec("strings: ", (int32_t)hits);
    put_str(" hit(s)\n");
}

static void cmd_poke(const char* args) {
    uint32_t a;
    uint32_t v;
    if (parse_u32(&args, &a) != 0 || parse_u32(&args, &v) != 0) {
        put_str("usage: poke <addr> <value>\n");
        return;
    }
    a &= ~3u;
    kv_hex("poke ", a, 8u);
    kv_hex(" <= ", v, 8u);
    mtk_write32(a, v);
    kv_hex("  readback=", mtk_read32(a), 8u);
    put_char('\n');
}

/*
 * `peek` caps at 64 words because that is all a line-oriented console can show,
 * which makes it useless for the question "did anything in this megabyte change".
 * The connectivity SRAM window is 1 MiB (AP 0x18000000 == CONSYS 0x02000000), and
 * the only way to tell a firmware that never ran from one that ran and died is to
 * fingerprint that window either side of WIFI_START and compare.
 *
 * One rotate-xor word per page, so an untouched page reads 00000000 and a page
 * that gained even a single bit cannot read 00000000. Diffing two sweeps by eye
 * then localises the change to 4 KiB, which `peek` can finish off.
 */
static void cmd_sweep(const char* args) {
    uint32_t base;
    uint32_t pages;
    uint32_t page_words;
    uint32_t p;
    uint32_t live = 0u;
    uint32_t unsafe;

    if (parse_u32(&args, &base) != 0) {
        put_str("usage: sweep <addr> [pages] [page-bytes] [1=allow the unsafe hole]\n");
        return;
    }
    pages = parse_u32_default(&args, 0x40u);
    page_words = parse_u32_default(&args, 0x1000u) >> 2;
    unsafe = parse_u32_default(&args, 0u);
    if (page_words == 0u) page_words = 1u;
    base &= ~3u;

    for (p = 0u; p < pages; ++p) {
        uint32_t sum = 0u;
        uint32_t i;
        const uint32_t page = base + p * page_words * 4u;

        /*
         * MMU and caches are off and there is no data-abort handler, so a read
         * of an address the bus does not decode does not fault -- it hangs the
         * AHB and takes the whole board with it. Measured: `sweep 18000000 100`
         * died silently somewhere in 0x180e8000..0x180effff, and the console
         * never came back. The HIF page at 0x180f0000 reads fine, so the hole is
         * between. Refuse it by default; `sweep <a> <pages> <sz> 1` overrides.
         */
        if (page >= 0x180e0000u && page < 0x180f0000u && !unsafe) {
            put_str("\n  0x180e0000..0x180effff SKIPPED: this range hung the board once.\n");
            put_str("  Pass a 4th argument of 1 to sweep it anyway.\n");
            base = page + 0x10000u;
            p = 0u;
            pages = pages > 0x10u ? pages - 0x10u : 0u;
            continue;
        }

        if ((p & 7u) == 0u) {
            if (p != 0u) put_char('\n');
            kv_hex("  ", page, 8u);
            put_char(':');
            /* Flush the address BEFORE the reads, twice: if the next read hangs
             * the board, this line is the only thing that will ever name where. */
            mt6592_usb_gadget_poll();
            mt6592_usb_gadget_poll();
        }
        for (i = 0u; i < page_words; ++i) {
            const uint32_t w = mtk_read32(page + i * 4u);
            sum = ((sum << 1) | (sum >> 31)) ^ w;
        }
        if (sum != 0u) ++live;
        kv_hex(" ", sum, 8u);
    }
    put_char('\n');
    kv_dec("sweep: ", (int32_t)live);
    kv_dec(" non-empty of ", (int32_t)pages);
    put_str(" pages\n");
}

/*
 * Search a window for a 32-bit word. The other half of `wifi mark`.
 *
 * `wifi mark` asks the WLAN boot ROM to write seed, seed+1, seed+2, ... at a
 * destination of our choosing; this finds where it actually landed. Together
 * they measured the AP<->CONSYS mapping instead of extrapolating it, and the
 * answer is AP = CONSYS + 0x17E90000 -- confirmed three ways: a marker write we
 * chose (CONSYS 0x001f0000 -> AP 0x18080000), an RX descriptor's own self
 * pointer (CONSYS 0x0020f100 read at AP 0x1809f100), and the ring layout. The
 * older 0x0209f800 -> 0x1809f800 guess is refuted; that address is ring slot
 * 15's staging buffer, not a landing zone.
 *
 * They also answered the question underneath every remaining hypothesis -- does
 * a status==0 ACK mean the copy happened? It does. Sections 2 and 3 read back
 * decrypted in DRAM at AP 0x83120000 and 0x83163000 (CONSYS 0xf0000000 == AP
 * 0x83100000, the EMI window).
 */
static void cmd_find(const char* args) {
    uint32_t needle;
    uint32_t base;
    uint32_t bytes;
    uint32_t i;
    uint32_t hits = 0u;

    if (parse_u32(&args, &needle) != 0) {
        put_str("usage: find <word> [base] [bytes]   (defaults 0x18000000, 0xe0000)\n");
        return;
    }
    base  = parse_u32_default(&args, 0x18000000u) & ~3u;
    bytes = parse_u32_default(&args, 0x000e0000u);

    /* Same hole as `sweep`: 0x180e0000..0x180effff hangs the AHB. */
    if (base < 0x180f0000u && base + bytes > 0x180e0000u && base + bytes <= 0x18100000u) {
        bytes = 0x180e0000u - base;
        put_str("find: window clamped at 0x180e0000 (that range hung the board once)\n");
    }

    for (i = 0u; i < bytes / 4u; ++i) {
        const uint32_t at = base + i * 4u;
        if (mtk_read32(at) == needle) {
            if (hits == 0u) put_str("find:");
            if (hits < 16u) {
                kv_hex(" ", at, 8u);
            }
            ++hits;
        }
        if ((i & 0x3fffu) == 0u) mt6592_usb_gadget_poll();
    }
    if (hits == 0u) {
        kv_hex("find: ", needle, 8u);
        kv_hex(" not present in ", base, 8u);
        kv_hex("..", base + bytes - 4u, 8u);
        put_char('\n');
        return;
    }
    put_char('\n');
    kv_dec("find: ", (int32_t)hits);
    put_str(" hit(s)\n");
}

/*
 * The LED evidence, laid out so the "all three are lit and nothing I write
 * changes that" report can be settled in one look.
 *
 * mt6592_led_init() clears the enable bits at 0x0356 and the operator still sees
 * three lit LEDs. Exactly one of three things is true: 0x0356 is not the enable
 * register, the ISINK block is not what drives these LEDs, or the writes are not
 * landing. This dump distinguishes all three -- it prints the clock gates (a
 * gated ISINK cannot respond), the whole 0x0330..0x0358 block, and the DRV/GPIO
 * registers MT6323 uses when a "LED" is really a general-purpose output.
 *
 * ANSWER: none of the three. The LEDs are not on this PMIC at all -- they are SoC
 * pads 47/48/49, active low, found with `pin`. `ledscan` below drives those. This
 * dump stays because "is it the PMIC?" is a question that will be asked again
 * about the next unexplained output, and because it is the artefact that closed
 * this one.
 */
static void cmd_isink(void) {
    put_str("isink: clock gates\n");
    dump_pmic_range(PMIC_TOP_CKPDN0, PMIC_TOP_CKPDN0 + 2u, 0);
    dump_pmic_range(PMIC_TOP_CKPDN2, PMIC_TOP_CKPDN2 + 2u, 0);
    put_str("isink: ISINK block 0330..0358\n");
    dump_pmic_range(PMIC_ISINK_BASE, PMIC_ISINK_EN_CTRL + 2u, 0);
    put_str("isink: DRV/output block 0400..0440 (if the LEDs are GPIO, they are here)\n");
    dump_pmic_range(0x0400u, 0x0440u, 0);
}

static void cmd_led(const char* args) {
    uint32_t r = parse_u32_default(&args, 0u);
    uint32_t g = parse_u32_default(&args, 0u);
    uint32_t b = parse_u32_default(&args, 0u);
    int rc = mt6592_led_set_channels(r, g, b);
    kv_dec("led: r=", (int32_t)r);
    kv_dec(" g=", (int32_t)g);
    kv_dec(" b=", (int32_t)b);
    kv_dec(" rc=", rc);
    put_char('\n');
}

static void dump_kpd(void) {
    uint32_t i;
    kv_hex("kpd: sta=", mtk_read32(KPD_BASE + 0x00u), 4u);
    kv_hex(" deb=", mtk_read32(KPD_BASE + 0x18u), 4u);
    kv_hex(" scan=", mtk_read32(KPD_BASE + 0x1cu), 4u);
    kv_hex(" sel=", mtk_read32(KPD_BASE + 0x20u), 4u);
    kv_hex(" en=", mtk_read32(KPD_BASE + 0x24u), 4u);
    put_char('\n');
    put_str("kpd: mem");
    for (i = 0u; i < 5u; ++i) {
        put_char(' ');
        put_hex(mtk_read32(KPD_BASE + 0x04u + i * 4u) & 0xffffu, 4u);
    }
    put_char('\n');
}

static void read_kpd_mems(uint32_t* mems) {
    uint32_t i;
    for (i = 0u; i < 5u; ++i) {
        mems[i] = mtk_read32(KPD_BASE + 0x04u + i * 4u) & (i == 4u ? 0x00ffu : 0xffffu);
    }
}

/*
 * The tool for "only the right click works".
 *
 * MVII maps buttons by matrix bit, and a button that reports nothing is either
 * on a bit the map does not know about or on a row/column the keypad is not
 * scanning. Watching the raw scan memories while the operator presses each
 * button in turn produces the mapping directly, with no interpretation in
 * between -- press, read the bit number, and the map is either right or it is
 * not.
 */
enum {
    KPDMON_EVENT_LIMIT = 400u,
};

/* kpdmon_run() lives further down, next to the GPIO helpers and the sampling
 * code it shares with kpdmap. It watches both the matrix and the GPIOs. */

/*
 * Guided button map: one button at a time, cued on the panel.
 *
 * The free-running sweep above got the right idea and the wrong filter. Its
 * per-pin event budget could not tell a chattering line from a button pressed
 * repeatedly -- pressing A three times spends exactly six edges, which is the
 * whole budget, so the one measurement being asked for got labelled noise and
 * muted. Worse, with a dozen buttons and 208 pins all reported into one flat
 * stream, nothing says which press produced which edge.
 *
 * This inverts both problems. Noise is rejected by *time*, not by count: a
 * level has to hold for MAP_STABLE_TICKS before it counts, so a pin tracking a
 * clock or a scan strobe never qualifies no matter how often it flips, while a
 * real switch closure qualifies immediately and can be pressed as many times as
 * you like. And attribution is solved by only ever watching one button at a
 * time, with a settling pass first that pins down which lines are quiet at rest.
 *
 * The cue has to be the panel, because during this the USB console has not
 * attached yet -- that is the whole reason this runs unprompted at entry. UART
 * and the eMMC ring both record the prompts, but neither is something an
 * operator can watch live, so the screen is the only channel that can say "now".
 * Green means press and hold the next button on the list, blue means let go.
 */
/* Order matters: the four known-good buttons come first so a run that reports
 * nothing for them is a broken run, not a board with unwired buttons. The six
 * that report nothing on hardware -- START, SELECT, MENU, VOL+, VOL-, R2 -- come
 * next, because they are what this exists to find. VOL+/VOL- were missing from
 * this list entirely, which is its own small answer to why they were never
 * found. */
static const char* const kMapButtons[] = {
    "B (known-good control)",
    "X (known-good control)",
    "L1 (known-good control)",
    "DPAD UP (known-good control, expect a GPIO)",
    "START",
    "SELECT",
    "MENU",
    "VOL UP",
    "VOL DOWN",
    "R2 right trigger",
    "A",
    "Y",
    "L2",
    "R1",
    "DPAD DOWN",
    "DPAD LEFT",
    "DPAD RIGHT",
    "POWER",
    0,
};

enum {
    /* The GPIO half stays here even though kpdmon dropped it. kpdmap only runs
     * when USB never attached, which is precisely the case where there is no
     * second chance to go looking, and its time-stability filter rejects the
     * free-running pins that defeated kpdmon's event budget. Cheap insurance in
     * the one path that cannot be re-run on demand. */
    MAP_GPIO_BANKS = 13u,           /* GPIO0..207, the whole register space */
    MAP_PINS = MAP_GPIO_BANKS * 16u,
    /* ...but the whole register space is wider than the pad space. Both the
     * preloader's mt_set_gpio_mode (0x4d90 in
     * Reference/J36-ULTRA/preloader_sf6592_wet_l.bin) and stock LK's own copy
     * (FUN_81e12e7c) reject anything above 0xa8 before touching a register, so
     * 168 is the last pad the hardware decodes. Reading past it is harmless, and
     * the sweeps still cover the full banks because a phantom pin that never
     * moves costs nothing; WRITING past it is not harmless -- pin 169 and up
     * still land in a real mode word, just not the one the number implies. Hence
     * every command that configures a pad bounds on this and not on MAP_PINS. */
    MAP_PAD_MAX = 168u,
    MAP_BITS = 80u,              /* 5 x 16 matrix scan slots */
    MAP_SLOTS = MAP_PINS + MAP_BITS,
    MAP_STABLE_TICKS = 5u,   /* 50 ms at a 10 ms tick */
    MAP_SETTLE_TICKS = 150u, /* 1.5 s of "what is quiet at rest?" */
    MAP_PRESS_TICKS = 200u,  /* 2.0 s to press and hold */
    MAP_RELEASE_TICKS = 100u,
    MAP_COLOUR_PRESS = 0xff00c000u,
    MAP_COLOUR_RELEASE = 0xff101060u,
    /* The controller's scan-memory geometry: 8 rows of 9 columns, flat bit index
     * row*9 + col, which is the same index the stock kernel's kpd_keymap[72] is
     * subscripted by (its kpd.c computes hw_keycode = word*16 + bit and looks it up
     * directly). Confirmed live: pressing one button in an unmuxed row moved bits
     * 2, 11, 20, 29, 38, 47, 56 and 65 together -- eight bits at stride 9, which is
     * one column across all eight rows. */
    KPD_MATRIX_COLS = 9u,
    /* Rows the vendor keymap defines: 0 (shoulders), 1 (face), 2 (unused on this
     * board -- its pad, 93, is DPAD_UP's EINT), 3 (volume/select/start/menu). The
     * engine scans all 8 rows regardless -- the rows 4..7 bits in a live capture prove
     * it -- so a bit at or above this row is never a key read. This used to say 2,
     * from mistaking mtk_kpd_gpio_set's fixup list for a census of the board's rows. */
    KPD_STROBED_ROWS = 4u,
};

static const mvii_debug_console_hooks_t* g_hooks;

static uint8_t g_map_now[MAP_SLOTS];
static uint8_t g_map_base[MAP_SLOTS];
static uint8_t g_map_level[MAP_SLOTS];
static uint8_t g_map_hold[MAP_SLOTS];
static uint8_t g_map_hit[MAP_SLOTS];
static uint8_t g_map_unstable[MAP_SLOTS];

/*
 * GPIO config window. DIR at 0x000, DIN at 0x500 and MODE at 0x600 are already
 * proven on this board (the OS keypad driver, mt6592_msdc.c); PULLEN at 0x100 and
 * PULLSEL at 0x200 are the standard MTK 0x100 spacing that completes that set.
 * Every register is a 16-pin bank with write-1-to-SET at +4 and write-1-to-RST
 * at +8, except MODE, which packs five 3-bit fields per bank.
 */
enum {
    GPIOC_BASE = 0x10005000u,
    GPIOC_DIR = 0x0000u,
    GPIOC_PULLEN = 0x0100u,
    GPIOC_PULLSEL = 0x0200u,
    GPIOC_DOUT = 0x0400u,
    GPIOC_DIN = 0x0500u,
    GPIOC_MODE = 0x0600u,
    GPIOC_STRIDE = 0x0010u,
    GPIOC_SET = 0x0004u,
    GPIOC_RST = 0x0008u,
};

static uint32_t gpio_bit_get(uint32_t base, uint32_t pin) {
    return (mtk_read32(GPIOC_BASE + base + (pin / 16u) * GPIOC_STRIDE) >> (pin % 16u)) & 1u;
}

static void gpio_bit_write(uint32_t base, uint32_t pin, uint32_t on) {
    const uint32_t reg =
        GPIOC_BASE + base + (pin / 16u) * GPIOC_STRIDE + (on != 0u ? GPIOC_SET : GPIOC_RST);
    mtk_write32(reg, 1u << (pin % 16u));
}

static uint32_t gpio_mode_get(uint32_t pin) {
    return (mtk_read32(GPIOC_BASE + GPIOC_MODE + (pin / 5u) * GPIOC_STRIDE) >> ((pin % 5u) * 3u)) &
           7u;
}

/* MODE is the only one of these registers without SET/RST aliases, so it is a
 * read-modify-write of one 3-bit field. */
static void gpio_mode_set(uint32_t pin, uint32_t mode) {
    const uint32_t reg = GPIOC_BASE + GPIOC_MODE + (pin / 5u) * GPIOC_STRIDE;
    const uint32_t shift = (pin % 5u) * 3u;
    mtk_write32(reg, (mtk_read32(reg) & ~(7u << shift)) | ((mode & 7u) << shift));
}

/* pull: 0 = disabled, 1 = pull-down, 2 = pull-up. PULLSEL is written before
 * PULLEN so the resistor is never enabled in the wrong direction, even briefly. */
static void gpio_pull_set(uint32_t pin, uint32_t pull) {
    if (pull == 0u) {
        gpio_bit_write(GPIOC_PULLEN, pin, 0u);
        return;
    }
    gpio_bit_write(GPIOC_PULLSEL, pin, pull >= 2u ? 1u : 0u);
    gpio_bit_write(GPIOC_PULLEN, pin, 1u);
}

static uint32_t gpio_pull_get(uint32_t pin) {
    if (gpio_bit_get(GPIOC_PULLEN, pin) == 0u) return 0u;
    return gpio_bit_get(GPIOC_PULLSEL, pin) != 0u ? 2u : 1u;
}

/*
 * Apply a full pad configuration in the order that never glitches.
 *
 * DOUT, then pull, then direction, then MODE. MODE is last because MODE is what
 * connects the pad to the GPIO block: setting the block up first means the pad
 * goes straight from its old function to the intended state with nothing in
 * between. Doing it the other way round drives whatever DOUT happened to be
 * holding onto a live net for a few cycles, which is how a debug command turns
 * into a reset.
 */
static void gpio_cfg_apply(uint32_t pin, uint32_t mode, uint32_t dir, uint32_t pull,
                           uint32_t dout) {
    gpio_bit_write(GPIOC_DOUT, pin, dout);
    gpio_pull_set(pin, pull);
    gpio_bit_write(GPIOC_DIR, pin, dir);
    gpio_mode_set(pin, mode);
}

static void gpio_report_cfg(uint32_t pin) {
    kv_dec("kpdmap:   gpio", (int32_t)pin);
    kv_dec(" mode=", (int32_t)gpio_mode_get(pin));
    kv_dec(" dir=", (int32_t)gpio_bit_get(GPIOC_DIR, pin));
    kv_dec(" pullen=", (int32_t)gpio_bit_get(GPIOC_PULLEN, pin));
    kv_dec(" pullsel=", (int32_t)gpio_bit_get(GPIOC_PULLSEL, pin));
    put_char('\n');
}

/*
 * Dump every pin's mux, direction and pull in one shot.
 *
 * Written to settle whether this board's buttons were on GPIOs. It reports that
 * the four pins the board header calls the d-pad -- 8, 20, 45 and 93 -- are mux
 * mode 0, inputs, pull enabled and selected DOWN, reading 0 at idle.
 *
 * That was once read as "a parked, unconnected pad". It is not: a switch to the
 * rail behind a pull-down reads exactly like this at rest and reads 1 when
 * closed, and the d-pad works on hardware only when those four pins are mapped.
 * The earlier reading came with "a full DIN sweep across every press never saw
 * one move", which was true and meaningless -- the sweep of the day rejected
 * pins by event budget, so the four pins that did move were dropped as noise
 * before anyone looked. Configuration alone cannot tell a wired switch from an
 * unused pad; only a press can, which is what kpdmon is for.
 *
 * It stays because pinmux questions keep coming up -- the panel, the backlight
 * PWM on GPIO90, the LED sinks -- and "what is this pad actually configured as"
 * is otherwise four peek commands and some shift arithmetic per pin. The most
 * recent thing it settled: the only pads on this board in mode 0 AND driven as
 * outputs are 6, 112 and 113. There are three indicator LEDs, none of them on
 * the MT6323 ISINK block (ledscan walks all four sinks and the LEDs do not
 * change), so those three pins are where to look.
 *
 * Raw registers, not decoded pin lists: 42 mode words and 13 of everything else
 * fit in a dozen lines, and decoding is cheaper to do off the device than to
 * write twice. Columns of eight, address-labelled, so a line can be matched back
 * to a bank without counting.
 */
static void dump_reg_run(const char* what, uint32_t off, uint32_t count) {
    uint32_t i;

    put_str(what);
    for (i = 0u; i < count; ++i) {
        if ((i % 8u) == 0u) {
            put_char('\n');
            kv_hex("  ", GPIOC_BASE + off + i * GPIOC_STRIDE, 8u);
            put_char(':');
        }
        kv_hex(" ", mtk_read32(GPIOC_BASE + off + i * GPIOC_STRIDE), 8u);
    }
    put_char('\n');
}

static void cmd_gpiodump(void) {
    enum { GPIOC_BANKS = 13u, GPIOC_MODE_WORDS = 42u }; /* 208 pins, 5 per mode word */

    dump_reg_run("gpio: mode (3 bits per pin, 5 pins per word, 0 = plain GPIO)", GPIOC_MODE,
                 GPIOC_MODE_WORDS);
    dump_reg_run("gpio: dir (1 = output)", GPIOC_DIR, GPIOC_BANKS);
    dump_reg_run("gpio: pullen", GPIOC_PULLEN, GPIOC_BANKS);
    dump_reg_run("gpio: pullsel (1 = up)", GPIOC_PULLSEL, GPIOC_BANKS);
    dump_reg_run("gpio: din", GPIOC_DIN, GPIOC_BANKS);
}

/* ── Dead-man's switch ──
 *
 * Every command that reassigns a pad arms this first and kicks it once per tick.
 *
 * The reason is a specific failure that has already happened twice. `kpdhunt`
 * remuxed a range, the board went away mid-run, and the restore -- which lives
 * after the watch loop -- never executed. Two of those runs left the board
 * unreachable across six reconnect attempts. Pad configuration is volatile, so a
 * power cycle always clears it; what a power cycle cannot clear is a CPU wedged
 * on a bus whose pads were pulled out from under it, and that is what the
 * watchdog is for. If a remux hangs us, the board resets itself into a clean boot
 * a few seconds later instead of sitting dark.
 *
 * Bit 0 of MODE only. Not bit 2 (EXTEN): that drives the external reset pad, and
 * asserting an unknown pad is the class of mistake this switch exists to contain.
 * LENGTH counts in 15.625 ms units, hence the *64. Disarming writes the same
 * mask stage1.c's wdt_off() clears, so the console leaves the block exactly as
 * quiet as it found it.
 */
enum {
    WDTC_BASE = 0x10007000u,
    WDTC_MODE = 0x0000u,
    WDTC_LENGTH = 0x0004u,
    WDTC_RESTART = 0x0008u,
    WDTC_MODE_KEY = 0x22000000u,
    WDTC_LENGTH_KEY = 0x00000008u,
    WDTC_RESTART_KEY = 0x00001971u,
    WDTC_MODE_ENABLE = 0x1u,
    WDTC_TICKS_PER_SEC = 64u,
};

static void wdt_kick(void) { mtk_write32(WDTC_BASE + WDTC_RESTART, WDTC_RESTART_KEY); }

static void wdt_arm(uint32_t secs) {
    uint32_t ticks = secs * WDTC_TICKS_PER_SEC;
    if (ticks > 0x7ffu) ticks = 0x7ffu; /* [15:5] is 11 bits: 31.9 s ceiling */
    mtk_write32(WDTC_BASE + WDTC_LENGTH, (ticks << 5u) | WDTC_LENGTH_KEY);
    wdt_kick();
    mtk_write32(WDTC_BASE + WDTC_MODE, WDTC_MODE_KEY | WDTC_MODE_ENABLE);
}

static void wdt_disarm(void) {
    mtk_write32(WDTC_BASE + WDTC_MODE, (mtk_read32(WDTC_BASE + WDTC_MODE) & ~0x4fu) | WDTC_MODE_KEY);
    wdt_kick();
}

/* The kick, reachable from a driver that has no idea a watchdog is running.
 * Declared weak on the driver side and resolved here; see the comment at
 * wifi_cooperative_yield() in mt6592_wifi_hif.c for the case that wanted it --
 * a long computation with no I/O in it, inside a window this console armed. It
 * is a single keyed store to a register the console already owns, so calling it
 * when nothing is armed does nothing. */
void mvii_console_watchdog_kick(void) { wdt_kick(); }

/*
 * The long-wait escape hatch the SD and Wi-Fi drivers already call.
 *
 * Both declare `minos_machine64_io_yield` weak and call it from inside their
 * bounded polling loops, so that under the OS a transfer that takes a quarter of
 * a second does not stall the scheduler. Under LK there is no scheduler and the
 * symbol resolves to null, which is correct for those drivers and wrong for this
 * console: pushing 257 KiB of WIFI_RAM_CODE_SOC through the HIF 2 KiB at a time
 * is many seconds during which nothing kicks the watchdog and nothing services
 * the USB endpoint. The watchdog's ceiling is 31.9 s (see wdt_arm), so the first
 * consequence is a reset in the middle of a firmware download, and the second is
 * a host-side console that sees no output and looks hung.
 *
 * Defining it strongly here fixes both for every caller at once, and costs
 * nothing when nobody is waiting. Both operations it performs are already done
 * unconditionally elsewhere in this file (delay_ms polls the gadget; wdt_kick is
 * a no-op write when the block is disabled), so it is safe from any context.
 */
void minos_machine64_io_yield(void);
void minos_machine64_io_yield(void) {
    wdt_kick();
    /* The release bootloader keeps this hook -- mt6592_msdc.c takes a weak
     * reference to it and the eMMC read loops there run on every boot, console
     * or no console, so the watchdog kick above is load-bearing. What it does
     * not keep is the endpoint service: with no console there is nothing on the
     * endpoint to service, and this was the last live reference holding the
     * peripheral gadget in that image. */
    console_gadget_service();
}

/*
 * Pads whose function is known and whose loss is not an experiment.
 *
 * Deliberately short. A long guessed list is worse than none, because it hides
 * the pad that actually matters behind eleven that do not and trains the operator
 * to reach for the override. These two are not guesses:
 *
 *   90  DISP_PWM, the backlight (MT6592_J36_DISP_PWM_GPIO, driven by
 *       mt6592_backlight_reassert -- reassigning it leaves the panel dark with no
 *       way to say so, since the console's own output is that panel).
 *   112 panel power. Found the hard way: `gpioout #112 0` blanks the display
 *       completely.
 *
 * Everything else is open, including 33 and 65. `pin` and `pinhunt` work on one
 * pad at a time and name it before they touch it, which is the property that
 * makes an unknown pad safe to try: whatever happens, the last line the host saw
 * says which pad did it.
 */
static uint32_t pin_is_reserved(uint32_t pin) {
    return (pin == 90u || pin == 112u) ? 1u : 0u;
}

static void pin_report(const char* tag, uint32_t pin) {
    put_str(tag);
    put_dec((int32_t)pin);
    kv_dec(" mode=", (int32_t)gpio_mode_get(pin));
    kv_dec(" dir=", (int32_t)gpio_bit_get(GPIOC_DIR, pin));
    kv_dec(" pull=", (int32_t)gpio_pull_get(pin));
    kv_dec(" dout=", (int32_t)gpio_bit_get(GPIOC_DOUT, pin));
    kv_dec(" din=", (int32_t)gpio_bit_get(GPIOC_DIN, pin));
    put_char('\n');
}

/*
 * Read or write one pad's complete configuration.
 *
 * This is the primitive the LED and button hunts were missing. Every candidate
 * left on this board is on a pad that boots in an alternate mux mode -- gpio 33
 * for the indicator LEDs, and whatever carries START, SELECT, MENU and R2 -- and
 * until now nothing here could change a mux except `kpdhunt`, which only did it
 * to a whole range at once and only in the one configuration it wanted. So a
 * single pad could not be tried at all, and a range could not be tried safely.
 *
 * Positional and hex-by-default, like the rest of the console:
 *
 *   pin 41                 report pad 65 (0x41) and change nothing
 *   pin 41 0 1 0 1         mode 0, output, no pull, driving high
 *   pin 41 0 0 2           mode 0, input, pull-up  (what a button wants)
 *   pin 21 1               put pad 33 back in mux mode 1
 *
 * Omitted fields keep their current value, so `pin 41 0` remuxes and touches
 * nothing else. pull is 0 = off, 1 = down, 2 = up.
 *
 * Nothing here is persistent: the GPIO block is volatile, so a power cycle undoes
 * anything this command did. That is the whole safety argument for letting it
 * write any pad it is asked to. It prints before and after, and the "before" line
 * is flushed prior to the write, so a pad that takes the board down still tells
 * you which pad it was -- and the reserved list above holds back the two whose
 * answer is already known.
 */
static void cmd_pin(const char* args) {
    uint32_t pin;
    uint32_t mode;
    uint32_t dir;
    uint32_t pull;
    uint32_t dout;

    if (parse_u32(&args, &pin) != 0) {
        put_str("usage: pin <n> [mode [dir [pull [dout]]]]   (all HEX; #65 for decimal)\n");
        put_str("pin: mode 0..7 (0 = plain gpio), dir 0=in 1=out, pull 0=off 1=down 2=up\n");
        put_str("pin: with no fields it only reports. Lost fields keep their value.\n");
        put_str("pin: volatile -- a power cycle undoes anything set here.\n");
        return;
    }
    if (pin > MAP_PAD_MAX) {
        kv_dec("pin: out of range -- the last pad this SoC decodes is #", (int32_t)MAP_PAD_MAX);
        put_char('\n');
        return;
    }
    if (parse_u32(&args, &mode) != 0) {
        pin_report("pin: gpio ", pin);
        return;
    }
    if (pin_is_reserved(pin)) {
        kv_dec("pin: gpio ", (int32_t)pin);
        put_str(" is reserved (backlight #90 / panel power #112); refusing to remux\n");
        return;
    }
    dir = parse_u32_default(&args, gpio_bit_get(GPIOC_DIR, pin));
    pull = parse_u32_default(&args, gpio_pull_get(pin));
    dout = parse_u32_default(&args, gpio_bit_get(GPIOC_DOUT, pin));

    pin_report("pin: was  gpio ", pin);
    flush_line(); /* the last thing the host sees if this write kills the board */

    gpio_cfg_apply(pin, mode, dir != 0u ? 1u : 0u, pull, dout != 0u ? 1u : 0u);
    pin_report("pin: now  gpio ", pin);
}

/*
 * Drive one pad as a plain GPIO output.
 *
 * This is what found the LEDs. `led` and `ledscan` used to drive the MT6323 ISINK
 * channels, which is where an MTK reference board puts its indicator and is simply
 * the wrong block here -- the old ledscan walked all four sinks with every channel
 * off in between and the three LEDs never changed. The gpiodump then narrowed the
 * candidates to 6, 112 and 113, the only pads that are both mux mode 0 and
 * outputs, and that was wrong too (112 is panel power). What settled it was `pin`,
 * one pad at a time across a wider sweep: 47 red, 48 green, 49 blue, active low.
 *
 * It stays for the next output-shaped mystery, and because it is the narrow form:
 * it refuses any pin that is not already an output in mode 0. That is not
 * timidity, it is the division of labour with `pin` -- the pads that are outputs
 * in an alternate mode are driving the eMMC clock, the DSI reset and the backlight
 * PWM, and a command that only flips a level should not be able to steal one of
 * those. Use `pin <n> 0 1` when a reassignment really is what you want.
 */
static void cmd_gpioout(const char* args) {
    uint32_t pin;
    uint32_t val;

    if (parse_u32(&args, &pin) != 0 || parse_u32(&args, &val) != 0) {
        put_str("usage: gpioout <pin> <0|1>   (pin is HEX; use #6 for decimal 6)\n");
        put_str("gpioout: the LEDs are #47 red / #48 green / #49 blue, and 0 lights them\n");
        return;
    }
    if (pin > MAP_PAD_MAX) {
        put_str("gpioout: pin out of range (last decoded pad is #168)\n");
        return;
    }
    if (gpio_mode_get(pin) != 0u) {
        kv_dec("gpioout: gpio ", (int32_t)pin);
        kv_dec(" is in mux mode ", (int32_t)gpio_mode_get(pin));
        put_str(", not plain GPIO; refusing\n");
        kv_dec("gpioout: `pin ", (int32_t)pin);
        put_str(" 0 1` remuxes it as an output first (volatile, undone by a power cycle)\n");
        return;
    }
    if (gpio_bit_get(GPIOC_DIR, pin) == 0u) {
        kv_dec("gpioout: gpio ", (int32_t)pin);
        put_str(" is an input; refusing (this command does not reassign pads)\n");
        kv_dec("gpioout: `pin ", (int32_t)pin);
        put_str(" 0 1` makes it an output\n");
        return;
    }

    gpio_bit_write(GPIOC_DOUT, pin, val != 0u ? 1u : 0u);
    kv_dec("gpioout: gpio ", (int32_t)pin);
    kv_dec(" <- ", (int32_t)(val != 0u ? 1 : 0));
    kv_dec("  din now ", (int32_t)gpio_bit_get(GPIOC_DIN, pin));
    put_char('\n');
}

/*
 * Walk the three indicator LEDs one pad at a time, announcing each step before it
 * takes it, so the transcript and what the operator saw can be lined up
 * afterwards. This is how the wiring was established and it is the check that it
 * has not been mis-transcribed since.
 *
 * Pads 47 / 48 / 49, active low: mode 0, output, no pull, DOUT 0 lights it. The
 * literals are here rather than from mt6592_board_j36.h on purpose (see the note
 * at the top of this file) -- the console tests the hardware, and mt6592_led.c is
 * one of the things it tests. `led <r> <g> <b>` goes through the driver; this goes
 * around it, so a disagreement between the two localises itself.
 *
 * No watchdog and no restore. These pads are already mode-0 outputs at boot, so
 * nothing is reassigned away from another function, and the state this leaves
 * behind -- all dark -- is the state the driver wants anyway.
 */
static void cmd_ledscan(void) {
    static const struct { uint32_t pin; const char* name; } leds[] = {
        {47u, "red"}, {48u, "green"}, {49u, "blue"},
    };
    const uint32_t n = (uint32_t)(sizeof(leds) / sizeof(leds[0]));
    uint32_t i;

    put_str("ledscan: pads #47 red / #48 green / #49 blue, active low (dout 0 = lit)\n");
    for (i = 0u; i < n; ++i) {
        pin_report("ledscan: before gpio ", leds[i].pin);
    }

    put_str("ledscan: all three DARK for 2s -- any LED still lit is not on these pads\n");
    flush_line();
    for (i = 0u; i < n; ++i) {
        gpio_cfg_apply(leds[i].pin, 0u, 1u, 0u, 1u);
    }
    delay_ms(2000u);

    for (i = 0u; i < n; ++i) {
        uint32_t j;
        put_str("ledscan: ");
        put_str(leds[i].name);
        kv_dec(" (gpio ", (int32_t)leds[i].pin);
        put_str(") ON for 2s\n");
        flush_line();
        for (j = 0u; j < n; ++j) {
            gpio_bit_write(GPIOC_DOUT, leds[j].pin, j == i ? 0u : 1u);
        }
        delay_ms(2000u);
    }

    for (i = 0u; i < n; ++i) {
        gpio_bit_write(GPIOC_DOUT, leds[i].pin, 1u);
    }
    put_str("ledscan: done; all three left dark\n");
}

/* ── Removable storage and the files on it ──
 *
 * LK has needed a filesystem exactly once before: never. It reads boot.img out
 * of a raw eMMC slot at a fixed offset and that is the whole of its storage
 * story. Wi-Fi changes that, because the connectivity chip is a blank ROM until
 * ~370 KiB of MediaTek firmware is pushed into it, and that firmware lives as
 * three ordinary files on the removable card:
 *
 *   System/Drivers/MediaTek/WiFi/MT6592/ROMv1_patch_1_1_hdr.bin
 *   System/Drivers/MediaTek/WiFi/MT6592/ROMv1_patch_1_0_hdr.bin
 *   System/Drivers/MediaTek/WiFi/MT6592/WIFI_RAM_CODE_SOC
 *
 * So: the SD block driver (which existed but was not linked into this image),
 * a read-only FAT reader (mvii_fat.c, which did not exist at all for this
 * target), and the three commands below. `sd` and `ls` are here rather than
 * being folded into `wifi` on purpose -- when `wifi fw` says it cannot find a
 * file, the operator needs to be able to ask whether the card mounted and what
 * the directory actually contains, without that answer coming from the same
 * code path that just failed.
 */

enum {
    /*
     * Where a firmware image lands on its way from the card to the chip.
     *
     * Not .bss. The LK slot is 512 KiB and mvii_lk_linker.ld asserts that
     * __bss_end stays clear of __stack_top; a 512 KiB staging array does not
     * link, and neither would a 264 KiB one. So it is a fixed DRAM address,
     * picked out of the gap nothing else claims:
     *
     *   0x80008000   kernel, 8 MiB reserved
     *   0x81e00000   this image, 512 KiB
     *   0x82700000   the LK framebuffer
     *   0x84000000   the ramdisk
     *
     * 0x83000000 is above the framebuffer and below the ramdisk. It is also
     * above stage1's RAMDISK_SCAN_END (0x82600000), which is the part that is
     * not obvious: when stage1 cannot get a ramdisk address from the ATAGs it
     * falls back to scanning DRAM for an MTK image magic, and a firmware blob
     * left lying inside that window by a `wifi fw` followed by `boot` is
     * precisely the sort of thing such a scan finds.
     */
    FW_SCRATCH_ADDR = 0x83000000u,
    FW_SCRATCH_SIZE = 0x00080000u, /* 512 KiB; the largest asset is ~257 KiB */

    FW_PROGRESS_STEP = 0x10000u    /* one dot per 64 KiB copied */
};

#define FW_DIR "System/Drivers/MediaTek/WiFi/MT6592"

/* Distinct addresses, used only as identities: mvii_fat keys its one-sector FAT
 * cache on the block context, so the card and the eMMC must not share one. */
static const char g_dev_sd_tag;
static const char g_dev_emmc_tag;

static mvii_fat_volume g_fs;
static int g_fs_ready;
static const char* g_fs_dev = "none";

static int sd_block_read(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    (void)ctx;
    return (mt6592_sd_read(lba, count, buffer) == MT6592_MSDC_OK) ? 0 : -1;
}

static int emmc_block_read(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    (void)ctx;
    return (mt6592_emmc_read_user(lba * 512ull, (uint8_t*)buffer, count * 512u) == MT6592_MSDC_OK) ? 0 : -1;
}

/* Rest of the line as a path, trailing blanks trimmed -- a stray space at the
 * end of a pasted command would otherwise become part of the last component and
 * turn a correct path into "no such file". */
static void arg_path(const char* args, char* out, uint32_t cap) {
    uint32_t n = 0u;
    args = skip_spaces(args);
    while (args[n] != '\0' && n + 1u < cap) {
        out[n] = args[n];
        ++n;
    }
    while (n > 0u && (out[n - 1u] == ' ' || out[n - 1u] == '\t')) --n;
    out[n] = '\0';
}

static uint32_t arg_len(const char* s) {
    uint32_t n = 0u;
    while (s[n] != '\0') ++n;
    return n;
}

static void fs_report(void) {
    if (!g_fs_ready) {
        put_str("sd: no volume mounted\n");
        return;
    }
    put_str("sd: mounted ");
    put_str(g_fs_dev);
    put_str(" fat");
    put_dec((int32_t)g_fs.type);
    put_str(" from ");
    put_str(g_fs.source);
    put_str(" label='");
    put_str(g_fs.label);
    put_str("'\n");
    kv_hex("sd: volume_lba=", (uint32_t)g_fs.volume_lba, 8u);
    kv_hex(" fat_lba=", (uint32_t)g_fs.fat_lba, 8u);
    kv_hex(" data_lba=", (uint32_t)g_fs.data_lba, 8u);
    kv_hex(" clusters=", g_fs.cluster_count, 8u);
    kv_hex(" cluster_bytes=", g_fs.cluster_bytes, 8u);
    put_char('\n');
}

/* `at_lba` of 0 means "search the device"; anything else mounts that sector as
 * a volume start directly, for a card whose partition table lies. */
static int fs_mount(int use_emmc, uint64_t at_lba) {
    mvii_fat_read_fn read = use_emmc ? emmc_block_read : sd_block_read;
    void* ctx = use_emmc ? (void*)&g_dev_emmc_tag : (void*)&g_dev_sd_tag;
    int rc;

    g_fs_ready = 0;
    g_fs_dev = "none";

    if (use_emmc) {
        if (mt6592_emmc_user_init() != MT6592_MSDC_OK) {
            put_str("sd: eMMC user area would not initialise\n");
            return -1;
        }
    } else {
        rc = mt6592_sd_probe();
        if (rc != MT6592_MSDC_OK) {
            kv_dec("sd: no card behind MSDC1, probe rc=", rc);
            put_str(" (is one inserted?)\n");
            return -1;
        }
        kv_hex("sd: card ready, capacity=", (uint32_t)mt6592_sd_capacity_sectors(), 8u);
        put_str(" sectors\n");
    }

    rc = (at_lba != 0u) ? mvii_fat_mount_at(&g_fs, read, ctx, at_lba)
                        : mvii_fat_mount(&g_fs, read, ctx);
    if (rc != MVII_FAT_OK) {
        put_str("sd: mount failed: ");
        put_str(mvii_fat_strerror(rc));
        put_char('\n');
        return -1;
    }

    g_fs_ready = 1;
    g_fs_dev = use_emmc ? "emmc" : "sdcard";
    fs_report();
    return 0;
}

/* Mount lazily, so `ls` and `wifi fw` work without a preceding `sd`. */
static int fs_require(void) {
    if (g_fs_ready) return 0;
    return fs_mount(0, 0u);
}

static void cmd_sd(const char* args) {
    const char* rest;

    args = skip_spaces(args);

    if (match_word(args, "forget") != 0) {
        mt6592_sd_forget();
        g_fs_ready = 0;
        g_fs_dev = "none";
        put_str("sd: card state dropped; next mount re-enumerates\n");
        return;
    }
    if (match_word(args, "emmc") != 0) {
        (void)fs_mount(1, 0u);
        return;
    }
    if ((rest = match_word(args, "at")) != 0) {
        uint32_t lba = 0u;
        if (parse_u32(&rest, &lba) != 0 || lba == 0u) {
            put_str("usage: sd at <lba>   (nonzero; 0 is what the search default means)\n");
            return;
        }
        (void)fs_mount(0, lba);
        return;
    }
    if (match_word(args, "status") != 0) {
        fs_report();
        return;
    }
    if (*args != '\0') {
        put_str("usage: sd [emmc|at <lba>|status|forget]\n");
        return;
    }
    (void)fs_mount(0, 0u);
}

static int ls_emit(void* ctx, const mvii_fat_dirent* entry) {
    uint32_t* count = (uint32_t*)ctx;

    ++*count;
    put_str("  ");
    if (entry->is_dir != 0u) {
        put_str("     <dir>  ");
    } else {
        put_hex(entry->size, 8u);
        put_str("  ");
    }
    put_str(entry->name);
    put_char('\n');
    /* A hundred-entry directory is a hundred USB writes and several seconds. */
    wdt_kick();
    mt6592_usb_gadget_poll();
    return 0;
}

static void cmd_ls(const char* args) {
    char path[128];
    uint32_t count = 0u;
    int rc;

    if (fs_require() != 0) return;

    arg_path(args, path, sizeof path);
    put_str("ls: /");
    put_str(path);
    put_char('\n');

    /* No wdt_arm() here, and not by oversight: mt6592_sd_read() writes the
     * watchdog's MODE register to zero on every burst (watchdog_disable(), for
     * the surprise-removal timeouts it has to survive), so anything armed around
     * a card read is disarmed by the first sector. Saying so beats arming it and
     * believing the board is protected. */
    rc = mvii_fat_list(&g_fs, path, ls_emit, &count);

    if (rc != MVII_FAT_OK) {
        put_str("ls: ");
        put_str(mvii_fat_strerror(rc));
        put_char('\n');
        return;
    }
    kv_dec("ls: ", (int32_t)count);
    put_str(" entries\n");
}

/* ── Firmware staging ── */

static uint32_t g_fw_mark;

static void fw_progress(void* ctx, uint32_t done, uint32_t total) {
    (void)ctx;
    (void)total;
    wdt_kick();
    mt6592_usb_gadget_poll();
    while (done >= g_fw_mark) {
        put_char('.');
        g_fw_mark += FW_PROGRESS_STEP;
    }
}

/* Copy one firmware asset into the DRAM scratch. Returns its length, or 0. */
static uint32_t fw_stage(const char* name) {
    char path[160];
    uint32_t n = 0u;
    uint32_t len = 0u;
    uint32_t i;
    int rc;

    for (i = 0u; FW_DIR[i] != '\0'; ++i) path[n++] = FW_DIR[i];
    path[n++] = '/';
    for (i = 0u; name[i] != '\0' && n + 1u < sizeof path; ++i) path[n++] = name[i];
    path[n] = '\0';

    put_str("wifi: reading ");
    put_str(name);
    put_char(' ');
    flush_line();

    g_fw_mark = FW_PROGRESS_STEP;
    mvii_fat_set_progress(&g_fs, fw_progress, 0);
    rc = mvii_fat_read_file(&g_fs, path, (void*)(uintptr_t)FW_SCRATCH_ADDR, FW_SCRATCH_SIZE, &len);
    mvii_fat_set_progress(&g_fs, 0, 0);

    if (rc != MVII_FAT_OK) {
        put_str(" failed: ");
        put_str(mvii_fat_strerror(rc));
        if (rc == MVII_FAT_ERR_TOOBIG) kv_hex(" (file is ", len, 8u);
        put_char('\n');
        return 0u;
    }

    {
        const uint8_t* p = (const uint8_t*)(uintptr_t)FW_SCRATCH_ADDR;
        kv_hex(" ok, ", len, 8u);
        put_str(" bytes, head=");
        for (i = 0u; i < 4u && i < len; ++i) put_hex(p[i], 2u);
        put_char('\n');
    }
    return len;
}

/*
 * The stock load order, taken from the OS side (WifiSystem.cpp) rather than
 * guessed: both ROM patches first and in this order -- the launcher metadata at
 * file offset 24 gives _1_1 sequence 1 and _1_0 sequence 2 -- and only then the
 * WLAN image, which needs the connectivity MCU already patched and calibrated
 * before the AHB HIF will answer at all.
 */
/*
 * Everything that has to happen before the WLAN boot ROM will answer an INIT
 * command: card, CONSYS rails + MTCMOS, the two WMT ROM patches over BTIF, and
 * HIF ownership. Split out of cmd_wifi_firmware() so `wifi mark` can reach the
 * ROM without downloading an image over it.
 *
 * Latched, because it is not repeatable: the WMT patch download is a one-shot
 * per power-up and a second attempt is rejected. `wifi mark` can then be issued
 * as many times as you like in one boot -- the ROM services unlimited
 * INIT_CMD_DOWNLOAD_BUFs, it is only WIFI_START that consumes it.
 */
/*
 * Print a WMT event the way a probe needs it: bytes first, meaning second.
 *
 * A WMT event is 0x02, opcode, length low, length high, then the payload, and
 * byte four is the status. The named fields are printed because they are the
 * contract, and the whole raw run is printed after them because on an
 * exploratory command the payload is the entire point and this console has no
 * second chance to ask -- the boot ROM does not survive `wifi go`.
 */
static void wifi_print_wmt_event(int rc, const uint8_t* event, uint32_t len) {
    if (len == 0u) {
        put_str("wifi: the connectivity MCU did not answer at all\n");
        return;
    }
    put_str("wifi: WMT rx");
    for (uint32_t i = 0u; i < len; ++i) {
        kv_hex(" ", event[i], 2u);
    }
    put_char('\n');
    if (len >= 5u) {
        kv_hex("wifi: evt type=", event[0], 2u);
        kv_hex(" opcode=", event[1], 2u);
        kv_hex(" len=", (uint32_t)event[2] | ((uint32_t)event[3] << 8), 4u);
        kv_hex(" status=", event[4], 2u);
        put_char('\n');
    }
    put_str(rc == 0 ? "wifi: accepted\n" : "wifi: the connectivity MCU refused it\n");
}

static int wifi_rom_ready(void) {
    static const char* const patches[2] = {
        "ROMv1_patch_1_1_hdr.bin",
        "ROMv1_patch_1_0_hdr.bin"
    };
    static int prepared = 0;
    const mt6592_wifi_wmt_state* wmt;
    uint32_t len;
    uint32_t i;

    if (prepared) return 0;

    if (fs_require() != 0) return -1;

    if (mt6592_wifi_sdio_bind() != 0) {
        put_str("wifi: CONSYS did not power up; firmware would go nowhere\n");
        return -1;
    }

    /*
     * The watchdog is armed per submit rather than once around the whole
     * command, because mt6592_sd_read() disables it on every burst -- so every
     * fw_stage() leaves it off no matter what was set beforehand. Arming after
     * the file is in DRAM covers the part that can actually hang: a chip that
     * takes the patch and never answers. minos_machine64_io_yield() above kicks
     * it from inside the driver's own 2 KiB chunk loop, so a download that is
     * making progress stays alive while a wedged one still resets the board.
     */
    for (i = 0u; i < 2u; ++i) {
        len = fw_stage(patches[i]);
        if (len == 0u) return -1;
        wdt_arm(0x1eu);
        if (mt6592_wifi_wmt_load_patch((const void*)(uintptr_t)FW_SCRATCH_ADDR, len) != 0) {
            wmt = mt6592_wifi_wmt_get_state();
            put_str("wifi: WMT rejected the patch: ");
            put_str(wmt->blocked != 0 ? wmt->blocked : "(no reason given)");
            put_char('\n');
            wdt_disarm();
            return -1;
        }
        wdt_disarm();
        put_str("wifi: patch accepted\n");
    }

    wmt = mt6592_wifi_wmt_get_state();
    if (!wmt->ready) {
        put_str("wifi: WMT is patched but not ready: ");
        put_str(wmt->blocked != 0 ? wmt->blocked : "(no reason given)");
        put_char('\n');
        return -1;
    }

    wdt_arm(0x1eu);
    if (mt6592_wifi_hif_bind() != 0) {
        const mt6592_wifi_hif_state* hif = mt6592_wifi_hif_get_state();
        wdt_disarm();
        put_str("wifi: HIF bind failed: ");
        put_str(hif->blocked != 0 ? hif->blocked : "(no reason given)");
        put_char('\n');
        return -1;
    }
    wdt_disarm();

    prepared = 1;
    return 0;
}

enum
{
    FW_DECOY_MAX = 64
};

static uint32_t g_fw_decoy_addr[FW_DECOY_MAX];
static uint32_t g_fw_decoy_count;

static uint32_t fw_apply_decoy(uint8_t* image, uint32_t len);
static void fw_clear_decoy(void);

static int cmd_wifi_firmware(int defer_start, int decoy) {
    uint32_t len;

    if (wifi_rom_ready() != 0) return -1;

    len = fw_stage("WIFI_RAM_CODE_SOC");
    if (len == 0u) return -1;
    if (decoy != 0) {
        const uint32_t patched = fw_apply_decoy((uint8_t*)(uintptr_t)FW_SCRATCH_ADDR, len);
        kv_dec("wifi: decoyed ", (int32_t)patched);
        kv_dec(" partial block(s), ", (int32_t)g_fw_decoy_count);
        put_str(" word(s) to restore after the download\n");
    } else {
        g_fw_decoy_count = 0u;
    }
    mt6592_wifi_hif_set_defer_start(defer_start);
    wdt_arm(0x1eu);
    if (mt6592_wifi_hif_load_firmware((const void*)(uintptr_t)FW_SCRATCH_ADDR, len) != 0) {
        const mt6592_wifi_hif_state* hif = mt6592_wifi_hif_get_state();
        put_str("wifi: WLAN firmware rejected: ");
        put_str(hif->blocked != 0 ? hif->blocked : "(no reason given)");
        put_char('\n');
        wdt_disarm();
        return -1;
    }

    wdt_disarm();
    /*
     * Only meaningful with the entry deferred: once WIFI_START is sent the ROM
     * stops answering and half of these words are only reachable through it.
     */
    if (defer_start) fw_clear_decoy();
    if (defer_start) {
        put_str("wifi: sections are down and the entry is NOT taken; `sweep`, then `wifi go`\n");
        return 0;
    }
    put_str("wifi: WIFI_RAM_CODE_SOC loaded and the AIS BSS is active\n");
    return 0;
}

/* ── Firmware read-back verification ── */

/*
 * The one ciphertext block whose plaintext is known without the key: the
 * encryption of sixteen zero bytes. It occurs 1069 times in section 1 and 2275
 * times in section 3, in long unbroken runs, which is how the zero stretches in
 * WIFI_RAM_CODE_SOC were identified in the first place.
 *
 * It is also, on its own, a two-sided test. The transform is deterministic and
 * position-dependent but not address-dependent -- a run of identical plaintext
 * blocks produces a run of identical ciphertext blocks, which is exactly what
 * the image shows -- so for any aligned block:
 *
 *     ciphertext == this constant   <=>   plaintext == sixteen zero bytes
 *
 * Left to right gives "these bytes must read back as zero"; right to left gives
 * "these bytes must NOT read back as zero". Neither direction needs the key, and
 * together they catch the two failures that matter: memory that was never
 * written (reads zero everywhere, so every non-E(0) block fails) and memory that
 * took the ciphertext undecrypted (reads non-zero everywhere, so every E(0)
 * block fails).
 *
 * Do not read the sixteen bytes as a cipher block. Section 3's first block
 * differs from this constant only in its leading four bytes, which no 16-byte
 * block cipher can do -- one plaintext byte would change all sixteen. The
 * transform is 32-bit granular, and the "identical 16-byte blocks" seen in the
 * zero runs are four identical 4-byte units. That does not affect the test
 * above, which only needs determinism.
 */
static const uint8_t g_fw_zero_block[16] = {
    0xb4u, 0x8du, 0x13u, 0x6fu, 0xe3u, 0x76u, 0x12u, 0x7cu,
    0xc5u, 0xf9u, 0x1fu, 0xb4u, 0x83u, 0xe9u, 0xd6u, 0x60u
};

enum {
    /* CONSYS 0xf0000000 is the 1 MiB EMI window and equals AP 0x83100000, so
     * anything landing there is plain DRAM to us and costs a load to check. The
     * other sections are connectivity SRAM and cost a ROM round trip per word. */
    FW_EMI_CONSYS_BASE = 0xf0000000u,
    FW_EMI_CONSYS_END  = 0xf0100000u,
    FW_EMI_AP_BASE     = 0x83100000u,
    /*
     * Round trips are the budget, so the two block classes get separate ones.
     * A uniform stride is the wrong sampler here: section 1 is 1069 zero blocks
     * and 78 data blocks, so striding the whole section spends 93% of the budget
     * re-proving that zeros are zero and checked only two of the blocks that
     * carry anything. The first run missed three of the four sparse data blocks
     * that way. Zero blocks are cheap evidence and 16 of them per section is
     * plenty; the data blocks are the ones a lost write can hide in, so they get
     * the room. 96 + 16 blocks is ~450 ACCESS_REG exchanges per SRAM section.
     */
    FW_VERIFY_DATA_BLOCKS = 96u,
    FW_VERIFY_ZERO_BLOCKS = 16u,
    /* Bad blocks are printed individually, not just counted. Last run said
     * "s3 BAD=5" and named one address, and the other four could not be found
     * offline -- four measurements taken and thrown away. */
    FW_VERIFY_MAX_REPORT = 24u
};

static uint32_t fw_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le32_at(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/*
 * One bit per 32-bit unit: set where this block's ciphertext differs from E(0),
 * i.e. where the plaintext is known to be non-zero. Bit 0 is the first word.
 * The transform is 32-bit granular, so this is meaningful per word and not only
 * per block -- every anomaly found so far has been a block whose plaintext is
 * non-zero in its leading word or two and zero for the rest, which is exactly
 * the shape a lost leading word would leave behind.
 */
static uint32_t fw_word_mask(const uint8_t* ct) {
    uint32_t mask = 0u;
    uint32_t w;
    for (w = 0u; w < 4u; ++w) {
        if (fw_le32(ct + w * 4u) != fw_le32(g_fw_zero_block + w * 4u)) mask |= 1u << w;
    }
    return mask;
}

static int fw_is_zero_block(const uint8_t* ct) {
    return fw_word_mask(ct) == 0u;
}

/*
 * One connectivity word, whichever door is open. Returns 0, or -1 if the ROM
 * stopped answering, or -2 if it answered for a different address (the events
 * have slipped and every later word would be a plausible lie).
 */
static int fw_read_back(uint32_t consys, uint32_t* out) {
    uint8_t event[32];
    uint32_t len = 0u;
    uint32_t echoed;

    if (consys >= FW_EMI_CONSYS_BASE && consys < FW_EMI_CONSYS_END) {
        *out = mtk_read32(FW_EMI_AP_BASE + (consys - FW_EMI_CONSYS_BASE));
        return 0;
    }
    if (mt6592_wifi_hif_rom_access_reg(0, consys, 0u, event, sizeof event, &len) != 0 || len < 12u) {
        return -1;
    }
    echoed = fw_le32(event + 4u);
    if (echoed != consys) return -2;
    *out = fw_le32(event + 8u);
    return 0;
}

/*
 * The same door in the other direction. ACCESS_REG carries a literal word and
 * EMI is just DRAM, so unlike DOWNLOAD_BUF neither of them puts the value
 * through the ROM's decryption -- which is precisely why they can place a word
 * the download refuses to carry.
 */
static int fw_write_back(uint32_t consys, uint32_t value) {
    uint8_t event[32];
    uint32_t len = 0u;

    if (consys >= FW_EMI_CONSYS_BASE && consys < FW_EMI_CONSYS_END) {
        mtk_write32(FW_EMI_AP_BASE + (consys - FW_EMI_CONSYS_BASE), value);
        return 0;
    }
    return mt6592_wifi_hif_rom_access_reg(1, consys, value, event, sizeof event, &len);
}

/*
 * The decoy, moved to where it works: into the image, before it is sent.
 *
 * `wifi patch` established two things at once. The decoy does defeat the skip --
 * all nine partial blocks landed non-zero, none was refused, where verbatim
 * re-sends had always read back zero. And a lone sixteen-byte packet is not a
 * substitute for those sixteen bytes inside the stream: two identical sends of
 * the same payload to the same address decrypted to two entirely different
 * plaintexts, and the second pair was read through a plain DRAM load with the
 * ROM nowhere in the path, so it is the decryption that differs and not the
 * reading of it. Whatever state the ROM's decryptor carries between packets, a
 * standalone packet does not reproduce it, and the data words in those nine
 * blocks came back as garbage both times.
 *
 * Patching the staged image instead removes the question. Every byte keeps its
 * offset in the download, the data words decrypt exactly as the other 15046
 * blocks always have, and the only bytes that differ are the ones that would
 * have decrypted to zero and taken their whole block with them. Those are stored
 * back to zero afterwards by a path with no decryption in it.
 *
 * The container carries a CRC32 over image+8..end and the loader checks it, so
 * it has to be recomputed after the edit; per-chunk CRCs are computed by the
 * loader from the bytes it is handed and need nothing.
 */
static uint32_t fw_apply_decoy(uint8_t* image, uint32_t len) {
    uint32_t count;
    uint32_t section;
    uint32_t patched = 0u;

    g_fw_decoy_count = 0u;

    if (image[0] != 'M' || image[1] != 'T' || image[2] != 'K' || image[3] != 'W') return 0u;
    count = fw_le32(image + 8u);
    if (count == 0u || count > 8u || 16u + count * 16u > len) return 0u;

    for (section = 0u; section < count; ++section) {
        const uint8_t* entry       = image + 16u + section * 16u;
        const uint32_t file_offset = fw_le32(entry + 0u);
        const uint32_t size        = fw_le32(entry + 8u);
        const uint32_t dest        = fw_le32(entry + 12u);
        const uint32_t blocks      = size / 16u;
        uint32_t block;

        if (blocks == 0u || file_offset > len || size > len - file_offset) continue;

        for (block = 0u; block < blocks; ++block) {
            uint8_t* ct         = image + file_offset + block * 16u;
            const uint32_t mask = fw_word_mask(ct);
            const uint32_t at   = dest + block * 16u;
            uint32_t i;

            if (mask == 0u || mask == 0xfu) continue;

            for (i = 0u; i < 4u; ++i) {
                uint32_t b;
                if ((mask & (1u << i)) != 0u) continue;
                for (b = 0u; b < 4u; ++b) {
                    ct[i * 4u + b] = (uint8_t)(g_fw_zero_block[i * 4u + b] ^ 0xffu);
                }
                if (g_fw_decoy_count < (uint32_t)FW_DECOY_MAX) {
                    g_fw_decoy_addr[g_fw_decoy_count++] = at + i * 4u;
                }
            }
            ++patched;
        }
    }

    if (patched != 0u) {
        /* The loader will not accept the container otherwise. */
        write_le32_at(image + 4u, mt6592_wifi_hif_crc32(image + 8u, len - 8u));
    }
    return patched;
}

/*
 * Take the decoys back out, and say for each word whether it actually came out.
 *
 * `wifi patch` reported zeroed=3 or 2 for every EMI block and zeroed=0 for every
 * connectivity-SRAM one, and `wifi state` then showed drop=22 -- exactly the
 * count of SRAM writes attempted across the two runs. So EMI stores land and
 * ACCESS_REG writes are being dropped. Rather than trust either, this stores and
 * then reads the word back through the same door `wifi verify` uses, and prints
 * what it finds.
 */
static void fw_clear_decoy(void) {
    uint32_t i;
    uint32_t cleared = 0u;

    if (g_fw_decoy_count == 0u) return;

    kv_dec("wifi: taking out ", (int32_t)g_fw_decoy_count);
    put_str(" decoy word(s)\n");

    for (i = 0u; i < g_fw_decoy_count; ++i) {
        const uint32_t at = g_fw_decoy_addr[i];
        uint32_t back     = 0xffffffffu;
        const int wrc     = fw_write_back(at, 0u);
        const int rrc     = fw_read_back(at, &back);

        kv_hex("  ", at, 8u);
        if (rrc != 0) {
            put_str(" <read failed>\n");
        } else if (back == 0u) {
            ++cleared;
            put_str(" zeroed\n");
        } else {
            put_str(" STILL ");
            put_hex(back, 8u);
            kv_dec(" (write rc=", (int32_t)wrc);
            put_str(")\n");
        }
        mt6592_usb_gadget_poll();
    }

    kv_dec("wifi: ", (int32_t)cleared);
    kv_dec(" of ", (int32_t)g_fw_decoy_count);
    put_str(" decoy words are back to zero\n");
    g_fw_decoy_count = 0u;
}

/*
 * `wifi verify` -- did the bytes we sent actually land, in the clear, where we
 * asked for them?
 *
 * Every chunk of the download ACKs with status 0 and the firmware still never
 * asserts WLAN_READY, so the ACK is worth exactly what it can be shown to be
 * worth. It only says the ROM liked the CRC of the packet it *received*; the
 * copy and the decryption happen afterwards, and nothing reports on them.
 *
 * Sections 2 and 3 go to EMI and have been read back by hand with `peek`, and
 * they are correct. Section 0 has been read back with `wifi dump` and is
 * correct. Section 1, at 0x0209f800, is the one region of this image that has
 * never been checked -- it is outside the EMI window and outside the AP's SRAM
 * aperture, so only the ROM can see it. If its 0x47b0 bytes never arrived, the
 * entry code in section 0 is intact, jumps, immediately touches data that is not
 * there, and stops: six seconds of silence with a perfect download behind it,
 * which is precisely the failure on the bench.
 *
 * Run it between `wifi dl` and `wifi go`, while the ROM is still alive.
 */
static void cmd_wifi_verify(void) {
    const uint8_t* image;
    uint32_t len;
    uint32_t count;
    uint32_t section;
    uint32_t total_bad = 0u;

    if (wifi_rom_ready() != 0) return;

    len = fw_stage("WIFI_RAM_CODE_SOC");
    if (len == 0u) return;
    image = (const uint8_t*)(uintptr_t)FW_SCRATCH_ADDR;

    if (image[0] != 'M' || image[1] != 'T' || image[2] != 'K' || image[3] != 'W') {
        put_str("wifi: that file is not an MTKW container\n");
        return;
    }
    count = fw_le32(image + 8u);
    if (count == 0u || count > 8u || 16u + count * 16u > len) {
        put_str("wifi: the MTKW section table is not usable\n");
        return;
    }

    put_str("wifi: E(0)-block read-back; 'zero' blocks must read 0, 'data' blocks must not\n");

    for (section = 0u; section < count; ++section) {
        const uint8_t* entry       = image + 16u + section * 16u;
        const uint32_t file_offset = fw_le32(entry + 0u);
        const uint32_t size        = fw_le32(entry + 8u);
        const uint32_t dest        = fw_le32(entry + 12u);
        const uint32_t blocks      = size / 16u;
        const int is_emi           = (dest >= FW_EMI_CONSYS_BASE && dest < FW_EMI_CONSYS_END);
        uint32_t data_total = 0u;
        uint32_t zero_total = 0u;
        uint32_t data_stride;
        uint32_t zero_stride;
        uint32_t data_index = 0u;
        uint32_t zero_index = 0u;
        uint32_t block;
        uint32_t checked   = 0u;
        uint32_t zero_seen = 0u;
        uint32_t bad       = 0u;
        uint32_t reported  = 0u;
        int stopped = 0;

        if (blocks == 0u || file_offset > len || size > len - file_offset) {
            kv_dec("wifi: s", (int32_t)section);
            put_str(" section range is unusable, skipped\n");
            continue;
        }

        /* EMI is a load; SRAM is a round trip. Spend accordingly, and spend it
         * on the data blocks -- see FW_VERIFY_DATA_BLOCKS. */
        for (block = 0u; block < blocks; ++block) {
            if (fw_is_zero_block(image + file_offset + block * 16u)) ++zero_total;
            else                                                    ++data_total;
        }
        data_stride = is_emi ? 1u : (data_total + FW_VERIFY_DATA_BLOCKS - 1u) / FW_VERIFY_DATA_BLOCKS;
        zero_stride = is_emi ? 1u : (zero_total + FW_VERIFY_ZERO_BLOCKS - 1u) / FW_VERIFY_ZERO_BLOCKS;
        if (data_stride == 0u) data_stride = 1u;
        if (zero_stride == 0u) zero_stride = 1u;

        for (block = 0u; block < blocks; ++block) {
            const uint8_t* ct     = image + file_offset + block * 16u;
            const uint32_t at     = dest + block * 16u;
            const uint32_t mask   = fw_word_mask(ct);
            const int expect_zero = (mask == 0u);
            const uint32_t index  = expect_zero ? zero_index++ : data_index++;
            const uint32_t stride = expect_zero ? zero_stride : data_stride;
            uint32_t words[4];
            uint32_t i;
            uint32_t acc = 0u;

            if ((index % stride) != 0u) continue;

            for (i = 0u; i < 4u; ++i) {
                const int rc = fw_read_back(at + i * 4u, &words[i]);
                if (rc != 0) {
                    kv_hex("wifi: the read-back stopped at ", at + i * 4u, 8u);
                    put_str(rc == -2 ? " (the ROM answered for another address)\n"
                                     : " (the ROM stopped answering)\n");
                    stopped = 1;
                    break;
                }
                acc |= words[i];
            }
            if (stopped) break;

            ++checked;
            if (expect_zero) ++zero_seen;
            if ((expect_zero && acc != 0u) || (!expect_zero && acc == 0u)) {
                ++bad;
                if (reported < FW_VERIFY_MAX_REPORT) {
                    ++reported;
                    kv_hex("  ", at, 8u);
                    put_str(expect_zero ? " want zero" : " want data");
                    /* One character per 32-bit unit, first word leftmost: 'D'
                     * where the plaintext is known non-zero, '.' where it is
                     * known zero. If the bad blocks all read D... then only
                     * leading words are going missing. */
                    put_str(" pt=");
                    for (i = 0u; i < 4u; ++i) put_char((mask & (1u << i)) != 0u ? 'D' : '.');
                    put_str(" got=");
                    for (i = 0u; i < 4u; ++i) {
                        put_char(' ');
                        put_hex(words[i], 8u);
                    }
                    put_char('\n');
                    mt6592_usb_gadget_poll();
                }
            }
            if ((checked & 0xffu) == 0u) mt6592_usb_gadget_poll();
        }

        total_bad += bad;

        kv_dec("wifi: s", (int32_t)section);
        kv_hex(" dst=", dest, 8u);
        put_str(is_emi ? " emi " : " rom ");
        kv_dec("checked=", (int32_t)checked);
        kv_dec(" zero=", (int32_t)zero_seen);
        kv_dec(" data=", (int32_t)(checked - zero_seen));
        kv_dec(" of ", (int32_t)data_total);
        kv_dec(" BAD=", (int32_t)bad);
        if (bad > reported) {
            kv_dec(" -- only the first ", (int32_t)reported);
            put_str(" are listed");
        }
        put_char('\n');
        mt6592_usb_gadget_poll();
        if (stopped) {
            put_str("wifi: giving up; the remaining sections were not checked\n");
            return;
        }
    }

    if (total_bad == 0u) {
        put_str("wifi: every sampled block landed decrypted at its destination\n");
    } else {
        kv_dec("wifi: ", (int32_t)total_bad);
        put_str(" blocks did not land as they should have\n");
    }
}

/*
 * `wifi patch` -- put the nine blocks the download refuses to carry.
 *
 * They sit at section offsets 0, 0x20, 0x250, 0x4490, 0, 0xaf40, 0xb600, 0xb620
 * and 0xb6c0, some in the middle of dense runs and some in the middle of zero
 * runs, none on a 2 KiB chunk boundary, so it is not position that loses them.
 * Four of the nine hold addresses inside the firmware's own memory -- two into
 * section 1 and two into EMI just past section 3 -- so they are pointers at the
 * head of the data segment, and losing them is enough on its own to explain a
 * core that takes the jump and dies without writing a byte.
 *
 * What the read-back measured, over 15055 blocks with no exception: a 16-byte
 * block whose four words all decrypt non-zero lands; a block that decrypts
 * wholly to zero is indistinguishable from one never written; and a block that
 * MIXES the two comes back all zero, every word of it, including the words that
 * carry data. The ROM is dropping any block that contains a word decrypting to
 * zero. There are exactly nine such blocks in this image and exactly nine
 * failures, and five of those nine are in EMI, where the read is a plain DRAM
 * load with the ROM nowhere near it -- so this is the copy, not the read.
 *
 * `wifi patch raw` re-sends a block exactly as it appears in the image. That is
 * the control: under the rule above it must fail, and if it succeeds the rule is
 * wrong and everything below is beside the point.
 *
 * `wifi patch` sends a decoy. Each word that would decrypt to zero is replaced
 * by K^0xff, whose every byte differs from the byte that decrypts to zero, so
 * the word cannot decrypt to zero and cannot trigger the skip; the words that
 * carry data keep their own ciphertext at their own offset in the packet, which
 * matters because the position key repeats every sixteen bytes. The block then
 * lands whole, and the decoy words -- which should have been zero -- are written
 * back to zero afterwards through ACCESS_REG for SRAM and a store for EMI.
 * Neither of those goes through the decryption, so neither can be skipped.
 *
 * This needs no knowledge of the cipher, only the two-sided fact that a
 * ciphertext byte equals K exactly when its plaintext byte is zero.
 */
static void cmd_wifi_patch(const char* args) {
    const uint8_t* image;
    /* match_word() anchors at the start of the string and `rest` still carries
     * the separating space, so without this `raw` never matched and the last run
     * took the decoy path twice while reporting that it had run the control. */
    uint32_t len;
    uint32_t count;
    uint32_t section;
    uint32_t resent = 0u;
    uint32_t failed = 0u;
    int raw;

    args = skip_spaces(args);
    raw  = (match_word(args, "raw") != 0);

    if (wifi_rom_ready() != 0) return;

    len = fw_stage("WIFI_RAM_CODE_SOC");
    if (len == 0u) return;
    image = (const uint8_t*)(uintptr_t)FW_SCRATCH_ADDR;

    if (image[0] != 'M' || image[1] != 'T' || image[2] != 'K' || image[3] != 'W') {
        put_str("wifi: that file is not an MTKW container\n");
        return;
    }
    count = fw_le32(image + 8u);
    if (count == 0u || count > 8u || 16u + count * 16u > len) {
        put_str("wifi: the MTKW section table is not usable\n");
        return;
    }

    for (section = 0u; section < count; ++section) {
        const uint8_t* entry       = image + 16u + section * 16u;
        const uint32_t file_offset = fw_le32(entry + 0u);
        const uint32_t size        = fw_le32(entry + 8u);
        const uint32_t dest        = fw_le32(entry + 12u);
        const uint32_t blocks      = size / 16u;
        uint32_t block;

        if (blocks == 0u || file_offset > len || size > len - file_offset) continue;

        for (block = 0u; block < blocks; ++block) {
            const uint8_t* ct   = image + file_offset + block * 16u;
            const uint32_t mask = fw_word_mask(ct);
            const uint32_t at   = dest + block * 16u;
            uint8_t payload[16];
            uint32_t words[4];
            uint32_t i;
            int rc;

            /* Only the partially-zero blocks. 0 is an all-zero block and 0xf is
             * an all-data one; both classes are already known to land. */
            if (mask == 0u || mask == 0xfu) continue;

            for (i = 0u; i < 16u; ++i) {
                const int decoy = raw == 0 && (mask & (1u << (i / 4u))) == 0u;
                payload[i] = decoy ? (uint8_t)(g_fw_zero_block[i] ^ 0xffu) : ct[i];
            }

            rc = mt6592_wifi_hif_rom_download(at, payload, 16u);
            kv_hex("  ", at, 8u);
            put_str(" pt=");
            for (i = 0u; i < 4u; ++i) put_char((mask & (1u << i)) != 0u ? 'D' : '.');
            if (rc != 0) {
                ++failed;
                put_str(" REFUSED by the ROM\n");
                mt6592_usb_gadget_poll();
                continue;
            }
            ++resent;

            /* Read it before touching it: this is the measurement. A decoy send
             * that still reads back all zero means the skip is not keyed on the
             * zero words at all and the rule above is wrong. */
            put_str(" got=");
            for (i = 0u; i < 4u; ++i) {
                if (fw_read_back(at + i * 4u, &words[i]) != 0) {
                    put_str(" <read failed>");
                    words[i] = 0u;
                    continue;
                }
                put_char(' ');
                put_hex(words[i], 8u);
            }

            /* Now take the decoys back out. Nothing here is decrypted, so
             * nothing here can be dropped. */
            if (raw == 0) {
                uint32_t restored = 0u;
                for (i = 0u; i < 4u; ++i) {
                    if ((mask & (1u << i)) != 0u) continue;
                    if (fw_write_back(at + i * 4u, 0u) == 0) ++restored;
                }
                kv_dec(" zeroed=", (int32_t)restored);
            }
            put_char('\n');
            mt6592_usb_gadget_poll();
        }
    }

    kv_dec("wifi: sent ", (int32_t)resent);
    put_str(raw != 0 ? " verbatim" : " decoyed");
    kv_dec(" partial block(s), refused ", (int32_t)failed);
    put_str("; run `wifi verify` to see whether they stuck\n");
}

/* ── Scan ── */

static mt6592_wifi_scan_result g_scan_list[32];
static uint32_t g_scan_count;
static char g_scan_password[65];

static void wifi_print_scan_list(void) {
    uint32_t i;

    if (g_scan_count == 0u) {
        put_str("wifi: no networks in the last scan\n");
        return;
    }
    put_str("wifi: idx  rssi  sec  ssid\n");
    for (i = 0u; i < g_scan_count; ++i) {
        put_str("wifi:  ");
        put_dec((int32_t)i);
        put_str("   ");
        put_dec((int32_t)g_scan_list[i].rssi);
        put_str("   ");
        /* "wpa2", not "wpa". The driver classifies exactly one secure profile --
         * WIFI_SECURITY_WPA2_PSK_CCMP, an RSN IE with CCMP as both the group and
         * a pairwise cipher and PSK as an AKM -- and refuses anything else as
         * WIFI_SECURITY_UNSUPPORTED before the join starts. So a row that reaches
         * this list with encrypted set is WPA2-PSK/CCMP and nothing else, and the
         * old label read as though the driver had settled for WPA1. */
        put_str(g_scan_list[i].encrypted != 0u ? "wpa2" : "open");
        put_str("  ");
        put_str(g_scan_list[i].ssid);
        put_char('\n');
        mt6592_usb_gadget_poll();
    }
}

static void wifi_print_statistics(void);

/* The two plain-English lines -- LINK and RADIO -- that every wifi command ends
 * with. Defined further down beside the join state it reads; declared here
 * because the scan wants it too. */
static void wifi_print_link_summary(void);

/* Ahead of cmd_wifi_scan as well as cmd_wifi_join: a scan run while associated
 * reports the link's phase afterwards, which is the only way to tell a sweep
 * that kept the connection from one that quietly dropped it. */
static const char* wifi_assoc_phase_name(uint32_t phase) {
    switch (phase) {
    case WIFI_ASSOC_IDLE:           return "idle";
    case WIFI_ASSOC_WAIT_CHANNEL:   return "waiting for the firmware's channel grant";
    case WIFI_ASSOC_WAIT_AUTH:      return "waiting for the AP's authentication reply";
    case WIFI_ASSOC_WAIT_ASSOC:     return "waiting for the AP's association response";
    case WIFI_ASSOC_WAIT_EAPOL_M1:  return "waiting for EAPOL message 1 of 4";
    case WIFI_ASSOC_WAIT_EAPOL_M3:  return "waiting for EAPOL message 3 of 4";
    case WIFI_ASSOC_CONNECTED:      return "connected";
    case WIFI_ASSOC_FAILED:         return "failed";
    default:                        return "unknown";
    }
}

static void cmd_wifi_scan(const char* args) {
    const mt6592_wifi_hif_state* hif = mt6592_wifi_hif_get_state();
    uint32_t waited = 0u;
    int active = 0;
    int was_associated;

    if (match_word(args, "active") != 0) {
        active = 1;
    } else if (*args != '\0' && match_word(args, "passive") == 0) {
        put_str("usage: wifi scan [passive|active]\n");
        return;
    }

    if (!hif->firmware_alive || !hif->configured) {
        put_str("wifi: firmware is not alive; run `wifi fw` first\n");
        return;
    }

    /* Recorded before the sweep so the report afterwards can say whether the
     * link survived it. Rescanning while connected is the whole point -- it is
     * how anyone changes network or notices they have walked out of range -- and
     * "did that cost me the connection?" is the only question that makes it
     * usable or not. */
    was_associated = hif->associated;
    if (was_associated) {
        put_str("wifi: scanning while associated; the link is expected to survive\n");
    }

    /*
     * Two things happen before the command goes out, and both are here because
     * the first scan ever submitted against a live firmware took the board off
     * the bus with no exception report, no watchdog reset and -- the second
     * time -- not even the breadcrumb that precedes the submit.
     *
     * The charger kick is the same guard intel_wifi_start_scan() has always
     * had and this path never did: a scan steps the draw up sharply, there is
     * no cell fitted, and mt6592_pmic_power_hold()'s whole premise is that
     * acting after the load arrives is too late.
     *
     * The watchdog is armed *before* the submit rather than after it, because
     * the second death was inside mt6592_wifi_hif_start_scan() itself. An AHB
     * read of a hung CONSYS bus stalls the core without taking an exception,
     * and a stall in an unarmed window is silent forever; armed, it is a reset,
     * which is at least an observation.
     */
    put_str(active ? "wifi: submitting ACTIVE scan (transmits)\n"
                   : "wifi: submitting PASSIVE scan (listen only)\n");
    flush_line();

    mt6592_pmic_power_hold();

    wdt_arm(0x1eu);
    if (mt6592_wifi_hif_start_scan(active) != 0) {
        wdt_disarm();
        put_str("wifi: scan request refused: ");
        put_str(hif->blocked != 0 ? hif->blocked : "(no reason given)");
        put_char('\n');
        return;
    }

    put_str("wifi: scan submitted; polling\n");
    flush_line();

    /*
     * The driver's own SCAN_TIMEOUT_US is 12 s and mt6592_wifi_hif_poll() is
     * what enforces it, so this loop must keep calling poll past the point where
     * it looks stuck -- returning early would leave scan_active set and make the
     * next `wifi scan` refuse. 15 s is that ceiling plus room for a slow reply.
     *
     * The per-second report used to be a single dot. It is a state line now,
     * because the first scan ever run against a live firmware took the board
     * down somewhere between "scanning" and the first dot, and a dot cannot say
     * whether it died acquiring ownership, reading a packet out of the HIF or
     * inside a handler for one. Every number here is a counter the poll itself
     * maintains, so the last line to arrive brackets the fault to one second of
     * work -- and if the fault is a data abort, the exception report now follows
     * it out of the same pipe.
     *
     * The watchdog is already armed and stays armed across this loop; wdt_kick()
     * at the bottom is what keeps it from firing on a scan that is merely slow.
     */
    while (waited < 15000u) {
        /*
         * TEN LINES IN THE FIRST SECOND, ONE A SECOND AFTER THAT.
         *
         * A scan run straight after a failed join wedged the board with not one
         * report line printed, which under the old one-second cadence bracketed
         * the death to "somewhere in the first fifty polls" -- and fifty polls is
         * the whole interesting part of this loop. The 100 ms window costs nine
         * extra lines on a healthy sweep, which finishes in 1341 ms anyway, and
         * buys the difference between "it died in the first second" and "it died
         * on poll three".
         *
         * Every line also lands in the eMMC console ring, so a stall that takes
         * the board off the USB bus still leaves its last breadcrumb behind for
         * `flash -mtk-read-boot-status`.
         */
        const uint32_t report = (waited < 1000u) ? 100u : 1000u;

        (void)mt6592_wifi_hif_poll();
        if (!hif->scan_active) break;
        delay_ms(20u);
        waited += 20u;
        if ((waited % report) == 0u) {
            kv_dec("wifi: +", (int32_t)waited);
            kv_hex("ms whisr=", hif->last_whisr, 8u);
            kv_hex(" wrplr=", hif->last_wrplr, 8u);
            kv_dec(" rx=", (int32_t)hif->rx_packets);
            kv_dec(" mgmt=", (int32_t)hif->rx_management);
            kv_dec(" evt=", (int32_t)hif->rx_events);
            kv_dec(" drop=", (int32_t)hif->dropped_packets);
            kv_dec(" found=", (int32_t)hif->scan_result_count);
            put_char('\n');
            flush_line();
        }
        wdt_kick();
    }
    wdt_disarm();

    if (hif->scan_active) put_str("wifi: scan did not finish; showing whatever arrived\n");

    /*
     * Say how the scan ended, because until now this command could not.
     *
     * scan_active is cleared from four places in the driver and the loop above
     * breaks on all four identically, so every one of them printed "scan found
     * 0 networks" -- a true sentence for exactly one of them. The last two runs
     * were read as "the scan completed and heard nothing" on that basis, and
     * the first of them cannot have been: no event of any kind arrived, WHISR
     * never raised anything, and it ended nowhere near the 12 s timeout, which
     * leaves only the ownership handshake failing 250 ms at a time.
     */
    put_str("wifi: scan ended: ");
    switch (hif->scan_end_reason) {
    case SCAN_END_DONE:
        put_str("SCAN_DONE from the firmware -- it looked and this count is real");
        break;
    case SCAN_END_OWN_LOST:
        put_str("DRIVER OWNERSHIP LOST -- the scan was abandoned, not completed");
        break;
    case SCAN_END_ABNORMAL:
        put_str("WHISR abnormal interrupt -- the HIF faulted mid-scan");
        break;
    case SCAN_END_TIMEOUT:
        put_str("12 s timeout with no SCAN_DONE -- the firmware never reported back");
        break;
    default:
        put_str("still running (the console gave up first)");
        break;
    }
    kv_dec(" own_lost=", (int32_t)hif->driver_own_lost);
    put_char('\n');

    /* Which events arrived during THIS scan, by ID and in order. 0x15 is
     * SCAN_DONE, 0x04 a legacy scan result, 0x01 a command result. An empty
     * list means the firmware said nothing at all about the request. */
    put_str("wifi: scan events:");
    if (hif->event_ids_used == 0u) {
        put_str(" (none -- the firmware never answered the scan request)");
    } else {
        for (uint32_t i = 0; i < hif->event_ids_used; ++i) kv_hex(" ", hif->event_ids[i], 2u);
    }
    put_char('\n');
    if (hif->scan_done_events != 0u) {
        kv_dec("wifi: scan_done events=", (int32_t)hif->scan_done_events);
        kv_hex(" seq=", hif->scan_done_seq, 2u);
        kv_hex(" wanted=", hif->scan_done_seq_want, 2u);
        put_str(hif->scan_done_seq == hif->scan_done_seq_want
                    ? " (accepted)\n"
                    : " (MISMATCH -- the completion was discarded)\n");
        /*
         * DID THE RECEIVER ACTUALLY RUN?
         *
         * Everything above this line describes the command path, and the command
         * path has been working for several sessions: the request is accepted and
         * the completion comes back with the right sequence byte. What none of it
         * says is whether anything was switched on between the two. "found 0
         * networks" is compatible with a radio that swept the band in a quiet room
         * and with a radio that never left the bench, and those want opposite
         * fixes.
         *
         * The firmware answers that itself, in the same event. Stock's
         * scnEventScanDone reads payload[1] as a validity flag and payload[2..3]
         * as the least-busy channel it measured -- and a quietest channel is a
         * comparison, so it cannot exist unless the receiver listened on several.
         * sparse=yes with a 2.4 GHz channel number means the sweep happened and
         * the room was empty of anything this build can hear. sparse=no means it
         * did not happen, and every hypothesis about beacons, filters and channel
         * lists is downstream of a radio that is off.
         *
         * The elapsed time is the same question asked a second way: fourteen
         * channels at one beacon interval each is seconds, not milliseconds.
         *
         * raw= is the whole payload head, unread, so that a field mapping which
         * turns out to be off by one is recoverable from this log instead of
         * costing another flash cycle.
         */
        put_str("wifi: scan_done sparse=");
        put_str(hif->scan_done_sparse_valid ? "yes" : "NO -- the firmware reports no channel survey");
        if (hif->scan_done_sparse_valid) {
            kv_dec(" band=", hif->scan_done_sparse_band);
            kv_dec(" quietest_ch=", hif->scan_done_sparse_channel);
        }
        kv_dec(" payload_len=", (int32_t)hif->scan_done_payload_len);
        kv_dec(" elapsed_ms=", (int32_t)hif->scan_elapsed_ms);
        put_str(" raw=");
        for (uint32_t i = 0; i < 8u; ++i) kv_hex("", hif->scan_done_raw[i], 2u);
        put_char('\n');
    }
    if (hif->blocked != 0) { put_str("wifi: blocked: "); put_str(hif->blocked); put_char('\n'); }
    if (hif->status != 0)  { put_str("wifi: status:  "); put_str(hif->status);  put_char('\n'); }
    flush_line();

    g_scan_count = mt6592_wifi_hif_get_scan_results(g_scan_list, 32u);
    if (g_scan_count > 32u) g_scan_count = 32u;
    {
        /* An SSID is 32 bytes of whatever the beacon carried, and the field is
         * 33 bytes so that it can be a C string -- but the byte that makes it
         * one comes from the beacon's length, not from a guarantee. Terminate
         * it here rather than trusting the air. */
        uint32_t i;
        for (i = 0u; i < g_scan_count; ++i) g_scan_list[i].ssid[32] = '\0';
    }
    kv_dec("wifi: scan found ", (int32_t)g_scan_count);
    put_str(" networks\n");
    wifi_print_scan_list();

    wifi_print_link_summary();
    if (was_associated) {
        hif = mt6592_wifi_hif_get_state();
        if (!hif->associated) {
            /* Do not blame the sweep without evidence. Nothing polls the HIF
             * while the console sits at its prompt, so a deauthentication that
             * arrived seconds earlier is only *processed* here, on the first
             * poll this scan performs -- which made every idle-time disconnect
             * look like the scan had caused it. When the AP said why, that is a
             * different failure with a different fix, so print the reason
             * instead of the guess. */
            if (hif->disconnect_reason != 0u || hif->disconnect_was_deauth) {
                kv_dec(hif->disconnect_was_deauth
                       ? "wifi: the AP deauthenticated us, reason "
                       : "wifi: the AP disassociated us, reason ",
                       (int32_t)hif->disconnect_reason);
                put_str("\nwifi: that frame was queued in the firmware and only read out\n");
                put_str("wifi: now, so it may predate this scan -- rejoin with `wifi join`.\n");
            } else {
                put_str("wifi: the sweep cost the association. The firmware went off-channel\n");
                put_str("wifi: and did not come back to it -- rejoin with `wifi join`.\n");
            }
        }
    }

    /* Taken here, immediately after the sweep, because the counters are only
     * evidence about the sweep that just happened. Asking later is asking about
     * a different interval. */
    wifi_print_statistics();
}

/*
 * THE FIRMWARE'S OWN RECEIVE COUNTERS -- the question the scan cannot answer.
 *
 * The sparse-channel report settled that the sweep runs: fourteen channels, a
 * beacon interval each, and a nominated quietest channel at the end. What it
 * cannot settle is whether anything is arriving to be heard. Zero beacons at the
 * host is produced identically by a receiver in a silent room, a receiver whose
 * analogue path is dead, and a receiver whose frames are being demodulated and
 * then filtered away before they reach the HIF. Three faults, three different
 * fixes, one indistinguishable symptom.
 *
 * FCSErrorCount separates them, and it is the only number on either side of this
 * interface that can. An FCS error is a frame that was received and demodulated
 * and turned out to be corrupt. A radio that is off, mistuned, or not connected
 * to an antenna cannot produce one; there is nothing to fail a checksum on. So:
 *
 *   fcs_errors > 0                  the analogue path works and the room is not
 *                                   silent. The fault is above the PHY.
 *   rx_fragments > 0, mgmt still 0  frames are being received and counted and
 *                                   not forwarded -- a filtering fault.
 *   both zero                       nothing reaches the demodulator at all, and
 *                                   every host-side theory about channel lists,
 *                                   beacon parsing and RX ports is moot.
 *
 * The transmit counters are printed beside them because they cost nothing and
 * they are the control: a passive scan should move none of them.
 */
static void wifi_print_statistics(void) {
    static const char* const k_names[12] = {
        "tx_fragments", "tx_multicast", "tx_failed",   "tx_retry",
        "tx_multiretry","rts_ok",       "rts_failed",  "ack_failed",
        "rx_duplicate", "rx_fragments", "rx_multicast","rx_fcs_errors",
    };
    const mt6592_wifi_hif_state* hif;

    if (mt6592_wifi_hif_query_statistics() != 0) {
        hif = mt6592_wifi_hif_get_state();
        put_str("wifi: stats: no answer to CID 130");
        kv_dec(" (polls=", (int32_t)hif->stats_polls);
        put_str(")\n");
        return;
    }
    hif = mt6592_wifi_hif_get_state();
    kv_hex("wifi: stats event=", hif->stats_event_id, 2u);
    kv_dec(" payload_len=", (int32_t)hif->stats_payload_len);
    kv_dec(" polls=", (int32_t)hif->stats_polls);
    put_char('\n');
    for (uint32_t i = 0u; i < 12u; ++i) {
        if ((i & 3u) == 0u) put_str("wifi:  ");
        put_char(' ');
        put_str(k_names[i]);
        kv_dec("=", (int32_t)hif->stats_lo[i]);
        if (hif->stats_hi[i] != 0u) kv_hex("+hi", hif->stats_hi[i], 8u);
        if ((i & 3u) == 3u) put_char('\n');
    }
    flush_line();
    put_str("wifi: ");
    if (hif->stats_lo[11] != 0u || hif->stats_hi[11] != 0u) {
        /* This used to read "RF and antenna are live", which claims more than
         * the counter can support. An FCS error is a frame the demodulator ran
         * to completion and then failed to checksum -- and noise that trips
         * preamble detection produces exactly that, with no transmitter
         * anywhere near. What the count proves is that the PHY is powered,
         * clocked and running; whether it is hearing real beacons badly or
         * hearing nothing loudly is the next question, not this one's answer.
         *
         * rx_fragments = 0 alongside it is likewise not proof of a filter
         * fault: it may well be a post-address-filter counter, in which case
         * zero is the correct reading for an unassociated station. */
        put_str("the PHY is RUNNING (frames demodulated, all failing FCS) -- but an FCS\n");
        put_str("wifi: error can equally be noise tripping preamble detect, so this does\n");
        put_str("wifi: not yet prove a real transmitter is being heard\n");
    } else if (hif->stats_lo[9] != 0u || hif->stats_hi[9] != 0u) {
        put_str("frames passed FCS and reached the counter -- if none reached the host,\n");
        put_str("wifi: the fault is above the PHY\n");
    } else {
        put_str("nothing reached the demodulator: no frames, not even corrupt ones\n");
    }
}

/*
 * `wifi mcr <addr> [value]` -- the only way to see inside the chip after start.
 *
 * `wifi rd`/`wifi dump` die at the WIFI_START handoff along with the boot ROM's
 * register window, and everything still unexplained about the scan happens after
 * that point. CID 194 goes through the firmware instead, so it keeps working.
 *
 * With no value it reads; with one it writes and then reads back through the
 * same command, so the printed result is what the register holds rather than
 * what was requested. Writes go straight to a live radio -- there is no address
 * validation here and none in the firmware worth relying on.
 */
static void cmd_wifi_mcr(const char* args) {
    uint32_t address = 0u;
    uint32_t value   = 0u;
    uint32_t readback = 0u;
    int have_value;

    if (parse_u32(&args, &address) != 0) {
        put_str("usage: wifi mcr <addr> [value]   (read, or write then read back)\n");
        return;
    }
    have_value = (parse_u32(&args, &value) == 0);

    if (have_value != 0) {
        if (mt6592_wifi_hif_mcr(address, &value, 0) != 0) {
            kv_hex("wifi: mcr write ", address, 8u);
            put_str(" -- no acknowledgement (firmware not started, or wedged)\n");
            return;
        }
    }
    if (mt6592_wifi_hif_mcr(address, 0, &readback) != 0) {
        kv_hex("wifi: mcr read ", address, 8u);
        put_str(" -- no answer (firmware not started?)\n");
        return;
    }
    kv_hex("wifi: mcr ", address, 8u);
    kv_hex(" = ", readback, 8u);
    if (have_value != 0 && readback != value) {
        kv_hex("  (wrote ", value, 8u);
        put_str(", did not stick)");
    }
    put_char('\n');
}

/*
 * `wifi join <index|ssid>`.
 *
 * The polling loop is the substance of this. mt6592_wifi_hif_auth_associate()
 * only *starts* the state machine -- it sends the STA record and the channel
 * request and returns -- and every step after that is driven by frames arriving
 * in mt6592_wifi_hif_poll(). This command used to return the moment the start
 * call did, which is why it reported `associate rc=0` and then, one line later,
 * `assoc=no`: nothing had gone wrong, nothing had been given the chance to go
 * right either. Five distinct waits sat behind that one `rc=0` and none of them
 * were ever pumped.
 *
 * Same 20 ms cadence and per-second report as `wifi scan`, for the same reason:
 * the phase name says which of the five steps is stuck, and rx/mgmt say whether
 * anything is being heard while it waits. A channel grant that never arrives is
 * a firmware problem; an authentication reply that never arrives with mgmt
 * frames still climbing is the AP declining to answer us specifically.
 *
 * The ceiling is 20 s: the driver's own retry ladder is three attempts at each
 * of a 1.5 s association timeout plus the channel and authentication waits, and
 * cutting the loop short would leave auth_active set and make the next attempt
 * look like a driver fault.
 */
/* The network the last successful join landed on. The driver does not publish an
 * associated SSID, and "associated=1" without a name is not an answer anybody can
 * use. Cleared whenever the link is not up, so it can never name a stale one. */
static char g_wifi_joined_ssid[33];

/*
 * TWO LINES THAT ANSWER THE TWO QUESTIONS ANYONE ACTUALLY HAS.
 *
 * Everything else this file prints about Wi-Fi is twenty-five lines of register
 * state, and the answer to "am I on the network" is one flag buried three
 * screens in, spelled `assoc=no`, next to sixteen other things spelled the same
 * way. That is not a readable instrument -- it was read as a link being up more
 * than once this week, and the console had all the information both times.
 *
 * So say it first and say it in words. And say the second thing too, because
 * "not joined" and "the radio stopped answering" look identical from the link
 * line alone and want completely different next moves: one is a join to retry,
 * the other is a firmware to reload. The last scan's end reason separates them
 * for free -- a timeout with no events at all is a firmware that is not there
 * any more, whatever its `fw_alive` flag still says.
 */
static void wifi_print_link_summary(void) {
    const mt6592_wifi_hif_state* h = mt6592_wifi_hif_get_state();

    if (!h->associated) g_wifi_joined_ssid[0] = '\0';

    put_str("wifi: LINK:  ");
    if (h->associated) {
        put_str("JOINED to ");
        put_str(g_wifi_joined_ssid[0] != '\0' ? g_wifi_joined_ssid : "(name not recorded)");
        put_str(h->secure ? ", encrypted" : ", open");
        put_str(h->data_path_ready ? ", data path up" : ", NO DATA PATH YET");
    } else if (h->assoc_phase != WIFI_ASSOC_IDLE && h->assoc_phase != WIFI_ASSOC_FAILED &&
               h->assoc_phase != WIFI_ASSOC_CONNECTED) {
        put_str("NOT JOINED -- an attempt is still running: ");
        put_str(wifi_assoc_phase_name(h->assoc_phase));
    } else if (h->assoc_phase == WIFI_ASSOC_FAILED) {
        put_str("NOT JOINED -- the last attempt failed");
    } else {
        put_str("NOT JOINED");
    }
    put_char('\n');

    put_str("wifi: RADIO: ");
    if (!h->firmware_loaded) {
        put_str("no firmware -- run `wifi fw`");
    } else if (h->scan_end_reason == SCAN_END_TIMEOUT && h->event_ids_used == 0u) {
        put_str("LOADED BUT NOT ANSWERING. The last scan went out cleanly and the\n");
        put_str("wifi:        firmware said nothing back at all -- not a completion, not an\n");
        put_str("wifi:        event. Nothing below this line will work until it is reloaded\n");
        put_str("wifi:        with `wifi fw`, and any test run in this state measures nothing");
    } else {
        put_str("answering");
        kv_dec("; last scan found ", (int32_t)h->scan_result_count);
        put_str(h->scan_result_count == 1u ? " network" : " networks");
    }
    put_char('\n');
}

/* Is the firmware still talking? The one question every other Wi-Fi test has to
 * be able to answer before its own result means anything. */
static int wifi_firmware_answering(void) {
    const mt6592_wifi_hif_state* h = mt6592_wifi_hif_get_state();
    if (!h->firmware_loaded) return 0;
    if (h->scan_end_reason == SCAN_END_TIMEOUT && h->event_ids_used == 0u) return 0;
    return 1;
}

/* Resolve an index-or-SSID argument against the console's copy of the scan list.
 * Shared by `wifi join` and `wifi step`, which have to accept the same thing. */
static const char* wifi_pick_target(const char* args, int* encrypted_out) {
    uint32_t idx = 0u;
    if (parse_u32(&args, &idx) == 0 && *skip_spaces(args) == '\0' && idx < g_scan_count) {
        if (encrypted_out != 0) *encrypted_out = g_scan_list[idx].encrypted != 0u;
        return g_scan_list[idx].ssid;
    }
    {
        const char* typed = skip_spaces(args);
        for (uint32_t i = 0u; i < g_scan_count; ++i) {
            if (same_text(g_scan_list[i].ssid, typed)) {
                if (encrypted_out != 0) *encrypted_out = g_scan_list[i].encrypted != 0u;
                return g_scan_list[i].ssid;
            }
        }
    }
    return 0;
}

/*
 * WHICH OF THE TWO COMMANDS SILENCES THE FIRMWARE.
 *
 * A join puts UPDATE_STA_RECORD and CH_PRIVILEGE on the wire back to back and
 * the firmware stops answering across the pair -- rx frozen, no channel grant,
 * and from then on scans submit cleanly and are answered by nothing. Two
 * suspects, one symptom, and no way to separate them while they always travel
 * together.
 *
 * This sends them one at a time and polls a full second between, printing the
 * counters either side. The firmware being alive after step 1 and dead after
 * step 2 names the culprit outright; both surviving moves the fault to what
 * comes after them, which is a different search. Either way the next step stops
 * being a guess.
 */
static void cmd_wifi_step(const char* args) {
    static const char* const kStepName[3] = { "UPDATE_STA_RECORD", "CH_PRIVILEGE request",
                                              "CH_PRIVILEGE release" };
    const mt6592_wifi_hif_state* hif;
    const char* ssid;

    args = skip_spaces(args);
    if (*args == '\0') {
        put_str("usage: wifi step <index from `wifi list`|ssid>\n");
        return;
    }
    ssid = wifi_pick_target(args, 0);
    if (ssid == 0) {
        put_str("wifi: no scanned network by that name; `wifi scan` then `wifi list`\n");
        return;
    }

    /*
     * REFUSED ON A DEAD FIRMWARE, because the first run of this test was wasted
     * exactly that way. All three commands went out, nothing moved after any of
     * them, and the result read as "all three are fine" -- except rx had already
     * been frozen at 28 BEFORE step 0. Nothing was measured. A test whose whole
     * output is "did the counters move" is worthless if they were not moving
     * when it started, so check that first and say so.
     */
    if (!wifi_firmware_answering()) {
        wifi_print_link_summary();
        put_str("wifi: refusing to step: the firmware is not answering, so every step\n");
        put_str("wifi: would report 'nothing moved' whether or not it was to blame.\n");
        put_str("wifi: Reload with `wifi fw`, get ONE clean `wifi scan` that finds\n");
        put_str("wifi: networks, and step immediately after that.\n");
        return;
    }

    put_str("wifi: stepping the association commands against ");
    put_str(ssid);
    put_str(" one at a time\n");
    flush_line();

    for (int step = 0; step < 3; ++step) {
        int rc;
        hif = mt6592_wifi_hif_get_state();
        kv_dec("wifi: before step ", (int32_t)step);
        kv_dec(" rx=", (int32_t)hif->rx_packets);
        kv_dec(" mgmt=", (int32_t)hif->rx_management);
        kv_dec(" evt=", (int32_t)hif->rx_events);
        put_char('\n');

        put_str("wifi: sending ");
        put_str(kStepName[step]);
        put_char('\n');
        flush_line();

        /* Only the SSID on the first step: steps 1 and 2 must act on the profile
         * step 0 already selected, and re-selecting would be fine but re-reading
         * the scan table would not if a scan has since wiped it. */
        rc = mt6592_wifi_hif_assoc_probe(step == 0 ? ssid : "", step);
        if (rc != 0) {
            hif = mt6592_wifi_hif_get_state();
            put_str("wifi: the command did not go out: ");
            put_str(hif->blocked != 0 ? hif->blocked : "(no reason given)");
            put_char('\n');
            return;
        }

        wdt_arm(20u);
        for (uint32_t waited = 0u; waited < 1000u; waited += 20u) {
            (void)mt6592_wifi_hif_poll();
            delay_ms(20u);
            wdt_kick();
        }
        wdt_disarm();

        hif = mt6592_wifi_hif_get_state();
        kv_dec("wifi: after  step ", (int32_t)step);
        kv_dec(" rx=", (int32_t)hif->rx_packets);
        kv_dec(" mgmt=", (int32_t)hif->rx_management);
        kv_dec(" evt=", (int32_t)hif->rx_events);
        kv_hex(" whisr_seen=", hif->whisr_seen, 8u);
        put_char('\n');
        flush_line();
    }

    put_str("wifi: stepping done. The step after which rx/evt stop moving is the\n");
    put_str("wifi: one the firmware does not survive. Follow with `wifi scan`: a\n");
    put_str("wifi: sweep that still completes means nothing here wedged it.\n");
    wifi_print_link_summary();
}

/* Defined with the rest of the firmware-log reader, below. Declared here because
 * a join now bookmarks the log before it starts and prints the delta after it
 * ends -- see wifi_log_delta(). */
static uint32_t wifi_log_count(void);
static void wifi_log_delta(const char* what, uint32_t before);

static void cmd_wifi_join(const char* args) {
    const mt6592_wifi_hif_state* hif = mt6592_wifi_hif_get_state();
    const char* ssid = 0;
    uint32_t waited = 0u;
    uint32_t last_phase = 0xffffffffu;
    uint32_t log_mark = 0u;
    int encrypted = 0;
    int rc;

    args = skip_spaces(args);
    if (*args == '\0') {
        put_str("usage: wifi join <index from `wifi list`|ssid>\n");
        return;
    }

    /* An index if it parses as one and names a row; otherwise the rest of the
     * line is an SSID. Taken literally including spaces, because "IZZI-1CA3"
     * and "Club Totalplay WiFi" are both things people have to type and only
     * one of them survives tokenising. */
    ssid = wifi_pick_target(args, &encrypted);
    if (ssid == 0) {
        put_str("wifi: no scanned network by that name; `wifi scan` then `wifi list`\n");
        return;
    }

    if (encrypted && g_scan_password[0] == '\0') {
        put_str("wifi: that network is encrypted; set `wifi pass <secret>` first\n");
        return;
    }

    put_str("wifi: associating with ");
    put_str(ssid);
    put_char('\n');
    flush_line();

    /*
     * ── STEP MARKERS, BECAUSE THIS COMMAND HAS TAKEN THE BOARD OFF THE BUS ──
     *
     * Measured: `wifi join IZZI-1CA3' printed "associating with" and the next
     * thing the host saw was LIBUSB_ERROR_IO -- the board gone. Not one line
     * from the poll loop below, and that loop prints on its FIRST pass (the
     * phase always differs from the 0xffffffff seed), so the board never
     * reached it. Everything between the line above and the loop is therefore
     * suspect and none of it says anything, which is why a whole run buys one
     * bit of information.
     *
     * Four markers, one per thing that can hang or reset here. They cost four
     * lines of a transcript the operator is already reading and they turn the
     * next failure from "somewhere in the join" into a named step.
     *
     * WHAT TO SUSPECT, in the order these print. The scan path does the first
     * two constantly and survives, so they start as controls, not suspects:
     * key derivation is the first thing a join does that a scan never does
     * (8192 HMAC-SHA1, and it is the reason mt6592_wifi_pbkdf2_sha1 takes a
     * yield at all), and the wire is the first TRANSMIT this radio has ever
     * been asked for -- `wifi: stats' reported tx_fragments=0 for the whole
     * session, both scans having been passive. A cell-less board and a charger
     * that cannot sink is exactly the supply a transmit burst steps on.
     */
    put_str("wifi: step 1/4 bookmarking the firmware log\n");
    flush_line();
    log_mark = wifi_log_count();

    /* Same charger kick as the scan path: association transmits, and there is
     * no cell fitted to absorb the step. */
    put_str("wifi: step 2/4 opening the charger input path\n");
    flush_line();
    mt6592_pmic_power_hold();

    put_str("wifi: step 3/4 deriving the key and requesting the channel\n");
    flush_line();
    wdt_arm(0x1eu);
    rc = mt6592_wifi_hif_auth_associate(ssid, g_scan_password);
    put_str("wifi: step 4/4 on the wire; waiting on the firmware\n");
    flush_line();
    if (rc != 0) {
        wdt_disarm();
        put_str("wifi: association refused: ");
        put_str(hif->blocked != 0 ? hif->blocked : "(no reason given)");
        put_char('\n');
        wifi_log_delta("refused join", log_mark);
        return;
    }

    while (waited < 20000u) {
        (void)mt6592_wifi_hif_poll();
        /*
         * REFETCHED EVERY ITERATION, and the pointer being stable is exactly why
         * that is not obvious. assoc_phase is a mirror that get_state() refreshes
         * ON READ -- so holding the pointer from before the association started
         * and dereferencing it in the loop returned the phase as it was BEFORE
         * `associate` ran, forever. That is IDLE, which is one of the terminal
         * values here, so the loop broke on its first pass and printed
         * "association ended: idle" while the state machine was still sitting in
         * wait-channel. Nothing was wrong with the association; the console was
         * reading a snapshot and calling it a live value.
         *
         * It also latched auth_active on, because the pump that reaps a stalled
         * attempt only runs inside poll() and nothing polled after this returned
         * -- which is where the permanent "scan-association-active" came from.
         * One stale read, two symptoms, neither of them in the radio.
         */
        hif = mt6592_wifi_hif_get_state();
        if (hif->assoc_phase == WIFI_ASSOC_CONNECTED || hif->assoc_phase == WIFI_ASSOC_FAILED ||
            hif->assoc_phase == WIFI_ASSOC_IDLE) {
            break;
        }
        /* Report on a phase change immediately and otherwise once a second, so a
         * handshake that walks all five steps in 200 ms still shows its work. */
        if (hif->assoc_phase != last_phase || (waited % 1000u) == 0u) {
            last_phase = hif->assoc_phase;
            kv_dec("wifi: +", (int32_t)(waited / 1000u));
            put_str("s ");
            put_str(wifi_assoc_phase_name(hif->assoc_phase));
            kv_dec(" retries=", (int32_t)hif->assoc_retries);
            kv_dec(" rx=", (int32_t)hif->rx_packets);
            kv_dec(" mgmt=", (int32_t)hif->rx_management);
            kv_dec(" evt=", (int32_t)hif->rx_events);
            /* THE counter this loop exists to show. rx is the total and mgmt/evt
             * are subsets, so `rx == mgmt + evt' at every sample of a failed WPA2
             * join says no data frame arrived -- and EAPOL-M1 is a data frame.
             * That was true of every line of the last transcript, but only
             * derivable by subtracting, which is how it went unnoticed for a whole
             * session. process_data_packet() has counted this since it was
             * written; nothing printed it. */
            kv_dec(" data=", (int32_t)hif->rx_data);
            kv_dec(" drop=", (int32_t)hif->dropped_packets);
            put_char('\n');
            flush_line();
        }
        delay_ms(20u);
        waited += 20u;
        wdt_kick();
    }
    wdt_disarm();

    hif = mt6592_wifi_hif_get_state();
    put_str("wifi: association ended: ");
    put_str(wifi_assoc_phase_name(hif->assoc_phase));
    if (hif->assoc_phase != WIFI_ASSOC_CONNECTED && hif->assoc_phase != WIFI_ASSOC_FAILED &&
        hif->assoc_phase != WIFI_ASSOC_IDLE) {
        put_str(" (still running -- the console gave up first)");
    }
    put_char('\n');
    put_str("wifi: ");
    put_str(hif->status != 0 ? hif->status : "(no status)");
    if (hif->blocked != 0) {
        put_str(" [");
        put_str(hif->blocked);
        put_str("]");
    }
    put_char('\n');
    kv_dec("wifi: associated=", hif->associated);
    kv_dec(" secure=", hif->secure);
    kv_dec(" data_path=", hif->data_path_ready);
    /* 0xfe means the station record was never activated -- see the field's note
     * in mt6592_wifi_hif.h. Anything else is the index the firmware handed us. */
    kv_hex(" sta_rec=", hif->sta_rec_index, 2u);
    /* Nonzero means message 4's TX-status event never came and the key install
     * fell back to racing the queue. See the field's note in mt6592_wifi_hif.h. */
    kv_dec(" m4_unconfirmed=", (int32_t)hif->m4_tx_done_timeouts);
    put_char('\n');

    /*
     * BEFORE THE ABORT, because the abort sends a CH_PRIVILEGE release and the
     * firmware may well log that too -- and a release's records mixed into the
     * request's would be indistinguishable from them. This delta is the join and
     * only the join.
     */
    wifi_log_delta("join", log_mark);

    /*
     * A JOIN THAT DID NOT JOIN MUST COST NOTHING BEYOND THE JOIN.
     *
     * Reported first, abandoned second, and that order is the point: the status
     * line above is the actual reason the attempt ended, and dropping the
     * attempt overwrites it. Print the diagnosis, then clean up.
     *
     * This is here because nothing else can do it. Once this command returns,
     * no code calls poll() until the next command, so the state machine is
     * frozen mid-step -- alive as far as the driver is concerned, and claiming
     * the radio -- with no clock running to ever time it out. This loop is the
     * association's only owner, so giving up has to be said, not assumed.
     */
    if (!hif->associated) {
        if (mt6592_wifi_hif_abort_association() != 0) {
            put_str("wifi: attempt dropped; scanning is available again\n");
        }
        /*
         * Said here because the command that hits it cannot say it.
         *
         * A `wifi scan` run after a failed join has twice taken the board off
         * the USB bus mid-loop, which looks from the terminal like the console
         * simply stopped. It did not: every line it prints is written to the
         * eMMC console ring first, so the lines that never made it down the wire
         * are still on the card.
         */
        put_str("wifi: if the next command stops printing, the board stalled -- power\n");
        put_str("wifi: cycle and run `flash -mtk-read-boot-status`; the console ring on\n");
        put_str("wifi: eMMC has the lines that never made it down the wire.\n");
    } else {
        /* Recorded here and nowhere else: the driver has no associated-SSID
         * field, and a link line that cannot name the network is half an
         * answer. */
        uint32_t i = 0u;
        while (i + 1u < sizeof(g_wifi_joined_ssid) && ssid[i] != '\0') {
            g_wifi_joined_ssid[i] = ssid[i];
            ++i;
        }
        g_wifi_joined_ssid[i] = '\0';
    }
    wifi_print_link_summary();
}

/* ── CONSYS / Wi-Fi ──
 *
 * The Wi-Fi driver keeps its own opinion of how far bring-up got, in two state
 * structs, and those structs are exactly as trustworthy as the driver -- which
 * is the thing under test. So this prints both: the opinion, and the registers
 * the opinion is about, read through this file's own copies of the addresses per
 * the note at the top. Where the two disagree is where to look.
 *
 * Everything here is reachable from LK, which is the point. Wi-Fi belongs to the
 * OS, but the OS is a forty-second boot away and reports failures as one frozen
 * string on a dashboard; the same power sequence, the same BTIF, and the same
 * HIF are all live in LK the moment the console is up.
 */
enum {
    /* Connectivity subsystem, as seen from the AP side of the bus. */
    CONSYS_MCU_BASE = 0x18070000u,
    CONSYS_MCU_CHIP_ID = CONSYS_MCU_BASE + 0x0008u,

    /* WLAN AHB host interface. */
    WHIF_BASE = 0x180f0000u,
    WHIF_WCIR = 0x0000u,   /* chip id, revision, WLAN_READY */
    WHIF_WHLPCR = 0x0004u, /* ownership request/status */
    WHIF_WHISR = 0x0010u,  /* interrupt status */
    WHIF_WCIR_WLAN_READY = 1u << 21,
    WHIF_WHLPCR_DRV_OWN_REQ = 1u << 9,
    WHIF_WHLPCR_IS_DRV_OWN = 1u << 8,

    /* The WMT control link to the connectivity MCU. */
    WBTIF_BASE = 0x1100c000u,
    WBTIF_LSR = 0x0014u,
    WBTIF_LSR_DR = 1u << 0,
    WBTIF_LSR_THRE = 1u << 5,

    /* Power: SPM domain control, and the two INFRA/PERI clock gates. */
    WSPM_CONN_PWR_CON = 0x10006280u,
    WSPM_PWR_STATUS = 0x1000660cu,
    WSPM_CONN_PWR_STA_BIT = 1u << 1,
    WINFRA_PDN_STA = 0x10001048u,
    WINFRA_CONNMCU_GATE = 1u << 12,
    WPERI_PDN0_STA = 0x10003018u,
    WPERI_BTIF_GATE = 1u << 20,

    /* MT6323 rails the CONSYS block runs on. */
    WPMIC_VCN18 = 0x0512u,
    WPMIC_VCN28 = 0x041cu,
    WPMIC_VCN33_WIFI = 0x0418u,
    WPMIC_VCN33_BT = 0x0416u,
};

static void wifi_yes_no(const char* label, int value) {
    put_str(label);
    put_str(value ? "yes" : "no");
}

static void wifi_pmic(const char* label, uint32_t addr) {
    uint32_t v = 0u;
    put_str(label);
    if (mt6592_pwrap_read(addr, &v) != MT6592_PWRAP_OK) {
        put_str("<pwrap error>");
        return;
    }
    put_hex(v, 4u);
}

/* Registers only. Safe with CONSYS off for the AP-side ones; the CONSYS-side
 * ones are read only when the power status bit says the domain is up, because a
 * read into an unpowered slave is how this bus hangs. */
static void wifi_dump_registers(void) {
    const uint32_t pwr_con = mtk_read32(WSPM_CONN_PWR_CON);
    const uint32_t pwr_sta = mtk_read32(WSPM_PWR_STATUS);
    const int domain_up = (pwr_sta & WSPM_CONN_PWR_STA_BIT) != 0u;

    wifi_pmic("wifi: rails vcn18=", WPMIC_VCN18);
    wifi_pmic(" vcn28=", WPMIC_VCN28);
    wifi_pmic(" vcn33wifi=", WPMIC_VCN33_WIFI);
    wifi_pmic(" vcn33bt=", WPMIC_VCN33_BT);
    put_char('\n');

    kv_hex("wifi: spm pwr_con=", pwr_con, 8u);
    kv_hex(" pwr_status=", pwr_sta, 8u);
    wifi_yes_no(" domain=", domain_up);
    put_char('\n');

    kv_hex("wifi: clocks infra_pdn_sta=", mtk_read32(WINFRA_PDN_STA), 8u);
    wifi_yes_no(" connmcu=", (mtk_read32(WINFRA_PDN_STA) & WINFRA_CONNMCU_GATE) == 0u);
    kv_hex(" peri_pdn0_sta=", mtk_read32(WPERI_PDN0_STA), 8u);
    wifi_yes_no(" btif=", (mtk_read32(WPERI_PDN0_STA) & WPERI_BTIF_GATE) == 0u);
    put_char('\n');

    if (!domain_up) {
        put_str("wifi: CONSYS domain is down; not reading its registers (that read hangs the bus)\n");
        return;
    }

    kv_hex("wifi: consys chip_id=", mtk_read32(CONSYS_MCU_CHIP_ID), 8u);
    put_char('\n');

    {
        const uint32_t wcir = mtk_read32(WHIF_BASE + WHIF_WCIR);
        const uint32_t whlpcr = mtk_read32(WHIF_BASE + WHIF_WHLPCR);
        kv_hex("wifi: hif wcir=", wcir, 8u);
        kv_hex(" id=", wcir & 0xffffu, 4u);
        kv_dec(" rev=", (int32_t)((wcir >> 16) & 0xfu));
        wifi_yes_no(" wlan_ready=", (wcir & WHIF_WCIR_WLAN_READY) != 0u);
        kv_hex(" whlpcr=", whlpcr, 8u);
        wifi_yes_no(" driver_own=", (whlpcr & WHIF_WHLPCR_IS_DRV_OWN) != 0u);
        kv_hex(" whisr=", mtk_read32(WHIF_BASE + WHIF_WHISR), 8u);
        put_char('\n');
    }

    {
        const uint32_t lsr = mtk_read32(WBTIF_BASE + WBTIF_LSR);
        kv_hex("wifi: btif lsr=", lsr, 8u);
        wifi_yes_no(" rx_pending=", (lsr & WBTIF_LSR_DR) != 0u);
        wifi_yes_no(" tx_empty=", (lsr & WBTIF_LSR_THRE) != 0u);
        put_char('\n');
    }
}

/* What the driver believes, for comparison with the above. */
static void wifi_dump_driver_state(void) {
    const mt6592_wifi_sdio_state* p = mt6592_wifi_sdio_get_state();
    const mt6592_wifi_wmt_state* w = mt6592_wifi_wmt_get_state();

    wifi_yes_no("wifi: power rails=", p->rails_programmed);
    wifi_yes_no(" mtcmos=", p->mtcmos_ready);
    wifi_yes_no(" clock=", p->infra_clock_ready);
    wifi_yes_no(" consys=", p->consys_responds);
    wifi_yes_no(" wifirail=", p->wifi_rail_ready);
    kv_hex(" chip=", p->chip_id, 4u);
    kv_dec(" err=", p->last_err);
    /* The EMI share window one of the firmware sections is downloaded into;
     * bit 12 enables it, the low twelve bits are its DRAM base in MiB. */
    kv_hex(" emimap=", p->emi_mapping, 8u);
    put_char('\n');
    if (p->blocked != 0) {
        put_str("wifi: power blocked: ");
        put_str(p->blocked);
        put_char('\n');
    }

    wifi_yes_no("wifi: wmt btif=", w->btif_ready);
    wifi_yes_no(" calibrated=", w->calibrated);
    wifi_yes_no(" ready=", w->ready);
    wifi_yes_no(" wlanfunc=", w->wifi_function_on);
    kv_hex(" tx=", w->tx_bytes, 8u);
    kv_hex(" rx=", w->rx_bytes, 8u);
    kv_hex(" lsr=", w->last_btif_lsr, 8u);
    kv_hex(" evt=", w->last_event_size, 4u);
    kv_hex(" evop=", w->last_event_opcode, 4u);
    put_char('\n');
    if (w->status != 0) {
        put_str("wifi: wmt status: ");
        put_str(w->status);
        put_char('\n');
    }
    if (w->blocked != 0) {
        put_str("wifi: wmt blocked: ");
        put_str(w->blocked);
        put_char('\n');
    }
}

/* Ask the HIF for ownership and watch the grant land. The request bit is
 * write-only and the grant appears in a different bit a moment later, so a
 * single read-back right after the write reports nothing and looks like a
 * failure -- poll instead. */
static void wifi_take_ownership(void) {
    uint32_t i;

    mtk_write32(WHIF_BASE + WHIF_WHLPCR, WHIF_WHLPCR_DRV_OWN_REQ);
    for (i = 0u; i < 100u; ++i) {
        const uint32_t whlpcr = mtk_read32(WHIF_BASE + WHIF_WHLPCR);
        if ((whlpcr & WHIF_WHLPCR_IS_DRV_OWN) != 0u) {
            kv_dec("wifi: driver-own granted after ", (int32_t)i);
            put_str(" ms\n");
            return;
        }
        delay_ms(1u);
    }
    put_str("wifi: driver-own was never granted\n");
}

/* What the HIF half believes. Split from wifi_dump_driver_state() because the
 * HIF is only linked once firmware staging exists, and because after `wifi fw`
 * these are the only counters that move. */
static void wifi_dump_hif_state(void) {
    const mt6592_wifi_hif_state* h = mt6592_wifi_hif_get_state();

    wifi_yes_no("wifi: hif ready=", h->hif_ready);
    wifi_yes_no(" own=", h->driver_own);
    wifi_yes_no(" fw_loaded=", h->firmware_loaded);
    wifi_yes_no(" fw_alive=", h->firmware_alive);
    wifi_yes_no(" configured=", h->configured);
    wifi_yes_no(" bss=", h->bss_active);
    put_char('\n');
    kv_hex("wifi: hif fw_bytes=", h->firmware_size, 8u);
    kv_hex(" sent=", h->downloaded_bytes, 8u);
    kv_dec(" sections=", (int32_t)h->firmware_sections);
    kv_dec(" scans=", (int32_t)h->scan_result_count);
    wifi_yes_no(" scanning=", h->scan_active);
    wifi_yes_no(" assoc=", h->associated);
    put_char('\n');
    kv_hex("wifi: hif whisr=", h->last_whisr, 8u);
    kv_hex(" wasr=", h->last_wasr, 8u);
    kv_hex(" wrplr=", h->last_wrplr, 8u);
    kv_dec(" rx=", (int32_t)h->rx_packets);
    kv_dec(" mgmt=", (int32_t)h->rx_management);
    kv_dec(" evt=", (int32_t)h->rx_events);
    kv_dec(" data=", (int32_t)h->rx_data);
    kv_dec(" drop=", (int32_t)h->dropped_packets);
    put_char('\n');
    /*
     * The same two registers ORed across every poll rather than sampled at the
     * end. `whisr=0 wrplr=0 rx=0` has been printed twice now and cannot tell a
     * chip that never raised anything from one whose interrupt the poll consumed
     * a moment before the dump. These can: any bit that was ever set is here.
     */
    kv_hex("wifi: hif whisr_seen=", h->whisr_seen, 8u);
    kv_hex(" wrplr_seen=", h->wrplr_seen, 8u);
    kv_dec(" polls=", (int32_t)h->poll_calls);
    put_char('\n');
    /*
     * The TX page credit, per traffic class. TC4 is the line to read: every
     * command and every management/EAPOL frame is TC4, and the firmware grants it
     * four pages. A join spends seven commands, so before this accounting existed
     * the fifth went into a full FIFO and the AHB write never came back -- which
     * is what "the board vanished off USB mid-join" was.
     *
     * Read credited= first, because it says which of two worlds this is. Above
     * zero and the accounting is confirmed against the chip: starved= counts real
     * refusals and forced= stays at zero. At zero with forced= climbing, WTSR is
     * not reporting freed pages at all, the driver let the sends through rather
     * than enforce a budget nothing had confirmed, and the four-page window was
     * never a budget in the first place -- which is the next thing to chase.
     */
    put_str("wifi: hif txres");
    for (uint32_t tc = 0; tc < MT6592_WIFI_TX_CLASSES; ++tc)
    {
        put_str(" tc");
        put_dec((int32_t)tc);
        put_char('=');
        put_dec((int32_t)h->tx_free[tc]);
        put_char('/');
        put_dec((int32_t)h->tx_max[tc]);
    }
    kv_dec(" credited=", (int32_t)h->tx_credited);
    kv_dec(" waits=", (int32_t)h->tx_waits);
    kv_dec(" starved=", (int32_t)h->tx_starved);
    kv_dec(" forced=", (int32_t)h->tx_forced);
    put_char('\n');
    /*
     * Whether the firmware has ever answered a query. g_wifi_mac has a built-in
     * default, so this line is the only thing that separates "the firmware told
     * us its address" from "the query timed out and we invented one and then
     * told the firmware to use it".
     */
    put_str("wifi: hif mac=");
    for (uint32_t i = 0; i < 6u; ++i) {
        if (i != 0u) put_char(':');
        kv_hex("", h->station_mac[i], 2u);
    }
    put_str(h->station_mac_from_firmware ? " (from firmware -- it answers queries)"
                                         : " (DEFAULT -- the firmware never answered the query)");
    kv_dec(" polls=", (int32_t)h->mac_query_polls);
    kv_dec(" domain_cmds=", (int32_t)h->domain_cmds_sent);
    kv_dec(" ps_cmds=", (int32_t)h->ps_cmds_sent);
    put_char('\n');
    /* The start half of the story: the entry point we actually handed to
     * WIFI_START, the last WCIR the readiness poll saw, and whatever the peer
     * volunteered afterwards. Silence here means the firmware said nothing at
     * all, which is a different failure from a refusal. */
    /* Whether the RF front end was configured by its own probe or whether we
     * forged the byte that says so. Nothing else printed here distinguishes
     * them, and a forged flag means every value in the MT6625L is still at
     * reset -- a receiver that hears nothing and a PA that must not be keyed. */
    put_str("wifi: hif adie=");
    switch (h->adie_probe_state) {
        case ADIE_PROBE_RAN:         put_str("probed");            break;
        case ADIE_PROBE_STUCK:       put_str("FORGED(gate-stuck)");break;
        case ADIE_PROBE_TIMEOUT:     put_str("FORGED(timeout)");   break;
        case ADIE_PROBE_ROM_SILENT:  put_str("FORGED(rom-silent)");break;
        default:                     put_str("not-run");           break;
    }
    kv_dec(" polls=", (int32_t)h->adie_probe_polls);
    if (h->adie_probe_stale) {
        put_str(" (flag was pre-set and cleared)");
    }
    put_char('\n');
    kv_hex("wifi: hif override=", (uint32_t)mt6592_wifi_hif_get_start_override(), 1u);
    kv_hex(" start=", h->firmware_start_address, 8u);
    kv_hex(" wcir=", h->last_wcir, 8u);
    if (h->start_event_valid) {
        kv_hex(" event len=", h->start_event_length, 4u);
        put_str(" [");
        for (uint32_t i = 0; i < 8u; ++i) {
            if (i != 0u) put_char(' ');
            kv_hex("", h->start_event[i], 2u);
        }
        put_char(']');
    } else {
        put_str(" event=none");
    }
    put_char('\n');
    /* The mailboxes, before the start command and after the readiness poll gave
     * up. Stock reads mailbox 0 on exactly this timeout and prints it as "ID=%u",
     * so a change between the two columns is the firmware's own handwriting --
     * proof it executed -- and equality is proof it never wrote one. */
    kv_hex("wifi: hif mbox0=", h->mailbox_at_start_0, 8u);
    kv_hex("->", h->last_mailbox_0, 8u);
    kv_hex(" mbox1=", h->mailbox_at_start_1, 8u);
    kv_hex("->", h->last_mailbox_1, 8u);
    /* Print it the way stock prints it. wlan_lib.c:1596-1601 is
     *     nicGetMailbox(prAdapter, 0, &u4MailBox0);
     *     DBGLOG(INIT, ERROR, ("Waiting for Ready bit: Timeout, ID=%d\n",
     *             (u4MailBox0 & 0x0000FFFF)));
     * so the low half is a firmware-authored failure ID and the high half is not
     * part of it. Decimal, because stock's %d is what any note or log about a
     * given ID will be written in. Suppressed when the mailbox never changed,
     * where the low half is a leftover rather than a report. */
    if (h->last_mailbox_0 != h->mailbox_at_start_0) {
        kv_dec(" fw id=", (int32_t)(h->last_mailbox_0 & 0xffffu));
    }
    put_char('\n');
    /* Every distinct WCIR the poll saw, with the 10 ms iteration it appeared on.
     * One entry means the chip never moved; a later entry losing POR_INDICATOR
     * (bit 20) would mean the WLAN subsystem re-entered reset underneath us. */
    kv_dec("wifi: hif wcir changes=", (int32_t)h->wcir_transitions);
    for (uint32_t i = 0; i < h->wcir_transitions && i < 8u; ++i) {
        kv_dec(" @", (int32_t)h->wcir_seen_poll[i]);
        kv_hex(":", h->wcir_seen[i], 8u);
    }
    put_char('\n');
    /*
     * The connectivity MCU's program counter across the jump, from CONN_MCU_CONFIG
     * 0x18070160 -- stock's wmt_plat_read_cpupcr reads this one register and
     * nothing else, and its whole post-mortem path is that read in a loop.
     *
     * Read the two lines in order. `before` is four back-to-back samples taken
     * with the boot ROM provably alive, immediately ahead of the WIFI_START
     * packet: they are the control, and if they are all equal this register does
     * not track a PC on this part and the rest is meaningless. `poll` is 513
     * samples at 10 ms spanning the whole readiness window; changes=0 there means
     * the core stopped, and `last` is the address it stopped at.
     */
    put_str("wifi: hif cpupcr before=");
    for (uint32_t i = 0; i < 4u; ++i) {
        if (i != 0u) put_char(' ');
        kv_hex("", h->cpupcr_before[i], 8u);
    }
    put_char('\n');
    kv_dec("wifi: hif cpupcr samples=", (int32_t)h->cpupcr_samples);
    kv_dec(" changes=", (int32_t)h->cpupcr_changes);
    kv_hex(" first=", h->cpupcr_first, 8u);
    kv_hex(" last=", h->cpupcr_last, 8u);
    kv_hex(" min=", h->cpupcr_min, 8u);
    kv_hex(" max=", h->cpupcr_max, 8u);
    for (uint32_t i = 0; i < h->cpupcr_trace_count && i < 8u; ++i) {
        kv_dec(" @", (int32_t)h->cpupcr_trace_poll[i]);
        kv_hex(":", h->cpupcr_trace[i], 8u);
    }
    put_char('\n');
    if (h->cpupcr_hist_used != 0u) {
        /* Hottest first, by selection over a scratch copy of the counts -- the
         * ordering is the whole point: the address the core sits on has to be
         * readable at a glance, not hunted for among sixteen. */
        uint32_t hits[16];
        for (uint32_t i = 0; i < h->cpupcr_hist_used && i < 16u; ++i) hits[i] = h->cpupcr_hist_hits[i];
        put_str("wifi: hif cpupcr hot");
        for (uint32_t n = 0; n < h->cpupcr_hist_used && n < 16u; ++n) {
            uint32_t best = 0u;
            for (uint32_t i = 1u; i < h->cpupcr_hist_used && i < 16u; ++i) {
                if (hits[i] > hits[best]) best = i;
            }
            if (hits[best] == 0u) break;
            kv_hex(" ", h->cpupcr_hist[best], 8u);
            kv_dec("x", (int32_t)hits[best]);
            hits[best] = 0u;
        }
        kv_dec(" missed=", (int32_t)h->cpupcr_hist_missed);
        put_char('\n');
    }
    if (h->cpupcr_page_used != 0u) {
        /* The 256-byte buckets, hottest first, printed as the full base address
         * rather than the shifted key so it can be pasted straight into a
         * disassembler. This is the line that says where the run actually went;
         * the exact-address line above only ever covers its beginning. */
        uint32_t hits[32];
        for (uint32_t i = 0; i < h->cpupcr_page_used && i < 32u; ++i) hits[i] = h->cpupcr_page_hits[i];
        put_str("wifi: hif cpupcr pages");
        for (uint32_t n = 0; n < h->cpupcr_page_used && n < 32u; ++n) {
            uint32_t best = 0u;
            for (uint32_t i = 1u; i < h->cpupcr_page_used && i < 32u; ++i) {
                if (hits[i] > hits[best]) best = i;
            }
            if (hits[best] == 0u) break;
            kv_hex(" ", h->cpupcr_page[best] << 8, 8u);
            kv_dec("x", (int32_t)hits[best]);
            hits[best] = 0u;
        }
        kv_dec(" missed=", (int32_t)h->cpupcr_page_missed);
        put_char('\n');
    }
    if (h->status != 0) {
        put_str("wifi: hif status: ");
        put_str(h->status);
        put_char('\n');
    }
    if (h->blocked != 0) {
        put_str("wifi: hif blocked: ");
        put_str(h->blocked);
        put_char('\n');
    }
}

/*
 * THE FIRMWARE'S OWN LOG, IN ITS OWN WORDS.
 *
 * The running WLAN firmware writes plain-text records into EMI at AP 0x83163010
 * -- MCU 0xf0063010, inside section 3, which is its rodata and string table --
 * in fixed 36-byte slots padded with underscores. `strings 83163000` finds them,
 * but a raw sweep prints them out of shape, cannot tell one record from the next,
 * and a long sweep has taken this board off the USB bus before now. This walks
 * the records instead: one slot, one line, in order, stopping as soon as the log
 * runs out rather than reading a megabyte to find out it has.
 *
 * WHY IT IS WORTH A COMMAND OF ITS OWN. Every question left about association is
 * a question about what the firmware DECIDED, and the firmware writes its
 * decisions down here. Measured 2026-08-09: CH_PRIVILEGE goes out byte-for-byte
 * as stock builds it -- 20 bytes, netTypeIndex/token/action/channel/sco/band/
 * reqtype/reserved, u4MaxInterval at 8, BSSID at 12, all confirmed against
 * cnmChMngrRequestPrivilege at 0xc041c850 in the stock kernel -- and the firmware
 * answered with nothing at all, then stopped answering scans and queries too.
 *
 * That was the join's FIRST command taking the station table out from under it:
 * the record update was going out under CID 0x18, which this device's own kernel
 * uses for cnmStaRecFree. Fixed. This command stays because the reasoning that
 * put it here has not changed -- the next question about what the firmware
 * decided still has to be answered by the firmware.
 *
 * Empty is a real answer too: the log only exists once firmware is running, so a
 * blank window after `wifi fw` says the download never started executing.
 */
enum {
    WIFI_LOG_BASE      = 0x83163010u, /* first record; 0x83163000 is a static header */
    WIFI_LOG_RECORD    = 36u,         /* 0x24, underscore-padded                     */
    WIFI_LOG_DEFAULT_N = 64u,
    WIFI_LOG_MAX_N     = 512u,
    WIFI_LOG_RUN_OUT   = 6u           /* consecutive blank slots that end the walk   */
};

/* One record, unpacked and trimmed. Returns its printable length; 0 is blank. */
static uint32_t wifi_log_record(uint32_t index, char* text) {
    const uint32_t addr = (uint32_t)WIFI_LOG_BASE + index * (uint32_t)WIFI_LOG_RECORD;
    uint32_t len = 0u;
    uint32_t i;

    for (i = 0u; i < (uint32_t)WIFI_LOG_RECORD; i += 4u) {
        const uint32_t w = mtk_read32(addr + i);
        uint32_t b;
        for (b = 0u; b < 4u; ++b) {
            const uint8_t c = (uint8_t)((w >> (b * 8u)) & 0xffu);
            /* Tabs and newlines flattened to spaces for the same reason
             * `strings` does it: one record must stay one line. */
            text[len++] = (c == 0x0au || c == 0x09u) ? ' '
                        : ((c >= 0x20u && c < 0x7fu) ? (char)c : '\0');
        }
    }
    /* Trim the underscore padding and anything unprintable off the tail, so
     * a 12-character message does not arrive with 24 underscores after it. */
    while (len > 0u && (text[len - 1u] == '_' || text[len - 1u] == ' ' ||
                        text[len - 1u] == '\0')) {
        --len;
    }
    text[len] = '\0';
    return len;
}

/*
 * How many records the firmware has written so far -- the index one past the
 * last non-blank slot, so it can be used as a bookmark.
 *
 * Reads only, and only EMI: no MCR aperture, no command, nothing that can take
 * the board off the USB bus. That is what makes it safe to call from inside
 * another command, which is the whole point of it existing separately.
 */
static uint32_t wifi_log_count(void) {
    char text[WIFI_LOG_RECORD + 1u];
    uint32_t blanks = 0u;
    uint32_t used = 0u;
    uint32_t r;

    for (r = 0u; r < (uint32_t)WIFI_LOG_MAX_N; ++r) {
        if (wifi_log_record(r, text) == 0u) {
            if (++blanks >= (uint32_t)WIFI_LOG_RUN_OUT) break;
            continue;
        }
        blanks = 0u;
        used = r + 1u;
    }
    return used;
}

/* Print records [first, first+count). Returns how many were not blank. */
static uint32_t wifi_log_show(uint32_t first, uint32_t count) {
    char text[WIFI_LOG_RECORD + 1u];
    uint32_t shown = 0u;
    uint32_t r;

    for (r = first; r < first + count && r < (uint32_t)WIFI_LOG_MAX_N; ++r) {
        if (wifi_log_record(r, text) == 0u) continue;
        kv_dec("wifi: ", (int32_t)r);
        put_str("  ");
        put_str(text);
        put_char('\n');
        flush_line();
        ++shown;
    }
    return shown;
}

/*
 * What the firmware wrote down WHILE a command ran, and nothing else.
 *
 * `before` is a bookmark taken with wifi_log_count() before the thing under
 * test. Printing the delta rather than the whole log is not tidiness: the
 * standing question about association is what the firmware DID with a command it
 * never answered, and a hundred lines of boot chatter with the answer somewhere
 * in the middle is how that question has stayed open. An empty delta is a result
 * too, and a strong one -- it means the command never reached anything that
 * logs.
 */
static void wifi_log_delta(const char* what, uint32_t before) {
    const uint32_t after = wifi_log_count();

    put_str("wifi: ---- the firmware's own log across the ");
    put_str(what);
    put_str(" ----\n");
    flush_line();
    if (after <= before) {
        put_str("wifi: NOTHING APPENDED. The firmware wrote not one record while that\n");
        put_str("wifi: ran, so whatever consumed the command did not reach any code\n");
        put_str("wifi: that logs -- it was dropped before the decision, not after it.\n");
        return;
    }
    if (wifi_log_show(before, after - before) == 0u) {
        put_str("wifi: (records appeared and then read back blank -- retry `wifi log`)\n");
    }
}

static void cmd_wifi_log(const char* args) {
    const uint32_t n = parse_u32_default(&args, (uint32_t)WIFI_LOG_DEFAULT_N);
    uint32_t count = n > (uint32_t)WIFI_LOG_MAX_N ? (uint32_t)WIFI_LOG_MAX_N : n;
    uint32_t shown;

    if (count == 0u) count = (uint32_t)WIFI_LOG_DEFAULT_N;

    kv_hex("wifi: firmware log at ", (uint32_t)WIFI_LOG_BASE, 8u);
    kv_dec(", up to ", (int32_t)count);
    kv_dec(" records of ", (int32_t)WIFI_LOG_RECORD);
    put_str(" bytes\n");
    flush_line();

    shown = wifi_log_show(0u, count);

    if (shown == 0u) {
        put_str("wifi: the log is empty. It only exists while firmware is running, so\n");
        put_str("wifi: this says the download never began executing -- run `wifi fw`\n");
        put_str("wifi: first, and if it is still empty after that, nothing started.\n");
        return;
    }
    kv_dec("wifi: ", (int32_t)shown);
    put_str(" records. `wifi join' now prints its own delta, so this is for\n");
    put_str("wifi: reading the whole thing rather than one command's worth.\n");
}

/*
 * Did the WLAN firmware execute even one instruction?
 *
 * mt6592_wifi_sdio_bind() zeroes the CONSYS EMI share window (base+0x80000, for
 * 0x55c00 bytes) before anything is downloaded, because that span is what stock
 * ioremaps as pEmiVirtBase and stock only ever reads it to pull out a
 * connectivity coredump. Nothing in the WIFI_RAM_CODE_SOC download targets DRAM
 * -- all four sections land in connectivity-internal memory -- so after a failed
 * `wifi fw` every word here is still a word the AP wrote.
 *
 * The inference only runs one way. Any non-zero word proves the firmware executed
 * far enough to touch DRAM -- which turns "the chip is silent" into "the chip
 * crashed, and here is where". All zero proves nothing on its own: firmware that
 * faults before its first EMI access leaves the window exactly as the AP left it,
 * and so does firmware that never started. Read a zero result as "no evidence",
 * not as "no execution".
 */
static void cmd_wifi_core(void) {
    const uint32_t base  = MT6592_CONSYS_EMI_PHYS_BASE + 0x00080000u;
    const uint32_t words = 0x00055c00u / 4u;
    uint32_t nonzero = 0u;
    uint32_t first   = 0u;
    uint32_t i;

    for (i = 0u; i < words; ++i) {
        if (mtk_read32(base + i * 4u) != 0u) {
            if (nonzero == 0u) first = base + i * 4u;
            ++nonzero;
        }
        if ((i & 0x3fffu) == 0u) mt6592_usb_gadget_poll();
    }

    kv_hex("wifi: coredump window ", base, 8u);
    kv_hex("..", base + 0x00055c00u - 4u, 8u);
    kv_dec("  non-zero words=", (int32_t)nonzero);
    put_char('\n');
    if (nonzero == 0u) {
        put_str("wifi: window untouched -- no evidence of execution (a fault before the\n");
        put_str("      first EMI access looks identical; this does not prove it never ran)\n");
        return;
    }
    kv_hex("wifi: firmware DID run; first non-zero word at ", first, 8u);
    put_char('\n');
    for (i = 0u; i < 32u; ++i) {
        if ((i & 3u) == 0u) {
            if (i != 0u) put_char('\n');
            kv_hex("  ", first + i * 4u, 8u);
            put_char(':');
        }
        kv_hex(" ", mtk_read32(first + i * 4u), 8u);
    }
    put_char('\n');
}

/*
 * Print a boot-ROM INIT event exactly as it arrived.
 *
 * The INIT_CMD/INIT_EVENT body layouts for ACCESS_REG and QUERY_PENDING_ERROR
 * are the documented MediaTek ones, not something read out of the J36 kernel --
 * that build has no ACCESS_REG symbols at all. So print the header words too
 * and name the field we expect rather than only the field we expect: if the
 * body is shifted, the raw words say so instead of a plausible wrong number.
 *
 * Event header is u2RxByteCount at 0, ucEID at 2, ucSeqNum at 3, body from 8.
 * For ACCESS_REG the body is ucSetQuery, 3 reserved, u4Address, u4Data -- so
 * the value read back is the word at +16.
 */
static void wifi_print_rom_event(const uint8_t* event, uint32_t len) {
    uint32_t i;

    kv_dec("wifi: event ", (int32_t)len);
    put_str(" bytes raw");
    for (i = 0u; i + 4u <= len && i < 32u; i += 4u) {
        kv_hex(" ", (uint32_t)event[i] | ((uint32_t)event[i + 1u] << 8) |
                        ((uint32_t)event[i + 2u] << 16) | ((uint32_t)event[i + 3u] << 24),
               8u);
    }
    put_char('\n');
    if (len >= 4u) {
        kv_hex("wifi: eid=", event[2], 2u);
        kv_hex(" seq=", event[3], 2u);
        put_char('\n');
    }
}

/*
 * `gpu` — Mali-450 MP4 bring-up, from the bootloader.
 *
 * Every entry point in mt6592_gpu_offload.c runs the one-shot probe first, so
 * asking for the device info is what powers the MFG domain, ungates its clocks,
 * disables the four per-core MMUs and runs the GP/PP self-tests. The interesting
 * output is on the UART and in the boot-status ring, where the driver traces
 * each step; what prints here is the verdict.
 *
 * This is also the only caller of the driver in the tree. That is deliberate and
 * it is what keeps MVIILKRelease.elf unchanged: --gc-sections drops the console
 * out of the release image, and with it the sole reference to the GPU, its ~1.9
 * MiB of .gpubss and the DRAM window at 0x82d00000 those buffers sit in.
 */
static void gpu_cap(const char* label, uint32_t caps, uint32_t bit) {
    put_str(label);
    put_str((caps & bit) != 0u ? "yes" : "no");
}

static void cmd_gpu(const char* args) {
    struct gpu_device_info info;

    (void)args;

    if (mvii_gpu_offload_get_info(&info) != 0) {
        put_str("gpu: driver refused to report\n");
        return;
    }

    kv_hex("gpu: vendor=", info.vendor, 4u);
    kv_hex(" device=", info.device, 4u);
    kv_hex(" caps=", info.caps, 8u);
    kv_dec(" queue=", (int32_t)info.queue_ready);
    put_char('\n');
    /* Named one by one rather than left as a hex word: each of these is a
     * separate self-test that can fail on its own, and "which one" is the whole
     * question when the pipeline comes up but the screen is wrong. */
    gpu_cap("gpu: shader=", info.caps, GPU_CAP_SHADER_PIPELINE);
    gpu_cap(" rgb565=", info.caps, GPU_CAP_COMPUTE_RGBA8_TO_RGB565);
    gpu_cap(" scaled=", info.caps, GPU_CAP_SCALED_BLIT);
    gpu_cap(" tris=", info.caps, GPU_CAP_TRIANGLE_LIST);
    gpu_cap(" preserve=", info.caps, GPU_CAP_TRIANGLE_PRESERVE);
    put_char('\n');
    put_str("gpu: ");
    put_str(info.status);
    put_char('\n');
}

static void cmd_wifi(const char* args) {
    const char* rest;
    const char* rest2;

    args = skip_spaces(args);

    if (match_word(args, "on") != 0) {
        kv_dec("wifi: power/probe rc=", mt6592_wifi_sdio_bind());
        put_char('\n');
    } else if (match_word(args, "own") != 0) {
        if (mt6592_wifi_sdio_bind() != 0) {
            put_str("wifi: CONSYS did not power up; not touching the HIF\n");
        } else {
            wifi_take_ownership();
        }
    } else if (match_word(args, "link") != 0) {
        if (mt6592_wifi_sdio_bind() != 0) {
            put_str("wifi: CONSYS did not power up; not touching the BTIF\n");
        } else {
            kv_dec("wifi: wmt link probe rc=", mt6592_wifi_wmt_probe_link());
            put_char('\n');
        }
    } else if ((rest = match_word(args, "fw")) != 0) {
        (void)cmd_wifi_firmware(0, match_word(skip_spaces(rest), "decoy") != 0);
    } else if ((rest = match_word(args, "dl")) != 0) {
        /*
         * `wifi fw` split in two at the one point that cannot be undone. Every
         * command we send is now byte-identical to the stock driver's and the
         * firmware still answers nothing after WIFI_START -- not an event, not
         * WLAN_READY -- and a second attempt fails on the first chunk, so the
         * jump is definitely taken and the boot ROM is definitely gone.
         *
         * "It never executed an instruction" and "it ran and fell over" want
         * opposite fixes, and the way to tell them apart is to look either side
         * of the jump. Connectivity SRAM has already answered: `wifi dl`,
         * `sweep 18070000 50`, `wifi go`, `sweep 18070000 50` moves exactly one
         * page, which the WIFI_START packet's own staging fully explains.
         *
         * The live window is DRAM instead. CONSYS 0xf0000000 == AP 0x83100000,
         * and sections 2 and 3 land there decrypted, so sweep 0x83100000 for
         * 0x100 pages either side of `wifi go`. Anything that moves after the
         * jump was written by firmware executing instructions.
         *
         * Note the boot ROM does not survive a `wifi go`: every later `wifi dl`
         * or `wifi mark` in the same boot fails its first ACK with sent=0.
         * Reboot between attempts or the run measures nothing -- one already
         * did.
         */
        (void)cmd_wifi_firmware(1, match_word(skip_spaces(rest), "decoy") != 0);
    } else if (match_word(args, "probe") != 0) {
        /*
         * The A-die probe on its own, before WIFI_START, so its result can be
         * read rather than inferred from how badly the radio behaves later.
         *
         * rc=1 means the probe's own completion byte at CONSYS 0x02090614 came
         * up -- and since nothing here writes it, that is the ROM saying the
         * MT6625L was actually interrogated. rc=0 means the command went out and
         * the byte never moved. Run it after `wifi dl` and before `wifi go`;
         * `wifi fw` does it automatically in between.
         */
        const int rc = mt6592_wifi_hif_adie_probe();
        const mt6592_wifi_hif_state* st = mt6592_wifi_hif_get_state();
        kv_dec("wifi: adie probe rc=", rc);
        put_str(rc > 0   ? " (probed for real)"
              : rc == 0  ? " (command sent, flag never set)"
              : st->adie_probe_state == ADIE_PROBE_STUCK
                           ? " (flag would not clear -- the probe gate stays shut)"
                           : " (rom silent)");
        if (st->adie_probe_stale) {
            put_str(" [flag was pre-set on entry]");
        }
        put_char('\n');
    } else if (match_word(args, "go") != 0) {
        kv_dec("wifi: WIFI_START rc=", mt6592_wifi_hif_start_firmware());
        put_char('\n');
    } else if (match_word(args, "verify") != 0) {
        cmd_wifi_verify();
        return;
    } else if ((rest = match_word(args, "patch")) != 0) {
        cmd_wifi_patch(rest);
        return;
    } else if (match_word(args, "core") != 0) {
        cmd_wifi_core();
        return;
    } else if ((rest = match_word(args, "log")) != 0) {
        cmd_wifi_log(rest);
        return;
    } else if ((rest = match_word(args, "mark")) != 0) {
        /*
         * Every stock code path is now matched instruction for instruction --
         * the divided-image loop at 0xc03c0f2c passes the same dest+offset, the
         * INIT packet built at 0xc03be9a8 is byte-identical, and nothing but a
         * printk separates the last ACK from wlanConfigWifiFunc. The chip still
         * does not move. So the untested assumption is the ACK itself: the ROM
         * CRC-checks the packet it *received*, and copies afterwards, on a bus it
         * never checks. 90.6% of this image (0x3a290 of 0x40350 bytes) goes to
         * 0xf0020000 and 0xf0063000; if that bus is dead the download looks
         * perfect and the firmware is two thirds absent.
         *
         *   wifi mark deadb000      -- an address the ROM cannot own. ACK => the
         *                              status byte says nothing about the write.
         *   wifi mark 209f800       -- known-good destination, for the control.
         *   wifi mark f0020000      -- the address in question.
         * then `find a5a50000` to see where, if anywhere, it landed.
         */
        uint32_t address;
        uint32_t seed;
        uint32_t bytes;
        if (parse_u32(&rest, &address) != 0) {
            put_str("usage: wifi mark <addr> [seed] [bytes]   (defaults seed=a5a50000, 100 bytes)\n");
            put_str("  has the WLAN boot ROM write seed,seed+1,... at <addr>. Before WIFI_START only.\n");
            return;
        }
        seed  = parse_u32_default(&rest, 0xa5a50000u);
        bytes = parse_u32_default(&rest, 0x100u);
        /* The ROM only answers once CONSYS is powered and the WMT patches are
         * in, which is the first half of `wifi fw`. Latched, so repeat marks in
         * one boot cost nothing. */
        if (wifi_rom_ready() != 0) {
            put_str("wifi: the boot ROM is not reachable; nothing was written\n");
            return;
        }
        kv_hex("wifi: asking the boot ROM to write ", bytes, 4u);
        kv_hex(" bytes of ", seed, 8u);
        kv_hex(".. at ", address, 8u);
        put_char('\n');
        kv_dec("wifi: rom write rc=", mt6592_wifi_hif_rom_write(address, seed, bytes));
        put_char('\n');
    } else if ((rest = match_word(args, "rd")) != 0) {
        /*
         * The one instrument that reaches where nothing else does.
         *
         * `wifi mark` can prove a copy landed, but only somewhere the AP can
         * read afterwards, and the two sections that decide whether the
         * firmware runs are not there. Sections 2 and 3 go to the EMI window
         * (CONSYS 0xf0000000 == AP 0x83100000) and were read back decrypted,
         * landing to the page and bit-identical across two boots. Section 0 --
         * 0x0006a000, which is also the WIFI_START entry -- and section 1 at
         * 0x0209f800 are MCU-internal, and both sweeps either side of `wifi go`
         * are flat, so the firmware executes nothing and we have never been able
         * to check whether the code it should execute arrived.
         *
         *   wifi rd 1f0000     control: SYSRAM, also visible at AP 0x18080000
         *   wifi rd f0020000   control: EMI, also visible at AP 0x83120000
         *   wifi rd 6a000      the entry point. Zero here ends the bring-up.
         *   wifi rd 209f800    section 1.
         *
         * Run the two controls first. If they disagree with `peek` at the
         * aliases above, ACCESS_REG is not doing what this assumes and nothing
         * after it counts.
         */
        uint32_t address;
        uint8_t event[32];
        uint32_t len = 0u;
        if (parse_u32(&rest, &address) != 0) {
            put_str("usage: wifi rd <addr>   (CONSYS address, via the boot ROM. Before WIFI_START only)\n");
            return;
        }
        if (wifi_rom_ready() != 0) {
            put_str("wifi: the boot ROM is not reachable; nothing was read\n");
            return;
        }
        kv_hex("wifi: boot ROM read of ", address, 8u);
        put_char('\n');
        const int rc = mt6592_wifi_hif_rom_access_reg(0, address, 0u, event, sizeof event, &len);
        if (rc > 0) {
            put_str("wifi: the ROM answered with a different event, not ACCESS_REG:\n");
            wifi_print_rom_event(event, len);
        } else if (rc != 0) {
            put_str("wifi: no ACCESS_REG answer (the ROM may not implement it)\n");
        } else {
            wifi_print_rom_event(event, len);
            /*
             * 12 bytes, measured: 4-byte header, then the address the ROM echoes
             * back and the word it read. Not the 8-byte header MediaTek's
             * INIT_WIFI_EVENT_T implies -- this decode was first written for that
             * layout, against `len >= 20`, and never fired. The raw dump above is
             * what caught it, which is the only reason it was printed.
             *
             * The echoed address is checked rather than assumed: it is the one
             * field whose correct value is known in advance, so a mismatch means
             * the answer belongs to some other request.
             */
            if (len >= 12u) {
                const uint32_t echoed = (uint32_t)event[4] | ((uint32_t)event[5] << 8) |
                                        ((uint32_t)event[6] << 16) | ((uint32_t)event[7] << 24);
                kv_hex("wifi: [", address, 8u);
                kv_hex("] = ", (uint32_t)event[8] | ((uint32_t)event[9] << 8) |
                                   ((uint32_t)event[10] << 16) | ((uint32_t)event[11] << 24),
                       8u);
                if (echoed != address) {
                    kv_hex("   (WRONG: the ROM answered for ", echoed, 8u);
                    put_str(")");
                }
                put_char('\n');
            }
        }
    } else if ((rest = match_word(args, "dump")) != 0) {
        /*
         * `wifi rd` one word at a time, in bulk. This is how the firmware gets
         * read back in the clear.
         *
         * The image on the card is encrypted. E(sixteen zero bytes) is the
         * constant block b48d136fe376127cc5f91fb483e9d660, which is how the
         * all-zero stretches were identified without the key -- see
         * g_fw_zero_block, and note that the transform is 32-bit granular, not
         * a 16-byte block cipher. Everything else in it is opaque. But the boot
         * ROM decrypts each section as it
         * lands, and ACCESS_REG will read any connectivity address back, so the
         * plaintext is available a word at a time from a chip that already did
         * the work. Section 0 is 0x1910 bytes at 0x0006a000 and is also the
         * WIFI_START entry, so `wifi dump 6a000 100` is the entry code itself.
         *
         * The echoed address is checked on every word. A mismatch means the
         * events have slipped by one and the dump would be silently shifted, so
         * it stops there rather than printing a plausible lie.
         *
         * Only valid before WIFI_START.
         */
        uint32_t address;
        uint32_t words;
        uint32_t i;
        if (parse_u32(&rest, &address) != 0) {
            put_str("usage: wifi dump <addr> [words]   (CONSYS address, via the boot ROM)\n");
            return;
        }
        words = parse_u32_default(&rest, 0x40u);
        if (words > 0x800u) words = 0x800u;
        address &= ~3u;
        if (wifi_rom_ready() != 0) {
            put_str("wifi: the boot ROM is not reachable; nothing was read\n");
            return;
        }
        for (i = 0u; i < words; ++i) {
            const uint32_t at = address + i * 4u;
            uint8_t event[32];
            uint32_t len = 0u;
            uint32_t echoed;
            if (mt6592_wifi_hif_rom_access_reg(0, at, 0u, event, sizeof event, &len) != 0 || len < 12u) {
                put_char('\n');
                kv_hex("wifi: the ROM stopped answering at ", at, 8u);
                put_char('\n');
                return;
            }
            echoed = (uint32_t)event[4] | ((uint32_t)event[5] << 8) | ((uint32_t)event[6] << 16) |
                     ((uint32_t)event[7] << 24);
            if (echoed != at) {
                put_char('\n');
                kv_hex("wifi: asked for ", at, 8u);
                kv_hex(" and the ROM answered for ", echoed, 8u);
                put_str("; the events have slipped, stopping\n");
                return;
            }
            if ((i & 7u) == 0u) {
                if (i != 0u) put_char('\n');
                kv_hex("  ", at, 8u);
                put_char(':');
                mt6592_usb_gadget_poll();
            }
            kv_hex(" ", (uint32_t)event[8] | ((uint32_t)event[9] << 8) | ((uint32_t)event[10] << 16) |
                            ((uint32_t)event[11] << 24),
                   8u);
        }
        put_char('\n');
        kv_dec("wifi: dumped ", (int32_t)words);
        put_str(" words\n");
        return;
    } else if ((rest = match_word(args, "func")) != 0) {
        /*
         * WMT FUNC_CTRL by hand: `wifi func <subsystem> [0|1]`, default on.
         * 3 is WIFI in MediaTek's ENUM_WMTDRV_TYPE_T.
         *
         * The bootstrap does NOT send this; wmt_finalize explains why. It is a
         * probe, and the ordering the old comment gave for it was wrong in a way
         * the board states plainly. Issued as the first console command it comes
         * back "the connectivity MCU refused it", and the dump underneath says
         * exactly why -- `spm pwr_con=0x00000112 domain=no`, `CONSYS domain is
         * down`. Nothing has powered the connectivity subsystem yet at that
         * point: the bootstrap that raises the rails, ungates the clocks and
         * brings the BTIF link up runs inside `wifi dl`. So the command has to
         * come AFTER `wifi dl`, not before it -- and after `wifi dl` the same
         * command answers "accepted".
         *
         * `wifi dl` leaves the entry deferred, so there is a window between the
         * download and `wifi go` in which the boot ROM is still alive and the
         * subsystem can still be switched. That window is where this belongs.
         */
        uint32_t subsystem;
        uint32_t on;
        if (parse_u32(&rest, &subsystem) != 0 || subsystem > 0xffu) {
            put_str("usage: wifi func <subsystem> [0|1]   (0 bt, 1 fm, 2 gps, 3 wifi, 4 wmt)\n");
            return;
        }
        on = parse_u32_default(&rest, 1u);
        kv_hex("wifi: WMT FUNC_CTRL subsystem=", subsystem, 2u);
        kv_dec(" on=", (int32_t)on);
        put_char('\n');
        if (mt6592_wifi_wmt_func_ctrl((uint8_t)subsystem, on != 0u) != 0) {
            put_str("wifi: the connectivity MCU refused it\n");
        } else {
            put_str("wifi: accepted\n");
        }
    } else if ((rest = match_word(args, "adie")) != 0) {
        /*
         * Read the A-die's chip id, using stock's own frame.
         *
         * This is the fork the whole bring-up now sits on. The WLAN firmware
         * ASSERTs in wifi/mgmt/mt6582/rlm_phy.c line 4209 because a boot-ROM
         * getter returned zero; that getter reads one byte at CONSYS 0x02090614;
         * the only code that ever writes it is the A-die probe at ROM 0x14fdc,
         * which is straight-line and sets the byte on every path it can reach.
         * So a zero there does not mean the probe failed -- it means the probe
         * was never entered. What we cannot yet tell is whether the A-die is
         * simply never asked, or is asked and is not there.
         *
         * This command asks it directly, over WMT, without going anywhere near
         * the firmware. The eight bytes are lifted verbatim from stock's
         * opfunc_adie_lpbk_test, which sends them and prints "read A die chipid
         * CMD fail(%d),size(%d)" when the peer will not answer; the event it
         * expects back is thirteen bytes.
         */
        static const uint8_t adie_chipid[] = {0x01u, 0x13u, 0x04u, 0x00u, 0x02u, 0x04u, 0x24u, 0x00u};
        uint8_t event[32];
        uint32_t len = 0u;
        int rc;
        (void)rest;
        put_str("wifi: WMT read A-die chip id (stock opfunc_adie_lpbk_test frame)\n");
        rc = mt6592_wifi_wmt_raw_command(adie_chipid, (uint32_t)sizeof(adie_chipid), event, (uint32_t)sizeof(event),
                                         &len);
        wifi_print_wmt_event(rc, event, len);
    } else if ((rest = match_word(args, "wmt")) != 0) {
        /*
         * Any WMT frame at all: `wifi wmt 01 13 04 00 02 04 24 00`.
         *
         * The stock kernel keeps its connectivity commands in tables, and this
         * driver reproduces the ones in the power-on path. The ones outside it
         * -- the A-die reads, the loopbacks, the thermal and efuse ops -- were
         * each a rebuild away, which is the wrong price for a question that
         * takes one frame to ask and one event to answer. Bytes go on the wire
         * exactly as typed, so the frames in the disassembly can be pasted in
         * without translation.
         *
         * 96 bytes, not 32. The buffer was sized for the frames this driver
         * already sends, all of which are under a dozen bytes, and that turned
         * out to exclude the two that mattered: the LTE coexistence filter
         * spec at 0xc0b6d27c is 73 bytes on the wire and the frequency index
         * table at 0xc0b6d254 is 37. Silently truncating them would have been
         * worse than refusing, so the parser now stops and says so.
         */
        uint8_t command[96];
        uint8_t event[96];
        uint32_t count = 0u;
        uint32_t len   = 0u;
        uint32_t byte;
        int rc;
        while (parse_u32(&rest, &byte) == 0) {
            if (byte > 0xffu) {
                put_str("wifi: each argument is one byte\n");
                return;
            }
            if (count >= (uint32_t)sizeof(command)) {
                put_str("wifi: frame longer than 96 bytes\n");
                return;
            }
            command[count++] = (uint8_t)byte;
        }
        if (count < 2u) {
            put_str("usage: wifi wmt <b0> <b1> ...   (hex bytes, e.g. 01 13 04 00 02 04 24 00)\n");
            return;
        }
        put_str("wifi: WMT tx");
        for (uint32_t i = 0u; i < count; ++i) {
            kv_hex(" ", command[i], 2u);
        }
        put_char('\n');
        rc = mt6592_wifi_wmt_raw_command(command, count, event, (uint32_t)sizeof(event), &len);
        wifi_print_wmt_event(rc, event, len);
    } else if ((rest = match_word(args, "wr")) != 0) {
        /* The write half of the same command. Unlike `wifi mark` this does not
         * go through INIT_CMD_DOWNLOAD_BUF, so the payload never meets the ROM's
         * decryptor and the word that lands is the word asked for. */
        uint32_t address;
        uint32_t value;
        uint8_t event[32];
        uint32_t len = 0u;
        if (parse_u32(&rest, &address) != 0 || parse_u32(&rest, &value) != 0) {
            put_str("usage: wifi wr <addr> <value>   (CONSYS address, via the boot ROM)\n");
            return;
        }
        if (wifi_rom_ready() != 0) {
            put_str("wifi: the boot ROM is not reachable; nothing was written\n");
            return;
        }
        kv_hex("wifi: boot ROM write ", value, 8u);
        kv_hex(" -> ", address, 8u);
        put_char('\n');
        /* The write lands either way -- CONSYS 0x001f0000 took 0xa5a5a5a5 and both
         * `wifi rd` and AP 0x18080000 read it back -- so a mismatched answer here
         * is a fact about the acknowledgement, not about the write. */
        const int rc = mt6592_wifi_hif_rom_access_reg(1, address, value, event, sizeof event, &len);
        if (rc > 0) {
            put_str("wifi: written; the ROM acknowledged with a different event:\n");
            wifi_print_rom_event(event, len);
        } else if (rc != 0) {
            put_str("wifi: no answer at all; whether the write landed is unknown -- read it back\n");
        } else {
            wifi_print_rom_event(event, len);
        }
    } else if (match_word(args, "err") != 0) {
        /*
         * All 130 chunks ACKed status==0, but that status is the ROM's verdict
         * on the packet it received -- the copy into the destination happens
         * afterwards, on a bus the CRC never covered. If the ROM noticed a bad
         * destination, a failed decrypt or a bus error, this is where it has
         * been waiting to say so. Nothing has ever asked.
         *
         * It had. The first run of this command, after a download that reported
         * all 130 chunks ACKed and sent=0x00040350, answered with status
         * 0x0209f800 -- the destination of section 1, and the one destination of
         * the four that reads back as zeros afterwards. The event is 8 bytes: the
         * 4-byte header and one status word.
         */
        uint8_t event[32];
        uint32_t len = 0u;
        if (wifi_rom_ready() != 0) {
            put_str("wifi: the boot ROM is not reachable\n");
            return;
        }
        const int rc = mt6592_wifi_hif_rom_query_error(event, sizeof event, &len);
        if (rc > 0) {
            put_str("wifi: the ROM answered with a different event, not PENDING_ERROR:\n");
            wifi_print_rom_event(event, len);
        } else if (rc != 0) {
            put_str("wifi: no PENDING_ERROR answer (the ROM may not implement it)\n");
        } else {
            wifi_print_rom_event(event, len);
            if (len >= 8u) {
                const uint32_t status = (uint32_t)event[4] | ((uint32_t)event[5] << 8) |
                                        ((uint32_t)event[6] << 16) | ((uint32_t)event[7] << 24);
                kv_hex("wifi: pending error status = ", status, 8u);
                if (status == 0u) {
                    put_str("   (none)");
                }
                put_char('\n');
            }
        }
    } else if (match_word(args, "domain") != 0) {
        /*
         * The largest remaining difference from stock, made testable without a
         * reflash.
         *
         * wlanAdapterStart() reaches wlanLoadManufactureData() before it lets
         * anything scan, and that sends CMD_ID_SET_DOMAIN_INFO twice through
         * rlmDomainSendCmd (0xc0424188) -- the allowed channels, then the
         * passive-only subset. We have never sent either, so the firmware has
         * whatever channel list it powers up with, and an empty one produces
         * exactly what we see: a scan that starts, completes, and reports
         * nothing.
         *
         * Deliberately not on the start path. It is a difference, not yet a
         * measured fault, so the way to find out is
         *
         *   wifi scan      -- the baseline, unchanged
         *   wifi domain
         *   wifi scan      -- the same scan against a firmware that now has a
         *                     channel table
         *
         * and to compare, rather than to fold it in and lose the comparison.
         */
        if (mt6592_wifi_hif_send_domain_info() != 0) {
            const mt6592_wifi_hif_state* st = mt6592_wifi_hif_get_state();
            put_str("wifi: domain info not sent");
            if (st->blocked != 0) { put_str(" ("); put_str(st->blocked); put_char(')'); }
            put_char('\n');
            return;
        }
        put_str("wifi: sent the EU channel table (2.4 GHz ch 1-13 + four 5 GHz subbands)\n");
        put_str("wifi: and the passive-only table, which stock leaves empty. Re-run `wifi scan`.\n");
        return;
    } else if (match_word(args, "stats") != 0) {
        wifi_print_statistics();
        return;
    } else if ((rest = match_word(args, "mcr")) != 0) {
        cmd_wifi_mcr(skip_spaces(rest));
        return;
    } else if ((rest = match_word(args, "scan")) != 0) {
        cmd_wifi_scan(skip_spaces(rest));
        /* The scan list is the whole point of the command; the register dumps
         * below would push it off a terminal scrollback. */
        return;
    } else if (match_word(args, "list") != 0) {
        wifi_print_scan_list();
        return;
    } else if ((rest = match_word(args, "pass")) != 0) {
        arg_path(rest, g_scan_password, sizeof g_scan_password);
        kv_dec("wifi: passphrase set, ", (int32_t)arg_len(g_scan_password));
        put_str(" characters\n");
        return;
    } else if ((rest = match_word(args, "join")) != 0) {
        cmd_wifi_join(rest);
    } else if ((rest = match_word(args, "step")) != 0) {
        cmd_wifi_step(rest);
        return;
    } else if (match_word(args, "drop") != 0) {
        /* The manual version of what `join` now does for itself on the way out.
         * Kept as a command because an image can be left holding a claimed radio
         * by any path that starts an association and stops polling -- and the
         * alternative to typing one word is a power cycle. */
        if (mt6592_wifi_hif_abort_association() != 0) {
            put_str("wifi: association dropped; scanning is available again\n");
        } else {
            put_str("wifi: nothing to drop -- no association is running\n");
        }
        return;
    } else if ((rest = match_word(args, "start")) != 0) {
        /*
         * Diagnostic knob, not part of the normal path: it arms what the NEXT
         * `wifi fw' will put in INIT_CMD_WIFI_START. Nothing is sent from here --
         * the command that was already downloaded cannot be restarted, and
         * WIFI_START is single-shot per download.
         *
         * Two settings, because the packet carries two fields and the references
         * disagree about the first:
         *
         *   `wifi start image'  -> u4Override 0, u4Address 0. What the vendor
         *                          source builds (Makefile:5 is -DMT6628, and
         *                          that arm sets CFG_OVERRIDE_FW_START_ADDRESS
         *                          0). "Enter where your own header says."
         *   `wifi start <addr>' -> u4Override 1, u4Address <addr>. What the
         *                          shipped J36 kernel does, with <addr> 0 meaning
         *                          the 0x00060000 that kernel passes.
         */
        uint32_t address;
        /* match_word() stops ON the delimiter, so the keyword arm needs the space
         * eaten first; the numeric arm below does not, parse_u32() eats its own. */
        if (match_word(skip_spaces(rest), "image") != 0) {
            mt6592_wifi_hif_set_start_override(0);
            put_str("wifi: WIFI_START override=0 address=0 (vendor MT6628 build; entry from the image header)\n");
            return;
        }
        if (parse_u32(&rest, &address) != 0) {
            put_str("usage: wifi start <addr>   force the entry (override=1); 0 = the 0x60000 default\n");
            put_str("       wifi start image    send override=0/address=0, as the vendor source builds\n");
            return;
        }
        mt6592_wifi_hif_set_start_override(1);
        mt6592_wifi_hif_set_start_address(address);
        kv_hex("wifi: WIFI_START override=1 address=", address, 8u);
        put_str(address != 0u ? " (diagnostic)\n" : " (build default 0x00060000)\n");
        return;
    } else if ((rest = match_word(args, "trace")) != 0) {
        /*
         * Trace the connectivity MCU across one command, at bus speed.
         *
         * `wifi pc` samples whenever it is typed, which is useless for this: the
         * ROM handles WIFI_START and is back in its idle loop inside a single
         * 10 ms tick of the readiness poll. The poll caught the handler exactly
         * once, on its first sample, and never again. So arm the sampler and let
         * the driver fire it in the instruction after the packet hits WTDR0.
         *
         *   wifi trace rd <addr>   ACCESS_REG -- a command the ROM ANSWERS. This
         *                          is the control, and it must be run first: it
         *                          is what the receive path looks like when it
         *                          works, and without it the WIFI_START trace is
         *                          just a list of addresses.
         *   wifi trace go          WIFI_START -- the command the ROM services and
         *                          then declines to dispatch.
         *
         * Diff the two. Addresses in both are the HIF receive path; addresses
         * only in the WIFI_START trace are the dispatch path, and the last of
         * those before the core falls back to its idle loop is where it decided
         * not to jump to 0x0006a000.
         */
        uint32_t address = 0x1f0000u;
        uint32_t gap     = 0u;
        int is_start     = 0;
        const char* what;

        /* match_word() stops ON the delimiter and does not consume it, so a
         * second-level keyword has to be un-indented first. cmd_wifi() does this
         * once at the top for the first level; every other subcommand gets away
         * without it because parse_u32() skips spaces of its own accord. */
        rest = skip_spaces(rest);

        if ((rest2 = match_word(rest, "go")) != 0) {
            is_start = 1;
            what     = "WIFI_START";
            (void)parse_u32(&rest2, &gap);
        } else if ((rest2 = match_word(rest, "rd")) != 0) {
            (void)parse_u32(&rest2, &address);
            (void)parse_u32(&rest2, &gap);
            what = "ACCESS_REG";
        } else {
            put_str("usage: wifi trace [rd <addr>|go] [gap-us]   (run `rd` first -- it is the control)\n");
            return;
        }
        if (gap > 1000u) gap = 1000u;
        if (wifi_rom_ready() != 0) {
            put_str("wifi: the boot ROM is not reachable; download first with `wifi dl`\n");
            return;
        }

        mt6592_wifi_hif_pc_trace_arm(gap);
        if (is_start) {
            kv_dec("wifi: WIFI_START rc=", mt6592_wifi_hif_start_firmware());
            put_char('\n');
        } else {
            uint8_t event[32];
            uint32_t len = 0u;
            kv_hex("wifi: boot ROM read of ", address, 8u);
            kv_dec(" rc=", mt6592_wifi_hif_rom_access_reg(0, address, 0u, event, sizeof event, &len));
            put_char('\n');
        }

        uint32_t count       = 0u;
        const uint32_t* pc   = mt6592_wifi_hif_pc_trace(&count);
        uint32_t runs        = 0u;
        uint32_t printed     = 0u;
        uint32_t in_firmware = 0u;
        uint32_t low         = 0xffffffffu;
        uint32_t high        = 0u;

        put_str("wifi: pc trace of ");
        put_str(what);
        kv_dec(", samples=", (int32_t)count);
        put_char('\n');
        for (uint32_t i = 0; i < count;) {
            uint32_t j = i + 1u;
            while (j < count && pc[j] == pc[i]) ++j;
            const uint32_t dwell = j - i;
            ++runs;
            if (pc[i] < low) low = pc[i];
            if (pc[i] > high) high = pc[i];
            /* Section 0 of WIFI_RAM_CODE_SOC lands at 0x0006a000 and is 0x1910
             * long. One sample in that span is the whole question answered. */
            if (pc[i] >= 0x0006a000u && pc[i] < 0x0006b910u) in_firmware += dwell;
            if (printed < 96u) {
                put_str("  ");
                kv_hex("", pc[i], 8u);
                kv_dec(" x", (int32_t)dwell);
                put_char('\n');
                ++printed;
            }
            i = j;
        }
        if (runs > printed) {
            kv_dec("  ... ", (int32_t)(runs - printed));
            put_str(" further runs not shown\n");
        }
        kv_dec("wifi: pc trace runs=", (int32_t)runs);
        kv_hex(" min=", low, 8u);
        kv_hex(" max=", high, 8u);
        kv_dec(" samples in section 0 (0006a000..0006b910)=", (int32_t)in_firmware);
        put_char('\n');
        return;
    } else if ((rest = match_word(args, "pc")) != 0) {
        /*
         * Burst-sample the connectivity MCU's program counter, CONN_MCU_CONFIG
         * 0x18070160. This is stock's stp_dbg_poll_cpupcr (0xc03b3e80) -- which
         * is literally wmt_plat_read_cpupcr in a loop with a sleep between reads
         * -- and it is the whole of MediaTek's own tooling for a connectivity
         * MCU that has stopped answering. wmt_plat_read_dmaregs next to it is a
         * stub returning 0, so there is nothing else to reach for.
         *
         * Unlike everything else under `wifi` this needs no HIF, no ownership
         * and no download: it is one register read, valid at any point in a
         * boot, including after the boot ROM has gone silent. So it can be run
         * before `wifi dl` for a baseline and again after `wifi go`.
         *
         * The samples are spaced by a busy delay rather than a sleep because a
         * PC that is moving will show it in tens of microseconds; the point is
         * to catch motion, not to trace a program.
         */
        uint32_t count = 16u;
        uint32_t gap   = 100u;
        (void)parse_u32(&rest, &count);
        (void)parse_u32(&rest, &gap);
        if (count == 0u || count > 256u) count = 16u;
        if (gap > 100000u) gap = 100000u;

        uint32_t previous = mt6592_wifi_hif_read_cpupcr();
        uint32_t changes  = 0u;
        uint32_t low      = previous;
        uint32_t high     = previous;

        put_str("wifi: cpupcr [0x18070160]");
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t pc = mt6592_wifi_hif_read_cpupcr();
            if ((i & 7u) == 0u) put_str("\n ");
            kv_hex(" ", pc, 8u);
            if (pc != previous) ++changes;
            if (pc < low) low = pc;
            if (pc > high) high = pc;
            previous = pc;
            mt6592_delay_cycles(gap * 33u); /* 33 cycles per microsecond, as elsewhere here */
        }
        put_char('\n');
        kv_dec("wifi: cpupcr samples=", (int32_t)count);
        kv_dec(" changes=", (int32_t)changes);
        kv_hex(" min=", low, 8u);
        kv_hex(" max=", high, 8u);
        put_str(changes != 0u ? "  (the core is executing)\n"
                              : "  (frozen -- halted or spinning at one address)\n");
        return;
    } else if (match_word(args, "state") != 0 || match_word(args, "status") != 0) {
        /* fall through to the dumps. `status` is an alias because it is what
         * gets typed, and answering a reasonable synonym with a usage line
         * teaches nothing. */
    } else if (*args != '\0') {
        put_str("usage: wifi [on|own|link|fw|dl|go|core|pc [n] [us]|trace [rd <addr>|go]|mark <addr>|\n");
        put_str("             scan [passive|active]|list|pass <secret>|join <idx|ssid>|\n");
        put_str("             step <idx|ssid>|drop|start <addr>|state|status]\n");
        return;
    }

    /* First, before three screens of registers. The verdict is what the dump is
     * for; making somebody derive it from `assoc=no` two hundred lines down is
     * how a dead link got read as a live one twice. */
    wifi_print_link_summary();
    wifi_dump_registers();
    wifi_dump_driver_state();
    wifi_dump_hif_state();
}

static void map_sample(uint8_t* out) {
    uint32_t i;
    uint32_t bit;
    uint32_t mems[5];

    for (i = 0u; i < MAP_GPIO_BANKS; ++i) {
        const uint32_t w = mtk_read32(GPIOC_BASE + GPIOC_DIN + i * GPIOC_STRIDE) & 0xffffu;
        for (bit = 0u; bit < 16u; ++bit) out[i * 16u + bit] = (uint8_t)((w >> bit) & 1u);
    }
    read_kpd_mems(mems);
    for (i = 0u; i < 5u; ++i) {
        for (bit = 0u; bit < 16u; ++bit) {
            out[MAP_PINS + i * 16u + bit] = (uint8_t)((mems[i] >> bit) & 1u);
        }
    }
}

static void map_pump(void) {
    mt6592_usb_gadget_poll();
    delay_ms(10u);
}

/* Adopt the present levels as "at rest" and treat them as already settled. */
static void map_rebaseline(void) {
    uint32_t i;
    map_sample(g_map_base);
    for (i = 0u; i < MAP_SLOTS; ++i) {
        g_map_level[i] = g_map_base[i];
        g_map_hold[i] = (uint8_t)MAP_STABLE_TICKS;
    }
}

/* Find the lines that will not sit still even with nothing touched, and drop
 * them for the rest of the run. Anything that moves during a second and a half
 * of deliberate inactivity is not a switch. */
static void map_settle(void) {
    uint32_t t;
    uint32_t i;
    uint32_t bad = 0u;

    map_sample(g_map_base);
    for (i = 0u; i < MAP_SLOTS; ++i) g_map_unstable[i] = 0u;

    for (t = 0u; t < MAP_SETTLE_TICKS; ++t) {
        map_sample(g_map_now);
        for (i = 0u; i < MAP_SLOTS; ++i) {
            if (g_map_now[i] != g_map_base[i]) g_map_unstable[i] = 1u;
        }
        map_pump();
    }

    put_str("kpdmap: restless lines:");
    for (i = 0u; i < MAP_SLOTS; ++i) {
        if (g_map_unstable[i] == 0u) continue;
        ++bad;
        if (i < MAP_PINS) {
            kv_dec(" gpio", (int32_t)i);
        } else {
            kv_dec(" bit", (int32_t)(i - MAP_PINS));
        }
    }
    if (bad == 0u) put_str(" (none)");
    put_char('\n');

    /*
     * Try to rescue the restless pins before writing them off.
     *
     * The last run threw away gpio21..24 and gpio35 as noise, and those were the
     * four consecutive pins that looked most like a d-pad. A floating input --
     * one configured as a GPIO input with no pull resistor -- chatters exactly
     * like a clock line does, so "will not sit still" does not distinguish "this
     * pin tracks a scan strobe" from "this pin is an unconnected switch input
     * that nobody enabled a pull-up on". LK never configures these pads; the OS
     * presumably does, which would explain why the buttons work there and are
     * invisible here.
     *
     * So: for every restless pin that is already in GPIO mode and already an
     * input, turn on a pull-up and look again. This changes no pin's function
     * and drives nothing -- it only decides what an undriven pad reads as -- so
     * it cannot disturb the display or eMMC pads, which are not mode 0 inputs.
     * A pin that goes quiet was floating and is a live button candidate. A pin
     * that keeps chattering is genuinely being driven, and stays excluded.
     */
    {
        uint32_t rescued = 0u;
        uint32_t tried = 0u;

        for (i = 0u; i < MAP_PINS; ++i) {
            if (g_map_unstable[i] == 0u) continue;
            if (gpio_mode_get(i) != 0u) continue;
            if (gpio_bit_get(GPIOC_DIR, i) != 0u) continue;
            gpio_bit_write(GPIOC_PULLSEL, i, 1u); /* 1 = pull up */
            gpio_bit_write(GPIOC_PULLEN, i, 1u);
            ++tried;
        }
        if (tried == 0u) {
            put_str("kpdmap: no floating inputs to rescue\n");
            flush_line();
            return;
        }

        kv_dec("kpdmap: pulled up ", (int32_t)tried);
        put_str(" floating input(s); re-checking\n");
        flush_line();

        map_sample(g_map_base);
        for (t = 0u; t < MAP_SETTLE_TICKS; ++t) {
            map_sample(g_map_now);
            for (i = 0u; i < MAP_PINS; ++i) {
                if (g_map_unstable[i] == 0u) continue;
                if (g_map_now[i] != g_map_base[i]) g_map_unstable[i] = 2u; /* still noisy */
            }
            map_pump();
        }

        put_str("kpdmap: rescued (now quiet, watching):");
        for (i = 0u; i < MAP_PINS; ++i) {
            if (g_map_unstable[i] != 1u) continue;
            g_map_unstable[i] = 0u;
            ++rescued;
            kv_dec(" gpio", (int32_t)i);
        }
        if (rescued == 0u) put_str(" (none)");
        put_char('\n');
        for (i = 0u; i < MAP_PINS; ++i) {
            if (g_map_unstable[i] != 0u || gpio_bit_get(GPIOC_PULLEN, i) == 0u) continue;
            if (gpio_mode_get(i) != 0u) continue;
            gpio_report_cfg(i);
        }
        flush_line();
    }
}

/*
 * Do not take a baseline while the operator is still holding the button that
 * summoned the console. That is not a hypothetical: the last run baselined with
 * matrix bit 11 low, so pressing B during B's own window changed nothing and the
 * known-good control -- the one measurement that says whether the capture is
 * trustworthy at all -- reported NOTHING MOVED. Wait for the matrix to actually
 * read idle, and say so on screen in a colour that is neither of the two the map
 * itself uses.
 */
static void map_wait_idle(void) {
    uint32_t mems[5];
    uint32_t quiet = 0u;
    uint32_t t;

    put_str("kpdmap: LET GO of everything -- screen is RED until the matrix is idle\n");
    flush_line();
    if (g_hooks != 0 && g_hooks->fb_fill != 0) g_hooks->fb_fill(0xffc00000u);

    for (t = 0u; t < 1500u; ++t) { /* up to 15 s */
        uint32_t i;
        uint32_t idle = 1u;
        read_kpd_mems(mems);
        for (i = 0u; i < 5u; ++i) {
            const uint32_t valid = (i == 4u) ? 0x00ffu : 0xffffu;
            if (mems[i] != valid) idle = 0u;
        }
        quiet = (idle != 0u) ? quiet + 1u : 0u;
        if (quiet >= 100u) { /* a full second of nothing pressed */
            put_str("kpdmap: matrix idle, baseline is clean\n");
            flush_line();
            return;
        }
        map_pump();
    }
    put_str("kpdmap: WARNING matrix never went idle; results below are suspect\n");
    flush_line();
}

/* Watch for `ticks`, recording every line that settles away from its baseline. */
static void map_watch(uint32_t ticks) {
    uint32_t t;
    uint32_t i;

    for (i = 0u; i < MAP_SLOTS; ++i) g_map_hit[i] = 0u;

    for (t = 0u; t < ticks; ++t) {
        map_sample(g_map_now);
        for (i = 0u; i < MAP_SLOTS; ++i) {
            if (g_map_now[i] == g_map_level[i]) {
                if (g_map_hold[i] < 255u) ++g_map_hold[i];
            } else {
                g_map_level[i] = g_map_now[i];
                g_map_hold[i] = 0u;
            }
            if (g_map_unstable[i] != 0u) continue;
            if (g_map_hold[i] < MAP_STABLE_TICKS) continue;
            if (g_map_level[i] == g_map_base[i]) continue;
            g_map_hit[i] = 1u;
        }
        map_pump();
    }
}

static void map_report(const char* name) {
    uint32_t i;
    uint32_t printed = 0u;

    put_str("kpdmap: ");
    put_str(name);
    put_str(" ->");
    for (i = 0u; i < MAP_SLOTS; ++i) {
        if (g_map_hit[i] == 0u) continue;
        if (i < MAP_PINS) {
            kv_dec(" gpio", (int32_t)i);
        } else {
            kv_dec(" bit", (int32_t)(i - MAP_PINS));
        }
        put_str(g_map_level[i] != 0u ? "=1" : "=0");
        ++printed;
    }
    if (printed == 0u) put_str(" NOTHING MOVED");
    put_char('\n');
    flush_line();
}

static void cmd_kpdmap(void) {
    uint32_t b;
    uint32_t t;

    mtk_write32(KPD_BASE + 0x18u, 0x0400u);
    mtk_write32(KPD_BASE + 0x24u, 1u);
    dump_kpd();

    put_str("kpdmap: guided map -- WATCH THE SCREEN\n");
    put_str("kpdmap:   GREEN = press and HOLD the next button on the list\n");
    put_str("kpdmap:   BLUE  = let go and keep hands off\n");
    put_str("kpdmap: the list is printed one button at a time; follow the prompts\n");
    flush_line();

    map_wait_idle();

    put_str("kpdmap: settling, hands off\n");
    flush_line();
    if (g_hooks != 0 && g_hooks->fb_fill != 0) g_hooks->fb_fill(MAP_COLOUR_RELEASE);
    map_settle();
    map_rebaseline();

    for (b = 0u; kMapButtons[b] != 0; ++b) {
        if (g_hooks != 0 && g_hooks->fb_fill != 0) g_hooks->fb_fill(MAP_COLOUR_PRESS);
        put_str("kpdmap: PRESS -> ");
        put_str(kMapButtons[b]);
        put_char('\n');
        flush_line();

        map_watch(MAP_PRESS_TICKS);
        map_report(kMapButtons[b]);

        if (g_hooks != 0 && g_hooks->fb_fill != 0) g_hooks->fb_fill(MAP_COLOUR_RELEASE);
        for (t = 0u; t < MAP_RELEASE_TICKS; ++t) map_pump();
        map_rebaseline();
    }

    put_str("kpdmap: done\n");
    flush_line();
}

/*
 * THE BUTTON MAP, live and by name.
 *
 * SOME of the buttons on this device are a switch between two pads, and for those, once both
 * sides are pulled up no pad's level says whether one is pressed -- closing it ties two
 * pulled-up nodes together and neither moves. The only way to see that closure is to drive
 * one side low on purpose and read the other, which is what `kpdwire` does one pad at a time.
 * This is the same operation for all five pairs at once, so `kpdmon` can print the button's
 * NAME instead of a pad number for someone to attribute by hand. Attribution by hand is what
 * produced every wrong entry this driver ever had.
 *
 * "SOME" IS THE WORD THAT TOOK FOUR RUNS. This table had eleven entries and now has five,
 * because six of the eleven were switches to something already LOW -- a pulse cannot
 * attribute those and this scan reported them anyway, for whichever pad it happened to be
 * pulsing. They are in g_level_keys[] now. A pulse scan and a level scan answer different
 * questions and neither is the general case here.
 *
 * The pull-up is this scan's own doing and it matters: at rest, every button pad on this
 * connector idles LOW on a parked pull-down and only the three keypad sense lines idle high
 * (measured -- see the resting-level table in mt6592_board_j36.h). A pad that idles low
 * cannot report a closure, so wire_keys_arm() pulls every probe pad up before the first
 * pulse and wire_keys_restore() puts them back.
 *
 * These literals are deliberately a SECOND COPY of the wire table in
 * mt6592_board_j36.h, for the reason given at the top of this file: the console exists
 * to test the drivers, and a monitor that shares its constants with the code under test
 * can only ever confirm that code's assumptions. If `kpdmon` and the OS disagree about a
 * button, the disagreement is itself the finding and it localises to whichever copy is
 * stale. Kept in the same order as the header so a diff is a diff.
 */
typedef struct {
    const char* name;
    uint8_t drive;      /* pulsed to output-low; never a keypad pad */
    uint8_t sense;      /* read while drive is low; 0 means the switch is closed */
    uint8_t ghost_drop; /* the triangle member to discard when all three light */
} wire_key_t;

/*
 * FIVE PAIRS, DOWN FROM ELEVEN, and every one of the six that left was removed by a
 * measurement rather than a rethink:
 *
 *   UP/DOWN/LEFT/RIGHT (35/45/20/8 -- 75)  never fired as pairs. They produced a
 *       twenty-five-line short storm instead, which is C(8,2) minus the three pairs the
 *       table already named -- the signature of a pad sitting low while eight pulses walk
 *       past it, not of a wiring pattern. They are passive levels; see g_level_keys[].
 *   A (30--12)  fired for A AND for R2, and the drive-high verdict test said NOT A PAIR:
 *       pad 30 driven high (its own line reads hi=1 lo=0, so the driver works) left pad 12
 *       at 0. Pad 12 was low on its own account. Also a level.
 *   R2 (12--167)  never fired in four runs, with both halves proven good in the same
 *       reports -- pad 12 is pulsed for START, pad 167 is sensed for VOL+. A pair whose
 *       drive works and whose sense works and which still reads open is not a pair.
 *
 * THE INVARIANT THAT SURVIVED: a pulse scan can only read a switch whose BOTH pads idle
 * high. All five remaining pairs satisfy it and all five work on hardware. Anything whose
 * pad idles low belongs in g_level_keys[] below, and the two halves of kpdmon exist to
 * decide which of the two a new button is.
 */
/*
 * THE THIRTEEN MATRIX KEYS BY NAME, so `kpdmon` reports "START PRESSED (bit 30)" instead of
 * a bit number that then has to be looked up by hand -- which is how six correct bits sat in
 * a transcript for four runs while the seven missing ones were hunted on the wrong mechanism.
 *
 * A SECOND COPY of the matrix half of mt6592_board_j36.h, for the same reason the wire table
 * below is a second copy: if this disagrees with the OS about a button, the disagreement is
 * the finding. Kept in bit order so a diff is a diff.
 */
static const struct {
    const char* name;
    uint8_t bit;
} g_matrix_keys[] = {
    {"L1", 0u},   {"L2", 1u},     {"R1", 2u},    {"R2", 3u},
    {"X", 9u},    {"Y", 10u},     {"B", 11u},    {"A", 12u},
    {"VOL-", 27u}, {"VOL+", 28u}, {"SELECT", 29u}, {"START", 30u}, {"MENU", 31u},
};

#define MATRIX_KEY_COUNT (sizeof(g_matrix_keys) / sizeof(g_matrix_keys[0]))

static const char* matrix_key_name(uint32_t bit) {
    uint32_t i;

    for (i = 0u; i < MATRIX_KEY_COUNT; ++i) {
        if (g_matrix_keys[i].bit == (uint8_t)bit) return g_matrix_keys[i].name;
    }
    return 0;
}

/*
 * WHAT THE SETTLE PASS THREW AWAY, said out loud, and one second chance for it.
 *
 * The settle marks any slot that moves during 1.5 s of "hands off" and the reporting loop
 * then skips that slot for the WHOLE RUN -- silently. That is exactly how a working key
 * goes missing from a transcript: L1 (bit 0) reported cleanly in two runs and was absent
 * from the third with no line of output explaining why, which sent the search after the
 * one thing that had changed (the driven read on pad 93) instead of after the filter.
 *
 * Two changes, both cheap:
 *   1. Name what was dropped. A named key in that list means "this key was moving while you
 *      were told not to touch it", which is a finding about the run, not about the board.
 *   2. Re-check the dropped bits for another 0.75 s and forgive the ones that now sit still.
 *      A switch brushed during settling chatters once and then stays put; a line tracking a
 *      clock never does. Same rescue idea as map_settle()'s floating-input pass, applied to
 *      matrix bits instead of pins. Nothing is driven and no mode is touched, so a bit that
 *      is genuinely being scanned stays excluded and the flood protection still holds.
 */
static void kpdmon_settle_verdict(void) {
    uint32_t i;
    uint32_t t;
    uint32_t dropped = 0u;
    uint32_t rescued = 0u;
    uint32_t named = 0u;

    put_str("kpdmon: matrix bits that would not sit still while settling:");
    for (i = MAP_PINS; i < MAP_SLOTS; ++i) {
        const uint32_t bit = i - MAP_PINS;
        const char* name;
        if (g_map_unstable[i] == 0u) continue;
        ++dropped;
        if (dropped > 16u) continue;
        kv_dec(" ", (int32_t)bit);
        name = matrix_key_name(bit);
        if (name != 0) {
            ++named;
            put_char('(');
            put_str(name);
            put_char(')');
        }
    }
    if (dropped == 0u) {
        put_str(" none\n");
        flush_line();
        return;
    }
    if (dropped > 16u) put_str(" ...");
    put_char('\n');
    if (named != 0u) {
        put_str("kpdmon: a NAMED key in that list was moving during 'hands off' -- it would be\n");
        put_str("kpdmon: dropped for the whole run, so nothing is reported for it. Re-checking.\n");
    }
    flush_line();

    map_sample(g_map_base);
    for (t = 0u; t < MAP_SETTLE_TICKS / 2u; ++t) {
        map_sample(g_map_now);
        for (i = MAP_PINS; i < MAP_SLOTS; ++i) {
            if (g_map_unstable[i] == 0u) continue;
            if (g_map_now[i] != g_map_base[i]) g_map_unstable[i] = 2u; /* still moving */
        }
        map_pump();
    }

    put_str("kpdmon: quiet the second time, watching after all:");
    for (i = MAP_PINS; i < MAP_SLOTS; ++i) {
        const uint32_t bit = i - MAP_PINS;
        const char* name;
        if (g_map_unstable[i] != 1u) continue;
        g_map_unstable[i] = 0u;
        ++rescued;
        kv_dec(" ", (int32_t)bit);
        name = matrix_key_name(bit);
        if (name != 0) {
            put_char('(');
            put_str(name);
            put_char(')');
        }
    }
    if (rescued == 0u) put_str(" none");
    put_char('\n');
    flush_line();
}

static const wire_key_t g_wire_keys[] = {
    {"START", 12u, 11u, 0u}, {"MENU", 2u, 11u, 0u},
    {"VOL+", 11u, 167u, 1u}, {"VOL-", 11u, 75u, 0u},
    /* Measured by this command's own unmapped-short loop: 11--168, while SELECT was the
     * key being held. Rides pad 11's existing pulse, so the poll is three pulses for five
     * keys -- pad 12 for START, pad 2 for MENU, pad 11 for the other three. */
    {"SELECT", 11u, 168u, 0u},
};

enum {
    WIRE_KEY_COUNT = (uint32_t)(sizeof(g_wire_keys) / sizeof(g_wire_keys[0])),
    /* ~30 us, in the 33-cycles-per-microsecond units mt6592_delay_cycles() takes. Eight
     * distinct drive pads, so a poll spends ~240 us pulsing and the rest of its 10 ms
     * tick idle. */
    WIRE_SETTLE_CYCLES = 1000u,
};

/* 0 = not in the table, 1 = tracked, pull left as found, 2 = tracked and changed. */
static uint8_t g_wirekey_touched[MAP_PAD_MAX + 1u];
static uint8_t g_wirekey_pull[MAP_PAD_MAX + 1u]; /* pull as found, restored at the end */
static uint8_t g_wirekey_idle[MAP_PAD_MAX + 1u]; /* pull to hold between pulses */
static uint8_t g_wirekey_mode[MAP_PAD_MAX + 1u]; /* mode as found, for the probe pads only */
static uint8_t g_wirekey_dir[MAP_PAD_MAX + 1u];  /* dir as found, ditto */
static uint8_t g_wirekey_remuxed[MAP_PAD_MAX + 1u];

/*
 * Every pad a button on this connector is known to land on, read on EVERY pulse so a short
 * nobody predicted still gets reported: the seven button pads plus the three keypad sense
 * lines. Any short between two of these that the table does not already name is a key this
 * table does not know about. It is also the set wire_keys_arm() pulls up, and the set the
 * passive-level half samples, so it has to stay a superset of both tables.
 *
 * SELECT is why this exists, AND IT WORKED: this loop reported 11--168 while SELECT was
 * held, and SELECT is now the eleventh entry in the table above. The prediction it was
 * built on was right in its mechanism -- SELECT shorts a keypad sense line to a pad that
 * was parked with a pull-DOWN, so pressing it dragged sense 168 low with nobody driving
 * anything and the KPD engine latched it, which is why it looked like a matrix key and why
 * it died the moment wire_keys_arm() started forcing pull-ups. It was also right that
 * `kpdwire` could never have found it: kpdwire drops a candidate that is already low at
 * rest as a fixed connection, and a parked pull-down is exactly that.
 *
 * Whether B/X/Y/L1/L2/R1 were the same story was the open question, and the census said
 * they were NOT -- six matrix keys against exactly six reachable bits left no room for them
 * to be anything else. The fourth run confirmed it key by key: all six answered on the
 * matrix half, none of them as a short. That question is closed.
 *
 * PAD 35 IS OUT OF THIS LIST. It free-runs -- ten level changes in thirty seconds with
 * nobody touching the device -- so it reports a short against whichever pad is being pulsed
 * whenever it happens to be low, which is what the fourth run's lone "pad 20 -- pad 35" was.
 * A probe pad has to be quiet at rest or it manufactures findings, and this list is the
 * instrument's own sense set: a bad entry here is worse than a missing one.
 */
/* The four D-pad EINT pads and nothing else. Everything the old list carried besides these
 * -- 2, 11, 12, 75, 167, 168 -- is a keypad row or column line, and pad 30 was the far side
 * of a pair that turned out not to exist. Probing a KPD line tells you the state of a column
 * the block is scanning, dressed up as a button. */
static const uint8_t g_wire_probe[] = {93u, 45u, 20u, 8u};

enum { WIRE_PROBE_COUNT = (uint32_t)(sizeof(g_wire_probe) / sizeof(g_wire_probe[0])) };

static uint16_t g_wire_probe_now[WIRE_PROBE_COUNT];  /* shorts seen on the last pass */
static uint16_t g_wire_probe_base[WIRE_PROBE_COUNT]; /* shorts present with nothing held */
static uint8_t g_wire_probe_hold[WIRE_PROBE_COUNT][WIRE_PROBE_COUNT];
static uint8_t g_wire_probe_done[WIRE_PROBE_COUNT][WIRE_PROBE_COUNT];

/* Pads the KPD block owns. Sensing one is free -- DIN reports the real level even in
 * mux mode 1 -- but driving one fights the keypad engine, so the table above never
 * puts a keypad pad in a `drive` field and this is the check that says so. */
/*
 * ALL EIGHT KPD LINES, not the five this used to list. The MT6592 KPD block on this board
 * owns two row pads it strobes (74 = KPROW0, 92 = KPROW1), a third (11 = KPROW3) and five
 * column pads (75, 167, 168, 12, 2 = KPCOL0..4). The old five-pad version left 11, 12 and 2
 * unprotected, and every command in this file that takes a pad took them: the wire scan
 * pulsed 11 as a row strobe and read 12 and 2 as columns, which is a hand-rolled keypad
 * matrix that works by taking row 3 and columns 3 and 4 away from the block that was
 * already scanning them.
 *
 * Pad 93 is deliberately NOT here. It is KPROW2's pad and this board spent it on UP's EINT,
 * so it is a plain GPIO input and probing it is how UP is read. See mt6592_board_j36.h.
 */
static uint32_t wire_pad_is_keypad(uint32_t pin) {
    return pin == 74u || pin == 92u || pin == 11u || pin == 75u || pin == 167u ||
           pin == 168u || pin == 12u || pin == 2u;
}

/*
 * START (11-12), R2 (12-167) and VOL+ (11-167) formed the board's one and only cycle, so
 * holding any two of them shorted pads 11, 12 and 167 into a single node and the third read
 * closed. Derived from the pads rather than from hardcoded indices, so it stays true if the
 * table is edited: a key is in the triangle when BOTH its pads are.
 *
 * R2 HAS LEFT THE TABLE, so the cycle is gone and this resolves nothing -- which the
 * derivation catches only if it also counts the members. Two of the three still qualify by
 * pads (START and VOL+), and two edges are not a cycle: without the count check below,
 * holding START and VOL+ together would drop VOL+ for no reason at all. That is the exact
 * failure mode a derived rule is supposed to avoid, and it took removing one table entry to
 * expose it, so the count stays whatever the table does next.
 */
static uint32_t wire_pad_in_triangle(uint32_t pin) {
    return pin == 11u || pin == 12u || pin == 167u;
}

static uint32_t wire_key_in_triangle(const wire_key_t* k) {
    return wire_pad_in_triangle(k->drive) && wire_pad_in_triangle(k->sense);
}

static uint32_t wire_probe_index(uint32_t pin) {
    uint32_t i;

    for (i = 0u; i < WIRE_PROBE_COUNT; ++i) {
        if (g_wire_probe[i] == pin) return i;
    }
    return WIRE_PROBE_COUNT;
}

/* Is this short one the table already accounts for? Checked both ways round, because a
 * short has no direction and the table's drive/sense split is a choice, not a fact. */
static uint32_t wire_pair_is_mapped(uint32_t a, uint32_t b) {
    uint32_t i;

    for (i = 0u; i < WIRE_KEY_COUNT; ++i) {
        if (g_wire_keys[i].drive == a && g_wire_keys[i].sense == b) return 1u;
        if (g_wire_keys[i].drive == b && g_wire_keys[i].sense == a) return 1u;
    }
    return 0u;
}

static uint32_t wire_pad_idle_pull(uint32_t pin) {
    if (pin <= MAP_PAD_MAX && g_wirekey_touched[pin] != 0u) return g_wirekey_idle[pin];
    return 2u;
}

/*
 * May this pad be pulsed?
 *
 * A MUXED pad is no longer refused, which is the fix for six dead buttons. The old rule
 * -- mode 0 or nothing -- was written to guarantee the scan could never steal a pad from
 * a peripheral, and it cost UP/DOWN/LEFT/RIGHT/A/R2: their drive pads are 8, 20, 30, 35
 * and 45, whose resting mux was never recorded, while the four keys that DID work drive
 * pads 2, 11 and 12. That is the whole of the difference between the working set and the
 * dead set, and it is a software rule rather than a wire.
 *
 * The pad is forced to mode 0 for the ~30 us of the pulse and put straight back, which
 * is exactly what `kpdwire` does to its named pin and has done to 11, 12, 30, 75 and 167
 * without incident. What is still refused is what could actually break something: a
 * keypad pad (the KPD engine drives those), a reserved pad (panel power, backlight), and
 * a mode-0 pad someone else has already made an OUTPUT.
 */
static uint32_t wire_drive_ok(uint32_t pin) {
    if (pin > MAP_PAD_MAX) return 0u;
    if (wire_pad_is_keypad(pin) || pin_is_reserved(pin)) return 0u;
    if (gpio_mode_get(pin) == 0u && gpio_bit_get(GPIOC_DIR, pin) != 0u) return 0u;
    return 1u;
}

/* One pulse per distinct drive pad; later entries sharing it ride along. Returns a bit
 * per g_wire_keys[] entry, set when that switch reads closed, and fills
 * g_wire_probe_now[] with every short seen between probe pads, mapped or not. */
static uint32_t wire_keys_scan(void) {
    uint32_t closed = 0u;
    uint32_t i;
    uint32_t j;

    for (i = 0u; i < WIRE_PROBE_COUNT; ++i) g_wire_probe_now[i] = 0u;

    for (i = 0u; i < WIRE_KEY_COUNT; ++i) {
        const uint32_t drive = g_wire_keys[i].drive;
        uint32_t already = 0u;
        uint32_t save_mode;
        uint32_t d;

        for (j = 0u; j < i; ++j) {
            if (g_wire_keys[j].drive == drive) already = 1u;
        }
        if (already || !wire_drive_ok(drive)) continue;

        save_mode = gpio_mode_get(drive);
        if (save_mode != 0u) gpio_mode_set(drive, 0u);

        /* DOUT before DIR, so the pad is never an output driving HIGH for a cycle. */
        gpio_bit_write(GPIOC_DOUT, drive, 0u);
        gpio_bit_write(GPIOC_PULLEN, drive, 0u);
        gpio_bit_write(GPIOC_DIR, drive, 1u);
        mt6592_delay_cycles(WIRE_SETTLE_CYCLES);

        for (j = i; j < WIRE_KEY_COUNT; ++j) {
            if (g_wire_keys[j].drive != drive) continue;
            if (gpio_bit_get(GPIOC_DIN, g_wire_keys[j].sense) == 0u) closed |= 1u << j;
        }
        d = wire_probe_index(drive);
        if (d < WIRE_PROBE_COUNT) {
            for (j = 0u; j < WIRE_PROBE_COUNT; ++j) {
                if (j == d) continue;
                if (gpio_bit_get(GPIOC_DIN, g_wire_probe[j]) == 0u) {
                    g_wire_probe_now[d] |= (uint16_t)(1u << j);
                }
            }
        }

        gpio_bit_write(GPIOC_DIR, drive, 0u);
        gpio_pull_set(drive, wire_pad_idle_pull(drive));
        if (save_mode != 0u) gpio_mode_set(drive, save_mode);
    }
    return closed;
}

/*
 * PASSIVE LEVELS -- the half of this connector the wire scan is blind to, and the half
 * that turned out to hold the D-pad.
 *
 * A pulse scan can only see a switch between two pads that both idle high. A switch
 * between a pad and something ALREADY LOW -- ground, or a muxed pad its owner holds low,
 * pad 30 being exactly that -- does not need a pulse and cannot be attributed by one: the
 * pad just reads 0 for as long as the button is held, no matter which pad is being driven.
 * That is what produced the twenty-five-line short storm on the third hardware run: every
 * pair among the eight button pads was reported, which is not a wiring pattern, it is one
 * pad sitting low while eight pulses walked past it.
 *
 * So this samples every probe pad with NOTHING driven, once per tick, and reports the
 * pads that move. A named pad prints its button; an unnamed one asks. The same mask then
 * suppresses the storm (a follower that is low anyway is not a short) and arms the verdict
 * test below.
 */
static uint16_t g_probe_idle_now;
static uint16_t g_probe_idle_level;
static uint8_t g_probe_idle_hold[WIRE_PROBE_COUNT];

/*
 * Buttons that read as a passive low. DOWN, LEFT and RIGHT each pull their own pad down
 * when pressed and all three work; pad 12 goes low for A and for R2 alike, and the verdict
 * test has now confirmed that as a shared node rather than a reporting artefact.
 *
 * UP IS PAD 93, from the stock kernel's own EINT table (GPIO 93 -> keycode 48 -> DPAD_UP),
 * which is the same table that supplies the three pads above -- all three confirmed by hand.
 * Pad 35 sat here for three revisions and was never in that table; it is muxed, idles low
 * and flapped ten times in thirty seconds with the device untouched. Pad 93 was displaced by
 * it partly on the strength of a sweep that never touched pad 93 at all (`sweep_is_skipped`
 * excludes 74, 92 and 93 as suspected strobes), and a pad an instrument skips is not a pad
 * the instrument has cleared.
 *
 * A AND R2 ARE NOT HERE ANY MORE, and pad 12 is why: it is KPCOL3, a keypad column line, and
 * both of those buttons are matrix keys on it (R2 = bit 3, A = bit 12). Reading a column line
 * as a button level is what produced "A-and-R2", one entry for two keys that the matrix
 * separates without difficulty. Pad 12 is now in wire_pad_is_keypad() and no command here
 * touches it.
 */
static const struct {
    const char* name;
    uint8_t pad;
} g_level_keys[] = {
    {"UP", 93u}, {"DOWN", 45u}, {"LEFT", 20u}, {"RIGHT", 8u},
};

enum { LEVEL_KEY_COUNT = (uint32_t)(sizeof(g_level_keys) / sizeof(g_level_keys[0])) };

static const char* level_key_name(uint32_t pad) {
    uint32_t i;

    for (i = 0u; i < LEVEL_KEY_COUNT; ++i) {
        if (g_level_keys[i].pad == pad) return g_level_keys[i].name;
    }
    return 0;
}

/*
 * A PAD THAT WILL NOT IDLE HIGH ON A PULL-UP, READ BY DRIVING IT HIGH INSTEAD.
 *
 * Pad 93 -- UP -- is the reason this exists, and the two console readings that pinned it
 * down are worth keeping verbatim because they separate three explanations that all look
 * the same from a distance:
 *
 *     pin 5d 0 0 2 0  ->  mode=0 dir=0 pull=2 dout=0 din=0     input, pulled UP, still low
 *     pin 5d          ->  din=0 HELD and din=0 RELEASED        the pull-up never wins
 *     pin 5d 0 1 0 1  ->  mode=0 dir=1 pull=0 dout=1
 *     pin 5d          ->  din=1 released, din=0 held           the DRIVER always wins
 *
 * Not a dead pad, not a dead switch, not a wrong pad number: the switch is an ordinary one
 * to ground and the input buffer is perfect. Something on this board loads pad 93 harder
 * than the internal pull-up can fight, and no register makes that resistor bigger. So the
 * console supplies the high with the output driver for ~30 us, samples DIN while the pad is
 * still driven, and hands the pad straight back. Released, the driver wins and DIN reads 1.
 * Held, the closed switch wins and DIN reads 0 -- the driver is deliberately not strong
 * enough to beat a short, which is exactly what makes the reading mean something.
 *
 * Holding the button shorts that driver to ground for the 30 us it is on, once per 10 ms
 * tick. The OS keypad driver does the same thing for the same pad with a 1 us window; the console
 * uses the longer WIRE_SETTLE_CYCLES it already uses for every other drive.
 */
static uint8_t g_probe_driven[WIRE_PROBE_COUNT];

static uint32_t wire_pad_read_driven(uint32_t pin) {
    uint32_t value;

    gpio_bit_write(GPIOC_PULLEN, pin, 0u); /* no resistor left fighting the driver */
    gpio_bit_write(GPIOC_DOUT, pin, 1u);   /* DOUT before DIR, always */
    gpio_bit_write(GPIOC_DIR, pin, 1u);
    mt6592_delay_cycles(WIRE_SETTLE_CYCLES);
    value = gpio_bit_get(GPIOC_DIN, pin);
    gpio_bit_write(GPIOC_DIR, pin, 0u);    /* stop driving the instant the sample is taken */
    gpio_pull_set(pin, 2u);
    return value;
}

/*
 * Which probe pads cannot idle high, measured rather than assumed. Call once, after
 * wire_keys_arm() has put a pull-up on every probe pad and the settle has run: a pad still
 * reading 0 with nothing held has nowhere to move when its switch closes, so it goes on the
 * driven list. A pad the user happens to be holding at this moment is a false positive and
 * costs nothing -- the driven read is correct for a healthy pad too.
 */
static void wire_probe_pick_driven(void) {
    uint32_t i;
    uint32_t driven = 0u;

    for (i = 0u; i < WIRE_PROBE_COUNT; ++i) {
        g_probe_driven[i] = 0u;
        if (gpio_bit_get(GPIOC_DIN, g_wire_probe[i]) != 0u) continue;
        g_probe_driven[i] = 1u;
        ++driven;
        kv_dec("kpdmon: pad ", (int32_t)g_wire_probe[i]);
        put_str(" will not idle high on its pull-up -- reading it DRIVEN instead\n");
    }
    if (driven == 0u) put_str("kpdmon: every probe pad idles high; no driven reads needed\n");
    flush_line();
}

static uint16_t wire_probe_sample_idle(void) {
    uint16_t low = 0u;
    uint32_t i;

    for (i = 0u; i < WIRE_PROBE_COUNT; ++i) {
        const uint32_t level = g_probe_driven[i] != 0u
                                   ? wire_pad_read_driven(g_wire_probe[i])
                                   : gpio_bit_get(GPIOC_DIN, g_wire_probe[i]);
        if (level == 0u) low |= (uint16_t)(1u << i);
    }
    return low;
}

/*
 * Drive `drive` HIGH and read `sense`. This is the test that separates a real pad-to-pad
 * switch from a sense pad that is merely tied to something low, and nothing else can: a
 * closed pair follows its driver in BOTH directions, while a grounded pad ignores it.
 * Returns the sense level, or 2 if the pad may not be driven.
 *
 * Safe for the same reason wire_keys_report()'s hi= test is: MODE is forced to 0 first, so
 * whatever owns the pad is disconnected before anything drives it, and the drive lasts
 * ~30 us against a KP_DEBOUNCE of ~32 ms.
 */
static uint32_t wire_drive_high_read(uint32_t drive, uint32_t sense) {
    uint32_t save_mode;
    uint32_t got;

    if (!wire_drive_ok(drive)) return 2u;
    save_mode = gpio_mode_get(drive);
    if (save_mode != 0u) gpio_mode_set(drive, 0u);
    gpio_bit_write(GPIOC_PULLEN, drive, 0u);
    gpio_bit_write(GPIOC_DOUT, drive, 1u);
    gpio_bit_write(GPIOC_DIR, drive, 1u);
    mt6592_delay_cycles(WIRE_SETTLE_CYCLES);
    got = gpio_bit_get(GPIOC_DIN, sense);
    gpio_bit_write(GPIOC_DIR, drive, 0u);
    gpio_bit_write(GPIOC_DOUT, drive, 0u);
    gpio_pull_set(drive, wire_pad_idle_pull(drive));
    if (save_mode != 0u) gpio_mode_set(drive, save_mode);
    return got;
}

/* Report the raw mask and the decoded one separately: the raw mask is the measurement
 * and the decode is a policy, and conflating them is how a policy becomes a fact. */
static uint32_t wire_keys_resolve(uint32_t closed) {
    uint32_t triangle = 0u;
    uint32_t drop = 0u;
    uint32_t members = 0u;
    uint32_t i;

    for (i = 0u; i < WIRE_KEY_COUNT; ++i) {
        if (!wire_key_in_triangle(&g_wire_keys[i])) continue;
        triangle |= 1u << i;
        ++members;
        if (g_wire_keys[i].ghost_drop != 0u) drop |= 1u << i;
    }
    /* Three edges or it is not a cycle. See above: the table now has two of them. */
    if (members < 3u) return closed;
    if (triangle == 0u || (closed & triangle) != triangle) return closed;
    return closed & ~drop;
}

/*
 * Give every non-keypad pad in the table an input pull-UP for the run, and record what
 * it was so it can be put back.
 *
 * THIS IS THE OPPOSITE OF WHAT THE PREVIOUS VERSION DID, and the reversal is the whole
 * lesson of the second hardware run. That version preserved a parked pull-DOWN, on the
 * theory that the pull-down was carrying somebody else's key. The theory was right about
 * SELECT and catastrophically wrong about everything else, because the run showed that
 * ALL EIGHT button pads are parked pull-down and idle LOW:
 *
 *     drv 2 p1 n0   8 p1 n0   11 p1 n0   12 p1 n0   20 p1 n0   30 p1 n0   35 p1 n0
 *     45 p1 n0      sns 75 p2 n1   167 p0 n1
 *
 * Only the keypad senses idle high. A sense pad that idles LOW cannot report a closure
 * at all -- it already reads 0 -- which is exactly why A, START and MENU came back
 * "SHORTED AT REST" with mask 0x00d0: their sense pads are 12 and 11, both parked low.
 * The pull-up is not an optional hardening step, it is the thing that makes a closure
 * observable, and it has to be applied to a muxed pad too, which the old candidate rule
 * refused to do.
 *
 * What that costs is SELECT, and the cost is correct rather than regrettable. SELECT
 * reads today by shorting pad 11 to keypad sense 168 and letting 168's pull-up drag the
 * parked pad 11 up; with pad 11 pulled up as well, nothing moves. That is not a key this
 * table can afford to read by accident -- it is the reason START and MENU appeared to
 * work in the run, both of them flipping every time SELECT was pressed because their
 * shared sense pad 11 was the pad SELECT was moving. SELECT belongs in the table as the
 * pair 11--168, once `kpdscan` has confirmed it, and then it reads like the rest.
 *
 * IT ARMS THE WHOLE PROBE LIST, not just the wire table's pads, and that became mandatory
 * the moment the D-pad and A left the wire table: pads 8, 20, 45 and 12 are now read as
 * passive LEVELS, and a level pad without a pull-up is exactly the dead read this function
 * was written to prevent. Arming the probe list covers both halves at once and cannot drift
 * out of step with either, because the probe list is a superset of both by construction.
 */
static void wire_pad_report(uint32_t pin); /* defined below, next to the per-key report */
static uint32_t kpd_lines_apply(const char* tag); /* defined below, next to `kpdmux` */

static void wire_keys_arm(void) {
    uint32_t i;
    uint32_t p;

    for (p = 0u; p <= MAP_PAD_MAX; ++p) {
        g_wirekey_touched[p] = 0u;
        g_wirekey_remuxed[p] = 0u;
    }
    /*
     * THE PROBE PADS ARE THE D-PAD, AND THIS LOOP USED TO GIVE UP ON EXACTLY THE ONE THAT
     * DOES NOT WORK. It skipped any pad reading as an output ("somebody drives it") and
     * never touched MODE at all, so a level key whose pad had been muxed or turned around
     * by something upstream got no pull-up and reported nothing -- which is pad 93, UP,
     * reported low at rest on every run. A pad in this list is one this console has
     * declared to be a button; taking it, reading it and handing it back is the whole job.
     * So: force mode 0 and input, save both, restore both in wire_keys_restore().
     */
    for (i = 0u; i < WIRE_PROBE_COUNT; ++i) {
        p = g_wire_probe[i];
        if (p > MAP_PAD_MAX || wire_pad_is_keypad(p) || pin_is_reserved(p)) continue;
        if (g_wirekey_touched[p] != 0u) continue;
        g_wirekey_mode[p] = (uint8_t)gpio_mode_get(p);
        g_wirekey_dir[p] = (uint8_t)gpio_bit_get(GPIOC_DIR, p);
        if (g_wirekey_mode[p] != 0u || g_wirekey_dir[p] != 0u) {
            put_str("kpdmon: reclaiming pad");
            wire_pad_report(p);
            put_str(" -- forcing mode 0 input for the press to be visible\n");
            flush_line();
            g_wirekey_remuxed[p] = 1u;
            if (g_wirekey_mode[p] != 0u) gpio_mode_set(p, 0u);
            gpio_bit_write(GPIOC_DIR, p, 0u);
        }
        g_wirekey_pull[p] = (uint8_t)gpio_pull_get(p);
        g_wirekey_idle[p] = 2u;
        g_wirekey_touched[p] = 2u;
        gpio_pull_set(p, 2u);
    }
    /* Belt and braces: a wire pair whose pad someone forgets to add to the probe list
     * would otherwise be scanned without a pull-up, which reads as permanently pressed. */
    for (i = 0u; i < WIRE_KEY_COUNT; ++i) {
        const uint32_t pads[2] = {g_wire_keys[i].drive, g_wire_keys[i].sense};
        uint32_t k;

        for (k = 0u; k < 2u; ++k) {
            p = pads[k];
            if (p > MAP_PAD_MAX || wire_pad_is_keypad(p) || pin_is_reserved(p)) continue;
            if (g_wirekey_touched[p] != 0u) continue;
            if (gpio_bit_get(GPIOC_DIR, p) != 0u) continue; /* somebody drives it */
            g_wirekey_pull[p] = (uint8_t)gpio_pull_get(p);
            g_wirekey_idle[p] = 2u;
            g_wirekey_touched[p] = 2u;
            gpio_pull_set(p, 2u);
        }
    }
}

static void wire_keys_restore(void) {
    uint32_t p;

    for (p = 0u; p <= MAP_PAD_MAX; ++p) {
        if (g_wirekey_touched[p] == 2u) gpio_pull_set(p, g_wirekey_pull[p]);
        if (g_wirekey_remuxed[p] != 0u) {
            gpio_bit_write(GPIOC_DIR, p, g_wirekey_dir[p]);
            if (g_wirekey_mode[p] != 0u) gpio_mode_set(p, g_wirekey_mode[p]);
            g_wirekey_remuxed[p] = 0u;
        }
        g_wirekey_touched[p] = 0u;
    }
}

/* Mode, direction, pull and level of one pad, in the compact form the per-key report
 * below prints twice per line. */
static void wire_pad_report(uint32_t pin) {
    kv_dec(" ", (int32_t)pin);
    kv_dec(" m", (int32_t)gpio_mode_get(pin));
    kv_dec(" d", (int32_t)gpio_bit_get(GPIOC_DIR, pin));
    kv_dec(" p", (int32_t)gpio_pull_get(pin));
    kv_dec(" n", (int32_t)gpio_bit_get(GPIOC_DIN, pin));
}

/*
 * One line per key, with the pulse actually performed so the line is a measurement.
 *
 * `hi=` is the part worth reading first, and it exists because `own=` -- the drive pad's
 * own level while being driven LOW -- turned out to prove nothing. Every button pad on
 * this board is parked pull-down and already reads 0, so `own=0` is what a pad reads
 * whether the driver works or not. The first run reported own=0 on all ten and that line
 * was worth exactly nothing.
 *
 * So the pad is driven HIGH first and read back. That is a real test: it has to win
 * against the parked pull-down, and if it does not, the pad is unusable and no switch on
 * it can be read whatever the table says. It is also safe in a way that driving high
 * would not normally be, because MODE is forced to 0 first -- which disconnects whatever
 * peripheral owns the pad before anything drives it, so there is no contention to have.
 */
static void wire_keys_report(void) {
    uint32_t i;

    for (i = 0u; i < WIRE_KEY_COUNT; ++i) {
        const uint32_t drive = g_wire_keys[i].drive;
        const uint32_t sense = g_wire_keys[i].sense;

        put_str("kpdmon: ");
        put_str(g_wire_keys[i].name);
        put_str("\tdrv");
        wire_pad_report(drive);
        put_str("  sns");
        wire_pad_report(sense);
        if (!wire_drive_ok(drive)) {
            put_str("  *** DRIVE REFUSED\n");
        } else {
            const uint32_t save_mode = gpio_mode_get(drive);
            uint32_t hi;
            uint32_t lo;
            uint32_t got;

            if (save_mode != 0u) gpio_mode_set(drive, 0u);
            gpio_bit_write(GPIOC_PULLEN, drive, 0u);
            gpio_bit_write(GPIOC_DOUT, drive, 1u);
            gpio_bit_write(GPIOC_DIR, drive, 1u);
            mt6592_delay_cycles(WIRE_SETTLE_CYCLES);
            hi = gpio_bit_get(GPIOC_DIN, drive);
            gpio_bit_write(GPIOC_DOUT, drive, 0u);
            mt6592_delay_cycles(WIRE_SETTLE_CYCLES);
            lo = gpio_bit_get(GPIOC_DIN, drive);
            got = gpio_bit_get(GPIOC_DIN, sense);
            gpio_bit_write(GPIOC_DIR, drive, 0u);
            gpio_pull_set(drive, wire_pad_idle_pull(drive));
            if (save_mode != 0u) gpio_mode_set(drive, save_mode);

            kv_dec("  hi=", (int32_t)hi);
            kv_dec(" lo=", (int32_t)lo);
            kv_dec(" sns=", (int32_t)got);
            if (hi == 0u || lo != 0u) {
                put_str("  *** DRIVE DEAD (pad will not follow its own driver)");
            } else if (got == 0u) {
                put_str("  *** SENSE STUCK LOW (cannot report a closure)");
            }
            put_char('\n');
        }
        flush_line();
    }
}

/*
 * Watch the buttons by name, plus every matrix bit, filtered by time not by count.
 *
 * THE GPIO-LEVEL HALF IS GONE, and this time for a reason rather than a theory. It was
 * deleted once on the belief that every button was a matrix key, hardware refuted that,
 * and it came back. What it could never do was attribute anything: it reported "gpio 11
 * changed", and gpio 11 is the node shared by START, MENU and both volume keys, so the
 * same line meant four different buttons depending on which was pressed. Every wrong
 * entry in mt6592_board_j36.h -- R2 that was really A, volume keys that were really
 * START and MENU -- came from reading one of those lines as a button. A level on a pad
 * shared by four switches is not evidence about any of them.
 *
 * It is also incompatible with what replaced it: the wire scan pulses pads low on
 * purpose, so a level monitor running alongside would report every one of those pulses
 * as a press. `kpdmap` is still there for raw level work on a pad nobody has mapped yet.
 *
 * FIRST HARDWARE RUN, and what it changed. START, MENU, VOL+ and VOL- reported by name;
 * UP, DOWN, LEFT, RIGHT, A and R2 did not; SELECT, which had always worked, stopped. Two
 * separate causes, both software, both now fixed:
 *
 *   - the six dead keys are exactly the ones whose drive pad is 8, 20, 30, 35 or 45, and
 *     the four live ones are exactly the ones whose drive pad is 2, 11 or 12. The scan
 *     refused to pulse a pad that was not already mode 0, so those six were never driven
 *     at all. wire_drive_ok() now forces mode 0 for the pulse and puts the mux back.
 *   - SELECT died the moment wire_keys_arm() started forcing pull-ups, which is the
 *     finding: SELECT reads by dragging a keypad sense line down to a pad PARKED WITH A
 *     PULL-DOWN, so it needs no pulse and the pull-up unplugged it. See wire_keys_arm().
 *
 * R2 was the one the two explanations did not cover, and it is settled now: pad 12 IS
 * driven, because START (12-11) works, and pad 167 reads fine, because VOL+ (11-167) works,
 * and the pair 12--167 still never fired in four runs. Both halves good and the entry silent
 * means the pair does not exist. The `kpdwire a7` report that named pad 12 was pad 12 going
 * low by itself, which is the same misattribution that cost this file the D-pad and A.
 *
 * FOURTH RUN, AND WHAT IT FINISHED. All six matrix bits attributed by name (L1=0, L2=1,
 * R1=2, X=9, Y=10, B=11 -- eight presses in a known order against eight event groups; the
 * derivation is written out in mt6592_board_j36.h). A confirmed as a level on pad 12 by the
 * drive-high verdict test, and R2 confirmed to be on the same node. UP disproved: pad 35 is
 * muxed, free-running, and low at rest, so it has left both tables and `kpdlow` is the
 * command that will name the real pad.
 *
 * EVERYTHING FROM HERE UP IS THE RECORD OF A WRONG MODEL, AND THE WIRE HALF WAS THE WRONG
 * MODEL'S INSTRUMENT. It is left in because the readings it took were real and they are what
 * finally identified the lines; only the interpretation was wrong. The pairs it read --
 * START 12-11, MENU 2-11, VOL+ 11-167, VOL- 11-75, SELECT 11-168 -- are row 3 of the vendor
 * keymap, and "node 11" is KPROW3. This command was hand-strobing a keypad row that this
 * driver had unplugged from the KPD block, one pad at a time, and calling the result a third
 * read mechanism. There are two read paths on this board, not three: EINT levels and the
 * matrix. See the superseding block in mt6592_board_j36.h for the full derivation.
 *
 * WHAT THAT MEANS FOR THIS COMMAND. wire_pad_is_keypad() now covers all eight KPD lines, so
 * every entry in the wire table is refused a drive and skipped by the arming pass: the wire
 * half is inert by construction rather than by an empty array, and the reports it prints are
 * there to show it staying quiet. If a pair ever fires again, something has taken a pad back.
 *
 * THE MATRIX HALF IS NOW THE WHOLE COMMAND, and it reports by name -- g_matrix_keys[] maps
 * all thirteen bits, so one run with every button pressed once verifies the entire keymap.
 * Bits that fire without a name are real hardware and an incomplete table; a bit in row 2
 * (18..26) firing would overturn the pad-93 finding, and is called out as such.
 *
 * The passive half stays, because a level on an EINT pad is still how the D-pad reads, and it
 * runs FIRST in each tick because that is the only moment when a low pad's low is its own.
 *
 * Both halves use the same discipline: a settling pass decides what will not sit still with
 * nothing touched and drops it for the run, and after that a level must hold for
 * MAP_STABLE_TICKS before it is believed. A line tracking a clock never qualifies however
 * often it flips; a switch closure qualifies at once and can be pressed as often as you like.
 * Pad 35 is the case that shows the limit of that rule -- it flips slowly enough to pass a
 * 1.5 s settle and a 50 ms hold, and only a human noticing "that button pressed itself" told
 * it apart from a key. The settle pass filters clocks, not intermittents.
 */
static void kpdmon_run(uint32_t secs) {
    uint32_t ticks;
    uint32_t reported = 0u;
    uint32_t i;
    uint32_t wire_level;
    uint32_t wire_raw;
    uint8_t wire_hold[WIRE_KEY_COUNT];

    /* A KPD line the boot chain parked as a plain GPIO cannot scan, and three of the eight
     * (11, 12, 2 -- row 3 and columns 3 and 4) arrive parked, each wanting a different mode:
     * 3, 3 and 6, all measured by `kpdmode`. Same table and same only-if-wrong rule as
     * MVII's kpd_pads_apply(), so a bare `kpdmon` is a faithful preview of what the kernel
     * will see rather than a different experiment. */
    if (kpd_lines_apply("kpdmon: ") == 0u) {
        put_str("kpdmon: all eight KPD lines were already muxed\n");
    }

    /* Make sure the block is actually scanning before believing a quiet result:
     * a disabled keypad reads "no keys pressed" forever. */
    mtk_write32(KPD_BASE + 0x18u, 0x0400u);
    mtk_write32(KPD_BASE + 0x24u, 1u);
    dump_kpd();

    /* Matrix bits are watched; GPIO levels are not, per the note above. Marking the
     * whole GPIO half unstable is how that is expressed, so the reporting loop below
     * needs no special case. */
    for (i = 0u; i < MAP_SLOTS; ++i) g_map_unstable[i] = 0u;
    for (i = 0u; i < MAP_PINS; ++i) g_map_unstable[i] = 1u;

    /* The level pads BEFORE arming, because arming changes three of the four fields and
     * the interesting question is what the boot chain left behind. Pad 93 is the reason
     * this report exists: it reads low at rest, and m/d/p says which kind of low. */
    put_str("kpdmon: level keys as the boot chain left them:\n");
    for (i = 0u; i < LEVEL_KEY_COUNT; ++i) {
        put_str("kpdmon:   ");
        put_str(g_level_keys[i].name);
        wire_pad_report(g_level_keys[i].pad);
        put_char('\n');
    }
    flush_line();

    wire_keys_arm();
    put_str("kpdmon: wire pairs -- all pads are KPD lines now, so all of these must stay silent:\n");
    wire_keys_report();
    put_str("kpdmon: settling, hands off\n");
    flush_line();

    map_sample(g_map_base);
    for (ticks = 0u; ticks < MAP_SETTLE_TICKS; ++ticks) {
        map_sample(g_map_now);
        for (i = 0u; i < MAP_SLOTS; ++i) {
            if (g_map_now[i] != g_map_base[i]) g_map_unstable[i] = 1u;
        }
        map_pump();
    }

    /* Say which matrix bits the settle just discarded, and give them one more chance. Until
     * this existed, a key that twitched during "hands off" was dropped without a word. */
    kpdmon_settle_verdict();

    put_str("kpdmon: watching all 80 matrix bits; 13 of them have names\n");

    /* Adopt the settled levels as "at rest" and start the hold counters clean. */
    map_sample(g_map_level);
    for (i = 0u; i < MAP_SLOTS; ++i) g_map_hold[i] = 0u;
    for (i = 0u; i < WIRE_KEY_COUNT; ++i) wire_hold[i] = 0u;
    wire_level = wire_keys_resolve(wire_keys_scan());
    /* Whatever is shorted with nothing held is this board's baseline, not a finding. */
    for (i = 0u; i < WIRE_PROBE_COUNT; ++i) {
        uint32_t k;

        g_wire_probe_base[i] = g_wire_probe_now[i];
        for (k = 0u; k < WIRE_PROBE_COUNT; ++k) {
            g_wire_probe_hold[i][k] = 0u;
            g_wire_probe_done[i][k] = 0u;
        }
    }
    if (wire_level != 0u) {
        kv_hex("kpdmon: WARNING -- something already reads closed at rest, mask=",
               wire_level, 4u);
        put_char('\n');
        put_str("kpdmon: hands off during settling, or a pad in the table is wrong\n");
    }

    /* Which pads need driving before their level means anything -- measured here, with the
     * pull-ups armed and the settle already done, so the answer is about this board and not
     * about this build's assumptions. Must run before the baseline below, or pad 93 goes
     * into the baseline as "already low" and can never report a press. */
    wire_probe_pick_driven();

    /* Resting passive levels. A pad already low here is one the pulse scan cannot use as a
     * sense, and a pad that goes low LATER is a button wired to something low. */
    g_probe_idle_level = wire_probe_sample_idle();
    for (i = 0u; i < WIRE_PROBE_COUNT; ++i) g_probe_idle_hold[i] = 0u;
    put_str("kpdmon: still low at rest, pulled up or driven:");
    if (g_probe_idle_level == 0u) {
        put_str(" none");
    } else {
        for (i = 0u; i < WIRE_PROBE_COUNT; ++i) {
            if (((g_probe_idle_level >> i) & 1u) != 0u) kv_dec(" pad ", (int32_t)g_wire_probe[i]);
        }
    }
    put_char('\n');

    kv_dec("kpdmon: watching for ", (int32_t)secs);
    put_str(" seconds -- press ONE button at a time and write down the order\n");
    flush_line();

    for (ticks = 0u; ticks < secs * 100u; ++ticks) {
        /* Matrix and scan memories FIRST, before this tick pulses anything: a pulse
         * drags the keypad column its pad sits on, so this is the honest sample. */
        map_sample(g_map_now);
        /* Passive levels before anything is driven, for the same reason: this is the only
         * moment in the tick when a low pad's low is its own. */
        g_probe_idle_now = wire_probe_sample_idle();
        wire_raw = wire_keys_resolve(wire_keys_scan());

        for (i = 0u; i < WIRE_KEY_COUNT; ++i) {
            const uint32_t now = (wire_raw >> i) & 1u;
            uint32_t sns_idx;
            if (now == ((wire_level >> i) & 1u)) {
                wire_hold[i] = 0u;
                continue;
            }
            if (++wire_hold[i] < (uint8_t)MAP_STABLE_TICKS) continue;
            wire_hold[i] = 0u;
            wire_level = (wire_level & ~(1u << i)) | (now << i);

            put_str("kpdmon: ");
            put_str(g_wire_keys[i].name);
            put_str(now != 0u ? " PRESSED" : " released");
            kv_dec("   (pads ", (int32_t)g_wire_keys[i].drive);
            kv_dec("-", (int32_t)g_wire_keys[i].sense);
            put_str(")\n");
            /* If the sense pad is low with nothing driven, this entry proved nothing: the
             * pad would read 0 whichever pad we pulsed. Settle it here, while the button is
             * still held, by driving the pair the other way. */
            sns_idx = wire_probe_index(g_wire_keys[i].sense);
            if (now != 0u && sns_idx < WIRE_PROBE_COUNT &&
                ((g_probe_idle_now >> sns_idx) & 1u) != 0u) {
                const uint32_t hi = wire_drive_high_read(g_wire_keys[i].drive,
                                                        g_wire_keys[i].sense);
                kv_dec("kpdmon:   ^ sense pad low with nothing driven; drive high -> sns=",
                       (int32_t)hi);
                put_str(hi == 1u ? "  => REAL PAIR\n"
                                 : "  => NOT A PAIR (sense tied low elsewhere)\n");
            }
            if (++reported >= KPDMON_EVENT_LIMIT) {
                put_str("kpdmon: event limit, stopping\n");
                wire_keys_restore();
                return;
            }
        }

        /* Pads that move with nothing driven: a switch to ground, or to a pad something
         * else holds low. This is the D-pad's read path and it has no pulse in it. */
        for (i = 0u; i < WIRE_PROBE_COUNT; ++i) {
            const uint32_t now = (g_probe_idle_now >> i) & 1u;
            const char* name;

            if (now == ((g_probe_idle_level >> i) & 1u)) {
                g_probe_idle_hold[i] = 0u;
                continue;
            }
            if (++g_probe_idle_hold[i] < (uint8_t)MAP_STABLE_TICKS) continue;
            g_probe_idle_hold[i] = 0u;
            g_probe_idle_level =
                (uint16_t)((g_probe_idle_level & ~(1u << i)) | (now << i));

            name = level_key_name(g_wire_probe[i]);
            put_str("kpdmon: ");
            if (name != 0) {
                put_str(name);
                put_str(now != 0u ? " PRESSED" : " released");
                kv_dec("   (pad ", (int32_t)g_wire_probe[i]);
                put_str(g_probe_driven[i] != 0u ? " low against a driven high)\n"
                                                : " low, no pulse)\n");
            } else {
                kv_dec("pad ", (int32_t)g_wire_probe[i]);
                put_str(now != 0u ? " LOW with nothing driven   <- name the button held\n"
                                  : " back high\n");
            }
            if (++reported >= KPDMON_EVENT_LIMIT) {
                put_str("kpdmon: event limit, stopping\n");
                wire_keys_restore();
                return;
            }
        }

        /* Shorts the table does not name. Reported once each, so holding an unknown
         * button for a second produces one line rather than a hundred, and the line is
         * the pair to add to mt6592_board_j36.h.
         *
         * A follower that is ALREADY LOW with nothing driven is skipped, and that one line
         * is what stops the twenty-five-line storm: a grounded pad follows every pulse in
         * the run, so without this it reports a short against all seven other pads and
         * buries the one real finding. It is not a short, it is the passive report above. */
        for (i = 0u; i < WIRE_PROBE_COUNT; ++i) {
            uint32_t s;

            for (s = 0u; s < WIRE_PROBE_COUNT; ++s) {
                if (s == i) continue;
                if (((g_probe_idle_now >> s) & 1u) != 0u) continue;
                if (((g_wire_probe_base[i] >> s) & 1u) != 0u) continue;
                if (wire_pair_is_mapped(g_wire_probe[i], g_wire_probe[s])) continue;
                if (g_wire_probe_done[i][s] != 0u) continue;
                if (((g_wire_probe_now[i] >> s) & 1u) == 0u) {
                    g_wire_probe_hold[i][s] = 0u;
                    continue;
                }
                if (++g_wire_probe_hold[i][s] < (uint8_t)MAP_STABLE_TICKS) continue;
                g_wire_probe_done[i][s] = 1u;
                kv_dec("kpdmon: UNMAPPED short: pad ", (int32_t)g_wire_probe[i]);
                kv_dec(" -- pad ", (int32_t)g_wire_probe[s]);
                put_str("   <- name the button held\n");
                if (++reported >= KPDMON_EVENT_LIMIT) {
                    put_str("kpdmon: event limit, stopping\n");
                    wire_keys_restore();
                    return;
                }
            }
        }

        for (i = 0u; i < MAP_SLOTS; ++i) {
            if (g_map_unstable[i] != 0u) continue;
            if (g_map_now[i] == g_map_level[i]) {
                g_map_hold[i] = 0u;
                continue;
            }
            if (++g_map_hold[i] < (uint8_t)MAP_STABLE_TICKS) continue;
            g_map_level[i] = g_map_now[i];
            g_map_hold[i] = 0u;

            {
                const uint32_t bit = i - MAP_PINS;
                const uint32_t row = bit / KPD_MATRIX_COLS;
                const char* name = matrix_key_name(bit);

                put_str("kpdmon: ");
                if (name != 0) {
                    put_str(name);
                    put_str(g_map_level[i] != 0u ? " released" : " PRESSED");
                    kv_dec("   (bit ", (int32_t)bit);
                } else {
                    kv_dec("bit ", (int32_t)bit);
                    put_str(g_map_level[i] != 0u ? " -> 1 (released)" : " -> 0 (PRESSED)");
                    put_str("   <- UNNAMED, (");
                }
                kv_dec(" row ", (int32_t)row);
                kv_dec(" col ", (int32_t)(bit % KPD_MATRIX_COLS));
                put_char(')');
                /* Row 2 is the one row this board cannot scan: its strobe pad is 93, which
                 * the board spent on UP's EINT. The vendor keymap does put the D-pad here,
                 * so bits 18..21 carry names in that keymap and can never fire; a bit moving
                 * in row 2 means the column moved for some other reason, not a key. Rows 0,
                 * 1 and 3 all strobe and all four of the block's scanned rows are mapped. */
                if (row == 2u) put_str("  [row 2 has no strobe -- pad 93 is UP's EINT]");
                if (row > 3u) put_str("  [row 4+ -- this board wires no such row]");
                put_char('\n');
            }
            if (++reported >= KPDMON_EVENT_LIMIT) {
                put_str("kpdmon: event limit, stopping\n");
                wire_keys_restore();
                return;
            }
        }

        /* Service the gadget on every tick, not just between commands.
         *
         * This loop previously called only mt6592_usb_gadget_recv(), which
         * returns -1 immediately while the device is unconfigured and so polls
         * nothing at all. That left EP0 unserviced for the entire sweep: the
         * host had already seen the pull-up and started enumerating, got a
         * handful of control transfers answered, then waited twenty seconds for
         * a device that was busy counting key edges and gave up. It is the best
         * explanation for the observed `setups=0x0a, cfg=0` -- enumeration that
         * starts, stalls partway, and never completes. */
        mt6592_usb_gadget_poll();
        delay_ms(10u);
        /* An empty command line from the host is a bail-out. */
        {
            char scratch[64];
            if (mt6592_usb_gadget_recv(scratch, sizeof(scratch)) > 0) {
                put_str("kpdmon: interrupted\n");
                wire_keys_restore();
                return;
            }
        }
    }
    wire_keys_restore();
    kv_dec("kpdmon: done, events=", (int32_t)reported);
    put_char('\n');
}

static void cmd_kpdmon(const char* args) {
    kpdmon_run(parse_u32_default(&args, 0x1eu)); /* 30s in hex-by-default */
}

/*
 * Hunt for buttons on pads that are currently muxed to something else.
 *
 * kpdmon can only watch pads that are already plain-GPIO inputs with a pull, and
 * that is a real limit rather than a conservative one: START, SELECT, MENU and R2
 * produce nothing on any matrix bit and nothing on any watched pin, and the
 * matrix has no free bit left for them (see mt6592_board_j36.h). They are on pads
 * that boot in an alternate mux mode, where the pad's input buffer is not feeding
 * DIN at all, so no amount of watching finds them.
 *
 * So this reconfigures pads to mode 0 / input / pull-up, watches for a press, and
 * puts every one of them back exactly as it found it -- mode, direction, dout and
 * both pull bits -- whether it finds anything or not.
 *
 * WHAT WENT WRONG THE FIRST TIME, AND WHAT REPLACED IT
 *
 * The first version took a 64-pad range, announced the pads whose mux would
 * change as one line, then reconfigured all of them and watched. Two runs killed
 * the board: `kpdhunt #16 #48` powered it off outright, and `kpdhunt #96 #128`
 * detached mid-watch and then would not re-enumerate across six attempts. Both
 * ranges were 20-30 pads wide, so neither told us anything except "somewhere in
 * here". That is the worst possible outcome for a probe: it costs a session and
 * buys no information.
 *
 * Three things changed, and the ordering of them is the point:
 *
 *   1. Attribution. The pad about to be written is printed and FLUSHED before the
 *      write, one pad at a time. Whatever happens next, the last line the host
 *      received names the pad that did it. A pad that kills the board is then a
 *      one-line fact instead of a lost range, and it can simply be skipped.
 *   2. Width. The cap is 16 and the suggested step is 8. Combined with (1) this
 *      turns a bad range into a bisection rather than a mystery.
 *   3. A dead-man's switch. wdt_arm() before the first write, kicked every tick,
 *      disarmed after the restore. The restore lives after the watch loop and
 *      only runs if we get there; the watchdog is what covers not getting there.
 *      A remux that wedges the CPU now costs a few seconds and an automatic
 *      reboot instead of a dark board.
 *
 * Reassigning pads still cannot be made risk-free -- some pad on this board holds
 * the power rail, and taking it low turns the console off no matter how carefully
 * the write is staged. What it can be made is *cheap to survive and informative
 * when it fails*, which is what the three changes above buy. Nothing here is
 * persistent either way: the GPIO block is volatile, so every one of these
 * experiments is undone by a power cycle.
 *
 * Known so far, from runs of this and of kpdmon: gpio 112 is panel power -- taking it
 * low blanks the display completely. 90 and 112 are on pin_is_reserved() and are
 * skipped by both hunts.
 *
 * One finding of this command has since been overturned, and it is worth keeping as a
 * warning about what a level hunt can and cannot prove. It reported that gpio 11
 * answers BOTH volume keys, and concluded the pair must be encoded rather than one pin
 * each. The level changes were real; the conclusion was not. `kpdwire` later measured
 * pad 11 as the node shared by START, MENU and both volume keys, so a change on it
 * meant whichever of those four was pressed. A hunt like this finds pads that MOVE. It
 * cannot say which switch moved them, and on this board most pads carry several.
 */
enum {
    KPDHUNT_MAX = 16u,
    KPDHUNT_WDT_SECS = 12u, /* >> the 1.5 s settle and one tick of USB flush */
};

static uint8_t g_hunt_mode[KPDHUNT_MAX];
static uint8_t g_hunt_dir[KPDHUNT_MAX];
static uint8_t g_hunt_pull[KPDHUNT_MAX];
static uint8_t g_hunt_dout[KPDHUNT_MAX];
static uint8_t g_hunt_skip[KPDHUNT_MAX];

/* Save a pad's whole configuration, then hand it to the GPIO block as a
 * pulled-up input. Announced and flushed first -- see (1) above. */
static void hunt_take_pin(const char* tag, uint32_t pin, uint32_t slot) {
    g_hunt_mode[slot] = (uint8_t)gpio_mode_get(pin);
    g_hunt_dir[slot] = (uint8_t)gpio_bit_get(GPIOC_DIR, pin);
    g_hunt_pull[slot] = (uint8_t)gpio_pull_get(pin);
    g_hunt_dout[slot] = (uint8_t)gpio_bit_get(GPIOC_DOUT, pin);

    put_str(tag);
    put_str(": taking gpio ");
    put_dec((int32_t)pin);
    kv_dec(" (was mode ", (int32_t)g_hunt_mode[slot]);
    kv_dec(" dir ", (int32_t)g_hunt_dir[slot]);
    kv_dec(" pull ", (int32_t)g_hunt_pull[slot]);
    put_str(")\n");
    flush_line();
    /* The flush above can burn a full TX spin cap per pad if the host has already
     * gone; kick so a 16-pad take does not trip the switch on its own. */
    wdt_kick();

    gpio_cfg_apply(pin, 0u, 0u, 2u, g_hunt_dout[slot]); /* mode 0, input, pull-up */
}

static void hunt_give_pin(uint32_t pin, uint32_t slot) {
    gpio_cfg_apply(pin, g_hunt_mode[slot], g_hunt_dir[slot], g_hunt_pull[slot],
                   g_hunt_dout[slot]);
}

/*
 * Watch a set of freshly-taken pads for a press.
 *
 * Shared by both hunts: `first`/`n` describe a contiguous run, which a single pad
 * satisfies as n == 1. Skipped slots are ignored throughout. Returns the number of
 * edges reported.
 */
static uint32_t hunt_watch(const char* tag, uint32_t first, uint32_t n, uint32_t secs) {
    uint32_t reported = 0u;
    uint32_t i;
    uint32_t t;

    /* Settle: anything that will not sit still with nothing touched is not a
     * switch, and a freshly-reassigned pad next to a live bus chatters. */
    for (i = 0u; i < n; ++i) g_map_unstable[first + i] = 0u;
    map_sample(g_map_base);
    for (t = 0u; t < MAP_SETTLE_TICKS; ++t) {
        wdt_kick();
        map_sample(g_map_now);
        for (i = 0u; i < n; ++i) {
            const uint32_t s = first + i;
            if (g_hunt_skip[i] == 0u && g_map_now[s] != g_map_base[s]) g_map_unstable[s] = 1u;
        }
        map_pump();
    }

    put_str(tag);
    put_str(": quiet and watchable:");
    for (i = 0u; i < n; ++i) {
        if (g_hunt_skip[i] == 0u && g_map_unstable[first + i] == 0u) {
            kv_dec(" ", (int32_t)(first + i));
        }
    }
    put_char('\n');
    put_str(tag);
    kv_dec(": press the missing buttons now, ", (int32_t)secs);
    put_str(" seconds\n");
    flush_line();

    map_sample(g_map_level);
    for (i = 0u; i < n; ++i) g_map_hold[first + i] = 0u;

    for (t = 0u; t < secs * 100u; ++t) {
        wdt_kick();
        map_sample(g_map_now);
        for (i = 0u; i < n; ++i) {
            const uint32_t s = first + i;
            if (g_hunt_skip[i] != 0u || g_map_unstable[s] != 0u) continue;
            if (g_map_now[s] == g_map_level[s]) {
                g_map_hold[s] = 0u;
                continue;
            }
            if (++g_map_hold[s] < (uint8_t)MAP_STABLE_TICKS) continue;
            g_map_level[s] = g_map_now[s];
            g_map_hold[s] = 0u;
            put_str(tag);
            kv_dec(": gpio ", (int32_t)s);
            /* Pulled up here by construction, so 0 is closed. */
            put_str(g_map_level[s] != 0u ? " -> 1 (released)\n" : " -> 0 (PRESSED)\n");
            ++reported;
        }
        map_pump();
    }
    return reported;
}


/*
 * The same hunt, one pad at a time.
 *
 * This is the form to reach for on an unknown pad, and the only form that should
 * be pointed at a pad suspected of being anything other than a button. A range
 * that kills the board costs a session and names nothing; a single pad that kills
 * the board names itself, and the next boot knows one more thing about this
 * hardware than the last one did.
 *
 * Reserved pads are refused rather than silently skipped here, because a single-pad
 * command that quietly does nothing is worse than one that says why.
 */
static void cmd_pinhunt(const char* args) {
    uint32_t pin;
    uint32_t secs;
    uint32_t found;

    if (parse_u32(&args, &pin) != 0) {
        put_str("usage: pinhunt <pin> [#secs]   (pin is HEX; #65 for decimal 65)\n");
        put_str("pinhunt: remux ONE pad to gpio-in-pullup, watch it, put it back.\n");
        put_str("pinhunt: the safe way to try an unknown pad -- it names itself if\n");
        put_str("pinhunt: the board goes down, so the loss buys a fact.\n");
        return;
    }
    secs = parse_u32_default(&args, 0x0fu);
    if (pin > MAP_PAD_MAX) {
        put_str("pinhunt: pin out of range (last decoded pad is #168)\n");
        return;
    }
    if (pin_is_reserved(pin)) {
        kv_dec("pinhunt: gpio ", (int32_t)pin);
        put_str(" is reserved (backlight #90 / panel power #112); refusing\n");
        return;
    }

    g_hunt_skip[0] = 0u;
    wdt_arm(KPDHUNT_WDT_SECS);
    hunt_take_pin("pinhunt", pin, 0u);
    found = hunt_watch("pinhunt", pin, 1u, secs);
    hunt_give_pin(pin, 0u);
    wdt_disarm();

    kv_dec("pinhunt: restored gpio ", (int32_t)pin);
    kv_dec(", events=", (int32_t)found);
    put_char('\n');
}

/*
 * Raise the matrix's six-key ceiling by muxing the strobe the preloader forgets.
 *
 * WHY THERE IS A CEILING AT ALL. Six matrix bits have ever moved on this board --
 * 0, 1, 2 and 9, 10, 11 -- and that was read for a long time as "the rest of the
 * buttons are not wired to the keypad". They are. The preloader's own keypad init
 * says so, and it is in Reference/J36-ULTRA/preloader_sf6592_wet_l.bin:
 *
 *   0x546c  mtk_kpd_init: read PMIC 0x0040, clear its bit 0, write it back -- the
 *           keypad's 32 kHz clock gate, named in its own log strings -- then call
 *           mtk_kpd_gpio_set, then KP_SEL &= ~1 and KP_EN = 1, then print
 *           "after set KP enable: KP_SEL = 0x%x !"
 *   0x53f8  mtk_kpd_gpio_set (it prints its own name), filler at 0x53d0:
 *           strobes {74, 92}      -> mode 1, OUT, no pull
 *           senses  {75, 167, 168} -> mode 1, IN, pull UP
 *
 * TWO strobes crossed with three senses is six keys at the controller's stride of
 * nine: bits 0,1,2 and 9,10,11, exactly the set that has ever moved. The ceiling is
 * the preloader's pad list, not the wiring, and nothing in the stock chain raises
 * it: stock LK contains no call to its own mt_set_gpio_mode at all and reads only
 * indices 0 and 1, and the stock kernel's 12 MB image contains no "KROW", "KCOL",
 * "kpd_gpio" or "mtk_kpd_gpio_set" string and no gpio-keys driver, so kpd.c takes
 * the pinmux as it finds it.
 *
 * SO WHY IS THIS COMMAND STILL HERE, if the pads are already the preloader's? Two
 * reasons. Pads get clobbered: a live dump caught 167 and 168 in mux mode 1 as
 * inputs with NO PULL, where the preloader had left them pulled up, which is a
 * floating sense line and most likely a `kpdpad`/`kpdsweep` restore landing on a
 * stale baseline. And the third-strobe A/B is worth being able to run on demand --
 * see below.
 *
 * PAD 93 IS KPROW2'S PAD, AND THIS BOARD GAVE IT TO DPAD_UP. Both halves of that are
 * now settled, and together they are the reason matrix row 2 is dead and the reason the
 * D-pad reads as EINTs at all. It is in the {pin,reg,bit} PUPD table (preloader 0x1aa8e,
 * kernel 0x887154) as register 0's third field, alongside 74@2 and 92@6 -- so the pad
 * CAN carry a row, and on other MT6592 boards it does. On this one kpd_pdrv_probe's EINT
 * list assigns GPIO 93 keycode 48 = DPAD_UP, which outranks a capability table, and the
 * D-pad has been observed working on GPIO 8/20/45/93. Muxing 93 is what coincided with
 * the D-pad dying. So there is no third-strobe A/B any more: it costs UP to light a row
 * whose keys (LT RT UP DN in the vendor keymap) this board does not put on the matrix.
 *
 * WHAT WAS ACTUALLY MISSING WAS PADS 11, 12 AND 2. mtk_kpd_gpio_set's five stores are a
 * FIXUP list, not a census -- pads whose DWS defaults are already right do not appear in
 * it. Reading it as a census is what limited this command to five pads and the block to
 * six scannable bits. See the table in cmd_kpdmux.
 *
 * PADS ALONE HAVE NEVER MOVED A BIT HERE. Run `kpdon` after this -- step 1 of the
 * vendor sequence, the PMIC clock gate, is the one MVII was skipping.
 */
/*
 * Run the vendor's keypad enable sequence, in the vendor's order.
 *
 * WHY THIS EXISTS. The scan memories on this board read 0xffff 0xffff 0xffff 0xffff
 * 0x00ff and never move, before or after a pad mux that `pin` confirms landed. The
 * block looks enabled -- en=1, deb=0x400 -- so the natural reading of a live dump is
 * "configured correctly, nobody pressing anything", and the next reading after that
 * is "sel=0x0000, so the columns are not enabled". Both are wrong, and the
 * preloader's own keypad routine (0x546c, Thumb-2) says why. In order, it:
 *
 *   1. reads PMIC register 0x0040, clears bit 0, writes it back -- the keypad's
 *      32 kHz clock gate. It names the register in its own log strings, "kpd read
 *      addr: 0x0040: data:0x%x" and "kpd write fail, addr: 0x0040".
 *   2. calls mtk_kpd_gpio_set (0x53f8) to mux the pads.
 *   3. clears bit 0 of KP_SEL -- MTK's double-key enable, and the only bit anything
 *      in the vendor chain ever writes there. sel=0x0000 is the intended state, not
 *      an uninitialised one.
 *   4. writes KP_EN = 1.
 *   5. prints "after set KP enable: KP_SEL = 0x%x !".
 *
 * Step 1 is the one nothing in MVII was doing. A gated scan engine with KP_EN set
 * produces exactly the idle all-ones pattern a working idle matrix produces, which
 * is why this cost several sessions.
 *
 * So: `kpdmux` first if the pads need it, then this, then `kpdmon`. Register writes
 * only -- no pinmux, so nothing here can wedge a bus and no watchdog is armed.
 */
static void cmd_kpdon(void) {
    enum {
        KPDON_PMIC_CLK_REG = 0x0040u,
        KPDON_PMIC_CLK_BIT = 0x0001u,
        KPDON_SEL_DOUBLE_KEY = 0x0001u,
        KPDON_DEBOUNCE = 0x0400u,
    };
    uint32_t val = 0u;

    put_str("kpdon: before\n");
    dump_kpd();

    if (mt6592_pwrap_read(KPDON_PMIC_CLK_REG, &val) != MT6592_PWRAP_OK) {
        put_str("kpdon: PMIC 0x0040 read FAILED -- cannot ungate the keypad clock\n");
    } else {
        kv_hex("kpdon: pmic 0x0040 = ", val, 4u);
        if ((val & KPDON_PMIC_CLK_BIT) == 0u) {
            put_str("  (bit 0 already clear: clock is ungated)\n");
        } else {
            put_str("  (bit 0 SET: keypad clock is GATED -- this is the bug)\n");
            if (mt6592_pwrap_write(KPDON_PMIC_CLK_REG, val & ~KPDON_PMIC_CLK_BIT) !=
                MT6592_PWRAP_OK) {
                put_str("kpdon: PMIC 0x0040 write FAILED\n");
            } else if (mt6592_pwrap_read(KPDON_PMIC_CLK_REG, &val) == MT6592_PWRAP_OK) {
                kv_hex("kpdon: pmic 0x0040 now ", val, 4u);
                put_char('\n');
            }
        }
    }

    mtk_write32(KPD_BASE + 0x18u, KPDON_DEBOUNCE);
    mtk_write32(KPD_BASE + 0x20u, mtk_read32(KPD_BASE + 0x20u) & ~KPDON_SEL_DOUBLE_KEY);
    mtk_write32(KPD_BASE + 0x24u, 1u);

    put_str("kpdon: after\n");
    dump_kpd();
    put_str("kpdon: the memories should still read ffff ffff ffff ffff 00ff at idle.\n");
    put_str("kpdon: now run `kpdmon 28` and press every button once. All thirteen matrix\n");
    put_str("kpdon: keys report by name: L1 L2 R1 R2 / X Y B A / VOL- VOL+ SELECT START\n");
    put_str("kpdon: MENU. The D-pad is not here -- it reads as EINT levels on 93/45/20/8.\n");
}

/*
 * THIS TABLE HAS BEEN WRONG TWICE, IN OPPOSITE DIRECTIONS, AND BOTH ARE RECORDED HERE.
 *
 * It began as five pads, copied from the preloader's mtk_kpd_gpio_set filler at 0x53d0
 * (74, 92, 75, 167, 168) on the reading that the filler is a census of this board's
 * keypad lines. It is not; it is a fixup list. Muxing only those five left the block
 * scanning bits {0,1,2,9,10,11} and nothing else, and those six survivors were then
 * mistaken for the six bits this board can scan -- that was the seven-dead-key regression.
 *
 * The correction to that was to add pads 11, 12 and 2 AT MODE 1, and hardware refused it.
 * Pad 11 at mode 1 is a STATIC LOW, not a strobed KPROW3: pressing any row-3 key then ties
 * its column to ground in every row at once, so one press lights eight bits at stride 9.
 * SELECT lit 2/11/20/29/38/47/56/65, VOL+ the column-1 eight, VOL- the column-0 eight --
 * three independent column drags, which is the strongest confirmation yet of the pad map
 * (11 really is row 3's common node; 75/167/168 really are columns 0/1/2) and a flat
 * refutation of the mode. Pads 12 and 2 at mode 1 produced no bit at all: not their own
 * keys, not a drag, nothing. Mode 1 is the keypad function on the five pads the preloader
 * names, and is something else entirely on these three.
 *
 * SO THE MODE COLUMN EXISTS, AND `kpdmode` MEASURED IT. Three sweeps of eight modes with
 * one button held: pad 12 answers at MODE 3 (column-3 drag, bits 3 12 21 30 39 48 57 66),
 * pad 2 at MODE 6 (column-4 drag, 4 13 22 31 40 49 58 67), pad 11 at MODE 3 with SELECT on
 * BIT 29 ALONE -- a strobed row, not a drag. Twenty-one other mode/pad combinations read
 * "scan low: none". A single KPD_LINE_MUX_MODE constant could not have expressed this, and
 * that is precisely why the previous revision got it wrong.
 *
 * KPROW2 IS ABSENT AND MUST STAY ABSENT. Its pad is 93, and this board wired 93 to
 * DPAD_UP's EINT, so there is no `kpdmux 1` any more: the old flag muxed 93 to light a
 * row whose four keys (LT, RT, UP, DN) this board does not have on the matrix, at the
 * price of the one button that pad does carry. One pad explains both the dead matrix row
 * and why the D-pad lives on EINTs.
 *
 * ONLY A PAD WHOSE MODE IS WRONG IS WRITTEN. The five the boot chain already muxes work,
 * and two of them (167, 168) sit in mode 1 with no pull at all while Y and B read fine
 * through them -- so the block supplies what a sense line needs and "correcting" that pull
 * would be a change with no evidence behind it. Same rule as MVII's kpd_pads_apply(), on
 * purpose: what this command does to the console must be what the driver does to the
 * kernel, or the console stops being a preview of it.
 */
static const struct {
    uint32_t pin;
    uint32_t mux_mode; /* per pad: the keypad lands at a different index on each */
    uint32_t dir;
    uint32_t pull;
    const char* what;
} g_kpd_lines[] = {
    {74u, 1u, 1u, 0u, "KPROW0 (L1 L2 R1 R2)"},
    {92u, 1u, 1u, 0u, "KPROW1 (X Y B A)"},
    {11u, 3u, 1u, 0u, "KPROW3 (VOL- VOL+ SELECT START MENU)"},
    {75u, 1u, 0u, 2u, "KPCOL0"},
    {167u, 1u, 0u, 2u, "KPCOL1"},
    {168u, 1u, 0u, 2u, "KPCOL2"},
    {12u, 3u, 0u, 2u, "KPCOL3 (R2, A, START)"},
    {2u, 6u, 0u, 2u, "KPCOL4 (MENU)"},
};

#define KPD_LINE_COUNT (sizeof(g_kpd_lines) / sizeof(g_kpd_lines[0]))

enum { KPDMUX_WDT_SECS = 8u };

/* Mux any KPD line the boot chain left parked, each to its own measured mode. Returns how
 * many it had to write, so the caller can say "nothing to do" honestly. Watchdog-armed for
 * the same reason kpdhunt arms: if remuxing a pad wedges us, the board comes back alone. */
static uint32_t kpd_lines_apply(const char* tag) {
    uint32_t fixed = 0u;
    uint32_t i;

    wdt_arm(KPDMUX_WDT_SECS);
    for (i = 0u; i < KPD_LINE_COUNT; ++i) {
        const uint32_t have = gpio_mode_get(g_kpd_lines[i].pin);

        if (have == g_kpd_lines[i].mux_mode) continue;

        put_str(tag);
        put_str(g_kpd_lines[i].what);
        kv_dec(" gpio ", (int32_t)g_kpd_lines[i].pin);
        kv_dec(" was PARKED at mode ", (int32_t)have);
        kv_dec(" -> mode ", (int32_t)g_kpd_lines[i].mux_mode);
        kv_dec(" dir ", (int32_t)g_kpd_lines[i].dir);
        kv_dec(" pull ", (int32_t)g_kpd_lines[i].pull);
        put_char('\n');
        flush_line(); /* the last line the host sees if a write takes the board down */
        gpio_cfg_apply(g_kpd_lines[i].pin, g_kpd_lines[i].mux_mode, g_kpd_lines[i].dir,
                       g_kpd_lines[i].pull, 0u);
        ++fixed;
    }
    wdt_disarm();
    return fixed;
}

static void cmd_kpdmux(const char* args) {
    uint32_t i;
    uint32_t fixed;

    (void)args;

    put_str("kpdmux: before\n");
    for (i = 0u; i < KPD_LINE_COUNT; ++i) {
        pin_report("kpdmux:   gpio ", g_kpd_lines[i].pin);
    }
    pin_report("kpdmux:   gpio ", 93u);
    put_str("kpdmux:   (93 is DPAD_UP's EINT -- reported, never written)\n");
    dump_kpd();

    put_str("kpdmux: muxing every KPD line the boot chain parked (11->3, 12->3, 2->6)\n");
    flush_line();
    fixed = kpd_lines_apply("kpdmux: ");
    if (fixed == 0u) put_str("kpdmux: nothing to do -- all eight were already muxed\n");

    put_str("kpdmux: after\n");
    for (i = 0u; i < KPD_LINE_COUNT; ++i) {
        pin_report("kpdmux:   gpio ", g_kpd_lines[i].pin);
    }
    dump_kpd();
    put_str("kpdmux: pads only -- the block still needs its clock. Run `kpdon` next,\n");
    put_str("kpdmux: then `kpdmon 28`. Pads alone have never moved a scan bit here.\n");
}

/*
 * Print which of the 72 real scan bits are low, and say what shape they make.
 *
 * The shape is the whole point. One low bit is a scanned key. EIGHT low bits at stride 9
 * are one column low in every row at once, which no key can do -- that is a column tied to
 * ground, and it is what a row line held statically low produces. Telling those two apart
 * by eye across five hex words is what cost the last hardware run its first reading.
 *
 * 72 and not 80: MEM5's valid mask is 0x00ff, so the block's bits stop at 71, and 8 rows x
 * KPD_MATRIX_COLS is exactly 72.
 */
static uint32_t kpd_scan_low_report(const char* tag) {
    uint32_t mems[5];
    uint32_t col_low[KPD_MATRIX_COLS];
    uint32_t low_total = 0u;
    uint32_t bit;
    uint32_t c;

    read_kpd_mems(mems);
    for (c = 0u; c < KPD_MATRIX_COLS; ++c) col_low[c] = 0u;

    put_str(tag);
    put_str("scan low:");
    for (bit = 0u; bit < 72u; ++bit) {
        if (((mems[bit / 16u] >> (bit % 16u)) & 1u) != 0u) continue;
        ++col_low[bit % KPD_MATRIX_COLS];
        ++low_total;
        if (low_total <= 12u) kv_dec(" ", (int32_t)bit);
    }
    if (low_total == 0u) put_str(" none");
    if (low_total > 12u) put_str(" ...");

    if (low_total == 1u) put_str("   <- ONE BIT: a scanned key, not a drag");
    if (low_total == 8u) {
        for (c = 0u; c < KPD_MATRIX_COLS; ++c) {
            if (col_low[c] != 8u) continue;
            kv_dec("   <- WHOLE COLUMN ", (int32_t)c);
            put_str(" tied low in every row");
        }
    }
    put_char('\n');
    return low_total;
}

/*
 * WHICH PINMUX MODE MAKES THIS PAD A KEYPAD LINE? -- sweep all eight, one button held.
 *
 * WHY THIS EXISTS. Muxing pads 11, 12 and 2 to mode 1 -- the mode the preloader uses for
 * 74, 92, 75, 167 and 168 -- did not make them keypad lines. Pad 11 became a STATIC LOW, so
 * pressing any row-3 key tied its column to ground in every row at once and one press lit
 * eight bits at stride 9; SELECT lit 2/11/20/29/38/47/56/65, VOL+ lit the column-1 eight,
 * VOL- the column-0 eight. Pads 12 and 2 at mode 1 did nothing whatsoever -- no bit 3, 12,
 * 30 or 31, and no column-3 or column-4 drag either, so START, MENU, A and R2 stayed dark.
 * Meanwhile L1/L2/R1/X/Y/B each still lit exactly one bit, which is the control: rows 0 and
 * 1 ARE being strobed, so the block is fine and the pinmux is not.
 *
 * The inference is that a pad's peripheral functions are per-pad on this SoC. Mode 1 is the
 * keypad on the five pads the preloader names and is something else on these three, and no
 * artefact in Reference/ names the right one: the preloader parks 11, 12 and 2 at mode 0
 * with a pull-down, stock LK sets no modes at all, and the stock kernel has no KPCOL,
 * KPROW, KROW, KCOL or kpd_gpio string anywhere in its 12 MB. So the board answers it.
 *
 * HOW IT READS. Pad 11 is the common node of all five row-3 switches, so it is driven low
 * as a plain mode-0 GPIO output for the whole sweep -- a deliberate ground, which is a
 * better instrument than mode 1's accidental one because it does not change when the swept
 * pad's mode does. Hold the button joining the swept pad to pad 11 and that pad is tied to
 * ground for the entire run. Then at each of the eight modes:
 *
 *   THE PAD'S OWN DIN SAYS WHETHER YOU ARE REALLY HOLDING THE BUTTON. At mode 0 with a
 *   pull-up, held reads 0 and not-held reads 1. That makes the sweep self-checking: a run
 *   whose mode-0 line says din=1 is a run where nobody pressed anything, and every other
 *   line in it is worthless. Check that line before reading any of the rest.
 *
 *   THE 72 SCAN BITS SAY WHETHER THE BLOCK CAN SEE THE PAD. A muxed column tied to ground
 *   reads low in every row, so the signature is one whole column -- eight bits at stride 9
 *   -- and kpd_scan_low_report() calls that out by name.
 *
 * SWEEPING PAD 11 ITSELF is the other half of the question and takes no ground: hold SELECT
 * (pad 11 to pad 168, a column that already works) and look for BIT 29 ALONE. One bit means
 * the block is strobing that row properly and the mode is right. A whole column means the
 * pad is a static low again. No bits at all means that mode does not connect it.
 *
 * WHAT IT COSTS IF A MODE IS WRONG: nothing that outlives the command. Every mode is tried
 * as an INPUT with a pull-up, never as an output, so the pad cannot fight anything; both
 * pads' full configurations are saved and restored; and the watchdog is armed across the
 * whole sweep so a mode that wedges a bus brings the board back by itself.
 */
static void cmd_kpdmode(const char* args) {
    enum { KPDMODE_GROUND_PAD = 11u, KPDMODE_WDT_SLACK = 6u };
    const uint32_t pad = parse_u32_default(&args, 0xffffffffu);
    const uint32_t secs = parse_u32_default(&args, 3u);
    uint32_t save_mode, save_dir, save_pull, save_dout;
    uint32_t gnd_mode = 0u, gnd_dir = 0u, gnd_pull = 0u, gnd_dout = 0u;
    uint32_t use_ground;
    uint32_t m;

    if (pad > MAP_PAD_MAX) {
        put_str("usage: kpdmode <pad> [#secs-per-mode]   (pad is HEX: c=12, 2=2, b=11)\n");
        put_str("kpdmode: hold the button that joins <pad> to pad 11 for the whole sweep:\n");
        put_str("kpdmode:   kpdmode c   hold START    kpdmode 2   hold MENU\n");
        put_str("kpdmode:   kpdmode b   hold SELECT   (sweeping the row itself, no ground)\n");
        return;
    }
    if (pin_is_reserved(pad)) {
        put_str("kpdmode: that pad is reserved -- refusing\n");
        return;
    }

    use_ground = (pad != KPDMODE_GROUND_PAD) ? 1u : 0u;

    save_mode = gpio_mode_get(pad);
    save_dir = gpio_bit_get(GPIOC_DIR, pad);
    save_pull = gpio_pull_get(pad);
    save_dout = gpio_bit_get(GPIOC_DOUT, pad);

    kv_dec("kpdmode: sweeping pad ", (int32_t)pad);
    kv_dec(" modes 0..7, ", (int32_t)secs);
    put_str(" s each. HOLD THE BUTTON for the whole sweep.\n");
    if (use_ground) {
        put_str("kpdmode: pad 11 is driven low as the row-3 ground for the duration\n");
    } else {
        put_str("kpdmode: sweeping the row pad itself -- hold SELECT, look for BIT 29 ALONE\n");
    }
    put_str("kpdmode: check the mode-0 line first: din must read 0, or nothing was held\n");
    flush_line();

    /* The block has to be scanning or every line of this reads "no keys pressed". */
    mtk_write32(KPD_BASE + 0x18u, 0x0400u);
    mtk_write32(KPD_BASE + 0x24u, 1u);

    wdt_arm(secs * 8u + KPDMODE_WDT_SLACK);

    if (use_ground) {
        gnd_mode = gpio_mode_get(KPDMODE_GROUND_PAD);
        gnd_dir = gpio_bit_get(GPIOC_DIR, KPDMODE_GROUND_PAD);
        gnd_pull = gpio_pull_get(KPDMODE_GROUND_PAD);
        gnd_dout = gpio_bit_get(GPIOC_DOUT, KPDMODE_GROUND_PAD);
        gpio_cfg_apply(KPDMODE_GROUND_PAD, 0u, 1u, 0u, 0u);
    }

    for (m = 0u; m < 8u; ++m) {
        uint32_t t;

        gpio_cfg_apply(pad, m, 0u, 2u, 0u); /* input, pulled up: it can never fight anything */
        for (t = 0u; t < secs * 100u; ++t) {
            map_pump();
            wdt_kick();
        }
        kv_dec("kpdmode: mode ", (int32_t)m);
        kv_dec("  din=", (int32_t)gpio_bit_get(GPIOC_DIN, pad));
        kv_dec("  readback mode=", (int32_t)gpio_mode_get(pad));
        put_str("  ");
        (void)kpd_scan_low_report("");
        flush_line();
    }

    gpio_cfg_apply(pad, save_mode, save_dir, save_pull, save_dout);
    if (use_ground) {
        gpio_cfg_apply(KPDMODE_GROUND_PAD, gnd_mode, gnd_dir, gnd_pull, gnd_dout);
    }
    wdt_disarm();

    put_str("kpdmode: pads restored. A mode whose line says WHOLE COLUMN k is the answer:\n");
    put_str("kpdmode: that pad is KPCOL k, and MVII should mux it to that mode.\n");
}

static void cmd_bl(const char* args) {
    uint32_t pct = parse_u32_default(&args, 0x64u); /* 100 in hex-by-default */
    if (pct > 100u) pct = 100u;
    kv_dec("bl: ", (int32_t)pct);
    put_char('\n');
    mt6592_backlight_reassert(pct);
}

/*
 * Battery presence by load step — MEASURED, AND IT DOES NOT WORK. Kept as a rail
 * instrument; it no longer votes on presence.
 *
 * The verdict was wired up and then calibrated against both cell states on this
 * board, panel up, backlight stepped 0 -> 100% with a CPU burn on top:
 *
 *   cell fitted   4182 / 4182 / 4182 mV   sag 0 mV
 *   cell removed  4177 / 4177 / 4177 mV   sag 0 mV
 *
 * Identical. The probe cannot tell the two apart, and worse, it confidently
 * answered "CELL FITTED" on a board with no cell in it, which latched a wrong
 * presence verdict that nothing could clear.
 *
 * The reason is a sampling-rate mistake, not a bad load. Source impedance shows up
 * in the TRANSIENT: the rail dips for as long as the regulator's control loop takes
 * to respond, which is microseconds to tens of microseconds. Reaching BATSNS costs
 * a pwrap round trip plus an AUXADC conversion, and this loop then waits 10 ms
 * between samples, so every reading lands long after the loop has recovered. What
 * gets measured is DC regulation, and a CV regulator's DC regulation is excellent
 * by construction -- which is exactly why both columns read 0 mV. No larger load
 * fixes that; the instrument is too slow by three orders of magnitude, and nothing
 * this PMIC exposes over pwrap is fast enough to fix it.
 *
 * What it is still good for: confirming the rail is steady, and showing the ~5 mV
 * standing difference between the two states (4182 against 4177), which is real and
 * repeatable but far too small to threshold against cell ageing and temperature.
 *
 * Original reasoning, kept because the physics is right and only the instrument
 * was wrong:
 *
 * Every BATON detect bit on this PMIC reports "fitted" with the connector empty:
 * CHR_CON7 bit 12 and RO 0x142 bit 5 have both been read with RG_BATON_EN set and
 * both said fitted on an empty board. The pin is not wired here.
 *
 * SUPERSEDED, and this paragraph is left standing because the conclusion it
 * reached is the thing being corrected. Those readings enabled RG_BATON_EN and
 * nothing else. Stock's CHARGING_CMD_GET_BATTERY_STATUS (charging_func[11],
 * 0xc05141b8) sets CHR_CON7 bit 2 FIRST and then bit 0, and no code in this tree
 * had ever written bit 2 -- so "the pin is not wired here" was a conclusion about
 * a detector that was switched off. `bat` now arms it stock's way and prints
 * whether the bit has ever been observed to change; see mt6592_pmic.c. This probe
 * remains the fallback for the case where it genuinely has not.
 *
 * And BATSNS cannot
 * answer it either, because with no power-path FET the charger regulates the very
 * node BATSNS measures, so it reads the CV setpoint whether a cell is fitted or
 * not (that is the "always 98%" bug).
 *
 * What is left is the difference in SOURCE IMPEDANCE, which no register hides.
 * A Li-ion cell is milliohms and farad-scale; the charger's CV loop is neither.
 * So: measure VBAT, apply the biggest load this image can switch, measure again.
 *
 *   cell fitted  the cell clamps the node. The step is in the noise, a few mV.
 *   no cell      the loop has to make up the whole step and cannot do it
 *                instantly. The rail visibly sags -- which is the same mechanism
 *                behind the battery-less brownouts this port has been chasing.
 *
 * THE LOAD HAS TO BE REAL, and the first revision of this probe could not promise
 * that. It stepped the backlight alone, and the backlight is only a load when the
 * panel is powered: run from a console that never brought the display up, all three
 * phases measured the same idle rail and reported a 0 mV sag, which is also exactly
 * what a fitted cell looks like. A probe whose "cell present" answer and whose
 * "nothing happened" answer are the same reading is not a probe. So this version
 *
 *   brings the panel up itself, through the same hook `panel` uses, and says so
 *     when it cannot -- in which case the run is refused rather than reported;
 *   adds a CPU burn on top of the backlight, because on a board this size the core
 *     rail swamps an LED string and it costs nothing to switch;
 *   sanity-checks the step by requiring the control phase to return to where the
 *     first phase was, so a rail drifting for its own reasons cannot be read as a
 *     step in either direction.
 *
 * The measurement is a median at each level, off then on then off again, so a
 * one-off spike cannot be mistaken for the step.
 *
 * Nothing is written to the PMIC and nothing is left changed except the backlight,
 * which is restored to the level given (default 100). This reads the rail; it does
 * not touch the charger.
 *
 * Nothing is published to the driver: the probe reaches no verdict on this board, and
 * there is nowhere to publish one to -- presence is undecidable here and the driver
 * has no property for it. This command survives as the record of why source
 * impedance is not the answer.
 */
enum {
    /* Drift the control phase may show before the run is called unusable. */
    BATLOAD_MAX_DRIFT_MV = 12,
};


/*
 * WATCH A CONNECTOR CHANGE HAPPEN, with a control phase.
 *
 * Presence on this board is a threshold decision over a dithering comparator, and
 * every attempt to tune it so far has been made against a number captured at rest
 * in one cell state. That is the wrong measurement: what matters is what MOVES
 * when the connector changes, and how fast, and whether it moves back. So this
 * cues the operator, samples continuously across the change, and reports each
 * phase separately.
 *
 * Three phases with the same duration:
 *
 *   A   the connector as it started        the reference
 *   B   after the change                   the signal
 *   C   after changing it back             THE CONTROL
 *
 * C is what makes the run falsifiable. A and C are the same physical state, so
 * anything that differs between them is drift, warm-up or the charger's own slow
 * loop, and any A-to-B difference smaller than the A-to-C difference has measured
 * nothing. `batload` earns its keep the same way and it is the reason that command
 * can honestly say NO VERDICT instead of inventing one.
 *
 * Every line is flushed as it is produced. If plugging a cell browns the board out
 * -- which is a real possibility with an empty connector and the charger holding
 * the rail on its own -- the last line the host received names the moment it died,
 * which is worth more than a summary that never arrives.
 *
 * WHAT TO LOOK FOR, in the order the numbers appear:
 *
 *   cs    the CS_DET duty. What the driver decides on today. If B lands between
 *         420 and 580 the dead band is the bug: the verdict cannot move, which is
 *         the "minutes" symptom, and a duty wandering across a threshold is the
 *         "flapping" one.
 *   edge  how often CS_DET CHANGES, per mille of samples. The hypothesis is that
 *         this separates the two states where cs does not: a current loop with a
 *         cell to charge closes and hunts (edge high), one with nothing to charge
 *         ramps to a rail and stops (edge near zero). If A and C are near zero and
 *         B is not, presence should be decided on this and not on cs.
 *   vbat  BATSNS. A cell below the CV setpoint holds this node down; an empty
 *         connector cannot. Only proves a cell if the cell is not already full.
 *   con0/con2  raw, because if the derived numbers disagree the answer is here.
 */
enum {
    BATMON_PHASE_MS  = 8000u,  /* per phase; long enough for several windows   */
    BATMON_SETTLE_MS = 2000u,  /* streamed but NOT accumulated: the operator's
                                * hand is in the picture and the charger loop is
                                * re-acquiring. Measuring that is measuring the
                                * transition, not either state.                */
    BATMON_LINE_MS   = 250u,   /* one report line                              */
    BATMON_TICK_MS   = 10u,    /* one CHR_CON2 sample                          */
    BATMON_WDT_SECS  = 0x3cu,  /* 60 s, hex-by-default: covers the whole run   */
};

/*
 * ---------------------------------------------------------------------------
 * batcal -- is anything holding the VBAT node other than the charger?
 * ---------------------------------------------------------------------------
 *
 * THE MEASUREMENT THIS BOARD HAS NEVER BEEN ABLE TO MAKE. Every presence signal
 * tried so far failed the same way: BATSNS reads 4213..4232 mV with the cell
 * fitted and 4213..4232 mV with the connector empty, byte-identical on every
 * AUXADC channel. That is not a broken sensor. On this family VBAT *is* VSYS --
 * there is no power-path FET between the charger output and the system rail --
 * so with a cable in, the charger's CV loop regulates that node to its setpoint
 * whether or not a cell is hanging off it. Reading the node tells you what the
 * charger was told to produce, not what is connected.
 *
 * So stop reading the node and start MOVING it. The setpoint is a register:
 *
 *   CHARGING_CMD_SET_CV_VOLTAGE, charging_func[3] at 0xc0895b3c -> 0xc0514dd4,
 *   looks its argument up in a 32-entry uV table at 0xc0895a7c and passes the
 *   INDEX to 0xc04f53e8, which is one call:
 *       pmic_config_interface(0x06, code, 0x1f, 0)     -- CHR_CON3[4:0]
 *   code  0 = 4200000 uV      code 24 = 4050000
 *   code 23 = 4000000         code 25 = 4100000
 *   code 22 = 3900000         code 26 = 4125000  ... (full table in cv_steps[])
 *
 * SWEEP UP, NOT DOWN, AND THE REASON IS THE WHOLE DESIGN.
 *
 * A battery charger SOURCES current. It has no sink path -- there is no way for
 * it to pull a node down, only to stop holding it up. So the two directions of a
 * CV sweep are not symmetric, and only one of them is a measurement:
 *
 *   UP    the charger is in control. Raise the setpoint above the present node
 *         voltage and it starts sourcing, and how fast the node answers is
 *         exactly the question being asked, because dV/dt = I/C and C is the
 *         thing under test.
 *
 *           NO CELL   the only capacitance is board bulk, hundreds of uF. A few
 *                     hundred mA moves that tens of millivolts in microseconds,
 *                     so VBAT lands on the new setpoint inside one settle window
 *                     and TRACKS THE LADDER UP step for step.
 *           CELL IN   the cell is farads-equivalent. At the ~450 mA this charger
 *                     is set to, lifting a real cell 20 mV takes MINUTES. VBAT
 *                     does not move, and where it sits is the cell's own voltage.
 *
 *         Going up cannot brown the board out: every step raises the rail.
 *
 *   DOWN  the charger is NOT in control -- it can only stop sourcing. What the
 *         node does next is set by whatever is holding it, which is the same
 *         question, but the no-cell answer is a collapse rather than a reading:
 *         bulk capacitance against a few hundred mA is about 1 mV per
 *         microsecond, so the rail reaches UVLO in under a millisecond and the
 *         PMIC latches off. The AUXADC needs milliseconds. The board dies before
 *         a single conversion completes and the transcript just stops.
 *
 * THIS FILE USED TO SWEEP DOWN BY DEFAULT AND CLAIM IT COULD NOT CUT THE POWER.
 * That claim was wrong, on hardware, in the way described above: with the cell
 * fitted the sweep ran to the bottom and reported 4183 mV correctly, and with the
 * cell out the board dropped off the USB bus five steps in -- at exactly the step
 * where the setpoint first crossed below the node with nothing left to hold it.
 * The physics was right and the direction was backwards. `bat cal` still
 * exists because it is the fastest decisive test once a cell IS known to be
 * fitted, but it now says what it can do.
 *
 * WHAT THE FIRST RUN ALSO SHOWED, and why every row is now printed with the
 * setpoint READ BACK rather than as requested: the board came up with CHR_CON3 =
 * 29, a 4162 mV target, while the node sat at 4183 mV -- above its own setpoint,
 * which is a state the charger cannot have produced and can only have inherited.
 * A sweep whose rows are the values it MEANT to write cannot tell that from a
 * write that never landed.
 *
 * `batcal cut` is the literal charger-off, using stock's own disable pair
 * (CHR_CON0 CSDAC_EN|CHR_EN, from charging_func[2] at 0xc051479c). It climbs from
 * a 1 ms pulse and prints each result before risking the next, so if the board
 * does drop, the last line printed is the pulse width that killed it.
 *
 * Nothing here writes a verdict anywhere. It prints a table; the baseline gets
 * baked in only once the two runs are in hand.
 */
enum {
    BATCAL_CV_REG        = 0x0006u, /* CHR_CON3, [4:0] = CV code            */
    BATCAL_CV_MASK       = 0x001fu,
    BATCAL_CHR_CON0      = 0x0000u,
    BATCAL_CON0_CSDAC_EN = 0x0008u, /* bit 3 */
    BATCAL_CON0_CHR_EN   = 0x0010u, /* bit 4 */
    BATCAL_CHR_CON2      = 0x0004u,
    BATCAL_CON2_CS_DET   = 0x0020u,
    BATCAL_CON2_CV_DET   = 0x0040u,
    BATCAL_CON2_CC_DET   = 0x0080u,
    /* BATON, the dedicated connector detect. Same registers the PMIC driver
     * uses; named here so `bat baton' can arm and watch the pin without going
     * through the driver's precedence logic. */
    BATCAL_CHR_CON7      = 0x000eu,
    BATCAL_CON7_BATON_EN = 0x0001u,
    BATCAL_CON7_TDET_EN  = 0x0004u,
    BATCAL_CON7_BATON_UNDET = 0x1000u,
    BATCAL_BATON_RO_REG  = 0x0142u,
    BATCAL_BATON_RO_UNDET = 0x0020u,
    /* One CV step is 12.5..50 mV and the loop is slow; 200 ms is several loop
     * time constants and still keeps the whole sweep inside a few seconds. */
    BATCAL_SETTLE_MS     = 200u,
    BATCAL_WDT_SECS      = 60u,
    /* Measured: pack out, the FIRST probe after the cell came out survived rung 5
     * and the second powered the board off at the same rung. One survival is not
     * the asymmetry; repeated survival is. The gap is short on purpose -- a long
     * one refills the rail and hands every trial the same free pass. */
    BATCAL_PROBE_TRIALS  = 3u,
    BATCAL_PROBE_GAP_MS  = 50u,
    /* A cell that flattens has to beat the sweep's own slop before it counts.
     * The steps below are 25..100 mV apart, so 150 mV is more than one step of
     * tracking error and less than the smallest gap a real cell opens up. */
    BATCAL_FLAT_MV       = 150,
    BATCAL_TRACK_MV      = 60,
    /* The up-sweep settles longer than the down-sweep because it is waiting on
     * the CSDAC ramp (CHR_CON20/21: step 1, delay 4) to walk the source current
     * up, not just on a comparator to trip. */
    BATCAL_UP_SETTLE_MS  = 300u,
    /* Ceiling for the up-sweep. Stock's table goes to 4425 mV, but this is
     * pointed at a possibly-fitted cell: 4300 mV is one ordinary step above a
     * full charge, and the ladder sits at it for under a second, which at the
     * ~450 mA CS_VTH default is a fraction of a milliamp-hour. Above that it
     * would stop being a measurement and start being an overcharge. */
    BATCAL_UP_CEILING_MV = 4300,
    /* How much of the setpoint's rise the node has to copy before the charger
     * counts as owning it. Board bulk capacitance answers essentially in full;
     * a cell answers by a percent or two. Two thirds is nowhere near either. */
    BATCAL_FOLLOW_NUM    = 2,
    BATCAL_FOLLOW_DEN    = 3
};

/* Stock's own 32-entry table at 0xc0895a7c, in mV. Indexed by the CHR_CON3 code,
 * so a code read back off the hardware can be printed as a voltage -- including
 * the board's power-on value, which turned out to be 29 (4162 mV) rather than
 * the 0 (4200 mV) that a first reading assumed. Code 15 really is 2200 mV. */
static const int16_t g_batcal_cv_mv[32] = {
    4200, 4212, 4225, 4237, 4250, 4262, 4275, 4300,
    4325, 4350, 4375, 4400, 4425, 4162, 4175, 2200,
    4050, 4100, 4125, 3775, 3800, 3850, 3900, 4000,
    4050, 4100, 4125, 4137, 4150, 4162, 4175, 4187
};

/* Decode a CHR_CON3 code for `bat', which prints the live CV target long before
 * this table is declared. Out of line rather than a forward-declared array so the
 * masking lives in exactly one place. */
static int32_t batcal_cv_mv_of(uint32_t code) {
    return (int32_t)g_batcal_cv_mv[code & (uint32_t)BATCAL_CV_MASK];
}

/* Ascending, and every entry is a real code from the table above. Starts below
 * any plausible cell voltage so that the ladder always has somewhere to climb
 * from, and stops at the ceiling. */
static const uint8_t g_batcal_up_codes[] = {
    22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u,
     0u,  1u,  2u,  3u,  4u,  5u,  6u,  7u
};

/* Descending, from the same table. Not the whole 32: the high half is where a
 * nearly-full cell and the charger are indistinguishable, so the sweep spends
 * its steps below 4.2 V where they separate, and stops at 3900 mV -- still an
 * ordinary setpoint in stock's own table (it lists 3775 mV). That is NOT a
 * safety margin with no cell fitted; see the note on direction above. */
static const struct { uint8_t code; int16_t mv; } g_batcal_cv_steps[] = {
    { 0u, 4200}, {31u, 4187}, {30u, 4175}, {29u, 4162}, {28u, 4150},
    {27u, 4137}, {26u, 4125}, {25u, 4100}, {24u, 4050}, {23u, 4000},
    {22u, 3900}
};

/* THE MEDIAN IS THE DRIVER'S JOB NOW. This used to take five samples here and
 * sort them, because the accessor it called handed back one raw conversion. The
 * blocking sampler is defined as stock's PMIC_AUXADC_STOCK_TIMES median, so
 * repeating it would be a median of medians: slower, and not one count steadier.
 * The reason a median is wanted at all is unchanged -- this rail also feeds the
 * panel and the radios and steps every time one of them switches. */
static int batcal_vbat_mv(void) {
    int uv = 0;
    if (mt6592_pmic_sample_blocking(MT6592_PSY_VOLTAGE_NOW, &uv) != 0) return -1;
    return uv / 1000;
}



static void batcal_print_flags(void) {
    uint32_t con2 = 0u;
    if (mt6592_pwrap_read(BATCAL_CHR_CON2, &con2) != MT6592_PWRAP_OK) {
        put_str(" con2=??");
        return;
    }
    put_str(" cs=");
    put_char((con2 & BATCAL_CON2_CS_DET) ? '1' : '0');
    put_str(" cv=");
    put_char((con2 & BATCAL_CON2_CV_DET) ? '1' : '0');
    put_str(" cc=");
    put_char((con2 & BATCAL_CON2_CC_DET) ? '1' : '0');
}

/* One implementation of the rung, and it lives in the PMIC driver, because the
 * driver is what has to be able to take one without the console present. It
 * read-back-verifies: 0 only if the code is genuinely in CHR_CON3 afterwards. */
static int batcal_set_cv(uint32_t code) {
    return mt6592_pmic_charger_cv_set(code);
}



/*
 * THE CRASH IS THE MEASUREMENT.
 *
 * Every version of this test so far has tried to read a voltage and infer a
 * battery from it, and every one of them has been arguing with the ADC. That
 * was the wrong instrument. There is one register on this board whose meaning
 * is not a number at all:
 *
 *     CHR_CON0 bit 3 CSDAC_EN, bit 4 CHR_EN
 *
 * Dropping that pair is stock's own charger-disable, and on a part where VBAT
 * *is* VSYS it means exactly one thing: the system stops being fed by the
 * charger and starts being fed by the battery. That is the transition, and the
 * board's answer to it is not a reading, it is whether it is still alive
 * afterwards. With a cell, nothing happens. Without one, the rail is bulk
 * capacitance against a few hundred milliamps -- about a millivolt per
 * microsecond -- and it is under UVLO before the AUXADC could have finished a
 * single conversion. No voltage, no threshold, no calibration, no ADC.
 *
 * WHICH LEAVES ONE PROBLEM: the negative answer kills the thing that has to
 * report it. `batcal cut` prints each pulse width before risking it, which
 * makes the no-cell case readable only if a human is watching the console and
 * willing to interpret a silence. That is not a measurement a driver can take.
 *
 * So the answer is written down before it is risked, on the SD card, where a
 * collapsed rail cannot reach it:
 *
 *     arm the breadcrumb  ->  read it back so the card really has it
 *     hand the load to the battery
 *     survive             ->  overwrite with SURVIVED
 *
 * and the verdict is read on the NEXT run. A breadcrumb still reading ARMED
 * means the board did not come back from its own test, which is the no-cell
 * answer stated in the only way a dead board can state anything. This is the
 * same dead-man's-switch shape as the keypad hunt's watchdog, for the same
 * reason: the interesting outcome is the one that stops the code running.
 *
 * The sector is the last one before the FAT volume -- alignment padding that no
 * filesystem addresses. It is read before it is written and the test refuses to
 * run if anything unrecognised is living there, because being wrong about that
 * costs somebody's partition table.
 */
enum {
    BATCAL_CELL_LBA     = 0x07ffu, /* last sector of the MBR gap; volume_lba is 0x800 */
    BATCAL_CELL_HOLD_MS = 300u,    /* far longer than any capacitive rail survives */
    BATCAL_CELL_STATE   = 12u,     /* byte offset of the state letter in the sector */
    BATCAL_CELL_EXTRA   = 20u,     /* and of the little-endian payload word after it */
    BATCAL_CELL_POINT   = 24u,     /* the calibrated point: 0x80000000|reg<<16|value */
    BATCAL_CELL_LAST    = 28u,     /* the LOG, which nothing ever clears -- see below */
    /*
     * THE STICKY VERDICT, and it is what finally pays off the reboot.
     *
     * Everything above this line is about ONE run: what was in flight, what
     * completed, where the point is. This byte is about the BOARD: 'P' a cell was
     * proven fitted, 'A' proven absent, 0 never established. It is written once
     * per battery-state change and read on every boot after, so the destructive
     * probe is taken ONCE and never again -- which is as close to "no reboot" as
     * a board with no working presence sensor can get.
     *
     * It has to live on the card rather than in RAM precisely because the "no
     * cell" answer arrives as a power loss: nothing in memory survives to carry
     * it. `bat forget' clears it, and changing the pack without clearing it is
     * the one way to get a stale answer -- which is why `bat' prints it loudly.
     */
    BATCAL_CELL_VERDICT = 36u,

    /*
     * THE PORT'S OWN BASELINE, in raw AUXADC counts, and the byte that says it
     * is real. Two bytes at 40, a 'B' at 42.
     *
     * This is the calibration that retires the destructive probe. VBAT is VSYS,
     * so with a cable in and no cell the port alone holds that node, and it
     * holds it at ONE repeatable voltage. Measure that voltage once, with an
     * empty holder, and every presence question afterwards is "is VBAT on the
     * baseline or off it?" -- a comparison, costing one ADC conversion and no
     * risk, answerable every second forever.
     *
     * Counts and not millivolts, because a millivolt through this converter is
     * 4.55 counts and the whole decision lives inside a millivolt. Storing the
     * scaled value would round away three quarters of the evidence, and the
     * scaling constant is separately known to be wrong by ~70 mV.
     */
    BATCAL_CELL_BASE    = 40u,
    BATCAL_CELL_BASE_OK = 42u,

    /*
     * THE SEPARATION, in raw AUXADC counts, signed. Two bytes at 44, an 'E' at
     * 46.
     *
     * The baseline says where the port sits. It does not say how far a pack
     * moves the node, and that distance is the whole decision: +-1 mV was an
     * estimate from two readings a minute apart, and this converter was measured
     * wandering four times that in one session. A threshold built on an estimate
     * that small is a coin toss dressed up as a measurement.
     *
     * So measure it. `bat base set' with the holder EMPTY records where the port
     * alone sits; `bat delta set' with the PACK IN records how far off that the
     * pack sits. The two together define the decision threshold as the MIDPOINT
     * between two states that were each observed on this board, instead of a
     * tolerance somebody guessed -- and the sign of the delta is itself evidence,
     * because a pack above the baseline and a pack below it are different
     * physics (a larger cell versus one being charged).
     *
     * Signed, because both are real. Stored two's-complement in 16 bits.
     */
    BATCAL_CELL_DELTA    = 44u,
    BATCAL_CELL_DELTA_OK = 46u
};

static uint8_t g_batcal_cell_sector[512];

/*
 * BYTES 40..46 ARE NOW RESERVED AND UNREAD. They held the voltage window's
 * baseline and separation; the window is gone (see the tombstone below `bat
 * forget'), so nothing writes them and nothing decodes them. The offsets stay
 * declared and stay skipped rather than being reused, because cards in the field
 * have real values in them and a future field that reused those two slots would
 * silently inherit a baseline as its own data.
 */

/*
 * THE LOG, at byte 28, and the reason it exists.
 *
 * The state letter at byte 12 is a dead-man's switch: it is armed before a risk
 * and CLEARED once it has been read, because that is the only way "still armed"
 * can mean "the board died in there". Clearing is correct and cannot be dropped.
 *
 * But clearing also threw the measurement away. Measured 2026-08-09: `bat cal`
 * died at rung 5 and the next `bat` reported it perfectly -- and the `bat` after
 * that said "nothing is known right now", because reading it had consumed it.
 * The result of a run that costs a power cycle must not be readable once.
 *
 * So the answer is written twice: once as the switch, which is consumed, and
 * once here, which is not. Nothing clears these bytes; a later run overwrites
 * them. They also carry the LAST RUNG THAT COMPLETED, which the switch cannot:
 * every rung's arm-write records the rung before it as done, so the card ends up
 * holding both halves of the bracket -- rung 4 finished, rung 5 was in flight --
 * for the price of the write that was happening anyway.
 *
 *   28  the state letter that was in flight ('D' step, 'P' probe, 'S' survived)
 *   29  its rung number, 0xff if the mark has no rung
 *   30  reg, little-endian u16
 *   32  value, little-endian u16
 *   34  the last rung that COMPLETED, 0xff if none
 *   35  that rung's CV code
 */
static uint8_t g_batcal_last_state = 0u;
static uint8_t g_batcal_last_rung  = 0xffu;
static uint32_t g_batcal_last_reg  = 0u;
static uint32_t g_batcal_last_val  = 0u;
static uint8_t g_batcal_done_rung  = 0xffu;
static uint8_t g_batcal_done_code  = 0xffu;

/*
 * THE CALIBRATED POINT: the one register write that decides whether this board
 * keeps running.
 *
 * This is the answer `bat cal` produces and everything after it consumes. It
 * lives in the breadcrumb sector rather than in a #define because it is a
 * property of THIS board that was measured, not a constant that was chosen, and
 * because the run that measures it ends with the board powered off -- so it has
 * to survive a reset to be of any use. Every mark rewrites it, so it is never
 * lost to a later run.
 *
 * IT IS ALSO BAKED IN, because it has now been measured and the answer did not
 * move. RUNG 5 -- CHR_CON3 = 27, CV code 27 = 4137 mV. Measured 2026-08-09:
 *
 *   pack OUT   the ladder died at rung 5, twice, having survived rungs 0..4
 *              (the last of them code 28 = 4150 mV) both times
 *   pack IN    the same ladder ran all the way down INCLUDING rung 5 and the
 *              300 ms charger-off after it, and reported SURVIVED
 *
 * Same write, same board, same session: fatal with nothing in the holder,
 * shrugged off with a cell in it. That is the discriminator, and the reason it
 * works is not subtle -- the rail sits at ~4180 mV, so a CV target of 4137 mV is
 * the charger being told to stop holding a node it is already above. Nothing
 * else on this board can hold that node.
 *
 * Compiled in rather than left to the card so detection works on a fresh card,
 * on a card that was reformatted, and on the first boot of a new image. A `bat
 * cal` run overwrites it, so the measurement still wins over the constant.
 */
static uint32_t g_batcal_point_reg   = (uint32_t)BATCAL_CV_REG;
static uint32_t g_batcal_point_val   = 27u;
static uint32_t g_batcal_point_step  = 5u;
static int      g_batcal_point_valid = 1;

/* 'P' cell proven fitted, 'A' proven absent, 0 never established. See
 * BATCAL_CELL_VERDICT. */
static uint8_t g_batcal_verdict;

static const char g_batcal_cell_magic[] = "MVII-BATCAL";

static int batcal_cell_has_magic(const uint8_t* s) {
    for (uint32_t i = 0u; g_batcal_cell_magic[i] != '\0'; ++i) {
        if (s[i] != (uint8_t)g_batcal_cell_magic[i]) return 0;
    }
    return 1;
}

static int batcal_cell_is_blank(const uint8_t* s) {
    uint8_t all_or = 0u;
    uint8_t all_and = 0xffu;
    for (uint32_t i = 0u; i < 512u; ++i) {
        all_or  |= s[i];
        all_and &= s[i];
    }
    return all_or == 0u || all_and == 0xffu;
}

/* Write the breadcrumb and READ IT BACK. The read-back is not paranoia: a write
 * still sitting in the card's cache is a write that the brownout erases, and
 * this whole test rests on the sector being on the medium before the rail is
 * touched. */
static int batcal_cell_mark(uint8_t state, uint32_t saved_con0, uint32_t extra) {
    for (uint32_t i = 0u; i < 512u; ++i) g_batcal_cell_sector[i] = 0u;
    for (uint32_t i = 0u; g_batcal_cell_magic[i] != '\0'; ++i) {
        g_batcal_cell_sector[i] = (uint8_t)g_batcal_cell_magic[i];
    }
    g_batcal_cell_sector[BATCAL_CELL_STATE] = state;
    g_batcal_cell_sector[16] = (uint8_t)(saved_con0 & 0xffu);
    g_batcal_cell_sector[17] = (uint8_t)((saved_con0 >> 8) & 0xffu);
    g_batcal_cell_sector[BATCAL_CELL_EXTRA + 0u] = (uint8_t)(extra & 0xffu);
    g_batcal_cell_sector[BATCAL_CELL_EXTRA + 1u] = (uint8_t)((extra >> 8) & 0xffu);
    g_batcal_cell_sector[BATCAL_CELL_EXTRA + 2u] = (uint8_t)((extra >> 16) & 0xffu);
    g_batcal_cell_sector[BATCAL_CELL_EXTRA + 3u] = (uint8_t)((extra >> 24) & 0xffu);

    /* Carry the calibrated point through every mark. Without this the run that
     * FINDS the point erases it on its way past, which is the one moment it
     * cannot be re-measured. */
    if (g_batcal_point_valid) {
        const uint32_t pk = 0x80000000u | ((g_batcal_point_reg & 0x7fffu) << 16)
                          | (g_batcal_point_val & 0xffffu);
        g_batcal_cell_sector[BATCAL_CELL_POINT + 0u] = (uint8_t)(pk & 0xffu);
        g_batcal_cell_sector[BATCAL_CELL_POINT + 1u] = (uint8_t)((pk >> 8) & 0xffu);
        g_batcal_cell_sector[BATCAL_CELL_POINT + 2u] = (uint8_t)((pk >> 16) & 0xffu);
        g_batcal_cell_sector[BATCAL_CELL_POINT + 3u] = (uint8_t)((pk >> 24) & 0xffu);
    }

    /* The log. Every mark except the CLEAR updates it -- a clear is the switch
     * being consumed, which is precisely the moment the log must not move. */
    if (state != (uint8_t)'N') {
        g_batcal_last_state = state;
        g_batcal_last_reg   = saved_con0 & 0xffffu;
        g_batcal_last_val   = extra & 0xffffu;
        g_batcal_last_rung  = (state == (uint8_t)'D') ? (uint8_t)((extra >> 16) & 0xffu)
                                                      : (uint8_t)0xffu;
    }
    g_batcal_cell_sector[BATCAL_CELL_LAST + 0u] = g_batcal_last_state;
    g_batcal_cell_sector[BATCAL_CELL_LAST + 1u] = g_batcal_last_rung;
    g_batcal_cell_sector[BATCAL_CELL_LAST + 2u] = (uint8_t)(g_batcal_last_reg & 0xffu);
    g_batcal_cell_sector[BATCAL_CELL_LAST + 3u] = (uint8_t)((g_batcal_last_reg >> 8) & 0xffu);
    g_batcal_cell_sector[BATCAL_CELL_LAST + 4u] = (uint8_t)(g_batcal_last_val & 0xffu);
    g_batcal_cell_sector[BATCAL_CELL_LAST + 5u] = (uint8_t)((g_batcal_last_val >> 8) & 0xffu);
    g_batcal_cell_sector[BATCAL_CELL_LAST + 6u] = g_batcal_done_rung;
    g_batcal_cell_sector[BATCAL_CELL_LAST + 7u] = g_batcal_done_code;

    /* The sticky verdict rides through every mark for the same reason the point
     * does: the mark rewrites the whole sector, so anything not carried is
     * erased -- and the marks that matter most are the ones written immediately
     * before the board may stop executing. */
    g_batcal_cell_sector[BATCAL_CELL_VERDICT] = g_batcal_verdict;

    /* THE BASELINE AND THE SEPARATION USED TO BE CARRIED HERE TOO, for the same
     * reason -- and their absence means every mark now clears bytes 40..46 on the
     * way past, because the sector is zeroed at the top of this function. That is
     * intended: the window they fed is retired, and a stale baseline that outlived
     * its reader is the kind of thing that gets resurrected by mistake. */

    if (mt6592_sd_write((uint64_t)BATCAL_CELL_LBA, 1u, g_batcal_cell_sector) != MT6592_MSDC_OK) return -1;
    for (uint32_t i = 0u; i < 512u; ++i) g_batcal_cell_sector[i] = 0u;
    if (mt6592_sd_read((uint64_t)BATCAL_CELL_LBA, 1u, g_batcal_cell_sector) != MT6592_MSDC_OK) return -1;
    if (!batcal_cell_has_magic(g_batcal_cell_sector)) return -1;
    if (g_batcal_cell_sector[BATCAL_CELL_STATE] != state) return -1;
    return 0;
}

/*
 * THE STICKY VERDICT, written on its own.
 *
 * Deliberately NOT folded into batcal_cell_mark(): that function rewrites the
 * whole sector including the state letter, and every caller here has just
 * finished deciding what the state letter should be. This one reads, patches a
 * single byte, and writes back, so recording an answer can never disturb the
 * dead-man's switch that produced it.
 *
 * The early return on an unchanged verdict is not an optimisation. The write
 * that matters happens in the moments around a probe, and the fewer card writes
 * there are in that window, the fewer there are to be caught half-done by a
 * brownout.
 */
static int batcal_verdict_record(uint8_t v) {
    if (v != (uint8_t)'P' && v != (uint8_t)'A') return -1;
    if (g_batcal_verdict == v) return 0;
    g_batcal_verdict = v;
    if (mt6592_sd_read((uint64_t)BATCAL_CELL_LBA, 1u, g_batcal_cell_sector) != MT6592_MSDC_OK)
        return -1;
    if (!batcal_cell_has_magic(g_batcal_cell_sector)) return -1;
    g_batcal_cell_sector[BATCAL_CELL_VERDICT] = v;
    if (mt6592_sd_write((uint64_t)BATCAL_CELL_LBA, 1u, g_batcal_cell_sector) != MT6592_MSDC_OK)
        return -1;
    return 0;
}

/* What the sector can be holding when a run starts. */
enum {
    BATCAL_MARK_ARMED    = 'A', /* the long cut was in progress and never cleared  */
    BATCAL_MARK_SURVIVED = 'S', /* the long cut came back                          */
    BATCAL_MARK_CLEARED  = 'N', /* a fatal verdict already read and consumed       */
    BATCAL_MARK_RUNG     = 'L', /* a sag-ladder rung was in progress; extra = us   */
    BATCAL_MARK_LADDER   = 'C', /* the ladder finished; extra = the top rung's us  */
    BATCAL_MARK_LIVE     = 'V', /* a `live' cut was in progress; extra = us.
                                 * NOT a verdict -- see the arm that reads it.     */
    BATCAL_MARK_STEP     = 'D', /* a ladder step was being written when the board
                                 * went down. reg in [16..17], extra = step<<16|val.
                                 * This is the CALIBRATION result, not a verdict.  */
    BATCAL_MARK_PROBE    = 'P'  /* the calibrated point was being held. THIS one
                                 * is a verdict, and it has a register on it.      */
};

/*
 * What batcal_breadcrumb() returns. These used to be the driver's own presence
 * tri-state, which is how a console read ended up publishing a verdict; they are
 * local now, and every caller that is not checking for UNUSABLE ignores them.
 *
 * UNUSABLE is separated from ABSENT so that "no answer on the card" can never
 * read as "no cell".
 */
enum {
    BATCAL_CRUMB_UNUSABLE = -1,
    BATCAL_CRUMB_UNKNOWN  = 0,
    BATCAL_CRUMB_PRESENT  = 1,
    BATCAL_CRUMB_ABSENT   = 2
};

static uint32_t batcal_crumb_extra(const uint8_t* s) {
    return (uint32_t)s[BATCAL_CELL_EXTRA]
         | ((uint32_t)s[BATCAL_CELL_EXTRA + 1u] << 8)
         | ((uint32_t)s[BATCAL_CELL_EXTRA + 2u] << 16)
         | ((uint32_t)s[BATCAL_CELL_EXTRA + 3u] << 24);
}

/*
 * READ LAST RUN'S ANSWER AND PRINT IT.
 *
 * This is the half of the test that makes it more than a console trick: the cut
 * measures presence once, and this is what makes that one measurement legible
 * afterwards, across a power cycle, without anybody re-running anything. It
 * touches no rail, so it is safe to call at any time and on any board.
 *
 * IT NO LONGER HANDS THE ANSWER TO THE DRIVER, because there is nowhere to hand
 * it: the presence API is gone (see the tombstone after `bat forget'). The
 * consequence is worth stating plainly -- the LED, the charge park and the ATAGs
 * do NOT change behaviour because of anything on the card. This is a report.
 *
 * Returns BATCAL_CRUMB_PRESENT / _ABSENT / _UNKNOWN, or BATCAL_CRUMB_UNUSABLE if
 * the sector cannot be used at all. `consume` clears a fatal verdict after
 * reporting it, so the next run starts from a known state -- a plain read leaves
 * it in place and can be repeated.
 */
static int batcal_breadcrumb(int consume) {
    uint8_t state;

    if (!mt6592_sd_present() && mt6592_sd_probe() != MT6592_MSDC_OK) {
        put_str("bat: no SD card. This test needs somewhere to leave its answer,\n");
        put_str("bat: because the no-cell answer powers off the board that would\n");
        put_str("bat: otherwise print it. Insert the card and re-run.\n");
        return BATCAL_CRUMB_UNUSABLE;
    }

    if (mt6592_sd_read((uint64_t)BATCAL_CELL_LBA, 1u, g_batcal_cell_sector) != MT6592_MSDC_OK) {
        put_str("bat: cannot read the breadcrumb sector; the card is not answering.\n");
        return BATCAL_CRUMB_UNUSABLE;
    }

    if (!batcal_cell_has_magic(g_batcal_cell_sector)) {
        if (batcal_cell_is_blank(g_batcal_cell_sector)) {
            put_str("bat: no answer on the card yet -- nothing has been measured.\n");
            return BATCAL_CRUMB_UNKNOWN;
        }
        kv_hex("bat: LBA ", (uint32_t)BATCAL_CELL_LBA, 4u);
        put_str(" is not blank and is not ours. Refusing to write:\n");
        kv_hex("bat:   first bytes ", (uint32_t)g_batcal_cell_sector[0], 2u);
        kv_hex(" ", (uint32_t)g_batcal_cell_sector[1], 2u);
        kv_hex(" ", (uint32_t)g_batcal_cell_sector[2], 2u);
        kv_hex(" ", (uint32_t)g_batcal_cell_sector[3], 2u);
        put_char('\n');
        put_str("bat: something on this card uses that sector. Move BATCAL_CELL_LBA.\n");
        return BATCAL_CRUMB_UNUSABLE;
    }

    state = g_batcal_cell_sector[BATCAL_CELL_STATE];

    /* Pick the calibrated point back up before anything else looks at the sector:
     * the whole reason it is written there is that the run which measured it did
     * not survive to report it. */
    {
        const uint32_t pk = (uint32_t)g_batcal_cell_sector[BATCAL_CELL_POINT]
                          | ((uint32_t)g_batcal_cell_sector[BATCAL_CELL_POINT + 1u] << 8)
                          | ((uint32_t)g_batcal_cell_sector[BATCAL_CELL_POINT + 2u] << 16)
                          | ((uint32_t)g_batcal_cell_sector[BATCAL_CELL_POINT + 3u] << 24);
        if ((pk & 0x80000000u) != 0u) {
            g_batcal_point_reg   = (pk >> 16) & 0x7fffu;
            g_batcal_point_val   = pk & 0xffffu;
            g_batcal_point_valid = 1;
        }
    }

    /* The sticky verdict next, and BEFORE the state letter is interpreted below:
     * a run that died in flight is about to turn the ARMED letter into a fresh
     * ABSENT, and it must not be overwriting a verdict it has not read yet. */
    {
        const uint8_t v = g_batcal_cell_sector[BATCAL_CELL_VERDICT];
        if (v == (uint8_t)'P' || v == (uint8_t)'A') g_batcal_verdict = v;
    }

    /* THE BASELINE AND THE SEPARATION ARE NOT DECODED. Bytes 40..46 were the
     * voltage window's calibration and were pushed into the driver from here; the
     * window is gone, so they are read past. A card written by an older image
     * still has them, and the first mark this image writes will clear them. */

    /* The log, which is what makes the answer readable more than once. */
    {
        const uint8_t ls = g_batcal_cell_sector[BATCAL_CELL_LAST];
        if (ls >= 0x20u && ls < 0x7fu) {
            g_batcal_last_state = ls;
            g_batcal_last_rung  = g_batcal_cell_sector[BATCAL_CELL_LAST + 1u];
            g_batcal_last_reg   = (uint32_t)g_batcal_cell_sector[BATCAL_CELL_LAST + 2u]
                                | ((uint32_t)g_batcal_cell_sector[BATCAL_CELL_LAST + 3u] << 8);
            g_batcal_last_val   = (uint32_t)g_batcal_cell_sector[BATCAL_CELL_LAST + 4u]
                                | ((uint32_t)g_batcal_cell_sector[BATCAL_CELL_LAST + 5u] << 8);
            g_batcal_done_rung  = g_batcal_cell_sector[BATCAL_CELL_LAST + 6u];
            g_batcal_done_code  = g_batcal_cell_sector[BATCAL_CELL_LAST + 7u];
        }
    }

    if (state == (uint8_t)BATCAL_MARK_ARMED) {
        put_str("bat: the breadcrumb from the previous run still reads ARMED.\n");
        put_str("bat: That run handed the load to the battery and never came back\n");
        put_str("bat: to clear it -- the board powered off inside its own test.\n");
        put_str("bat: NO CELL FITTED. Nothing but the charger was holding VSYS.\n");
        put_str("bat: This is a stronger answer than any voltage: the rail did not\n");
        put_str("bat: sag, it ceased.\n");
        batcal_verdict_record((uint8_t)'A');
        put_str("bat: RECORDED ON THE CARD, AND NOWHERE ELSE. The driver has no\n");
        put_str("bat: presence property, so the LED, the park and the kernel go on\n");
        put_str("bat: assuming a cell is fitted. This line is for the operator.\n");
        if (consume) {
            put_str("bat: clearing the breadcrumb; re-run to test again.\n");
            (void)batcal_cell_mark((uint8_t)BATCAL_MARK_CLEARED, 0u, 0u);
        }
        return BATCAL_CRUMB_ABSENT;
    }

    /*
     * A MARK LEFT BY A COMMAND THAT NO LONGER EXISTS, and it cost a wrong answer
     * before it was caught. Measured 2026-08-09: a `bat cal` run opened by
     * announcing "the board did not survive a cut of 1000 us / NO CELL FITTED /
     * the driver now reports ABSENT" -- from an 'L' left on the card by the
     * deleted sag ladder, on a board nobody had touched, before the new run had
     * written anything at all.
     *
     * Two things were wrong with it and both are worth keeping written down. The
     * mark belonged to a retired command, so it described a rail event from an
     * unknown earlier session; and its inference -- cut not survived, therefore
     * no cell -- is the one measured to be false, because a 1 ms handover kills
     * a cell-fitted board about one run in three. So these two letters are now
     * read as history, reported as history, and cleared without voting.
     */
    if (state == (uint8_t)BATCAL_MARK_RUNG || state == (uint8_t)BATCAL_MARK_LADDER) {
        const uint32_t us = batcal_crumb_extra(g_batcal_cell_sector);
        put_str("bat: the card holds a mark from a command that has been removed\n");
        kv_dec("bat: (a cut ladder, ", (int32_t)us);
        put_str(" us). It is not a verdict and it is not\n");
        put_str("bat: this board's current state -- clearing it and leaving the\n");
        put_str("bat: driver's reading exactly as it was. Use `bat cal'.\n");
        if (consume) (void)batcal_cell_mark((uint8_t)BATCAL_MARK_CLEARED, 0u, us);
        return BATCAL_CRUMB_UNKNOWN;
    }

    /*
     * THE CALIBRATION LANDED. A ladder step was in flight when the board went
     * down, and the sector names it: which register, which value, which rung.
     *
     * This is the thing the sweeps could never report. `bat cal` has powered
     * this board off before, and all that was left afterwards was the last line
     * the console had managed to flush -- which is the step BEFORE the fatal one,
     * because the fatal one never printed. Writing the step to the card first
     * turns "it died somewhere in there" into an address and a value.
     *
     * It deliberately does NOT decide presence. The run that produces it is the
     * one taken with the pack out, on purpose, to find the edge; calling that
     * ABSENT would be reporting the experiment's own setup as its result. It
     * arms `bat probe`, and `probe` is what answers the question afterwards.
     */
    if (state == (uint8_t)BATCAL_MARK_STEP) {
        const uint32_t packed = batcal_crumb_extra(g_batcal_cell_sector);
        const uint32_t reg    = (uint32_t)g_batcal_cell_sector[16]
                              | ((uint32_t)g_batcal_cell_sector[17] << 8);
        const uint32_t val    = packed & 0xffffu;
        const uint32_t step   = packed >> 16;

        put_str("bat: the breadcrumb reads LADDER STEP IN PROGRESS -- and that is the\n");
        put_str("bat: calibration coming back.\n");
        kv_hex("bat: the board went down writing PMIC ", reg, 4u);
        kv_hex(" = ", val, 4u);
        kv_dec("  (rung ", (int32_t)step);
        put_str(")\n");
        if (reg == (uint32_t)BATCAL_CV_REG) {
            kv_dec("bat: that is CHR_CON3, CV code ", (int32_t)(val & (uint32_t)BATCAL_CV_MASK));
            kv_dec(" = ", (int32_t)g_batcal_cv_mv[val & (uint32_t)BATCAL_CV_MASK]);
            put_str(" mV.\n");
        }
        put_str("bat: THAT WRITE IS THE POINT. It is where the charger stops holding\n");
        put_str("bat: VSYS, so it is where the system has to draw from the cell or stop\n");
        put_str("bat: -- which is exactly what the operator has been saying: the\n");
        put_str("bat: register that crashes the board is the register that says whether\n");
        put_str("bat: a battery is there.\n");
        g_batcal_point_reg   = reg;
        g_batcal_point_val   = val;
        g_batcal_point_step  = step;
        g_batcal_point_valid = 1;
        put_str("bat: `bat probe' now goes straight to it -- one write, no ladder,\n");
        put_str("bat: no sweep -- and answers the question in a fraction of a second.\n");
        put_str("bat: This is NOT a presence verdict: the run that found it was meant\n");
        put_str("bat: to die. Nothing has been pushed into the driver.\n");
        if (consume) (void)batcal_cell_mark((uint8_t)BATCAL_MARK_CLEARED, reg, packed);
        return BATCAL_CRUMB_UNKNOWN;
    }

    /*
     * The calibrated point was being HELD when the board went down. Unlike every
     * other fatal mark here, this one is a verdict, and it is entitled to be:
     * the point was measured on this board, the same hold was demonstrated
     * survivable with a cell fitted, and the value is on the card.
     */
    if (state == (uint8_t)BATCAL_MARK_PROBE) {
        const uint32_t packed = batcal_crumb_extra(g_batcal_cell_sector);
        const uint32_t reg    = (uint32_t)g_batcal_cell_sector[16]
                              | ((uint32_t)g_batcal_cell_sector[17] << 8);
        const uint32_t val    = packed & 0xffffu;

        put_str("bat: the breadcrumb reads PROBE IN PROGRESS.\n");
        kv_hex("bat: the board did not survive PMIC ", reg, 4u);
        kv_hex(" = ", val, 4u);
        put_char('\n');
        if (reg == (uint32_t)BATCAL_CV_REG) {
            kv_dec("bat: CHR_CON3 CV code ", (int32_t)(val & (uint32_t)BATCAL_CV_MASK));
            kv_dec(" = ", (int32_t)g_batcal_cv_mv[val & (uint32_t)BATCAL_CV_MASK]);
            put_str(" mV -- below that the charger stops\n");
            put_str("bat: holding VSYS, and nothing else did.\n");
        }
        put_str("bat: NO CELL FITTED, with a register and a value on it rather than a\n");
        put_str("bat: silence: this exact write was demonstrated survivable with a cell\n");
        put_str("bat: in, and it was not survived.\n");
        batcal_verdict_record((uint8_t)'A');
        put_str("bat: on the card. Nothing downstream reads it; see the ARMED branch.\n");
        if (consume) (void)batcal_cell_mark((uint8_t)BATCAL_MARK_CLEARED, reg, packed);
        return BATCAL_CRUMB_ABSENT;
    }

    /*
     * A `live' cut was in progress and the board went down inside it. This USED
     * TO BE the RUNG arm below, and it reported NO CELL FITTED -- which is now
     * measured to be wrong, and wrong in the direction that pushes a false
     * ABSENT into the driver.
     *
     * Measured 2026-08-09, one session, one battery state, cell fitted
     * throughout: `bat probe' survived 31 consecutive 1000 us cuts; the very
     * next invocation took the board off the bus while still printing its
     * banner; a third survived 21 cuts and died at +20 s. So a 1 ms handover is
     * NOT reliably survivable with a cell fitted, and the inference the loop
     * rests on -- "300 ms was shrugged off, therefore 1 ms is trivial" -- is
     * false on this hardware.
     *
     * Death inside a cut therefore says the rail did not hold. It does not say
     * nothing was holding it. Report UNKNOWN and leave the driver alone.
     */
    if (state == (uint8_t)BATCAL_MARK_LIVE) {
        const uint32_t us = batcal_crumb_extra(g_batcal_cell_sector);
        put_str("bat: the breadcrumb reads LIVE CUT IN PROGRESS.\n");
        kv_dec("bat: the board went down inside a `live' cut of ", (int32_t)us);
        put_str(" us.\n");
        put_str("bat: THAT IS NOT A PRESENCE VERDICT. Measured 2026-08-09: a cut of\n");
        put_str("bat: this length took this board down TWICE WITH THE CELL FITTED, in\n");
        put_str("bat: the same session and the same battery state in which it had just\n");
        put_str("bat: been survived 31 times in a row. A cut that is not survived says\n");
        put_str("bat: the rail did not hold -- not that nothing was holding it.\n");
        put_str("bat: The driver's reading is left exactly as it was. Use `bat cal',\n");
        put_str("bat: which finds the point that answers this without risking a rail.\n");
        if (consume) (void)batcal_cell_mark((uint8_t)BATCAL_MARK_CLEARED, 0u, us);
        return BATCAL_CRUMB_UNKNOWN;
    }

    if (state == (uint8_t)BATCAL_MARK_SURVIVED) {
        put_str("bat: last run: SURVIVED the full cut. CELL FITTED.\n");
        batcal_verdict_record((uint8_t)'P');
        put_str("bat: on the card. Nothing downstream reads it; see the ARMED branch.\n");
        return BATCAL_CRUMB_PRESENT;
    }

    put_str("bat: the dead-man's switch is clear -- no run is outstanding and\n");
    put_str("bat: nothing is being risked right now. What the last run MEASURED is\n");
    put_str("bat: below; it is kept separately and is not consumed by reading it.\n");
    return BATCAL_CRUMB_UNKNOWN;
}

/*
 * WHAT THE LAST RUN MEASURED, printed every time and never cleared.
 *
 * The switch above answers "is a run outstanding"; this answers "what did the
 * last one find", which is the question anybody actually types `bat` for. They
 * were the same field until 2026-08-09, and that is why the second `bat` after a
 * successful calibration said nothing was known.
 */
static void batcal_last_show(void) {
    if (g_batcal_last_state == 0u) return;

    put_str("bat: LAST MEASUREMENT (kept on the card, never cleared):\n");

    if (g_batcal_done_rung != 0xffu) {
        kv_dec("bat:   last rung COMPLETED: rung ", (int32_t)g_batcal_done_rung);
        kv_dec(", CV code ", (int32_t)g_batcal_done_code);
        if (g_batcal_done_code <= (uint32_t)BATCAL_CV_MASK) {
            kv_dec(" = ", (int32_t)g_batcal_cv_mv[g_batcal_done_code]);
            put_str(" mV");
        }
        put_str(" -- survived\n");
    }

    if (g_batcal_last_state == (uint8_t)BATCAL_MARK_STEP) {
        put_str("bat:   then the board went down writing");
        if (g_batcal_last_rung != 0xffu) {
            kv_dec(" rung ", (int32_t)g_batcal_last_rung);
        }
        kv_hex(": PMIC ", g_batcal_last_reg, 4u);
        kv_hex(" = ", g_batcal_last_val, 4u);
        put_char('\n');
        if (g_batcal_last_reg == (uint32_t)BATCAL_CV_REG) {
            kv_dec("bat:   CHR_CON3 CV code ",
                   (int32_t)(g_batcal_last_val & (uint32_t)BATCAL_CV_MASK));
            kv_dec(" = ", (int32_t)g_batcal_cv_mv[g_batcal_last_val & (uint32_t)BATCAL_CV_MASK]);
            put_str(" mV\n");
        } else if (g_batcal_last_reg == (uint32_t)BATCAL_CHR_CON0) {
            put_str("bat:   CHR_CON0 with CSDAC_EN + CHR_EN cleared -- the charger-off\n");
        }
        put_str("bat:   so the answer is bracketed: the rung above was survived and\n");
        put_str("bat:   this one was not. That write IS the point.\n");
    } else if (g_batcal_last_state == (uint8_t)BATCAL_MARK_PROBE) {
        kv_hex("bat:   the board went down holding PMIC ", g_batcal_last_reg, 4u);
        kv_hex(" = ", g_batcal_last_val, 4u);
        put_str("  (probe)\n");
        put_str("bat:   NO CELL FITTED, at the time that rung was taken.\n");
        batcal_verdict_record((uint8_t)'A');
    } else if (g_batcal_last_state == (uint8_t)BATCAL_MARK_SURVIVED) {
        put_str("bat:   the rung was taken and survived. CELL FITTED, at the time.\n");
        batcal_verdict_record((uint8_t)'P');
    } else if (g_batcal_last_state == (uint8_t)BATCAL_MARK_ARMED) {
        put_str("bat:   the board went down inside a 300 ms charger-off.\n");
        put_str("bat:   NO CELL FITTED, at the time that rung was taken.\n");
        batcal_verdict_record((uint8_t)'A');
    }

    /*
     * ── THIS IS HISTORY, AND HISTORY IS ALL IT EVER GETS TO BE. ──
     *
     * Each of the three branches above used to push its verdict back into the
     * driver, on the argument that the dead-man's switch is consumed on first read
     * and something had to keep the answer alive across a second `bat'.
     *
     * That argument was written when the rung was the only instrument there was,
     * and it became the reason the board reported the wrong state: this function
     * replayed a probe from some earlier session and overwrote a live reading, so
     * the NEXT command opened with ABSENT and a pack sitting in the holder. LK did
     * the same on every boot and never left the unfitted state.
     *
     * The driver has no presence API at all now, so there is nothing left to
     * overwrite -- but the ordering lesson outlives the API and is worth keeping
     * written down: a verdict from the last time the rung was TAKEN cannot notice
     * a pack pulled since. Stale evidence does not outrank fresh evidence, and a
     * console that prints history must not be able to publish it.
     *
     * The card's own record is still updated (batcal_verdict_record) because
     * knowing which write stopped the board is worth having across a power cycle.
     * It is read back by `bat' and by nothing else.
     */
}








/*
 * ── bat cal ── WHICH WRITE IS THE ONE THAT KILLS IT?
 *
 * `bat cal` has powered this board off before, with the pack out, and that
 * run is where the whole line of attack came from. But it came back with nothing
 * usable, and the reason is worth stating plainly: THE FATAL STEP NEVER PRINTS.
 * The console flushes a line after a rung succeeds, so the last line on screen is
 * always the rung BEFORE the one that did it, and everything past that is a
 * silence that could have been any of the remaining steps.
 *
 * So the ladder now writes each rung to the card BEFORE taking it -- the same
 * dead-man's switch the 300 ms cut uses, one per step instead of one per run.
 * The board goes down mid-write, the sector still says which write, and the next
 * boot reads back a register, a value and a rung number.
 *
 * That is the calibration the operator asked for, in the form they asked for it:
 * not a sensing bit, because there is no sensing bit -- the point where the board
 * stops being able to run IS the point where it must be drawing from the cell,
 * and the register that moves it there is the register that answers the
 * question. Everything downstream is then one write instead of a sweep.
 *
 * THE LAST RUNG IS THE 300 ms CHARGER-OFF, so the ladder cannot fall off the
 * bottom without ever reaching a fatal write. The CV codes only lower the
 * setpoint; that final rung lets go of VSYS outright, which is the one write
 * that has never been survived with an empty holder. Folding it in here is what
 * retired the separate `cell' command -- same test, same 300 ms, now just the
 * end of the same ladder and breadcrumbed like every other step.
 *
 * WHICH RUNS PRODUCE AN ANSWER. The one taken with the pack OUT dies, and the
 * rung it dies on is the point: pull the pack, run `bat cal', let it die, power
 * cycle, and the first `bat' after that prints the register and the value.
 *
 * The one taken with the pack IN survives to the end, charger-off included, and
 * that is a verdict rather than a calibration -- nothing but a cell can hold the
 * rail across that rung, so it reports CELL FITTED and pushes PRESENT. It
 * produces no point, because a ladder with no fatal rung in it has nothing to
 * name. Note the asymmetry and do not read it backwards: surviving proves a
 * cell; dying proves only that the rail let go, which is why the fatal run
 * calibrates instead of declaring.
 */
static void batcal_find(int upward) {
    const uint32_t n = upward
        ? (uint32_t)(sizeof(g_batcal_up_codes) / sizeof(g_batcal_up_codes[0]))
        : (uint32_t)(sizeof(g_batcal_cv_steps) / sizeof(g_batcal_cv_steps[0]));
    const uint32_t settle = upward ? (uint32_t)BATCAL_UP_SETTLE_MS : (uint32_t)BATCAL_SETTLE_MS;
    uint32_t saved = 0u;
    uint32_t i;
    uint32_t reached = 0u;
    int      any = 0;

    if (batcal_breadcrumb(1) == BATCAL_CRUMB_UNUSABLE) return;

    if (mt6592_pwrap_read(BATCAL_CV_REG, &saved) != MT6592_PWRAP_OK) {
        put_str("bat: cannot read CHR_CON3; pwrap is down\n");
        return;
    }
    saved &= (uint32_t)BATCAL_CV_MASK;

    /* A new ladder, so nothing has completed yet. Leaving the previous run's
     * value here would let an old rung be reported as this run's bracket. */
    g_batcal_done_rung = 0xffu;
    g_batcal_done_code = 0xffu;

    put_str(upward ? "bat: WALKING THE CV SETPOINT UP, ONE BREADCRUMB PER RUNG.\n"
                   : "bat: WALKING THE CV SETPOINT DOWN, ONE BREADCRUMB PER RUNG.\n");
    put_str("bat: THIS IS MEANT TO POWER THE BOARD OFF. Run it with the pack OUT --\n");
    put_str("bat: that is the run that produces the answer. Each rung goes onto the\n");
    put_str("bat: card BEFORE it is written to the charger, so the step that kills\n");
    put_str("bat: the board is on the medium when the board stops. Power cycle after\n");
    put_str("bat: it dies and the next `bat' names the register and the value.\n");
    kv_dec("bat: starting from CV code ", (int32_t)saved);
    kv_dec(" = ", (int32_t)g_batcal_cv_mv[saved]);
    put_str(" mV\n");
    put_str("bat:  rung  code  set_mv   vbat_mv  charger\n");
    flush_line();

    wdt_arm(BATCAL_WDT_SECS);

    for (i = 0u; i < n; ++i) {
        const uint32_t code = upward ? (uint32_t)g_batcal_up_codes[i]
                                     : (uint32_t)g_batcal_cv_steps[i].code;
        const int32_t  mv   = (int32_t)g_batcal_cv_mv[code & (uint32_t)BATCAL_CV_MASK];
        int vbat;

        /* The card first, then the charger. Reversing these two lines is the
         * whole difference between this and the sweep it replaces. */
        if (batcal_cell_mark((uint8_t)BATCAL_MARK_STEP, (uint32_t)BATCAL_CV_REG,
                             (i << 16) | (code & 0xffffu)) != 0) {
            put_str("bat: the rung did not land on the card. Refusing to write it: a\n");
            put_str("bat: death with nothing recorded is the one outcome that wastes a\n");
            put_str("bat: power cycle and answers nothing.\n");
            break;
        }
        if (batcal_set_cv(code) != 0) {
            put_str("bat: CV write failed; restoring\n");
            break;
        }
        /* Read it back before calling it survived. A rung the charger ignored is
         * not evidence of anything, and a ladder full of those calibrates a
         * point that was never applied. */
        {
            uint32_t rbk = 0u;
            if (mt6592_pwrap_read(BATCAL_CV_REG, &rbk) != MT6592_PWRAP_OK) rbk = 0xffffffffu;
            else rbk &= (uint32_t)BATCAL_CV_MASK;
            if (rbk != code) {
                kv_dec("bat: rung ", (int32_t)i);
                kv_dec(" DID NOT LAND: CHR_CON3 reads ", (int32_t)rbk);
                kv_dec(" not ", (int32_t)code);
                put_str("\nbat: stopping -- the ladder below this rung would measure nothing.\n");
                break;
            }
        }
        delay_ms(settle);
        wdt_kick();

        vbat = batcal_vbat_mv();
        put_str("bat:   ");
        put_dec((int32_t)i);
        kv_dec("     ", (int32_t)code);
        kv_dec("    ", mv);
        if (vbat < 0) put_str("      --   ");
        else          kv_dec("     ", (int32_t)vbat);
        put_str("  ");
        batcal_print_flags();
        put_str("  survived\n");
        flush_line();

        /* The rung is done. Record it here and the NEXT rung's arm-write carries
         * it to the card for free -- so when the board dies, the sector holds the
         * bracket: this rung completed, that one was in flight. The console line
         * above says the same thing, but the console is what the death eats. */
        g_batcal_done_rung = (uint8_t)i;
        g_batcal_done_code = (uint8_t)code;

        reached = code;
        any = 1;
    }

    (void)batcal_set_cv(saved);
    delay_ms(settle);

    if (!any) {
        wdt_disarm();
        (void)batcal_cell_mark((uint8_t)BATCAL_MARK_CLEARED, (uint32_t)BATCAL_CV_REG, 0u);
        put_str("bat: no rung completed; nothing was measured.\n");
        return;
    }

    kv_dec("bat: every CV rung survived, down to code ", (int32_t)reached);
    kv_dec(" = ", (int32_t)g_batcal_cv_mv[reached & (uint32_t)BATCAL_CV_MASK]);
    put_str(" mV.\n");

    /*
     * THE LAST RUNG, and the only write on this board that has never failed to be
     * fatal with nothing in the holder: stock's own charger-off, CHR_CON0
     * CSDAC_EN + CHR_EN, from charging_func[2] at 0xc051479c.
     *
     * The CV rungs lower the setpoint; this one lets go of the node entirely, so
     * the ladder ends somewhere it cannot fall through. That is what folds the
     * old separate 300 ms `cell' test in here: this rung IS that test, taken last
     * and breadcrumbed like every other rung, so surviving it is the same
     * evidence it always was and dying in it now names a register.
     */
    {
        uint32_t con0 = 0u;
        if (mt6592_pwrap_read(BATCAL_CHR_CON0, &con0) != MT6592_PWRAP_OK) {
            wdt_disarm();
            put_str("bat: cannot read CHR_CON0; stopping before the last rung.\n");
            (void)batcal_cell_mark((uint8_t)BATCAL_MARK_CLEARED, (uint32_t)BATCAL_CV_REG, 0u);
            return;
        }
        {
            const uint32_t off = con0 & ~(uint32_t)(BATCAL_CON0_CSDAC_EN | BATCAL_CON0_CHR_EN);
            kv_hex("bat: last rung -- stock's charger-off, CHR_CON0 ", con0, 4u);
            kv_hex(" -> ", off, 4u);
            kv_dec(", held ", (int32_t)BATCAL_CELL_HOLD_MS);
            put_str(" ms.\n");
            put_str("bat: this is not a setpoint, it is the handover itself: across it\n");
            put_str("bat: nothing but a cell can hold VSYS.\n");
            flush_line();

            if (batcal_cell_mark((uint8_t)BATCAL_MARK_STEP, (uint32_t)BATCAL_CHR_CON0,
                                 (n << 16) | (off & 0xffffu)) != 0) {
                wdt_disarm();
                put_str("bat: the rung did not land on the card; refusing to take it.\n");
                return;
            }
            (void)mt6592_pwrap_write(BATCAL_CHR_CON0, off);
            delay_ms((uint32_t)BATCAL_CELL_HOLD_MS);
            (void)mt6592_pwrap_write(BATCAL_CHR_CON0, con0);
            mt6592_pmic_power_hold();
        }
    }
    wdt_disarm();

    /*
     * Survived the lot, charger-off included. That is the ground truth the old
     * `cell' command produced, so it is entitled to the same verdict -- and the
     * rung is cleared rather than left on the card, because a rung that was
     * survived is not the point and must never be read back as one.
     */
    (void)batcal_cell_mark((uint8_t)BATCAL_MARK_SURVIVED, (uint32_t)BATCAL_CHR_CON0, 0u);
    batcal_verdict_record((uint8_t)'P');

    put_str("bat: EVERY RUNG SURVIVED, charger-off included. CELL FITTED -- the\n");
    put_str("bat: charger let go of VSYS for 300 ms and the board stayed up, so\n");
    put_str("bat: something else was holding it.\n");
    put_str("bat: THIS SCREEN IS THE ONLY PLACE THAT SAYS SO. The verdict goes on the\n");
    put_str("bat: card and nowhere else -- the driver has no presence property to set.\n");
    put_str("bat: NOTE this run therefore produced no calibrated point: a ladder that\n");
    put_str("bat: is survived has no fatal rung in it. Pull the pack and run `bat cal'\n");
    put_str("bat: again -- that run dies, and the rung it dies on is the point.\n");
    if (g_batcal_point_valid) {
        kv_hex("bat: an earlier run's point is still on the card: PMIC ",
               g_batcal_point_reg, 4u);
        kv_hex(" = ", g_batcal_point_val, 4u);
        put_str(", untouched.\n");
    }
}

/*
 * ── bat probe ── THE CALIBRATED RUNG, THE WAY IT WAS CALIBRATED.
 *
 * No converter, no sweep, no sampling: take the same rungs `bat cal` took, in
 * the same order, and stop at the one it measured as fatal. Surviving that rung
 * is CELL FITTED; not surviving leaves NO CELL FITTED on the card with the
 * register and the value attached, so the answer is readable on the next boot
 * rather than inferred from a silence.
 *
 * THE RUNG IS THE MEASUREMENT, NOT A DUTY CYCLE OR A MILLIVOLT. Every attempt to
 * sample presence -- static AUXADC, CS_DET per-mille duty, edge rates, load-step
 * sag -- was measured and killed, most of them twice. What is left is one write
 * on one rung, and it is binary.
 *
 * WHY THE HOLD IS SAFE WITH A CELL, WHICH IS THE QUESTION THE OLD `live` LOOP
 * GOT WRONG. That loop argued from a different test -- 300 ms was survived once,
 * so 1 ms must be trivial -- and hardware answered that a 1 ms handover kills a
 * cell-fitted board about one run in three. This argues from THE SAME test:
 * `bat cal` with the pack in sits at every rung of this ladder, including this
 * one, for the same settle time, and runs to the bottom. A single rung held for
 * one settle is strictly less than what the calibration run already
 * demonstrated.
 *
 * A CHR_CON0 POINT IS RECOMPUTED, NEVER REPLAYED. If the calibrated register is
 * the charger-enable pair, the value on the card is a word that was correct on
 * the run that recorded it and is not necessarily correct now -- CHR_CON0 also
 * carries current-limit and enable state the charger driver moves on its own.
 * Writing it verbatim would restore a stale charger configuration under the
 * guise of a presence test. So the probe reads CHR_CON0 first and clears only
 * CSDAC_EN + CHR_EN out of the live value: the same two bits, nothing else.
 */
static void batcal_probe(uint32_t ms) {
    uint32_t saved = 0u;
    uint32_t apply = 0u;
    uint32_t rb    = 0u;
    uint32_t trial;

    if (batcal_breadcrumb(1) == BATCAL_CRUMB_UNUSABLE) return;

    if (ms == 0u) ms = (uint32_t)BATCAL_SETTLE_MS;
    if (ms > 2000u) ms = 2000u;

    if (!g_batcal_point_valid) {
        put_str("bat: no calibrated point on this board yet, so there is nothing to\n");
        put_str("bat: probe. Get one first, and it takes one power cycle:\n");
        put_str("bat:\n");
        put_str("bat:   1. pull the pack, leave the board on DC\n");
        put_str("bat:   2. bat cal        <- it is meant to die\n");
        put_str("bat:   3. power cycle, then: bat\n");
        put_str("bat:\n");
        put_str("bat: That prints the register and the value that stopped the board,\n");
        put_str("bat: and arms this command permanently -- the point lives on the card.\n");
        return;
    }

    if (g_batcal_point_reg == (uint32_t)BATCAL_CV_REG) {
        if (mt6592_pwrap_read(BATCAL_CV_REG, &saved) != MT6592_PWRAP_OK) {
            put_str("bat: cannot read CHR_CON3; pwrap is down\n");
            return;
        }
        saved &= (uint32_t)BATCAL_CV_MASK;
    } else if (mt6592_pwrap_read(g_batcal_point_reg, &saved) != MT6592_PWRAP_OK) {
        put_str("bat: cannot read the calibrated register; pwrap is down\n");
        return;
    }

    apply = g_batcal_point_val;
    if (g_batcal_point_reg == (uint32_t)BATCAL_CV_REG) {
        apply &= (uint32_t)BATCAL_CV_MASK;
    } else if (g_batcal_point_reg == (uint32_t)BATCAL_CHR_CON0) {
        apply = saved & ~(uint32_t)(BATCAL_CON0_CSDAC_EN | BATCAL_CON0_CHR_EN);
    }

    kv_hex("bat: probing the calibrated point: PMIC ", g_batcal_point_reg, 4u);
    kv_hex(" = ", apply, 4u);
    if (g_batcal_point_reg == (uint32_t)BATCAL_CV_REG) {
        kv_dec("  (", (int32_t)g_batcal_cv_mv[apply & (uint32_t)BATCAL_CV_MASK]);
        put_str(" mV)");
    } else if (g_batcal_point_reg == (uint32_t)BATCAL_CHR_CON0) {
        kv_hex("  (CSDAC_EN+CHR_EN cleared out of the live ", saved, 4u);
        put_str(")");
    }
    kv_dec(", held ", (int32_t)ms);
    put_str(" ms.\n");
    put_str("bat: this is the write that took the board down when nothing was holding\n");
    put_str("bat: VSYS but the charger. Surviving it means something else is.\n");
    flush_line();

    g_batcal_done_rung = 0xffu;
    g_batcal_done_code = 0xffu;

    wdt_arm(BATCAL_WDT_SECS);

    /*
     * WALK THE RUNGS RATHER THAN JUMPING TO THE LAST ONE.
     *
     * The point was measured at the end of a ladder: rungs 0..4 taken in order,
     * 200 ms apart, and the board let go on rung 5. Jumping straight from
     * whatever CHR_CON3 happens to hold to code 27 is a different stimulus --
     * one large step into a control loop instead of six small ones -- and a
     * presence test that does not reproduce the conditions it was calibrated
     * under is a test of something else.
     *
     * So take the same rungs, in the same order, with the same settle, each one
     * breadcrumbed exactly as the calibration breadcrumbs it. Dying on an
     * INTERMEDIATE rung is then not a presence verdict but a recalibration: the
     * point moved, and the card says which rung it moved to.
     */
    if (g_batcal_point_reg == (uint32_t)BATCAL_CV_REG) {
        const uint32_t n = (uint32_t)(sizeof(g_batcal_cv_steps) / sizeof(g_batcal_cv_steps[0]));
        uint32_t target = 0xffffffffu;
        uint32_t i;

        for (i = 0u; i < n; ++i) {
            if ((uint32_t)g_batcal_cv_steps[i].code == apply) { target = i; break; }
        }
        if (target != 0xffffffffu && target > 0u) {
            kv_dec("bat: walking rungs 0..", (int32_t)(target - 1u));
            put_str(" first, the same way the point was measured.\n");
            flush_line();
            for (i = 0u; i < target; ++i) {
                const uint32_t code = (uint32_t)g_batcal_cv_steps[i].code;
                if (batcal_cell_mark((uint8_t)BATCAL_MARK_STEP, (uint32_t)BATCAL_CV_REG,
                                     (i << 16) | (code & 0xffffu)) != 0) {
                    wdt_disarm();
                    (void)batcal_set_cv(saved);
                    put_str("bat: a rung did not land on the card; stopping before it.\n");
                    return;
                }
                uint32_t rbk = 0u;

                if (batcal_set_cv(code) != 0) {
                    wdt_disarm();
                    (void)batcal_set_cv(saved);
                    put_str("bat: CV write failed; restored and stopping.\n");
                    return;
                }
                /* READ IT BACK. A rung that did not land is not a rung that was
                 * survived, and printing "survived" for a write the charger
                 * ignored is how a probe reports CELL FITTED with an empty
                 * holder. */
                if (mt6592_pwrap_read(BATCAL_CV_REG, &rbk) != MT6592_PWRAP_OK) rbk = 0xffffffffu;
                else rbk &= (uint32_t)BATCAL_CV_MASK;
                if (rbk != code) {
                    wdt_disarm();
                    (void)batcal_set_cv(saved);
                    (void)batcal_cell_mark((uint8_t)BATCAL_MARK_CLEARED, (uint32_t)BATCAL_CV_REG, 0u);
                    kv_dec("bat: rung ", (int32_t)i);
                    kv_dec(" DID NOT LAND: CHR_CON3 reads ", (int32_t)rbk);
                    kv_dec(" not ", (int32_t)code);
                    put_str("\nbat: INCONCLUSIVE -- the ladder was not walked, so nothing is\n");
                    put_str("bat: declared and the driver's reading is left alone.\n");
                    return;
                }
                delay_ms((uint32_t)BATCAL_SETTLE_MS);
                wdt_kick();
                put_str("bat:   rung ");
                put_dec((int32_t)i);
                kv_dec("  code ", (int32_t)code);
                kv_dec("  ", (int32_t)g_batcal_cv_steps[i].mv);
                put_str(" mV  in effect, survived\n");
                flush_line();
                g_batcal_done_rung = (uint8_t)i;
                g_batcal_done_code = (uint8_t)code;
            }
        }
    }

    /*
     * THE FATAL RUNG, TAKEN MORE THAN ONCE, AND PROVEN TO HAVE BEEN TAKEN.
     *
     * TWO SEPARATE THINGS WENT WRONG HERE AND EACH PRODUCED A FALSE CELL FITTED.
     *
     * The first was writing the point and printing SURVIVED without checking the
     * write. An ignored write is survived by anything, and the board proved it:
     * with the holder EMPTY it walked the ladder and declared a cell. So the
     * register is read back WHILE THE RUNG IS APPLIED, and that read-back is
     * printed as the evidence line. A disagreement is INCONCLUSIVE, never FITTED.
     *
     * The second is that ONE TRIAL IS NOT ENOUGH, measured on this board: pack
     * out, the first `bat probe` after the cell came out survived rung 5 and
     * declared CELL FITTED, and the very next one powered the board off at the
     * same rung. The rail evidently comes into the first probe with charge left
     * in it. So surviving once is not the asymmetry this test rests on --
     * surviving REPEATEDLY is. The rung is taken BATCAL_PROBE_TRIALS times with a
     * short recovery between, and CELL FITTED is only printed if every one of
     * them is survived. Dying on any trial is the answer, and the card already
     * holds it.
     *
     * The recovery gap is deliberately short. A long one would let the rail fill
     * back up between trials and hand every trial the same free pass the first
     * one got, which is the failure being fixed.
     */
    for (trial = 0u; trial < (uint32_t)BATCAL_PROBE_TRIALS; ++trial) {
        uint32_t live = 0u;

        if (batcal_cell_mark((uint8_t)BATCAL_MARK_PROBE, g_batcal_point_reg,
                             apply & 0xffffu) != 0) {
            wdt_disarm();
            if (g_batcal_point_reg == (uint32_t)BATCAL_CV_REG) (void)batcal_set_cv(saved);
            put_str("bat: the breadcrumb did not land on the card; refusing to probe.\n");
            return;
        }

        wdt_arm(BATCAL_WDT_SECS);

        if (g_batcal_point_reg == (uint32_t)BATCAL_CV_REG) (void)batcal_set_cv(apply);
        else (void)mt6592_pwrap_write(g_batcal_point_reg, apply);

        if (mt6592_pwrap_read(g_batcal_point_reg, &live) != MT6592_PWRAP_OK) live = 0xffffffffu;
        else if (g_batcal_point_reg == (uint32_t)BATCAL_CV_REG) live &= (uint32_t)BATCAL_CV_MASK;

        if (live != apply) {
            if (g_batcal_point_reg == (uint32_t)BATCAL_CV_REG) (void)batcal_set_cv(saved);
            else {
                (void)mt6592_pwrap_write(g_batcal_point_reg, saved);
                if (g_batcal_point_reg == (uint32_t)BATCAL_CHR_CON0) mt6592_pmic_power_hold();
            }
            wdt_disarm();
            (void)batcal_cell_mark((uint8_t)BATCAL_MARK_CLEARED, g_batcal_point_reg, 0u);
            kv_hex("bat: THE FATAL RUNG DID NOT LAND: the register reads ", live, 4u);
            kv_hex(" not ", apply, 4u);
            put_str(".\n");
            put_str("bat: INCONCLUSIVE. The charger ignored or overrode the write, so the\n");
            put_str("bat: board surviving means nothing about the cell. Not declaring, and\n");
            put_str("bat: leaving the driver's reading exactly as it was.\n");
            return;
        }

        put_str("bat:   rung ");
        put_dec((int32_t)g_batcal_point_step);
        put_str("  trial ");
        put_dec((int32_t)(trial + 1u));
        put_char('/');
        put_dec((int32_t)BATCAL_PROBE_TRIALS);
        kv_hex("  register now reads ", live, 4u);
        if (g_batcal_point_reg == (uint32_t)BATCAL_CV_REG) {
            kv_dec("  code ", (int32_t)live);
            kv_dec(" = ", (int32_t)g_batcal_cv_mv[live & (uint32_t)BATCAL_CV_MASK]);
            put_str(" mV");
        }
        kv_dec("  -- IN EFFECT, holding ", (int32_t)ms);
        put_str(" ms\n");
        flush_line();

        delay_ms(ms);

        g_batcal_done_rung = (uint8_t)g_batcal_point_step;
        g_batcal_done_code = (uint8_t)apply;

        if (g_batcal_point_reg == (uint32_t)BATCAL_CV_REG) (void)batcal_set_cv(saved);
        else {
            (void)mt6592_pwrap_write(g_batcal_point_reg, saved);
            if (g_batcal_point_reg == (uint32_t)BATCAL_CHR_CON0) mt6592_pmic_power_hold();
        }
        delay_ms((uint32_t)BATCAL_PROBE_GAP_MS);
        wdt_kick();
    }
    delay_ms(20u);
    wdt_disarm();

    (void)batcal_cell_mark((uint8_t)BATCAL_MARK_SURVIVED, g_batcal_point_reg,
                           apply & 0xffffu);
    batcal_verdict_record((uint8_t)'P');

    put_str("bat: SURVIVED");
    if (g_batcal_point_step != 0xffffffffu) {
        kv_dec(" rung ", (int32_t)g_batcal_point_step);
    }
    put_str(", with the rung read back out of the register while it\n");
    put_str("bat: was applied. CELL FITTED -- the charger let go of VSYS and the node\n");
    put_str("bat: stayed up, so something else was holding it, and on this board\n");
    put_str("bat: there is only one thing that can be.\n");
    put_str("bat: IF THE HOLDER IS EMPTY, this verdict is wrong and the point has\n");
    put_str("bat: moved: re-run `bat cal' pack OUT and let it name the new rung.\n");
    put_str("bat: the verdict is on the card, which is the only place it goes; `bat'\n");
    put_str("bat: re-reads it across a reset without touching a rail.\n");

    (void)mt6592_pwrap_read(g_batcal_point_reg, &rb);
    if (g_batcal_point_reg == (uint32_t)BATCAL_CV_REG) rb &= (uint32_t)BATCAL_CV_MASK;
    if (rb != saved) {
        kv_hex("bat: WARNING: the register came back as ", rb, 4u);
        kv_hex(" not ", saved, 4u);
        put_str("; power\n");
        put_str("bat: cycle before trusting the charger.\n");
    }
}

/*
 * ── bat rung ── THE SAME RUNG, TOO BRIEFLY TO DIE IN.
 *
 * `bat probe` works and has now been confirmed both ways: pack out the board goes
 * down on rung 5, pack in it walks straight through it. But the answer costs a
 * reboot every time it is "no cell", because with nothing in the holder the
 * charger letting go of VSYS is the end of the story -- there is no code left
 * running to report it. The reboot IS the report, and that is not good enough for
 * anything but a bench session.
 *
 * SO SHORTEN THE RUNG UNTIL IT IS SURVIVABLE IN BOTH STATES, AND READ THE
 * CHARGER'S OWN STATE MACHINE WHILE IT IS APPLIED. The rail does not vanish the
 * instant CHR_CON3 changes; it falls at some rate set by bulk capacitance against
 * the system load. Somewhere below 200 ms there is a hold the cell-less board
 * still comes back from, and this command is how that boundary gets found -- it
 * takes the rung for exactly N back-to-back reads of CHR_CON2 and no longer, so
 * the hold is as short as the pwrap bus can make it, and it prints the elapsed
 * microseconds so the number is known rather than assumed.
 *
 * WHY CHR_CON2 IS WORTH READING HERE WHEN ITS DUTY WAS THROWN OUT. The duty was
 * measured at the RESTING CV code, where the two states are physically identical:
 * the charger is regulating a node at its setpoint either way, so of course three
 * reads gave 500, 484 and 421 per mille. At the calibrated rung they are not
 * identical. With a cell, the node sits above the target and the loop is parked
 * hard off -- no current, no hunting. With nothing fitted, the loop is actively
 * winding down towards a target it can reach, and it can never arrive above it.
 * Same three bits, completely different question.
 *
 * MEASURED IN BOTH STATES, AND THE ANSWER IS THAT NEITHER BIT CARRIES IT.
 *
 * cs went first. Pack out, rung 1/8 and 3/8 against controls of 8/8 and 6/8 --
 * decisive-looking over two trials, and duly written into this file as "cs is the
 * bit carrying the answer". Pack in then gave rung 7,7,2,7 against CONTROLS of
 * 5,6,5,0: overlapping states, and a control that alone spans the entire range.
 *
 * cv went next, and it had the better story: VBAT_CV_DET means `the node is at or
 * above the CV target', a fitted cell holds ~4180 mV, the rung asks for 4137 mV,
 * and an empty holder has nothing that could be sitting above anything. It fit
 * the first eight trains perfectly -- 0 in every pack-out train, 6/8 and 8/8 at
 * the rung with a cell in. Then a full eight-trial pack-out run asserted 8/8 in
 * trials 1 and 7. Two of eight, against two of four with a cell in: THE SAME
 * RATE.
 *
 * WHAT THE CONTROL WAS SAYING ALL ALONG. In that same run, with nothing changed
 * between trials, ctl cs read 3,1,0,3,3,8,6,8. These bits chatter at a rate close
 * to the 125 us window, so an eight-read train is an ALIASED SAMPLE OF A SQUARE
 * WAVE rather than a state -- identically in both battery states. That is the
 * single explanation for the duty (500/484/421), the edge rate (227 vs 669), the
 * cs trains and the cv trains: five instruments, one cause. Presence is not
 * recoverable from CHR_CON2 on this timescale, and no threshold will change it.
 *
 * THE COMMAND IS KEPT, WITHOUT A VERDICT, because the trains and their controls
 * are the evidence for that conclusion and it should be possible to re-run it in
 * one line rather than re-derive it. `bat baton' is where presence actually
 * comes from now.
 *
 * The trains stay raw, one bit per read: that is what showed cv rising two reads
 * into the window rather than being flat, and a per-mille would have hidden the
 * shape while inviting a threshold on it.
 *
 * IT CAN STILL KILL THE BOARD, so it breadcrumbs like everything else. If the
 * cell-less run dies, halve N and go again; the hold is roughly N pwrap reads.
 */
/* The gap between trials is this command's OWN, and deliberately not the 50 ms
 * `bat probe' uses. There the short gap is the point -- a long one refills the
 * rail and hands every trial the free pass the first one got. Here the opposite
 * is wanted: each trial must start from a settled, re-armed charger loop, or the
 * loop parks after the first assertion and every later trial reads a stale zero.
 * Measured cell in at 50 ms: trials 1 and 2 asserted CV_DET, 3 and 4 did not. */
enum { BATRUNG_MAX = 64u, BATRUNG_DEFAULT = 8u, BATRUNG_TRIALS = 8u,
       BATRUNG_GAP_MS = 250u };

/* One train, as bits, with the raw count of ones beside it. The count is a count
 * and not a fraction on purpose: a per-mille invites a threshold, and every
 * threshold placed on these three bits so far has been placed on noise. */
static uint32_t batrung_train(const char* label, const uint8_t* s, uint32_t n, uint8_t mask) {
    uint32_t i, ones = 0u;
    put_str(label);
    for (i = 0u; i < n; ++i) {
        const int one = (s[i] & mask) ? 1 : 0;
        put_char(one ? '1' : '0');
        ones += (uint32_t)one;
    }
    put_str("  (");
    put_dec((int32_t)ones);
    put_char('/');
    put_dec((int32_t)n);
    put_str(" set)\n");
    return ones;
}

static void batcal_rung(uint32_t n, uint32_t trials) {
    uint8_t  base[BATRUNG_MAX];
    uint8_t  hot[BATRUNG_MAX];
    uint64_t t_base = 0u, t_hot = 0u, t0;
    uint32_t target, i, trial;
    uint32_t cv_ctl_total = 0u, cv_run_total = 0u, cv_run_trials = 0u;
    uint32_t cs_ctl_total = 0u, cs_run_total = 0u;
    int      saved;

    if (batcal_breadcrumb(1) == BATCAL_CRUMB_UNUSABLE) return;

    if (n == 0u) n = (uint32_t)BATRUNG_DEFAULT;
    if (n > (uint32_t)BATRUNG_MAX) n = (uint32_t)BATRUNG_MAX;
    if (trials == 0u) trials = (uint32_t)BATRUNG_TRIALS;
    if (trials > 16u) trials = 16u;

    if (!g_batcal_point_valid || g_batcal_point_reg != (uint32_t)BATCAL_CV_REG) {
        put_str("bat: this needs a CV rung as the calibrated point. `bat cal' pack OUT\n");
        put_str("bat: names one; `bat point 6 1b' sets the measured one by hand.\n");
        return;
    }
    target = g_batcal_point_val & (uint32_t)BATCAL_CV_MASK;

    saved = mt6592_pmic_charger_cv_code();
    if (saved < 0) {
        put_str("bat: cannot read CHR_CON3; pwrap is down\n");
        return;
    }

    kv_dec("bat: rung ", (int32_t)g_batcal_point_step);
    kv_dec(" (CV code ", (int32_t)target);
    kv_dec(" = ", (int32_t)g_batcal_cv_mv[target]);
    kv_dec(" mV) held for ", (int32_t)n);
    kv_dec(" reads of CHR_CON2 and no longer, x", (int32_t)trials);
    put_str(" trials.\n");
    put_str("bat: this is the SHORT form of `bat probe' -- it is trying NOT to kill\n");
    put_str("bat: the board, and measured in both states it does not.\n");
    put_str("bat: THE BIT IS cv (CHR_CON2 bit 6, VBAT_CV_DET): the node is at or\n");
    put_str("bat: above the CV target. Drop the target to the rung and a fitted cell\n");
    put_str("bat: is already above it, so cv asserts; with nothing fitted the node is\n");
    put_str("bat: falling AWAY from that target and it cannot. Measured: cv was 0 in\n");
    put_str("bat: every pack-out train and in every control train in both states,\n");
    put_str("bat: then 6/8 and 8/8 at the rung with a cell in.\n");
    flush_line();

    for (trial = 0u; trial < trials; ++trial) {
        /* The control first, at the resting setpoint, with the same loop and the
         * same bus. Without it a train from the rung is a picture of the pwrap
         * read rate as much as of the charger. */
        t0 = mt6592_timer_microseconds();
        for (i = 0u; i < n; ++i) {
            uint32_t v = 0u;
            (void)mt6592_pwrap_read(BATCAL_CHR_CON2, &v);
            base[i] = (uint8_t)(v & 0xffu);
        }
        t_base = mt6592_timer_microseconds() - t0;

        wdt_arm(BATCAL_WDT_SECS);
        if (batcal_cell_mark((uint8_t)BATCAL_MARK_PROBE, (uint32_t)BATCAL_CV_REG, target) != 0) {
            wdt_disarm();
            put_str("bat: the breadcrumb did not land on the card; refusing to take it.\n");
            return;
        }

        /* THE RUNG. Nothing between the write and the restore but the reads -- no
         * printing, no flushing, no SD. Every instruction in here is hold time the
         * cell-less board has to survive. */
        if (mt6592_pmic_charger_cv_set(target) != 0) {
            (void)mt6592_pmic_charger_cv_set((uint32_t)saved);
            wdt_disarm();
            (void)batcal_cell_mark((uint8_t)BATCAL_MARK_CLEARED, (uint32_t)BATCAL_CV_REG, 0u);
            put_str("bat: the rung DID NOT LAND, so nothing was tested. INCONCLUSIVE.\n");
            return;
        }
        t0 = mt6592_timer_microseconds();
        for (i = 0u; i < n; ++i) {
            uint32_t v = 0u;
            (void)mt6592_pwrap_read(BATCAL_CHR_CON2, &v);
            hot[i] = (uint8_t)(v & 0xffu);
        }
        t_hot = mt6592_timer_microseconds() - t0;
        (void)mt6592_pmic_charger_cv_set((uint32_t)saved);

        wdt_disarm();
        (void)batcal_cell_mark((uint8_t)BATCAL_MARK_CLEARED, (uint32_t)BATCAL_CV_REG, 0u);

        put_str("bat: trial ");
        put_dec((int32_t)(trial + 1u));
        put_char('/');
        put_dec((int32_t)trials);
        kv_dec("   control code ", (int32_t)saved);
        kv_dec(", ", (int32_t)(uint32_t)t_base);
        kv_dec(" us   rung code ", (int32_t)target);
        kv_dec(", ", (int32_t)(uint32_t)t_hot);
        put_str(" us\n");
        cs_ctl_total += batrung_train("bat:   ctl cs ", base, n, (uint8_t)BATCAL_CON2_CS_DET);
        cs_run_total += batrung_train("bat:   run cs ", hot,  n, (uint8_t)BATCAL_CON2_CS_DET);
        cv_ctl_total += batrung_train("bat:   ctl cv ", base, n, (uint8_t)BATCAL_CON2_CV_DET);
        {
            const uint32_t hits = batrung_train("bat:   run cv ", hot, n,
                                                (uint8_t)BATCAL_CON2_CV_DET);
            cv_run_total += hits;
            if (hits != 0u) ++cv_run_trials;
        }
        batrung_train("bat:   ctl cc ", base, n, (uint8_t)BATCAL_CON2_CC_DET);
        batrung_train("bat:   run cc ", hot,  n, (uint8_t)BATCAL_CON2_CC_DET);
        flush_line();
        delay_ms((uint32_t)BATRUNG_GAP_MS);
    }

    /* SURVIVAL IS NOT THE VERDICT HERE. The whole point of shortening the hold is
     * that both states live through it, so the answer has to come out of the
     * trains -- and it comes out of cv. */
    put_str("bat: --- totals over ");
    put_dec((int32_t)trials);
    put_str(" trials ---\n");
    kv_dec("bat:   cv  control ", (int32_t)cv_ctl_total);
    kv_dec(" of ", (int32_t)(trials * n));
    kv_dec("   AT THE RUNG ", (int32_t)cv_run_total);
    kv_dec(" of ", (int32_t)(trials * n));
    put_str("\n");
    kv_dec("bat:   cv asserted at the rung in ", (int32_t)cv_run_trials);
    kv_dec(" of ", (int32_t)trials);
    put_str(" trials\n");
    kv_dec("bat:   cs  control ", (int32_t)cs_ctl_total);
    kv_dec("   at the rung ", (int32_t)cs_run_total);
    put_str("   (secondary; see below)\n");
    flush_line();

    /* NOTHING IS DECIDED HERE, AND THAT IS A RESULT, NOT AN OMISSION. Both bits
     * were proposed as the discriminator and both were falsified by the other
     * battery state -- the numbers are printed above so the next person can see
     * for themselves rather than take it on faith. */
    put_str("bat: SURVIVED every short rung, in both states. NO VERDICT: cs and cv\n");
    put_str("bat: have each been proposed as the discriminator here and each was\n");
    put_str("bat: killed by running the other battery state.\n");
    put_str("bat:   cv: pack OUT asserted 8/8 at the rung in 2 of 8 trials. Pack IN,\n");
    put_str("bat:       2 of 4. Same rate, same full-window shape. The prediction was\n");
    put_str("bat:       that an empty holder could never read at-or-above the target;\n");
    put_str("bat:       it did, twice, and the rule died on its first honest test.\n");
    put_str("bat:   cs: pack OUT 1/8 and 3/8 at the rung looked decisive over two\n");
    put_str("bat:       trials. Pack IN gave 7,7,2,7 over CONTROLS of 5,6,5,0.\n");
    put_str("bat: THE CONTROL IS THE TELL. With nothing changed between trials, ctl cs\n");
    put_str("bat: went 3,1,0,3,3,8,6,8 in one run. These bits chatter at a rate close\n");
    put_str("bat: to the 125 us window, so an 8-read train is an ALIASED SAMPLE OF A\n");
    put_str("bat: SQUARE WAVE, not a state -- in both battery states alike. No\n");
    put_str("bat: threshold on them can survive, and four instruments have now died\n");
    put_str("bat: proving it. Presence is not in CHR_CON2 on this timescale.\n");
    put_str("bat: USE `bat baton' INSTEAD. CHR_CON7 bit 12 is the part's dedicated\n");
    put_str("bat: connector detect, it was never dead, it was never read -- the\n");
    put_str("bat: driver returned on the rung verdict two lines above the sample.\n");
    flush_line();
    /* The saved code is back in the register, so the sweep is over -- hand CHR_CON3
     * to the charger service, which is what re-establishes the 4200 mV CV target.
     * Without this the board keeps the probe's restored power-on code (29, 4162 mV)
     * and goes back to not charging until the cable is pulled. */
    mt6592_pmic_charger_cv_release();
}

/*
 * ── bat baton ── WATCH THE PIN THAT IS BUILT TO ANSWER THIS.
 *
 * Every instrument in this file so far has been an attempt to infer presence from
 * the charger's behaviour, because BATON -- CHR_CON7 bit 12, RGS_BATON_UNDET, the
 * part's dedicated battery-connector detect -- was written off as dead. It was
 * never dead. It was never read: the driver's presence call returned on the rung
 * verdict two lines before the BATON sample, so the bit could not be observed
 * changing, so it could never earn trust, so it stayed skipped. The
 * board printed `undet=-1 seen=0' -- literally "never read, never seen either
 * way" -- and that was taken as evidence about the hardware.
 *
 * This command asks it directly and repeatedly, and it is completely
 * non-destructive: two enable writes once, then one register read per sample. No
 * rung, no cut, nothing that can drop the rail. CHANGE THE CONNECTOR WHILE IT
 * RUNS -- that is the whole experiment. If bit 12 follows the holder, presence
 * detection on this board is a register read and every rung command becomes a
 * bench curiosity.
 *
 * 0x0142 bit 5 is sampled alongside it because the MT6323 map lists a read-only
 * mirror of the same detect there. If CHR_CON7 bit 12 is stuck but 0x0142 bit 5
 * moves, the answer is the same and the address is different.
 */
enum { BATON_MON_DEFAULT_SECS = 20u, BATON_MON_HZ = 20u };

static void batcal_baton(uint32_t secs) {
    uint32_t con7 = 0u, ro = 0u;
    uint32_t i, samples;
    int last_undet = -2, last_ro = -2;
    int seen_lo = 0, seen_hi = 0;
    int ro_lo = 0, ro_hi = 0;

    if (secs == 0u) secs = (uint32_t)BATON_MON_DEFAULT_SECS;
    if (secs > 120u) secs = 120u;
    samples = secs * (uint32_t)BATON_MON_HZ;

    if (mt6592_pwrap_read(BATCAL_CHR_CON7, &con7) != MT6592_PWRAP_OK) {
        put_str("bat: cannot read CHR_CON7; pwrap is down\n");
        return;
    }

    /* Stock's order, as two transactions: bit 2 arms the detector, bit 0 enables
     * it. Folding them into one write would discard the order, which is the only
     * part of this sequence that was ever in doubt. */
    (void)mt6592_pwrap_write(BATCAL_CHR_CON7, con7 | (uint32_t)BATCAL_CON7_TDET_EN);
    (void)mt6592_pwrap_write(BATCAL_CHR_CON7,
                             con7 | (uint32_t)BATCAL_CON7_TDET_EN |
                                 (uint32_t)BATCAL_CON7_BATON_EN);
    delay_ms(5u);
    if (mt6592_pwrap_read(BATCAL_CHR_CON7, &con7) != MT6592_PWRAP_OK) {
        put_str("bat: cannot read CHR_CON7 back\n");
        return;
    }

    kv_hex("bat: CHR_CON7 = ", con7, 4u);
    kv_dec("  tdet_en ", (con7 & (uint32_t)BATCAL_CON7_TDET_EN) ? 1 : 0);
    kv_dec("  baton_en ", (con7 & (uint32_t)BATCAL_CON7_BATON_EN) ? 1 : 0);
    put_str("\n");
    if ((con7 & (uint32_t)BATCAL_CON7_BATON_EN) == 0u) {
        put_str("bat: BATON_EN WILL NOT SET. The detector is refusing to arm, and\n");
        put_str("bat: that is a fact about the register, not about the connector.\n");
        return;
    }

    put_str("bat: BATON armed. UNDET is active high: 1 = no cell, 0 = cell fitted.\n");
    kv_dec("bat: watching CHR_CON7 bit 12 and 0142 bit 5 for ", (int32_t)secs);
    put_str(" seconds.\n");
    put_str("bat: >>> CHANGE THE CONNECTOR WHILE THIS RUNS. <<< A line prints on\n");
    put_str("bat: every change; silence means the bit is not moving. This writes no\n");
    put_str("bat: rung and touches no rail -- it cannot power the board off.\n");
    flush_line();
    wdt_arm((uint32_t)BATCAL_WDT_SECS);

    for (i = 0u; i < samples; ++i) {
        int undet, robit;

        if (mt6592_pwrap_read(BATCAL_CHR_CON7, &con7) != MT6592_PWRAP_OK) continue;
        undet = (con7 & (uint32_t)BATCAL_CON7_BATON_UNDET) ? 1 : 0;
        robit = (mt6592_pwrap_read(BATCAL_BATON_RO_REG, &ro) == MT6592_PWRAP_OK)
                    ? ((ro & (uint32_t)BATCAL_BATON_RO_UNDET) ? 1 : 0)
                    : -1;

        if (undet) seen_hi = 1; else seen_lo = 1;
        if (robit == 1) ro_hi = 1; else if (robit == 0) ro_lo = 1;

        if (undet != last_undet || robit != last_ro) {
            kv_dec("bat: t+", (int32_t)(i * (1000u / (uint32_t)BATON_MON_HZ)));
            put_str(" ms  UNDET ");
            put_dec(undet);
            kv_hex("  con7 ", con7, 4u);
            put_str("   0142 bit5 ");
            if (robit < 0) put_char('?'); else put_dec(robit);
            kv_hex("  0142 ", ro, 4u);
            put_str(last_undet == -2 ? "   (first sample)\n" : "   <<< CHANGED\n");
            flush_line();
            last_undet = undet;
            last_ro    = robit;
        }
        delay_ms(1000u / (uint32_t)BATON_MON_HZ);
        if ((i % (uint32_t)BATON_MON_HZ) == 0u) wdt_kick();
    }
    wdt_disarm();

    put_str("bat: --- done ---\n");
    put_str("bat:   CHR_CON7 bit 12 seen: ");
    put_str(seen_lo ? "0 " : "");
    put_str(seen_hi ? "1 " : "");
    put_str((seen_lo && seen_hi) ? " BOTH WAYS -- THE PIN TRACKS THE HOLDER.\n"
                                 : " one value only.\n");
    put_str("bat:   0142 bit 5 seen: ");
    put_str(ro_lo ? "0 " : "");
    put_str(ro_hi ? "1 " : "");
    put_str((ro_lo && ro_hi) ? " BOTH WAYS.\n" : " one value only.\n");
    if ((seen_lo && seen_hi) || (ro_lo && ro_hi)) {
        put_str("bat: THAT IS THE DETECTOR. It is a register read, it is live, it\n");
        put_str("bat: costs no reboot, and it is what the part was built to do.\n");
        put_str("bat: IT HAS NEVER PRINTED THIS ON THIS BOARD. If it does, that is a\n");
        put_str("bat: new fact and the driver should get a presence property back --\n");
        put_str("bat: it has none today precisely because this line never appeared.\n");
    } else {
        put_str("bat: The bit did not move. That is only meaningful IF THE CONNECTOR\n");
        put_str("bat: WAS CHANGED during the window -- if it was not, this run says\n");
        put_str("bat: nothing at all and is worth repeating. If it was, note WHICH\n");
        put_str("bat: value is stuck: a detect input that is unpopulated reads a\n");
        put_str("bat: stable level and is indistinguishable from a working one.\n");
    }
    flush_line();
    /* NOTHING IS FED BACK. This used to call into the driver's own BATON sampler so
     * that its seen-mask agreed with what just printed. The driver no longer keeps
     * one -- across every connector change tried, on this board, the mask never
     * reached 3 -- so this command is now the whole of the experiment and its
     * output is the whole of the result. */
}

/*
 * ── bat track ── DOES THE RAIL FOLLOW THE SETPOINT?
 *
 * Every instrument before this one asked the charger's STATUS BITS what state the
 * board is in, and all five died the same death: those bits chatter faster than
 * they can be sampled, so a train is an aliased square wave in either battery
 * state. This asks a different question, and it is the one the topology actually
 * answers.
 *
 * VBAT IS VSYS. With nothing in the holder, the charger is the only thing on the
 * node, so VSYS *is* whatever CHR_CON3 is regulating to -- move the setpoint and
 * the rail has to move with it. With a cell fitted the node is the cell, sitting
 * at its own voltage above every one of these setpoints, and moving the target
 * does nothing at all because a charger cannot sink. So:
 *
 *      no cell   -> the reading TRACKS the ladder, ~50 mV across rungs 0..4
 *      cell in   -> the reading is FLAT, because the cell is holding the node
 *
 * IT IS THE SLOPE THAT ANSWERS, NOT THE LEVEL, and that is why this survives
 * where "read VBAT and compare" did not. The old conclusion -- "cell in and cell
 * out read within 2 mV on every channel" -- compared ABSOLUTE readings, which are
 * hostage to the divider, the full-scale constant and any offset in the
 * converter. A differential measurement against a stimulus we control is immune
 * to all three: an ADC with the wrong gain still moves when the rail moves.
 *
 * NOTHING HERE CAN POWER THE BOARD OFF. It uses rungs 0..4 only -- codes 0, 31,
 * 30, 29, 28, i.e. 4200 down to 4150 mV -- which `bat probe' has walked in both
 * battery states many times over. Rung 5 (4137 mV), the fatal one, is never
 * written.
 *
 * AND IT CARRIES ITS OWN CONTROL, because that is the lesson the cs and cv trains
 * cost. A control pass sits at the resting setpoint and takes the same number of
 * samples with the same timing, so the ladder's span is printed next to the span
 * of a ladder that was never climbed. A slope smaller than the control's spread
 * is not a slope.
 *
 * If the ladder is flat in BOTH states, that is not a failure of this test -- it
 * is proof the converter is not converting, which would invalidate every voltage
 * this driver has ever printed. Either outcome is worth more than another
 * status-bit train.
 */
enum { BATTRACK_RUNGS = 5u, BATTRACK_SETTLE_MS = 100u, BATTRACK_REPS = 3u };

static int battrack_raw(void) {
    int b = 0, v = 0, d = 0, ma = 0;
    if (mt6592_pmic_sense_pair(&b, &v, &d, &ma) != 0) return -1;
    return b;
}

/* raw counts -> mV, the same scale the driver uses: raw / 32768 * 7200. */
static int32_t battrack_mv(int raw) {
    if (raw < 0) return -1;
    return (int32_t)(((uint64_t)(uint32_t)raw * 7200u) >> 15);
}

static void batcal_track(uint32_t reps) {
    int      lad[BATTRACK_RUNGS];
    int      ctl[BATTRACK_RUNGS];
    int      saved;
    uint32_t rep, i;
    int32_t  lad_span_sum = 0, ctl_span_sum = 0;

    if (reps == 0u) reps = (uint32_t)BATTRACK_REPS;
    if (reps > 8u) reps = 8u;

    saved = mt6592_pmic_charger_cv_code();
    if (saved < 0) {
        put_str("bat: cannot read CHR_CON3; pwrap is down\n");
        return;
    }

    put_str("bat: stepping the CV setpoint down rungs 0..4 (4200 -> 4150 mV) and\n");
    put_str("bat: reading BATSNS at each. VBAT IS VSYS: with no cell the charger is\n");
    put_str("bat: the only thing on the node, so the rail must follow the setpoint.\n");
    put_str("bat: With a cell fitted the node is the cell, above every rung, and a\n");
    put_str("bat: charger cannot sink -- so it cannot move. THE SLOPE IS THE ANSWER.\n");
    put_str("bat: Rung 5 (4137 mV, the fatal one) is never written. This cannot\n");
    put_str("bat: power the board off; `bat probe' walks these same rungs routinely.\n");
    flush_line();
    wdt_arm((uint32_t)BATCAL_WDT_SECS);

    for (rep = 0u; rep < reps; ++rep) {
        int32_t lad_span, ctl_span;

        /* THE LADDER. */
        for (i = 0u; i < (uint32_t)BATTRACK_RUNGS; ++i) {
            const uint32_t code = (uint32_t)g_batcal_cv_steps[i].code;
            if (mt6592_pmic_charger_cv_set(code) != 0) {
                (void)mt6592_pmic_charger_cv_set((uint32_t)saved);
                wdt_disarm();
                put_str("bat: a rung did not land in CHR_CON3. INCONCLUSIVE -- an ignored\n");
                put_str("bat: write is not a stimulus, so the reading measures nothing.\n");
                return;
            }
            delay_ms((uint32_t)BATTRACK_SETTLE_MS);
            lad[i] = battrack_raw();
            wdt_kick();
        }
        (void)mt6592_pmic_charger_cv_set((uint32_t)saved);
        delay_ms((uint32_t)BATTRACK_SETTLE_MS);

        /* THE CONTROL: same count, same timing, setpoint never moved. */
        for (i = 0u; i < (uint32_t)BATTRACK_RUNGS; ++i) {
            delay_ms((uint32_t)BATTRACK_SETTLE_MS);
            ctl[i] = battrack_raw();
            wdt_kick();
        }

        put_str("bat: pass ");
        put_dec((int32_t)(rep + 1u));
        put_char('/');
        put_dec((int32_t)reps);
        put_str("\n");
        put_str("bat:   ladder ");
        for (i = 0u; i < (uint32_t)BATTRACK_RUNGS; ++i) {
            kv_dec(" ", (int32_t)g_batcal_cv_steps[i].mv);
            put_str("mV=");
            if (lad[i] < 0) put_str("ERR"); else put_dec((int32_t)lad[i]);
        }
        put_str("\n");
        put_str("bat:   control");
        for (i = 0u; i < (uint32_t)BATTRACK_RUNGS; ++i) {
            put_str("  ");
            if (ctl[i] < 0) put_str("ERR"); else put_dec((int32_t)ctl[i]);
        }
        put_str("\n");

        lad_span = (lad[0] >= 0 && lad[BATTRACK_RUNGS - 1u] >= 0)
                       ? (int32_t)(lad[0] - lad[BATTRACK_RUNGS - 1u]) : 0;
        ctl_span = (ctl[0] >= 0 && ctl[BATTRACK_RUNGS - 1u] >= 0)
                       ? (int32_t)(ctl[0] - ctl[BATTRACK_RUNGS - 1u]) : 0;
        kv_dec("bat:   first-to-last  ladder ", lad_span);
        kv_dec(" counts (", battrack_mv(lad[0]) - battrack_mv(lad[BATTRACK_RUNGS - 1u]));
        kv_dec(" mV)   control ", ctl_span);
        put_str(" counts\n");
        flush_line();
        lad_span_sum += lad_span;
        ctl_span_sum += ctl_span;
    }

    (void)mt6592_pmic_charger_cv_set((uint32_t)saved);
    wdt_disarm();
    /* Sweep over: give CHR_CON3 back so the service re-asserts the 4200 mV target. */
    mt6592_pmic_charger_cv_release();

    put_str("bat: --- totals ---\n");
    kv_dec("bat:   ladder span summed over passes ", lad_span_sum);
    kv_dec(" counts, control ", ctl_span_sum);
    put_str(" counts\n");
    put_str("bat: 50 mV of ladder is about 227 counts at this scale (raw/32768*7200),\n");
    put_str("bat: so a rail that follows the setpoint cannot be subtle. If the ladder\n");
    put_str("bat: span is not far outside the control span, THE RAIL IS NOT FOLLOWING:\n");
    put_str("bat: either something is holding the node -- a cell -- or the converter is\n");
    put_str("bat: not converting, and running this pack IN separates those two.\n");
    put_str("bat: Run it in both states. Flat in BOTH means the ADC is the fault, and\n");
    put_str("bat: that would retire `cell in and cell out read within 2 mV' as a\n");
    put_str("bat: statement about the converter rather than about the battery.\n");
    flush_line();
}

/*
 * ── bat auto ── THE BOOT POLICY, AND THE ANSWER TO "NO REBOOT WHEN THE CELL IS
 * UNFITTED".
 *
 * The honest position after six instruments: this board has no working battery
 * presence sensor. BATON is unpopulated and reads a stuck 0. The CHR_CON2 status
 * bits chatter faster than they can be sampled and are identical in both states.
 * BATSNS does not track VSYS -- it did not move for a 50 mV setpoint step or for
 * a backlight-plus-CPU load step, in either state. The ONLY thing that has ever
 * separated cell-in from cell-out on this hardware is whether the board is still
 * executing after the charger is told to let go of a node it is already above.
 *
 * That test cannot be made non-destructive, because with nothing in the holder
 * the result IS the power loss -- there is no code left running to report it. So
 * stop trying to pay less than one reboot, and make sure one is all it ever
 * costs:
 *
 *   verdict on the card  -> apply it, touch nothing, boot.
 *   no verdict           -> probe once. Cell in, it survives and writes 'P'.
 *                           Cell out, the board dies mid-probe with the
 *                           breadcrumb ARMED, and THE NEXT BOOT reads that,
 *                           writes 'A', and never probes again.
 *
 * One reboot per battery-state change, and none at all on any boot after it.
 * `bat forget' is the other half: it clears the verdict, and it is what has to be
 * run when the pack is swapped, because a stale answer here is worse than none.
 * That is a real limitation and it is printed, not hidden -- a board that cannot
 * sense its battery cannot notice the swap either.
 *
 * This is the function the LK boot path should call. It is a command as well so
 * the policy can be exercised without reflashing to test it.
 */
static void batcal_auto(int quiet) {
    if (g_batcal_verdict == (uint8_t)'P' || g_batcal_verdict == (uint8_t)'A') {
        const int fitted = (g_batcal_verdict == (uint8_t)'P');
        if (!quiet) {
            put_str("bat: the card already holds the answer: ");
            put_str(fitted ? "CELL FITTED.\n" : "NO CELL FITTED.\n");
            put_str("bat: AND IT GOES NO FURTHER THAN THIS SCREEN. The driver has no\n");
            put_str("bat: presence property to apply it to, so nothing downstream --\n");
            put_str("bat: not the LED, not the park, not the ATAGs -- changes behaviour\n");
            put_str("bat: because of this line. It is an operator's note, on a card.\n");
            put_str("bat: NO RUNG, NO RISK, NO REBOOT -- and none on any boot after\n");
            put_str("bat: this one either. The probe was spent once and that is all\n");
            put_str("bat: it will ever cost, until `bat forget'.\n");
            put_str("bat: SWAP THE PACK AND THIS GOES STALE. Nothing on this board can\n");
            put_str("bat: sense the swap -- that is why the verdict is sticky in the\n");
            put_str("bat: first place. Run `bat forget' when the cell changes.\n");
            flush_line();
        }
        return;
    }

    if (!quiet) {
        put_str("bat: no verdict on the card, so the probe gets taken ONCE.\n");
        put_str("bat: If a cell is fitted this survives and the answer is written.\n");
        put_str("bat: IF THE HOLDER IS EMPTY THE BOARD WILL GO DOWN HERE -- that is\n");
        put_str("bat: the measurement, not a fault. Power back in and run `bat auto'\n");
        put_str("bat: again: it will read the breadcrumb, record NO CELL, and never\n");
        put_str("bat: probe again.\n");
        flush_line();
    }
    batcal_probe((uint32_t)BATCAL_SETTLE_MS);
}

static void batcal_forget(void) {
    g_batcal_verdict = 0u;
    /* THE RAM HALF OF THIS IS GONE, because what it cleared no longer exists. It
     * used to reset the driver's session peak on the vbat presence window -- the
     * thing that made a pack once seen above the port's baseline stay CELL FITTED
     * for the rest of the session, and the one piece of state a pack swap silently
     * invalidated. The window was retired (a full pack and an empty holder read
     * within one count of each other), so the only stale answer left to clear is
     * the letter on the card. */
    if (mt6592_sd_read((uint64_t)BATCAL_CELL_LBA, 1u, g_batcal_cell_sector) != MT6592_MSDC_OK ||
        !batcal_cell_has_magic(g_batcal_cell_sector)) {
        put_str("bat: nothing of ours on the card to clear; the verdict is already\n");
        put_str("bat: unset in RAM, so the next `bat auto' will probe.\n");
        return;
    }
    g_batcal_cell_sector[BATCAL_CELL_VERDICT] = 0u;
    if (mt6592_sd_write((uint64_t)BATCAL_CELL_LBA, 1u, g_batcal_cell_sector) != MT6592_MSDC_OK) {
        put_str("bat: the card refused the write; the verdict may still be on it.\n");
        return;
    }
    put_str("bat: verdict cleared. The next `bat auto' spends one probe to\n");
    put_str("bat: re-establish it. Do this every time the pack changes.\n");
}

/*
 * ════════════════════════════════════════════════════════════════════════════
 *  THE VOLTAGE WINDOW IS GONE, AND THIS IS WHERE IT WAS
 * ════════════════════════════════════════════════════════════════════════════
 *
 * Seven hundred lines used to live here: a calibrated baseline for the port's own
 * voltage, a separation threshold, a peak latch, `bat base', `bat delta', a
 * five-verdict decoder, and `bat cal' as a timed charger-off drain. The premise
 * was that VBAT is VSYS with no power-path FET, so with a cable in there is one
 * node and two things that can hold it -- the port at one very repeatable
 * voltage, or a cell, which cannot sit exactly there.
 *
 * A CELL CAN SIT EXACTLY THERE. Measured on this board: a full pack reads 4177 mV
 * and an EMPTY HOLDER reads 4178 mV through the same converter. One millivolt is
 * four counts of separation between the two states the instrument existed to
 * separate, which is inside its own sample-to-sample spread -- and the same
 * transcript has a fitted pack reading the baseline on the first two windows of a
 * session and 29 counts above it a minute later. So the baseline was not a
 * property of the port, the peak latch was what made a wrong answer sticky, and
 * the whole apparatus reported NO CELL with a pack in the holder.
 *
 * That was the last candidate. BATON never moved, the CHR_CON2 status bits are
 * identical in both states, the CS_DET duty spread inside one state (500, 484,
 * 421 per mille in three consecutive reads) is wider than the difference between
 * states, and the only test that ever worked -- cut the charger and see whether
 * the board is still executing -- costs a reboot and cannot run on a boot path.
 *
 * PRESENCE IS UNDECIDABLE ON THIS BOARD. mt6592_pmic.h therefore has no presence
 * property and no window API, minos_platform_battery_present() returns -1, and
 * the renderer treats -1 as "assume fitted". Deleting the instrument is the
 * finding, not a regression: an instrument that cannot separate its two states
 * does not become useful by being kept in the menu, it becomes a trap for the
 * next person who runs it and believes it.
 *
 * What survives above is everything that measures something real -- the CV
 * ladder, the breadcrumb, `bat probe', `bat track', `bat baton', `bat scan'.
 */


/*
 * ── THE BOOT HALF OF THE POLICY, AND IT NO LONGER HAS A VERDICT TO CARRY ──
 *
 * mvii_lk_main.c calls this before the charge park. It used to establish battery
 * presence so that everything downstream -- the park's own branch, the LED, the
 * ATAGs -- read a measurement instead of a boot default, first by applying a
 * verdict some earlier reboot had paid for, later by taking three voltage windows
 * out of the ADC for free.
 *
 * BOTH ARE GONE, AND THE FUNCTION STAYS. Presence is undecidable on this board
 * (full pack 4177 mV, empty holder 4178 mV, no power-path FET, BATON never moved,
 * CS_DET duty spread wider inside one state than between states), so there is no
 * presence property in mt6592_pmic.h to set and nothing downstream that branches
 * on one -- minos_platform_battery_present() answers -1 and the renderer treats
 * that as "assume fitted". A boot path that kept computing a verdict nobody reads
 * would only be a slower boot with a more confident log.
 *
 * WHAT IT STILL DOES, and why it is still called: read the breadcrumb. That is
 * where an operator's `bat cal'/`bat probe' result arrives -- a run that died with
 * the holder empty left the sector ARMED, and reading it here is what converts
 * that into a sticky letter and clears the switch, so the boot after an
 * empty-holder probe is the last one that costs anything. It writes no PMIC
 * register, touches no rail, and its answer goes to the log and to the card.
 *
 * Returns 1 cell fitted, 0 absent, -1 nothing known -- and it is now -1 on every
 * board that has never been probed by hand, which is the honest answer.
 */
int mvii_battery_boot_policy(void) {
    /* The card first and last. batcal_breadcrumb() prints what it finds; consume,
     * because the dead-man's switch is only meaningful if it is cleared once
     * read, and a boot is exactly the reader it was armed for. */
    (void)batcal_breadcrumb(1);

    if (g_batcal_verdict == (uint8_t)'P' || g_batcal_verdict == (uint8_t)'A') {
        const int stored = (g_batcal_verdict == (uint8_t)'P');
        put_str(stored ? "lk: battery verdict on card: CELL FITTED"
                       : "lk: battery verdict on card: NO CELL FITTED");
        put_str(" (an operator's probe, replayed;\n");
        put_str("lk: nothing was written and nothing downstream reads it)\n");
        flush_line();
        return stored;
    }

    put_str("lk: battery presence unknown, and it stays unknown: this board has no\n");
    put_str("lk: sensor that separates a fitted pack from an empty holder, so boot\n");
    put_str("lk: assumes one is fitted rather than probing for an answer.\n");
    flush_line();
    return -1;
}

static void batcal_point_show(void) {
    if (!g_batcal_point_valid) {
        put_str("bat: no calibrated point. Pack OUT, `bat cal', power cycle,\n");
        put_str("bat: then `bat' reads back the write that stopped the board.\n");
        return;
    }
    kv_hex("bat: calibrated point PMIC ", g_batcal_point_reg, 4u);
    kv_hex(" = ", g_batcal_point_val, 4u);
    kv_dec("  (rung ", (int32_t)g_batcal_point_step);
    put_char(')');
    if (g_batcal_point_reg == (uint32_t)BATCAL_CV_REG) {
        kv_dec("  CHR_CON3 CV code ", (int32_t)(g_batcal_point_val & (uint32_t)BATCAL_CV_MASK));
        kv_dec(" = ", (int32_t)g_batcal_cv_mv[g_batcal_point_val & (uint32_t)BATCAL_CV_MASK]);
        put_str(" mV");
    }
    put_str("\nbat: `bat probe' tests it. It survives a reset on the card.\n");
}




/*
 * ── bat ── ONE COMMAND FOR THE BATTERY, AND FIVE THINGS IT CAN DO.
 *
 * This used to be nine top-level commands and fourteen batcal subcommands, and
 * most of them had been measured into the ground: the AUXADC sag ladder (the
 * converter needs 992 us and the window is 130), the register diff (no bit
 * latches a cut), the edge-rate hunt (227 and 669 per thousand in the same
 * state), the CS_DET duty monitor, the load sweeps, the live loop (killed a
 * cell-fitted board twice in three runs). Keeping a dead instrument in the menu
 * is worse than not having written it: somebody runs it and believes it.
 *
 * The voltage window went the same way and took `bat base', `bat delta' and its
 * charger-off drain with it -- a full pack and an empty holder read one millivolt
 * apart, which is four counts, which is inside the converter's own spread. So
 * `bat cal' means the CV descent again, which is what it meant first.
 *
 * What is left either measures something real or costs a power cycle honestly:
 *
 *   bat            what the charger is doing, plus whatever the card is holding
 *   bat cal [up]   find the write that stops the board  (costs one power cycle)
 *   bat probe      go to that write and see if the board survives it
 *   bat auto       apply the card's verdict, or spend one probe to get one
 *   bat forget     clear the card's verdict (do this when the pack changes)
 *   bat track      does the rail follow the CV setpoint?
 *   bat baton      watch CHR_CON7 bit 12 while the connector is changed
 *   bat rung       train the sag ladder
 *   bat point      show or set that write by hand
 *   bat scan       walk the AUXADC channels
 */
static void cmd_bat(const char* args) {
    const char* rest;

    args = skip_spaces(args);

    if (*args == '\0') {
        /* Status, then the card. The answer always arrives across a power cycle --
         * `cal' and `probe' both end with the board off when they have something to
         * say -- so "power cycle, then type bat" has to be the shortest thing there
         * is. Writes no register.
         *
         * NOTHING IS MEASURED BEFORE THIS ANY MORE. A window decision used to run
         * first so that the status line's `present=' field was fresh rather than
         * whatever the previous command had left in the driver. There is no
         * `present=' field and no driver state to stale, so the ordering problem it
         * solved cannot recur -- and bat_status() takes its own blocking sample. */
        bat_status();
        put_char('\n');
        (void)batcal_breadcrumb(1);
        batcal_last_show();
        batcal_point_show();
        return;
    }
    if ((rest = match_word(args, "cal")) != 0) {
        /* `bat cal' is the destructive CV descent again, which is what it was
         * before the window borrowed the name for a charger-off drain. RUN IT WITH
         * THE PACK OUT: it is meant to die, and the rung it dies on is the result.
         * `ladder' is still accepted so the old spelling keeps working. */
        const char* p = match_word(rest, "ladder");
        batcal_find(match_word(p != 0 ? p : rest, "up") != 0);
        return;
    }
    if ((rest = match_word(args, "probe")) != 0) {
        batcal_probe(parse_u32_default(&rest, (uint32_t)BATCAL_SETTLE_MS));
        return;
    }
    if ((rest = match_word(args, "auto")) != 0) {
        (void)rest;
        batcal_auto(0);
        return;
    }
    if ((rest = match_word(args, "forget")) != 0) {
        (void)rest;
        batcal_forget();
        return;
    }
    if ((rest = match_word(args, "track")) != 0) {
        batcal_track(parse_u32_default(&rest, (uint32_t)BATTRACK_REPS));
        return;
    }
    if ((rest = match_word(args, "baton")) != 0) {
        batcal_baton(parse_u32_default(&rest, (uint32_t)BATON_MON_DEFAULT_SECS));
        return;
    }
    if ((rest = match_word(args, "rung")) != 0) {
        const uint32_t n = parse_u32_default(&rest, (uint32_t)BATRUNG_DEFAULT);
        batcal_rung(n, parse_u32_default(&rest, (uint32_t)BATRUNG_TRIALS));
        return;
    }
    if ((rest = match_word(args, "point")) != 0) {
        const char* p = skip_spaces(rest);
        if (*p != '\0') {
            const uint32_t reg = parse_u32_default(&p, 0xffffffffu);
            const uint32_t val = parse_u32_default(&p, 0xffffffffu);
            if (reg > 0x07feu || val > 0xffffu) {
                put_str("usage: bat point [reg] [val]   (reg 0000..07fe, val 0000..ffff)\n");
                return;
            }
            g_batcal_point_reg   = reg;
            g_batcal_point_val   = val;
            g_batcal_point_step  = 0u;
            g_batcal_point_valid = 1;
            (void)batcal_cell_mark((uint8_t)BATCAL_MARK_CLEARED, reg, val);
            put_str("bat: point set by hand and written to the card.\n");
        }
        batcal_point_show();
        return;
    }
    if (match_word(args, "scan") != 0) {
        cmd_batscan();
        return;
    }

    put_str("usage: bat            charger state, mV, percent, raw regs, and what the\n");
    put_str("                      card is holding. Writes nothing. Type this after a\n");
    put_str("                      power cycle -- it is where the answer comes back.\n");
    put_str("       bat cal [up]   THE CALIBRATION: find WHICH WRITE STOPS THE BOARD.\n");
    put_str("                      Ladders the CV setpoint down and ends at stock's own\n");
    put_str("                      charger-off, putting each rung ON THE CARD BEFORE\n");
    put_str("                      writing it -- so the step that kills the board is on\n");
    put_str("                      the medium when the board stops. RUN IT WITH THE PACK\n");
    put_str("                      OUT; it is meant to die. Power cycle, then `bat'.\n");
    put_str("       bat probe [#ms]  that point, once. Surviving is CELL FITTED; dying\n");
    put_str("                      leaves NO CELL FITTED on the card with the register\n");
    put_str("                      and the value on it.\n");
    put_str("       bat auto       apply the card's verdict, or spend one probe for one\n");
    put_str("       bat forget     clear it -- REQUIRED when the pack is swapped\n");
    put_str("       bat track [#]  step the CV setpoint and see if the rail follows\n");
    put_str("       bat baton [#s] watch CHR_CON7 bit 12; change the connector as it runs\n");
    put_str("       bat rung [n] [trials]  train the sag ladder\n");
    put_str("       bat point [reg] [val]  show or set the point by hand\n");
    put_str("       bat scan       walk every AUXADC channel with the muxes open\n");
    put_str("  NOTE none of these publishes anything: presence is undecidable on this\n");
    put_str("  board and the driver has no property for it. Verdicts go on the card.\n");
}

/*
 * THE PREVIOUS IMAGE'S LAST WORDS, read back from eMMC without leaving this console.
 *
 * The OS dies during boot, so `boot` proves nothing and the only evidence is
 * whatever the dying image managed to write down. It writes to two places:
 *
 *   the status RECORD -- one sector: a stage code and a rolling message
 *   the console RING  -- 64 KiB: the lines an image printed, from byte 0 forward
 *
 * Both are shared with this LK, and the ring is NOT the protected place an earlier
 * version of this comment claimed. That claim rested on console_ring_putc() being
 * an empty stub outside MVII_ARMV7_FULL_OS, which is true, plus MVIILK.elf not
 * being built with it, which is false: the LK target compiles mt6592_bootstatus.c
 * with -DMVII_ARMV7_FULL_OS=1 like everything else. LK writes this ring, and a
 * post-crash capture showing 424 bytes of pure `lk:` output was the proof.
 *
 * What the images do NOT share is the write position. g_ring_pos is a plain static,
 * so it starts at 0 in every image, and each one writes from byte 0 -- it does not
 * append. So after booting into this console the ring holds:
 *
 *   [0, write_pos)          this LK, this boot: its own boot lines plus every line
 *                           this console has printed since
 *   [write_pos, residue)    the previous image, in order, its head eaten by the above
 *   [residue, data_size)    never written -- zeros
 *
 * Which inverts the usual reading. write_pos is not "how much log there is", it is
 * how much of the previous image's log has already been destroyed, and the part
 * worth reading is the part PAST it. That is also why the interesting end survives:
 * LK overwrites from the front, so a crashing OS's final lines are the safest bytes
 * in the slot. And it is why this command stops mirroring its own output first --
 * printing 16 KiB with the mirror live would consume from the front exactly what it
 * is trying to show.
 *
 * The record is worthless from here for the same shared-writer reason, minus the
 * saving grace: mvii_lk_main flushes its own milestone the instant storage comes up
 * and flush_line() mirrors every console line into it, so by the time an operator
 * can type, the OS's record is not stale -- it is gone, with nothing left over.
 *
 * Procedure: let it crash, power into debug mode, run `bootlog` FIRST -- every other
 * command typed before it eats further into the residue. The tail is the last thing
 * the OS said, including the "ARM trap kind=.. pc=.. dfsr=.." line the OS entry writes
 * from its trap handler, which names the faulting address outright. No BROM round
 * trip, no reflash, no other machine.
 */
enum {
    BOOTLOG_DEFAULT_BYTES = 0x2000u, /* 8 KiB of tail: a few hundred lines */
    BOOTLOG_CHUNK = 512u,
    BOOTLOG_WDT_SECS = 0x3cu,
    /* A 512-byte block of log text holds far more than this. The threshold only has
     * to separate "text" from "the zeros a freshly flashed slot is padded with". */
    BOOTLOG_MIN_TEXT = 16u,
};

static uint8_t g_bootlog_chunk[BOOTLOG_CHUNK];

static uint32_t bootlog_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int bootlog_read(uint64_t off, uint8_t* dst) {
    int rc = mt6592_emmc_read_user(off, dst, BOOTLOG_CHUNK);
    if (rc == MT6592_MSDC_OK) return rc;
    /* One retry behind a fresh init: the console can be reached before anything
     * else in this image has had a reason to touch MSDC. */
    (void)mt6592_emmc_user_init();
    return mt6592_emmc_read_user(off, dst, BOOTLOG_CHUNK);
}

static uint32_t bootlog_block_text_bytes(const uint8_t* p) {
    uint32_t i;
    uint32_t n = 0u;
    for (i = 0u; i < BOOTLOG_CHUNK; ++i) {
        if (p[i] == (uint8_t)'\n' || (p[i] >= 0x20u && p[i] <= 0x7eu)) ++n;
    }
    return n;
}

/* Print n bytes as text. Newlines pass through; anything else unprintable becomes a
 * dot rather than being dropped, so a corrupted stretch still occupies its width. */
static void bootlog_emit(const uint8_t* p, uint32_t n) {
    uint32_t i;
    for (i = 0u; i < n; ++i) {
        const char c = (char)p[i];
        if (c == '\n') put_char('\n');
        else put_char((c >= 0x20 && c <= 0x7e) ? c : '.');
    }
}

static void cmd_bootlog(const char* args) {
    uint32_t want = parse_u32_default(&args, BOOTLOG_DEFAULT_BYTES);
    uint32_t magic;
    uint32_t version;
    uint32_t data_size;
    uint32_t write_pos;
    uint32_t residue_end;
    uint32_t start;
    uint32_t pos;
    int rc;

    wdt_arm(BOOTLOG_WDT_SECS);

    rc = bootlog_read((uint64_t)MT6592_CONSOLE_RING_OFFSET, g_bootlog_chunk);
    if (rc != MT6592_MSDC_OK) {
        kv_dec("bootlog: eMMC read failed, rc=", (int32_t)rc);
        put_str(" -- MSDC would not come up, so there is nothing to read\n");
        flush_line();
        wdt_disarm();
        return;
    }

    magic = bootlog_u32(&g_bootlog_chunk[0]);
    version = bootlog_u32(&g_bootlog_chunk[4]);
    data_size = bootlog_u32(&g_bootlog_chunk[8]);
    write_pos = bootlog_u32(&g_bootlog_chunk[12]);

    if (magic != MT6592_CONSOLE_RING_MAGIC) {
        kv_hex("bootlog: no ring at ", (uint32_t)MT6592_CONSOLE_RING_OFFSET, 8u);
        kv_hex(", magic=", magic, 8u);
        put_str("\nbootlog: nothing has ever committed a log line to this slot -- not even\n");
        put_str("bootlog: this LK, which normally writes its header as storage comes up.\n");
        flush_line();
        wdt_disarm();
        return;
    }
    if (data_size == 0u || data_size > (uint32_t)MT6592_CONSOLE_RING_DATA_SIZE ||
        (data_size % BOOTLOG_CHUNK) != 0u) {
        kv_dec("bootlog: header claims data_size=", (int32_t)data_size);
        put_str(" which this slot cannot hold -- refusing to read it\n");
        flush_line();
        wdt_disarm();
        return;
    }

    /* Stop recording before anything else. Everything below is output, and with the
     * mirror live that output is written back over the residue being read. */
    mt6592_bootstatus_console_set_mirror(0);

    /* Where the previous image's text ends: scan forward from the block write_pos
     * lands in until a block carries no text. A freshly flashed slot is zero-padded,
     * so that terminator is reliable -- unless the previous image lapped the whole
     * 63 KiB, in which case the scan runs to the end and the oldest bytes are out of
     * order. Said plainly below rather than guessed at. */
    residue_end = write_pos - (write_pos % BOOTLOG_CHUNK);
    for (pos = residue_end; pos < data_size; pos += BOOTLOG_CHUNK) {
        wdt_kick();
        if (bootlog_read((uint64_t)MT6592_CONSOLE_RING_DATA_OFFSET + (uint64_t)pos,
                         g_bootlog_chunk) != MT6592_MSDC_OK) {
            break;
        }
        if (bootlog_block_text_bytes(g_bootlog_chunk) < BOOTLOG_MIN_TEXT) break;
        residue_end = pos + BOOTLOG_CHUNK;
    }

    kv_dec("bootlog: ring v", (int32_t)version);
    kv_dec(", this image has overwritten the first ", (int32_t)write_pos);
    put_str(" bytes\n");

    if (residue_end <= write_pos) {
        put_str("bootlog: nothing survives past that -- the previous image logged less\n");
        put_str("bootlog: than this LK has already printed, or never logged at all. Either\n");
        put_str("bootlog: way its words are gone; there is nothing here to read.\n");
        mt6592_bootstatus_console_set_mirror(1);
        flush_line();
        wdt_disarm();
        return;
    }

    if (want == 0u || want > residue_end - write_pos) want = residue_end - write_pos;
    start = residue_end - want;
    if (start < write_pos) start = write_pos;

    kv_dec("bootlog: previous image left ", (int32_t)(residue_end - write_pos));
    kv_dec(" readable bytes, showing last ", (int32_t)(residue_end - start));
    put_char('\n');
    if (residue_end >= data_size) {
        put_str("bootlog: residue runs to the end of the slot -- the writer lapped, so the\n");
        put_str("bootlog: earliest lines below may be out of order. The tail is still sound.\n");
    }
    put_str("bootlog: ---------------- begin ----------------\n");
    flush_line();

    /* Our own output also goes to the status record, which autoflushes per line.
     * Left on, dumping this would fire hundreds of eMMC writes and take minutes. */
    mt6592_bootstatus_console_set_autoflush(0);

    for (pos = start - (start % BOOTLOG_CHUNK); pos < residue_end; pos += BOOTLOG_CHUNK) {
        uint32_t skip = (pos < start) ? (start - pos) : 0u;
        uint32_t n = residue_end - pos;
        if (n > BOOTLOG_CHUNK) n = BOOTLOG_CHUNK;
        wdt_kick();
        if (bootlog_read((uint64_t)MT6592_CONSOLE_RING_DATA_OFFSET + (uint64_t)pos,
                         g_bootlog_chunk) != MT6592_MSDC_OK) {
            put_str("\nbootlog: read failed mid-ring; stopping here\n");
            break;
        }
        bootlog_emit(&g_bootlog_chunk[skip], n - skip);
        mt6592_usb_gadget_poll();
    }

    mt6592_bootstatus_console_set_autoflush(1);
    put_str("\nbootlog: ----------------- end -----------------\n");
    flush_line();
    /* Recording back on: leaving it off would silently stop logging this boot. The
     * bytes just shown are now on the clock again -- run bootlog before anything
     * else next time. */
    mt6592_bootstatus_console_set_mirror(1);
    wdt_disarm();
}

static void mtk_reboot_now(void) {
    mtk_write32(0x10007008u, 0x1971u);
    mtk_write32(0x10007000u, 0x22000014u);
    mtk_write32(0x10007014u, 0x1209u);
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/*
 * Ask the BROM to stay in USB download mode across the next reset.
 *
 * Same three registers the BROM payload uses in flash_stage.c
 * (mt6592_set_brom_usbdl_flag): 0x10002050 is the misc write lock, opened with
 * 0xad98; bit 0 of 0x10002058 keeps the download path armed through the reset;
 * 0x10002030 takes the "DL" magic 0x444cfffd that the BROM reads on its way up.
 *
 * This is the one boot mode that cannot go through the eMMC flag: the flag is
 * read by our LK, and LK does not run at all in the mode being asked for.
 */
static void mtk_arm_brom_usbdl(void) {
    mtk_write32(0x10002050u, 0xad98u);
    mtk_write32(0x10002058u, mtk_read32(0x10002058u) | 1u);
    mtk_write32(0x10002050u, 0u);
    mtk_write32(0x10002030u, 0x444cfffdu);
}

/* Shut the gadget down cleanly before a reset, so the host sees a detach rather
 * than a stall. Shared by `reboot` and by `flag brom`, which resets too. */
static void console_reset_now(void) {
    flush_line();
    delay_ms(50u);
    mvii_lk_set_log_sink(0);
    mt6592_uart_set_tap(0);
    mt6592_usb_gadget_shutdown();
    mtk_reboot_now();
}

/*
 * `flag [debug|boot|brom]` -- choose what the NEXT boot does.
 *
 * Three modes, and they do not share a mechanism, because they cannot:
 *
 *   debug  eMMC one-shot flag, mode CONSOLE. LK reads it and serves this
 *          console instead of booting the kernel. Cleared by the read, so a
 *          console that wedges costs one power cycle, not a brick.
 *   boot   the same sector with mode NORMAL, which overwrites any armed flag.
 *          Written rather than erased so the sector is always valid and
 *          `-mtk-read-boot-status` can tell "asked for a normal boot" apart
 *          from "the write never landed".
 *   brom   NOT the eMMC flag -- the flag is read by LK, and LK is exactly what
 *          this mode skips. It is the BROM's own USBDL latch, set here and
 *          taken by the BROM on its way up after the reset this issues.
 *
 * `debug` and `boot` only arm; they do not reset. `brom` has to reset, because
 * the latch it sets is consumed by the reset itself and leaving it armed while
 * the board keeps running would send the *next* unrelated reboot into BROM.
 *
 * No argument still means `debug` -- that is what this command has always done,
 * and it is the one worth typing blind.
 */
static void cmd_flag(const char* args) {
    const char* p = skip_spaces(args);
    uint32_t mode;
    int rc;

    if (*p == 0 || match_word(p, "debug") != 0) {
        mode = MT6592_DBG_MODE_CONSOLE;
    } else if (match_word(p, "boot") != 0) {
        mode = MT6592_DBG_MODE_NORMAL;
    } else if (match_word(p, "brom") != 0) {
        put_str("flag: arming the BROM USBDL latch and resetting\n");
        put_str("flag: the board will come back in BROM download mode, not in LK\n");
        mtk_arm_brom_usbdl();
        console_reset_now();
        return; /* not reached */
    } else {
        put_str("flag: expected `debug`, `boot` or `brom`\n");
        return;
    }

    rc = mt6592_dbgflag_arm(mode);
    put_str(mode == MT6592_DBG_MODE_CONSOLE ? "flag: next boot = debug console"
                                            : "flag: next boot = normal boot");
    kv_dec(", rc=", rc);
    if (rc != 0) {
        put_str(" (eMMC write FAILED; the flag did not land)");
    }
    put_char('\n');
}

/* ── Dispatcher ── */

static int handle_line(const char* line, const mvii_debug_console_hooks_t* hooks) {
    const char* args;

    line = skip_spaces(line);
    if (*line == '\0') return 0;

    /* The typed spelling of the greeting. `!MVIIDBG1', the host's wire spelling
     * of the same thing, never reaches here -- the input loop matches it as a
     * substring and answers before dispatch, because its job is to recover a
     * line that has rubbish glued to the front of it and this dispatcher only
     * matches at the start. */
    if ((args = match_word(line, "magic")) != 0) {
        console_put_magic();
    } else if ((args = match_word(line, "help")) != 0 || (args = match_word(line, "?")) != 0) {
        cmd_help();
    } else if ((args = match_word(line, "bat")) != 0) {
        cmd_bat(args);
    } else if ((args = match_word(line, "chg")) != 0) {
        cmd_chg();
    } else if ((args = match_word(line, "adcscan")) != 0) {
        uint32_t a = parse_u32_default(&args, 0x0700u);
        uint32_t b = parse_u32_default(&args, 0x0770u);
        kv_hex("adcscan ", a, 4u);
        kv_hex("..", b, 4u);
        put_char('\n');
        dump_pmic_range(a, b, 1);
    } else if ((args = match_word(line, "socadc")) != 0) {
        cmd_soc_adc(args);
    } else if ((args = match_word(line, "pmicr")) != 0) {
        cmd_pmicr(args);
    } else if ((args = match_word(line, "pmicw")) != 0) {
        cmd_pmicw(args);
    } else if ((args = match_word(line, "strings")) != 0) {
        cmd_strings(args);
    } else if ((args = match_word(line, "peek")) != 0) {
        cmd_peek(args);
    } else if ((args = match_word(line, "poke")) != 0) {
        cmd_poke(args);
    } else if ((args = match_word(line, "sweep")) != 0) {
        cmd_sweep(args);
    } else if ((args = match_word(line, "find")) != 0) {
        cmd_find(args);
    } else if ((args = match_word(line, "isink")) != 0) {
        cmd_isink();
    } else if ((args = match_word(line, "ledscan")) != 0) {
        cmd_ledscan();
    } else if ((args = match_word(line, "led")) != 0) {
        cmd_led(args);
    } else if ((args = match_word(line, "gpiodump")) != 0) {
        (void)args;
        cmd_gpiodump();
    } else if ((args = match_word(line, "gpioout")) != 0) {
        cmd_gpioout(args);
    } else if ((args = match_word(line, "pinhunt")) != 0) {
        cmd_pinhunt(args);
    } else if ((args = match_word(line, "pin")) != 0) {
        cmd_pin(args);
    } else if ((args = match_word(line, "wifi")) != 0) {
        cmd_wifi(args);
    } else if ((args = match_word(line, "gpu")) != 0) {
        cmd_gpu(args);
    } else if ((args = match_word(line, "sd")) != 0) {
        cmd_sd(args);
    } else if ((args = match_word(line, "ls")) != 0) {
        cmd_ls(args);
    } else if ((args = match_word(line, "kpdmap")) != 0) {
        cmd_kpdmap();
    } else if ((args = match_word(line, "kpdmode")) != 0) {
        cmd_kpdmode(args);
    } else if ((args = match_word(line, "kpdmon")) != 0) {
        cmd_kpdmon(args);
    } else if ((args = match_word(line, "kpdmux")) != 0) {
        cmd_kpdmux(args);
    } else if ((args = match_word(line, "kpdon")) != 0) {
        cmd_kpdon();
    } else if ((args = match_word(line, "kpd")) != 0) {
        /* Hold a key and run this: the hex dump says what the block latched, and the low
         * report says which bit that is by number. One-shot and read-only -- no settling,
         * no stability filter, nothing driven -- so it answers "does the KPD block see this
         * key at all", which is the question kpdmon cannot answer about a key it dropped. */
        dump_kpd();
        (void)kpd_scan_low_report("kpd: ");
    } else if ((args = match_word(line, "bl")) != 0) {
        cmd_bl(args);
    } else if ((args = match_word(line, "panel")) != 0) {
        if (hooks != 0 && hooks->display_on != 0) {
            int rc = hooks->display_on();
            kv_dec("panel: rc=", rc);
            put_char('\n');
        } else {
            put_str("panel: unavailable in this build\n");
        }
    } else if ((args = match_word(line, "fill")) != 0) {
        if (hooks != 0 && hooks->fb_fill != 0) {
            uint32_t argb = parse_u32_default(&args, 0xff000000u);
            kv_hex("fill: ", argb, 8u);
            put_char('\n');
            hooks->fb_fill(argb);
        } else {
            put_str("fill: unavailable in this build\n");
        }
    } else if ((args = match_word(line, "flag")) != 0) {
        cmd_flag(args);
    } else if ((args = match_word(line, "bootlog")) != 0) {
        cmd_bootlog(args);
    } else if ((args = match_word(line, "boot")) != 0) {
        put_str("boot: leaving the console\n");
        return 1;
    } else if ((args = match_word(line, "reboot")) != 0) {
        put_str("reboot: resetting\n");
        console_reset_now();
    } else {
        put_str("unknown command; try `help`\n");
    }
    return 0;
}

/* ── Entry ── */

/*
 * Power attendance while the console is idle.
 *
 * Both wait loops below can sit for minutes with nothing happening, and neither
 * used to touch the charger. On this PMIC the charger output *is* the system rail
 * (no power-path FET), so with no cell fitted an unattended stretch is the board
 * losing its supply -- and the console is the one place in the image most likely
 * to be left sitting. Every register the service call writes (charger-watchdog
 * disarm, UVLO widen, VCDT_HV clear, CHR_EN re-arm) only changes when the part
 * gives up, never how hard it drives, so this is safe to run from an idle loop.
 *
 * Gated by pass count rather than by the clock: the command loop spins on a
 * 200 us delay, so servicing every pass would be five thousand timer reads a
 * second to discover the 500 ms interval has not elapsed. 100 passes is ~20 ms,
 * still 25x finer than the interval it feeds.
 */
enum { CONSOLE_POWER_TICK_PASSES = 100u };

static void console_power_tick(void) {
    static uint32_t passes;
    if (++passes < (uint32_t)CONSOLE_POWER_TICK_PASSES) return;
    passes = 0u;
    mt6592_pmic_charger_service();
}

/*
 * Service the WLAN link while the console sits at its prompt.
 *
 * Nothing used to. mt6592_wifi_hif_poll() ran only inside the scan and join
 * loops, so between commands the driver was blind: the firmware's receive queue
 * filled with beacons that were never drained, and anything that mattered --
 * a deauthentication above all -- sat in it until some later command happened
 * to poll. That is why a dead link kept reporting JOINED for as long as nobody
 * looked, and why the first `wifi scan' after a join appeared to *cause* a
 * disconnect that it had merely been the first to read.
 *
 * Every idle pass, not divided down: a pass is roughly the 10 ms that
 * CONSOLE_POWER_TICK_PASSES turns into a one-second charger print, and 10 ms is
 * an ordinary service interval for a receive queue -- the scan loop polls
 * harder than this. Gated on firmware_alive so a boot that never brought Wi-Fi
 * up pays one predicate.
 */
static void console_wifi_tick(void) {
    const mt6592_wifi_hif_state* hif = mt6592_wifi_hif_get_state();
    if (hif == 0 || !hif->firmware_alive) return;
    (void)mt6592_wifi_hif_poll();
}

void mvii_debug_console_run(const mvii_debug_console_hooks_t* hooks) {
    /* 320, because `wifi wmt` can now carry a 73-byte frame and 73 bytes typed
     * as spaced hex is 227 characters before the command word. Overflow here is
     * silent -- the input loop simply stops storing at sizeof(cmd) -- so a
     * buffer too short for a pasted frame would truncate it into a different,
     * valid-looking frame and send that. */
    static char cmd[320];
    static char rx[MT6592_GADGET_PACKET];
    /* Sessions served on this one gadget bring-up. Zero means nobody has ever
     * attached, which is a different situation from a link that dropped: the
     * first wait is the operator walking to the keyboard, every later one is a
     * cable being reseated by someone plainly still there. */
    uint32_t attaches = 0u;
    int rc;

    /* The watchdog is already off in the LK, but this console can sit for
     * minutes with the CPU spinning in one place, which is exactly the shape a
     * watchdog exists to kill. Disarm it again and do not kick it. */
    mtk_watchdog_disable();
    g_hooks = hooks;

    put_str("\n=== MVII live debug console ===\n");

    /*
     * Say on the panel that we are here, before waiting for anything.
     *
     * Until now a board in console mode and a board that ignored the button
     * combo looked identical -- both black, both silent -- so every failed
     * attempt collapsed three separate questions into one unanswerable one: did
     * the keys read, did the LK reach this function, did USB enumerate. A solid
     * magenta screen answers the first two on its own, with no host, no cable and
     * no round trip through BROM, and it turns "nope, does not boot into debug
     * mode" into a report that says which half is broken.
     *
     * It costs the seamless bring-up nothing: this path only runs when the
     * operator deliberately asked for it, and a normal boot never gets here.
     * Magenta specifically because it is the one colour no other stage paints --
     * the removed probes were white, the charge park is dim, the logo is dark.
     */
    if (hooks != 0 && hooks->display_on != 0) {
        const int drc = hooks->display_on();
        kv_dec("console: panel rc=", drc);
        put_char('\n');
        if (drc == 0 && hooks->fb_fill != 0) hooks->fb_fill(0xffff00ffu);
    }

    /*
     * Attach the pull-up only now, after the panel is up.
     *
     * Order matters here and it did not used to. SOFTCONN is what tells the host
     * "a device just plugged in", and the host starts its enumeration clock the
     * instant it sees it. Asserting it first and then spending a second or two
     * bringing up display clocks, MIPI PLLs and the JD9365 program -- none of
     * which services EP0 -- means the very first control transfers of a fresh
     * enumeration land while nobody is listening. Doing the slow, silent work
     * first and raising the pull-up afterwards costs nothing and hands the host
     * a device that answers from the first SETUP onwards.
     */
    rc = mt6592_usb_gadget_init();
    kv_dec("console: gadget init rc=", rc);
    put_char('\n');
    if (rc != 0) {
        put_str("console: no USB; commands unavailable, booting on\n");
        flush_line();
        return;
    }

    /*
     * ── THE BRIDGE OUTLIVES THE LINK ──
     *
     * Everything from here down runs in a loop, and that loop is the whole
     * feature. This function used to be straight-line: wait once, serve once,
     * and on the first `recv` that returned -1 print "link lost; booting on",
     * tear the gadget down and go boot the kernel. Which meant a cable nudged
     * out and back in, a host process restarted, or -- most often -- another
     * `flash` invocation probing for a BROM ended the debug session outright,
     * and the only way back was a power cycle and the whole hold-a-button dance
     * again. On a board being debugged over USB that is the wrong end of every
     * trade: the link is the flaky part, and the session is the expensive one.
     *
     * So a lost link is now a return to the wait, not an exit. The gadget stays
     * initialised and the pull-up stays attached, so the host's next open is an
     * ordinary re-enumeration: g_configured goes 0 on the DISCON or the bus
     * reset, comes back to 1 on the next SET_CONFIGURATION, and the session
     * resumes with the same console state, the same hooks and the same buffers.
     *
     * Only two things still leave: `boot`/`reboot`, which are the operator
     * saying so, and a wait that times out, which is the board declining to sit
     * lit and unattended forever. And the timeout is far cheaper than it was --
     * a double-tap on the charge screen re-enters this function without a
     * reboot (see lk_charge_park() in mvii_lk_main.c), so "it timed out" no
     * longer means "start over from power-on".
     */
    for (;;) {
        uint32_t cmd_len = 0u;
        int cmd_overflowed = 0;
        uint32_t waited_ms = 0u;
        /* The first wait is sized to a human walking to a terminal; a re-attach is
         * sized to one already at it, fumbling with a cable or restarting a host
         * tool. Both are bounded, because an unattended board on this PMIC is a
         * board draining its cell behind a lit panel. */
        const uint32_t wait_limit = (attaches == 0u) ? (uint32_t)MVII_DEBUG_CONSOLE_WAIT_MS
                                                     : (uint32_t)MVII_DEBUG_CONSOLE_REATTACH_MS;

        if (attaches == 0u) {
            put_str("console: waiting for a host on the OTG port\n");
        } else {
            /* Back to magenta: the same "up and waiting" the operator was taught to
             * read at the top, so a dropped link looks like the state it actually
             * is rather than like a dead board. */
            if (hooks != 0 && hooks->fb_fill != 0) hooks->fb_fill(0xffff00ffu);
            put_str("console: link dropped; bridge still up, waiting for the host to come back\n");
        }
        flush_line();
        while (mt6592_usb_gadget_configured() == 0) {
            mt6592_usb_gadget_poll();
            /* One pass is 1 ms here, so this one is ungated -- the service call's own
             * 500 ms interval makes it a timer read per millisecond. */
            mt6592_pmic_charger_service();
            mt6592_delay_cycles(1000u * 33u);
            /* Heartbeat every 5 s, to storage. A wait that ends in silence tells the
             * next iteration nothing; these lines say whether the host ever drove a
             * bus reset, whether any SETUP arrived, and what the pull-up is doing --
             * which is the difference between a cable problem, a descriptor problem
             * and a driver-binding problem. They are readable afterwards with
             * `flash -mtk-read-boot-status`, so the diagnosis does not depend on the
             * very link that is failing. */
            if ((waited_ms % 5000u) == 0u) {
                mt6592_usb_gadget_log_state();
                /* Flush it, do not just buffer it. Two captures in a row ended with
                 * the ring stopping dead at the last sweep line and no heartbeat at
                 * all, because these lines sat in the line buffer waiting for it to
                 * fill while the operator power-cycled to go read them. A diagnostic
                 * that only reaches storage if enough of it accumulates first is not
                 * one you can rely on to explain a hang. */
                flush_line();
            }
            if (++waited_ms >= wait_limit) {
                /*
                 * Time out into the normal boot, and do not run the blind button map.
                 *
                 * The map used to run here as a fallback: the console had never once
                 * attached, and a diagnostic that only works after the thing being
                 * diagnosed works is not a diagnostic. That reasoning has expired.
                 * The gadget attaches reliably now, so reaching this branch no longer
                 * means "USB is broken and the blind path is all we have" -- it means
                 * the operator plugged in and walked away, or is powering the board
                 * from a charger. Sweeping the keypad for forty seconds in that case
                 * costs a boot and answers a question nobody asked.
                 *
                 * The map itself has not gone anywhere: `kpdmap` runs it and `kpdmon`
                 * watches the matrix live, both on request, which is how every useful
                 * keypad reading so far was actually taken. The state dump below stays
                 * because it is what says *why* no host arrived, and it reaches
                 * storage through the eMMC ring -- `flash -mtk-read-boot-status` reads
                 * it back even when the gadget never enumerates.
                 */
                put_str(attaches == 0u
                            ? "console: no host claimed the device; booting on (run `kpdmap` for the button map)\n"
                            : "console: the host never came back; booting on (double-tap MENU on the charge screen to return)\n");
                mt6592_usb_gadget_log_state();
                flush_line();
                put_str("console: booting on\n");
                flush_line();
                mvii_lk_set_log_sink(0);
                mt6592_uart_set_tap(0);
                mt6592_usb_gadget_shutdown();
                return;
            }
        }

        ++attaches;

        /* Green once a host is actually talking to us, so the operator can tell
         * "waiting" from "attached" across the room. */
        if (hooks != 0 && hooks->fb_fill != 0) hooks->fb_fill(0xff00c000u);

        /* Only now: before the host is configured, mt6592_usb_gadget_send has
         * nowhere to put bytes, and lk_log runs on every boot. Idempotent, and
         * re-armed on every attach because a lost link unhooks them below. */
        mvii_lk_set_log_sink(console_log_sink);
        /* And the driver trace with it. Same reason, same moment: `wifi fw' runs the
         * entire WMT bootstrap from inside a console command, and until now every
         * word of what the connectivity MCU said back went to a port with no cable
         * on it. */
        mt6592_uart_set_tap(console_uart_tap);

        /* The magic before the prose, on every attach including re-attaches. It is
         * the first thing a host reads, so it is what lets one that opened this port
         * expecting a BROM find out from four bytes instead of from a handshake
         * timeout -- and what lets one that reconnected after a drop confirm it is
         * back on the same bridge rather than on a freshly booted board. */
        console_put_magic();
        put_str(attaches == 1u ? "console: host attached. `help` for commands, `boot` to continue.\n"
                               : "console: host back. `help` for commands, `boot` to continue.\n");
        /* The full help once. Re-pasting ~300 lines of it into every reconnect would
         * bury the reason the operator reconnected. */
        if (attaches == 1u) cmd_help();
        put_str("> ");
        flush_line();

        for (;;) {
            int n;
            int i;

            mt6592_usb_gadget_poll();
            n = mt6592_usb_gadget_recv(rx, sizeof(rx));
            if (n < 0) {
                /* NOT an exit. See the block above the outer loop: the gadget stays
                 * up, the pull-up stays attached, and we go back to waiting. This is
                 * the ONLY way out of this inner loop -- `boot` returns from the
                 * function outright -- so the code after it needs no verdict flag. */
                break;
            }
            if (n == 0) {
                console_power_tick();
                console_wifi_tick();
                mt6592_delay_cycles(200u * 33u);
                continue;
            }
            for (i = 0; i < n; ++i) {
                char c = rx[i];

                /* Ahead of everything, including the newline test: a BROM probe's
                 * 0x0A is not a line ending. See console_swallow_brom_byte(). */
                if (console_swallow_brom_byte((uint8_t)c)) continue;

                if (c == '\n' || c == '\r') {
                    cmd[cmd_len] = '\0';
                    cmd_len = 0u;
                    /*
                     * The host's hello is checked anywhere in the line, not just at
                     * the start, and BEFORE the overflow rule -- because the whole
                     * point of it is to recover a buffer that already has rubbish in
                     * it. A link that dropped mid-command leaves a half-typed
                     * fragment sitting here with no newline behind it, so the first
                     * thing the returning host sends arrives glued to that fragment:
                     * `wifi f!MVIIDBG1'. Prefix matching would call that an unknown
                     * command and leave the operator to work out why their first
                     * line after reconnecting vanished.
                     */
                    if (console_find_host_magic(cmd)) {
                        cmd_overflowed = 0;
                        console_put_magic();
                        put_str("> ");
                        flush_line();
                        continue;
                    }
                    /* A line that hit the cap is not a line: the tail is gone and
                     * running the head of it would execute something the operator
                     * did not type. Say so and drop it. */
                    if (cmd_overflowed) {
                        cmd_overflowed = 0;
                        put_str("console: line too long, ignored\n> ");
                        flush_line();
                        continue;
                    }
                    if (handle_line(cmd, hooks) != 0) {
                        flush_line();
                        delay_ms(20u);
                        mvii_lk_set_log_sink(0);
                        mt6592_uart_set_tap(0);
                        mt6592_usb_gadget_shutdown();
                        return;
                    }
                    put_str("> ");
                    flush_line();
                } else if (cmd_len + 1u < sizeof(cmd)) {
                    cmd[cmd_len++] = c;
                } else {
                    cmd_overflowed = 1;
                }
            }
        }

        /* The link went away. Unhook the sinks before going back to the wait: with
         * no host, lk_log and the driver trace would be pushing every line at an
         * endpoint that can only refuse it, and the wait's own heartbeat is the
         * thing worth having in the eMMC ring meanwhile. Both are re-armed on the
         * next attach. The gadget itself is left alone -- shutting it down here is
         * exactly what used to make a reseated cable cost a power cycle. */
        put_str("console: link lost\n");
        flush_line();
        mvii_lk_set_log_sink(0);
        mt6592_uart_set_tap(0);
    } /* for (;;) -- back to the wait */
}
