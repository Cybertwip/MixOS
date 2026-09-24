/*
 * mvii_lk_main.c — MVII minimal LK for the J36 Ultra UBOOT/LK slot.
 *
 * Replaces the stock MediaTek LK entirely. The stock one is an Android
 * bootloader: the preloader classifies the power-on cause and hands it over by
 * ATAG, and when that cause is "USB/charger" the LK paints the battery cell
 * picture and parks in a pre-jump loop whose only two exits are "unplugged ->
 * power off" and "power key -> reboot". On a board whose cell is flat, missing,
 * or simply low, that loop is the whole boot: there is no OS, no shell, and no
 * way to use the wall power that is sitting right there on the DC rail. It also
 * decides all of this from a charger block that only ever saw the OTG port.
 *
 * This image does the four things a bootloader on this board actually has to
 * do, in the order the panel needs them, and then gets out of the way:
 *
 *   1. arm the input path so the board runs off whichever port is plugged in,
 *      with no cell fitted and no opinion about how full the cell is;
 *   2. light the panel (MTCMOS -> clocks -> panel power/reset -> MIPITX PLL ->
 *      DSI host -> JD9365 program -> OVL/RDMA/COLOR/BLS -> DDP mutex commit);
 *   3. load boot.img out of the BOOTIMG slot;
 *   4. jump to it the way the stock LK did, with an ATAG list that says
 *      "normal boot" so nothing downstream re-derives a charging mode.
 *
 * There is no battery gate anywhere in here. There is no charging UI. A boot
 * that reaches this code boots.
 *
 * Everything below step 3 is best-effort: a panel that fails to come up must
 * not cost the user their OS, so the display path logs its failure and the boot
 * carries on. Only the boot.img load can abort the boot, because past that
 * point there is nothing left to hand control to.
 */

#include <stdint.h>

#include "backlight.h"
#include "dsi_drv.h"
#include "lcd_drv.h"
#include "mt6592_bootstatus.h"
#include "mt6592_dbgflag.h"
#include "mt6592_delay.h"
#include "mt6592_disp_hw.h"
#include "mt6592_led.h"
#include "mt6592_msdc.h"
#include "mt6592_pmic.h"
#ifdef MVII_MT6592_LK_SD_HANDOFF
#include "mt6592_fdt.h"
#include "mt6592_msdc_sd.h"
#include "mvii_fat.h"
#endif
#include "mt6592_pwrap.h"
#include "mt6592_timer.h"
#include "mt6592_uart.h"
#include "mvii_assets.h"
#include "mvii_debug_console.h"
#include "panel_bringup.h"

/* ── Board layout (overridden from CMake; defaults match the stock scatter) ── */

#ifndef MVII_MT6592_LK_FB_ADDR
#define MVII_MT6592_LK_FB_ADDR 0x82700000u
#endif
#ifndef MVII_MT6592_LK_FB_WIDTH
#define MVII_MT6592_LK_FB_WIDTH 640u
#endif
#ifndef MVII_MT6592_LK_FB_HEIGHT
#define MVII_MT6592_LK_FB_HEIGHT 480u
#endif
#ifndef MVII_MT6592_LK_FB_PITCH
#define MVII_MT6592_LK_FB_PITCH 2560u
#endif
#ifndef MVII_MT6592_LK_FB_BPP
#define MVII_MT6592_LK_FB_BPP 32u
#endif

/* MT6592_Android_scatter.txt: BOOTIMG at 0x1f40000, 0x900000 long. */
#ifndef MVII_MT6592_LK_BOOTIMG_OFFSET
#define MVII_MT6592_LK_BOOTIMG_OFFSET 0x01f40000u
#endif
#ifndef MVII_MT6592_LK_BOOTIMG_SIZE
#define MVII_MT6592_LK_BOOTIMG_SIZE 0x00900000u
#endif

/* board_machtype() in the stock LK (FUN_81e00348) returns a constant 0x19c0. */
#ifndef MVII_MT6592_LK_MACHTYPE
#define MVII_MT6592_LK_MACHTYPE 0x19c0u
#endif

/* MVII_MT6592_LK_BACKLIGHT_PCT — the resting brightness — is defined in
 * panel_bringup.h, which is the file that also has to light the splash with it.
 * It is half, and the reasoning is there. */

/* The park sits at the same resting brightness as everything else. It used to be
 * dimmer than the splash (40 vs 100) because the splash was full; with the
 * resting level now half there is nothing left for the park to be dim relative
 * to, and two numbers for one brightness only make the transitions visible. */
#ifndef MVII_MT6592_LK_PARK_BACKLIGHT_PCT
#define MVII_MT6592_LK_PARK_BACKLIGHT_PCT MVII_MT6592_LK_BACKLIGHT_PCT
#endif

/*
 * Full brightness, and the ONE place in the loader that uses it: the handoff.
 *
 * The power key ends the park and starts the boot, and from that point the panel
 * stops being the loader's to conserve -- the OS is about to own it and set its
 * own level anyway, and the eMMC load and the kernel's bring-up are seconds, not
 * the indefinite park this dims for. Going to full there is also the only
 * feedback the key press gets, since the frame it leaves behind is black.
 *
 * It is deliberately NOT a plug/unplug acknowledgement. That was the earlier
 * shape of this and it put the largest load on the board at full power in the
 * middle of a charge, on the node the charge is trying to reach, to signal
 * something the picture and the LED already say.
 */
#ifndef MVII_MT6592_LK_HANDOFF_BACKLIGHT_PCT
#define MVII_MT6592_LK_HANDOFF_BACKLIGHT_PCT 100u
#endif

/*
 * How long the greyed gauge stays on the panel after the cable comes out, before
 * the park ends and the loader boots.
 *
 * The gauge has to survive the unplug -- reading the level at the moment the
 * cable parts is the whole point, and a park that blanked instantly was the "the
 * lk battery indicator does not show when the cable is unplugged" bug. But an
 * unattended board holding a lit panel off its own cell is not a park, it is a
 * drain: past this window the number has been read or it has not.
 *
 * It is also the debounce on CHRDET. A cable being reseated, or a comparator
 * chattering as a barrel jack is nudged, is back well inside five seconds and
 * the park simply un-greys; only a removal that is still a removal at the end of
 * the window is treated as one.
 *
 * THIS USED TO BE _BLACKOUT_MS and it used to end in a dark board that never
 * booted. The old name is still honoured for anything setting it out of tree.
 */
#ifdef MVII_MT6592_LK_UNPLUG_BLACKOUT_MS
#define MVII_MT6592_LK_UNPLUG_BOOT_MS MVII_MT6592_LK_UNPLUG_BLACKOUT_MS
#endif
#ifndef MVII_MT6592_LK_UNPLUG_BOOT_MS
#define MVII_MT6592_LK_UNPLUG_BOOT_MS 5000u
#endif

/* ── ATAGs (same constants stage1.c parses) ── */

#define ATAG_NONE 0x00000000u
#define ATAG_CORE 0x54410001u
#define ATAG_MEM 0x54410002u
#define ATAG_VIDEOLFB 0x54410008u
#define ATAG_CMDLINE 0x54410009u
#define ATAG_INITRD2 0x54420005u
/* MediaTek's private boot-mode tag {size=3, tag, mode}; mode 0 is NORMAL. The
 * stock LK emits this from FUN_81e02ae8 with whatever the preloader told it.
 * We emit NORMAL unconditionally — that is the entire point of this image. */
#define ATAG_MTK_BOOT 0x41000802u
#define MTK_BOOT_MODE_NORMAL 0u

#define DRAM_BASE 0x80000000u
#define DRAM_LIMIT 0xc0000000u

/* Dual 512 MiB ranks, contiguous from DRAM_BASE. Matches mt6592_dram.c's
 * EMI_CONA extract and the window addr_in_dram() already accepts. */
#ifndef MVII_MT6592_LK_DRAM_START
#define MVII_MT6592_LK_DRAM_START DRAM_BASE
#endif
#ifndef MVII_MT6592_LK_DRAM_SIZE
#define MVII_MT6592_LK_DRAM_SIZE (DRAM_LIMIT - DRAM_BASE)
#endif

/* Stock LK default (lk.bin:0x350d0) and the stock kernel's CONFIG_CMDLINE.
 * Used when boot.img's own cmdline field is empty, which the stock image is. */
#ifndef MVII_MT6592_LK_STOCK_CMDLINE
#define MVII_MT6592_LK_STOCK_CMDLINE \
    "console=tty0 console=ttyMT3,921600n1 root=/dev/ram vmalloc=496M slub_max_order=0 slub_debug=O"
#endif

#define MTK_IMG_MAGIC 0x58881688u

/* How much of a payload one eMMC call moves. See lk_load_payload(). */
#define LK_LOAD_SEGMENT_BYTES 0x00080000u

/*
 * lcd_drv.c guards its DRAM/register probes with these two words and the data
 * abort trampoline in mvii_lk.S drives them. In the stage1 images they come
 * from mt6592_dram.c; this LK does not train DRAM (the preloader already did),
 * so they live here rather than dragging in the EMI driver for two words.
 */
volatile uint32_t mt6592_stage1_abort_flag = 0u;
volatile uint32_t mt6592_stage1_abort_resume = 0u;

/* mvii_lk.S */
void mvii_lk_jump_to_kernel(uint32_t entry, uint32_t machtype, uint32_t tags) __attribute__((noreturn));

/* One page of boot.img header / MTK wrapper, read straight off the eMMC. */
static uint8_t g_page[2048] __attribute__((aligned(64)));

/* ── Logging ──
 * UART for a bench with a cable on it, and the eMMC boot-status record for the
 * far more common case of a bench without one (`flash -mtk-read-boot-status`).
 *
 * Autoflush stays off until storage is up, and goes on the moment it is. From
 * that point every completed line is appended to the record's rolling message
 * field and written straight out, so a board that stops anywhere past eMMC init
 * says over USB which line it got to. That costs one small write per line and
 * this image emits about a dozen. */
/*
 * A third destination, installed by whoever owns the host link.
 *
 * The two above are both write-only from the operator's seat: this board has no
 * UART cable, and the eMMC ring can only be read on the NEXT boot. That is fine
 * for a boot that logs and continues, and it is useless for a CPU exception
 * taken inside a live console command -- mvii_lk_report_exception prints pc,
 * spsr, dfsr and dfar through here and then halts, so the host attached to the
 * console sees no report at all, just silence and then a detach when the
 * watchdog resets the SoC. `wifi scan` looked exactly like a USB fault for that
 * reason. The debug console registers itself here so the post-mortem comes out
 * on the same pipe that issued the command that caused it.
 */
static void (*g_log_sink)(const char* text);

void mvii_lk_set_log_sink(void (*sink)(const char* text)) { g_log_sink = sink; }

static void lk_log(const char* text) {
    mt6592_uart_puts(text);
    mt6592_bootstatus_log_text(text);
    if (g_log_sink != 0) g_log_sink(text);
}

static void lk_log_hex(const char* label, uint32_t value) {
    char buf[11];
    static const char kHex[] = "0123456789abcdef";
    buf[0] = '0';
    buf[1] = 'x';
    for (uint32_t i = 0; i < 8u; ++i) buf[2u + i] = kHex[(value >> (28u - 4u * i)) & 0xfu];
    buf[10] = 0;
    lk_log(label);
    lk_log(buf);
    lk_log("\n");
}

/* The register-name tables for both readback dumps live in panel_bringup.c
 * (mt6592_dsi_readback_names / mt6592_lcd_readback_names) so this loader and
 * the live MVIIFlash console label the same words identically. */

/* ── Progress beacon (bring-up instrument, off by default) ──
 * While the DSI host was dead this was the only report a board with no serial
 * cable and no USB host could give: each milestone repainted the whole canvas a
 * distinct colour and added a white tick to a tally along the top edge, so a
 * photograph of a stopped panel named the step it stopped on.
 *
 *   1  LK entered                    (before the panel; tally only)
 *   2  PMIC input path armed         (before the panel; tally only)
 *   3  eMMC ready                    (before the panel; tally only)
 *   4  scanout live                  dark blue
 *   5  boot.img header accepted      teal
 *   6  kernel in DRAM                dark green
 *   7  ramdisk in DRAM               green
 *   8  jumping to the kernel         white
 *      fatal: no bootable image      red
 *
 * The panel works now, and full-screen milestone colours would paint straight
 * over the charge screen and fight the "plugged, no cell -> black screen"
 * requirement. So the painting compiles out and only the log/stage half of each
 * milestone survives. Build with -DMVII_MT6592_LK_BEACONS=1 to get it back for
 * the next display regression; the colours below are kept for that reason and
 * are not otherwise referenced.
 */
#ifndef MVII_MT6592_LK_BEACONS
#define MVII_MT6592_LK_BEACONS 0
#endif

#define LK_BEACON_SCANOUT 0xff000060u
#define LK_BEACON_HEADER 0xff006060u
#define LK_BEACON_KERNEL 0xff005000u
#define LK_BEACON_RAMDISK 0xff00b000u
#define LK_BEACON_HANDOFF 0xffffffffu
#define LK_BEACON_FAILED 0xffb00000u

static uint32_t g_fb_live;     /* set once the DDP route is actually scanning out */
static uint32_t g_storage_up;  /* set once an eMMC write is a safe thing to do */

/* GPT4 tick at the moment the backlight came up on the logo, so the splash can
 * be held for a fixed two seconds of WALL CLOCK rather than two seconds plus
 * however long the boot took to get from there to the charge park. Zero means
 * the panel never lit and there is no splash to hold. */
static uint32_t g_splash_ticks;
static uint32_t g_splash_lit;

#if MVII_MT6592_LK_BEACONS
static uint32_t g_beacon_step;

static void lk_beacon(uint32_t argb) {
    const uint32_t w = (uint32_t)MVII_MT6592_LK_FB_WIDTH;
    const uint32_t h = (uint32_t)MVII_MT6592_LK_FB_HEIGHT;
    const uint32_t stride = (uint32_t)MVII_MT6592_LK_FB_PITCH / 4u;
    volatile uint32_t* px = (volatile uint32_t*)(uintptr_t)MVII_MT6592_LK_FB_ADDR;

    /* Counted even when there is no panel to draw on, so the tally a later
     * milestone paints still matches the table above. */
    ++g_beacon_step;
    if (!g_fb_live) return;

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) px[y * stride + x] = argb;
    }
    for (uint32_t i = 0; i < g_beacon_step; ++i) {
        const uint32_t x0 = 8u + i * 24u;
        if (x0 + 16u > w) break;
        for (uint32_t y = 8u; y < 28u; ++y) {
            for (uint32_t x = x0; x < x0 + 16u; ++x) px[y * stride + x] = 0xffffffffu;
        }
    }
    mt6592_lcd_clean_fb((uintptr_t)MVII_MT6592_LK_FB_ADDR,
                        (uintptr_t)((uint32_t)MVII_MT6592_LK_FB_PITCH * h));
}
#else
#define lk_beacon(argb) ((void)(argb))
#endif

/*
 * Held-button entry to the live console: ANY keypad button held at this instant.
 *
 * This deliberately does not name a button, and that is the whole point. The
 * first attempt asked for VOL_UP + VOL_DOWN, matrix bits 28 and 27 -- and those
 * two bits are among the ones this board has never been seen to fire.
 * mt6592_board_j36.h says so in its own comment: face-button mashing lights
 * exactly bits 0, 9..12, 29 and 30, and the 27/28 assignment comes from the
 * stock kernel keymap rather than from a capture. Worse, the labels are not what
 * they look like -- the physical side + and - keys are wired as L1/L2 (bits 0
 * and 1), not as VOL_UP/VOL_DOWN. So the trigger was asking for two bits that
 * probably do not exist, on a board where the map it was reading is known to be
 * partly wrong.
 *
 * A trigger whose job is to be reachable must not depend on the very mapping it
 * exists to help fix. "Any bit low" depends on none of it: whichever button the
 * operator happens to press, on whatever row it really lives, opens the console.
 * POWER is not on the matrix at all here (MT6592_J36_KEY_POWER_MATRIX is
 * unmapped), so powering on normally holds nothing down and a plain boot cannot
 * false-trigger.
 *
 * Polarity is vendor active-low: the scan words idle at mem1..4 = 0xffff,
 * mem5 = 0xff, and a held key pulls its bit to 0.
 *
 * KPD_EN is forced on first. A disabled keypad does not scan, and a keypad that
 * does not scan reports every key released forever -- which would make a held
 * button indistinguishable from no button, silently, on every boot.
 */
enum {
    LK_KPD_BASE = 0x10011000u,
    LK_KPD_MEM1 = 0x0004u,
    LK_KPD_DEBOUNCE = 0x0018u,
    LK_KPD_EN = 0x0024u,
    LK_KPD_DEBOUNCE_DEFAULT = 0x0400u,
    LK_KPD_NUM_MEMS = 5u,
};

/*
 * ── MENU, WITHOUT THE KEYPAD DRIVER ──
 *
 * The detail screen in the charge park is reached with MENU, and MENU is the one
 * button on this board that cannot simply be read. mt6592_board_j36.h:180 records
 * that the boot chain parks pads 11, 12 and 2 in mode 0, so the whole of row 3 --
 * VOL-, VOL+, SELECT, START, MENU -- is dead on any build that does not mux them,
 * and the code that does mux them (kpd_pads_apply() in the OS keypad driver) is not in
 * either LK target's source list. Linking that file in for one bit would drag the
 * entire keypad and joystick driver, AUXADC calibration included, into the
 * bootloader.
 *
 * So MENU's two pads get muxed here instead. The block's key code is row*9 + col
 * and MENU is matrix bit 31 (MT6592_J36_KEY_MENU_MATRIX), so bit 31 is row 3,
 * column 4 -- which is pads 11 and 2:
 *
 *   pad 11 -> mode 3, output      KPROW3, the strobe (MT6592_J36_KPD_STROBE3_*)
 *   pad  2 -> mode 6, input + up  KPCOL4, the sense  (MT6592_J36_KPD_SENSE4_*)
 *
 * Both modes are hardware-measured rather than derived -- the keypad lands at a
 * different mux index on each of these pads, see the table at
 * mt6592_board_j36.h:218. Column 3 (pad 12) is deliberately left alone: START and
 * R2 live there and nothing in the loader reads them.
 *
 * MODE BEFORE DIRECTION, for the reason kpd_pads_apply() gives: a row pad turned
 * into a GPIO output before it is muxed drives its whole row low, and every key on
 * that row reads pressed for as long as that lasts. PULLSEL before PULLEN for the
 * matching reason on the sense pad -- the resistor is never briefly enabled in the
 * wrong direction. The pull-up is what makes a closure observable at all: MVII
 * reads these active-low and a switch to ground has nothing to idle against
 * without it.
 *
 * KPD_EN is forced on for the same reason lk_debug_combo_held() forces it: a
 * keypad that does not scan reports every key released forever, which is
 * indistinguishable from nobody pressing anything.
 */
enum {
    LK_KPD_PULLEN_BASE = 0x0100u, /* GPIO block offsets, from the OS keypad driver */
    LK_KPD_PULLSEL_BASE = 0x0200u,
    LK_KPD_MENU_ROW_PAD = 11u,
    LK_KPD_MENU_ROW_MODE = 3u,
    LK_KPD_MENU_COL_PAD = 2u,
    LK_KPD_MENU_COL_MODE = 6u,
    /* Bit 31 of the matrix: word 31/16, bit 31%16. */
    LK_KPD_MENU_MEM = 1u,
    LK_KPD_MENU_BIT = 15u,
};

/* Write-1-to-SET at +4, write-1-to-RST at +8: one bit, no read, no neighbours
 * disturbed. Every 16-pin GPIO value register has this pair; MODE does not, which
 * is why mtk_gpio_set_mode() reads back and this does not. */
static void lk_gpio_val_bit(uint32_t base, uint32_t pin, uint32_t on) {
    const uint32_t reg = MTK_GPIO_BASE + base +
                         (pin / MTK_GPIO_PIN_PER_REG) * MTK_GPIO_STRIDE +
                         (on != 0u ? MTK_GPIO_SET : MTK_GPIO_RST);
    mtk_write32(reg, 1u << (pin % MTK_GPIO_PIN_PER_REG));
}

static void lk_kpd_menu_arm(void) {
    volatile uint16_t* const en = (volatile uint16_t*)(uintptr_t)(LK_KPD_BASE + LK_KPD_EN);
    volatile uint16_t* const deb = (volatile uint16_t*)(uintptr_t)(LK_KPD_BASE + LK_KPD_DEBOUNCE);

    mtk_gpio_set_mode(LK_KPD_MENU_ROW_PAD, LK_KPD_MENU_ROW_MODE);
    mtk_gpio_dir_output(LK_KPD_MENU_ROW_PAD);

    mtk_gpio_set_mode(LK_KPD_MENU_COL_PAD, LK_KPD_MENU_COL_MODE);
    lk_gpio_val_bit(MTK_GPIO_DIR_BASE, LK_KPD_MENU_COL_PAD, 0u);   /* input */
    lk_gpio_val_bit(LK_KPD_PULLSEL_BASE, LK_KPD_MENU_COL_PAD, 1u); /* up */
    lk_gpio_val_bit(LK_KPD_PULLEN_BASE, LK_KPD_MENU_COL_PAD, 1u);

    *deb = (uint16_t)LK_KPD_DEBOUNCE_DEFAULT;
    *en = 1u;
}

/* Nonzero while MENU is held. Active low, one latched scan memory read -- the
 * block refreshes it once per debounce interval, so polling faster than that only
 * re-reads the same word. */
static uint32_t lk_kpd_menu_down(void) {
    const uint32_t word = (uint32_t)*(volatile uint16_t*)(uintptr_t)(
        LK_KPD_BASE + LK_KPD_MEM1 + LK_KPD_MENU_MEM * 4u);

    return ((word >> LK_KPD_MENU_BIT) & 1u) ? 0u : 1u;
}

#ifndef MVII_MT6592_LK_RELEASE
/* One scan of the five matrix words. Returns nonzero if any bit is low, and
 * leaves the words in `out` so the caller can log the baseline exactly once
 * rather than once per poll.
 *
 * This and lk_debug_combo_held() below exist only to open the debug console, so
 * the release bootloader does not compile them. That also gives the release
 * build back the two-second scan window every normal boot pays for here. */
static uint32_t lk_kpd_scan(uint32_t out[LK_KPD_NUM_MEMS]) {
    uint32_t held = 0u;
    uint32_t i;

    for (i = 0; i < LK_KPD_NUM_MEMS; ++i) {
        const uint32_t valid = (i == LK_KPD_NUM_MEMS - 1u) ? 0x00ffu : 0xffffu;
        const uint32_t word =
            (uint32_t)*(volatile uint16_t*)(uintptr_t)(LK_KPD_BASE + LK_KPD_MEM1 + i * 4u) & valid;
        out[i] = word;
        if (word != valid) held = 1u;
    }
    return held;
}

/*
 * ── A WINDOW, NOT AN INSTANT ──
 *
 * This used to be a single scan. That made the trigger a timing puzzle: the
 * check lands after pwrap, after eMMC init and after the asset slot is staged,
 * which is a variable couple of seconds into a boot, and a button that was down
 * a moment before or a moment after that one instant was simply not seen. The
 * only reliable technique was to hold a key from before power-on and hope.
 *
 * So the scan is now a window, and the two numbers in it are the operator's:
 * about two seconds wide, and NOT waited out. The loop returns on the first
 * scan that shows a bit low, so a press costs the boot nothing beyond the time
 * the operator took to make it -- "as soon as you detect a button press, start
 * in debug mode, don't wait for a timeframe". A boot with nobody pressing pays
 * the full window and nothing else, which is why it is two seconds and not ten:
 * every normal boot buys the window for the rare one that needs it.
 *
 * The poll interval is one debounce period. Polling faster would only re-read
 * the same latched scan, since the block only refreshes the mem words once per
 * debounce interval; polling slower would start dropping short presses.
 */
enum {
    LK_DEBUG_WINDOW_MS = 2000u,
    LK_DEBUG_POLL_MS = 20u,
    LK_DEBUG_POLLS = LK_DEBUG_WINDOW_MS / LK_DEBUG_POLL_MS,
};

/* Defined with the charge park, ~600 lines down, because that is where its slice
 * width is argued. Declared here because this window is the OTHER long wait in
 * the image, and the converter has to be clocked through both. */
static void lk_park_hold_ms(uint32_t ms);

static uint32_t lk_debug_combo_held(void) {
    volatile uint16_t* const en = (volatile uint16_t*)(uintptr_t)(LK_KPD_BASE + LK_KPD_EN);
    volatile uint16_t* const deb = (volatile uint16_t*)(uintptr_t)(LK_KPD_BASE + LK_KPD_DEBOUNCE);
    uint32_t words[LK_KPD_NUM_MEMS] = {0u, 0u, 0u, 0u, 0u};
    uint32_t held = 0u;
    uint32_t polls = 0u;
    uint32_t i;

    *deb = (uint16_t)LK_KPD_DEBOUNCE_DEFAULT;
    *en = 1u;

    for (polls = 0u; polls < (uint32_t)LK_DEBUG_POLLS; ++polls) {
        /* One debounce interval BEFORE the first scan as well as between the
         * rest: the block was just enabled and the mem words still hold
         * whatever they powered up with until it has scanned once.
         *
         * Spent in 1 ms slices with the PMIC serviced in each, rather than as one
         * bare delay. This window is two seconds long and sits AFTER pwrap
         * bring-up, so it is two seconds the AUXADC could be converting -- fifteen
         * conversions is all it takes to fill every ring, and there are hundreds
         * of milliseconds here. The console's first frame then opens on warm
         * medians instead of on the em-dashes that mean "no sample yet", which is
         * most of what "the battery reading takes forever to appear" was. */
        lk_park_hold_ms((uint32_t)LK_DEBUG_POLL_MS);
        held = lk_kpd_scan(words);
        if (held) break;
    }

    /* Logged whether or not anything matches. If the trigger does not take, the
     * next question is always "what did the scan words actually read", and a
     * boot that cannot answer it costs another round trip. These five words are
     * also the idle baseline for the button map -- see the mapping sweep the
     * console runs, which needs to know what "nothing pressed" looks like. The
     * poll count says where in the window the press landed, which is the one
     * thing a single scan could never report. */
    lk_log("lk: dbg kpd");
    for (i = 0; i < LK_KPD_NUM_MEMS; ++i) lk_log_hex(" mem=", words[i]);
    lk_log_hex(" pwrkey=", (uint32_t)mt6592_pmic_pwrkey_pressed());
    lk_log_hex(" after_ms=", polls * (uint32_t)LK_DEBUG_POLL_MS);
    lk_log("\n");

    return held;
}
#endif /* !MVII_MT6592_LK_RELEASE */

/*
 * One milestone: log it, record it, show it.
 *
 * The stage code is set after the log line and not before, because the console
 * mirror in mt6592_bootstatus.c rewrites g_status.stage to its RUNTIME
 * heartbeat every time a line completes — a stage set first is gone by the time
 * anything reaches storage.
 */
static void lk_mark(uint32_t stage, const char* message, uint32_t argb) {
    lk_log(message);
    mt6592_bootstatus_set_stage(stage);
    if (g_storage_up) (void)mt6592_bootstatus_flush();
    lk_beacon(argb);
    mtk_watchdog_kick();
}

/* ── CPU exceptions ──
 *
 * Called from the vector stubs in mvii_lk.S, which have already parked the
 * faulting mode on a usable stack and collected the CP15 fault registers. This
 * never returns to the faulting code; the caller halts as soon as it comes
 * back.
 *
 * Vector numbers are ARM's own, so a number in the log can be checked straight
 * against the table in mvii_lk.S.
 */
static const char* const kExceptionNames[8] = {
    "reset", "undefined instruction", "supervisor call", "prefetch abort",
    "data abort", "reserved vector", "irq", "fiq",
};

void mvii_lk_report_exception(uint32_t vec, uint32_t pc, uint32_t spsr, uint32_t dfsr, uint32_t dfar,
                              uint32_t ifsr, uint32_t ifar);

void mvii_lk_report_exception(uint32_t vec, uint32_t pc, uint32_t spsr, uint32_t dfsr, uint32_t dfar,
                              uint32_t ifsr, uint32_t ifar) {
    static uint32_t reporting;

    /* Everything below touches the eMMC and, on a live panel, the framebuffer.
     * If one of those is what faulted, the second fault must halt rather than
     * re-enter — the stubs reset SP each time, so an unguarded recursion would
     * not even overflow the stack, it would just spin forever and hand back the
     * silent board this whole path exists to eliminate. */
    if (reporting) return;
    reporting = 1u;

    /*
     * Kill the watchdog before saying anything.
     *
     * The path below ends in `wfi`, deliberately -- but a halt only reports if
     * the board is still there to be read. Console commands that poll for
     * seconds arm the watchdog on purpose (`wifi scan` uses 30 s), so a fault
     * inside one of them used to be followed by a reset just as the operator
     * started reading, taking the USB link with it. That turned every fault into
     * the same symptom, a bare detach, which is the one symptom that names
     * nothing.
     */
    mtk_watchdog_disable();

    /*
     * The likeliest place to fault is inside the multi-megabyte eMMC read, and
     * that is exactly the state in which the driver this report has to travel
     * through is halfway into a transfer. Re-initialise the controller first,
     * unconditionally: every loop in that path is bounded, so the worst case is
     * a bounded wait and an error nobody reads, and the best case is the
     * difference between a diagnosis and another silent board.
     */
    if (g_storage_up) (void)mt6592_emmc_user_init();

    lk_log("\nlk: CPU exception: ");
    lk_log(vec < 8u ? kExceptionNames[vec] : "unknown vector");
    lk_log("\n");
    lk_log_hex("lk: exc pc=", pc);
    lk_log_hex("lk: exc spsr=", spsr);
    lk_log_hex("lk: exc dfsr=", dfsr);
    lk_log_hex("lk: exc dfar=", dfar);
    lk_log_hex("lk: exc ifsr=", ifsr);
    lk_log_hex("lk: exc ifar=", ifar);

    /* 0xexxxxxxx so the error word alone identifies a fault halt: vector in the
     * middle nibble, DFSR status in the low bits. */
    mt6592_bootstatus_set_error(0xe0000000u | ((vec & 0xfu) << 16) | (dfsr & 0xffffu));
    lk_mark(MT6592_BOOT_STATUS_STAGE_LK_EXCEPTION, "lk: halted on a CPU exception\n", LK_BEACON_FAILED);
}

/* ── Panel power/reset ──
 * Lives in panel_bringup.c as mt6592_panel_power_on(), shared with the resident
 * MVIIFlash BROM payload so its live "panel" command drives the same pins in
 * the same order this loader does. */

/* Fill the whole LK framebuffer with one ARGB colour and push it to memory the
 * OVL will actually read.
 *
 * Eight words a pass and non-volatile, for the reasons argued at length over
 * lk_fb_paint_logo() below -- the MMU is off, so the loop around the pixels
 * costs more than the pixels. This one is on the charge screen's repaint path
 * rather than the splash's, so it is the same 1.2 MB paid every time the
 * percentage changes. */
static void lk_fb_fill(uint32_t argb) {
    const uint32_t h = (uint32_t)MVII_MT6592_LK_FB_HEIGHT;
    const uint32_t words = ((uint32_t)MVII_MT6592_LK_FB_PITCH / 4u) * h;
    uint32_t* px = (uint32_t*)(uintptr_t)MVII_MT6592_LK_FB_ADDR;
    uint32_t n;
    uint32_t tail;

    for (n = words / 8u; n != 0u; --n) {
        px[0] = argb;
        px[1] = argb;
        px[2] = argb;
        px[3] = argb;
        px[4] = argb;
        px[5] = argb;
        px[6] = argb;
        px[7] = argb;
        px += 8u;
    }
    for (tail = words & 7u; tail != 0u; --tail) *px++ = argb;

    mt6592_lcd_clean_fb((uintptr_t)MVII_MT6592_LK_FB_ADDR,
                        (uintptr_t)((uint32_t)MVII_MT6592_LK_FB_PITCH * h));
}

/*
 * ── THE FIRST LIT FRAME IS THE LOGO ──
 *
 * The panel used to come up on a black surface and stay black -- backlight at
 * full, nothing on it -- for the whole boot.img load and then two more seconds
 * inside stage1, because the logo was painted by stage1 and stage1 does not run
 * until the kernel and ramdisk are off the eMMC. Several seconds of a lit black
 * screen is indistinguishable from a board that hung during panel bring-up, and
 * it is the first thing anyone sees.
 *
 * Nothing about that ordering was necessary. The asset slot is staged in DRAM
 * above, before the display, precisely because both consumers want it early;
 * the logo is stored uncompressed ARGB8888 for exactly this reason; and the
 * surface is filled before the bring-up rather than after it, since nothing
 * reads it until the OVL is armed at the end. So the fill just paints the logo
 * instead of a colour, and the panel's first frame -- the one the backlight
 * comes up on, since the backlight is now the last step of the bring-up -- is
 * already the picture.
 *
 * A board with no asset slot flashed still gets black, which is what it got
 * before, and stage1 still paints its own copy for boots under the STOCK
 * MediaTek LK where nobody staged anything.
 *
 * The stored bytes ARE the frame: little-endian ARGB8888 at the panel's exact
 * geometry, and this framebuffer is ARGB8888 with pitch == width*4, so the
 * whole image is one contiguous run and this is a copy, not a composite.
 *
 * ── EIGHT WORDS A PASS, AND NOT THROUGH A VOLATILE POINTER ──
 *
 * 640x480 is 307,200 words, and a one-word-per-iteration loop through a
 * volatile pointer spends four instructions on each of them: ldr, str, and the
 * counter's subs/bne. Eight words a pass with the pointers post-incremented
 * instead of indexed is nineteen instructions per eight words -- 1.7x fewer,
 * measured off the emitted ARM, not assumed.
 *
 * Dropping volatile is what allows it. Nothing is lost by dropping it: this
 * surface is plain DRAM, not a register file, and what makes the writes visible
 * to the OVL is the cache clean below, never the qualifier. The clean's own
 * "memory" clobber is also what stops the optimiser treating a write-only
 * buffer as dead.
 *
 * HOW MUCH THIS BUYS DEPENDS ON SOMETHING NOT SETTLED HERE. The loader runs
 * with the MMU off, so all 614,400 data accesses are Strongly-Ordered and
 * un-coalesced whatever we do -- no C-level change avoids that, and ldm/stm
 * would not either, since Strongly-Ordered memory performs them as individual
 * accesses anyway (clang declines to form them at -Os and -O2 regardless). If
 * instruction fetch is also uncached, the 1.7x lands in full; if the I-cache is
 * on, this tight a loop sits in it and the win is small. It is the free half of
 * the problem either way.
 *
 * -ffreestanding -fno-builtin is why this cannot quietly become a memcpy call:
 * with no libc declared available, the loop-idiom pass has nothing to call. A
 * struct-assignment copy would NOT be safe for the same reason in reverse --
 * that lowers to __aeabi_memcpy, whose implementation is the byte-at-a-time
 * volatile loop this is trying to get away from (see mvii_arm_aeabi_mem.c).
 */
static int lk_fb_paint_logo(void) {
    const uint32_t w = (uint32_t)MVII_MT6592_LK_FB_WIDTH;
    const uint32_t h = (uint32_t)MVII_MT6592_LK_FB_HEIGHT;
    const uint32_t pitch = (uint32_t)MVII_MT6592_LK_FB_PITCH;
    const uint32_t words = w * h;
    const mvii_asset_entry_t* e;
    const uint32_t* s;
    uint32_t* d = (uint32_t*)(uintptr_t)MVII_MT6592_LK_FB_ADDR;
    uint32_t bytes = 0u;
    uint32_t n;
    uint32_t tail;

    if (pitch != w * 4u) return 0;

    e = mvii_asset_find(MVII_ASSET_LOGO);
    if (!e || e->format != MVII_ASSET_FMT_ARGB8888) return 0;
    if ((uint32_t)e->width != w || (uint32_t)e->height != h) return 0;

    s = (const uint32_t*)(const void*)mvii_asset_raw(MVII_ASSET_LOGO, &bytes);
    if (!s || bytes < words * 4u) return 0;

    for (n = words / 8u; n != 0u; --n) {
        const uint32_t a0 = s[0];
        const uint32_t a1 = s[1];
        const uint32_t a2 = s[2];
        const uint32_t a3 = s[3];
        const uint32_t a4 = s[4];
        const uint32_t a5 = s[5];
        const uint32_t a6 = s[6];
        const uint32_t a7 = s[7];
        d[0] = a0;
        d[1] = a1;
        d[2] = a2;
        d[3] = a3;
        d[4] = a4;
        d[5] = a5;
        d[6] = a6;
        d[7] = a7;
        s += 8u;
        d += 8u;
    }
    /* 307,200 is a multiple of eight, so this runs zero times at the geometry
     * this loader is built for -- and is the one line that keeps it from being a
     * silently truncated frame at a geometry it is not. */
    for (tail = words & 7u; tail != 0u; --tail) *d++ = *s++;

    mt6592_lcd_clean_fb((uintptr_t)MVII_MT6592_LK_FB_ADDR, (uintptr_t)(pitch * h));
    return 1;
}

/* ── Display ──
 * Order is the reference bring-up's (Reference/J36-ULTRA/src/lcd.c
 * lcd_bringup): clocks, panel power, MIPITX PLL + DSI host, DCS program in LP,
 * then video mode + the DDP route in one call, and the backlight LAST. */
/* Adapter so the shared bring-up's trace comes out in this loader's format. */
static void lk_panel_log(void* ctx, const char* label, int has_value, uint32_t value) {
    (void)ctx;
    lk_log("lk: ");
    if (has_value) {
        lk_log_hex(label, value);
    } else {
        lk_log(label);
        lk_log("\n");
    }
}

static int lk_display_on(void) {
    const uint32_t fb = (uint32_t)MVII_MT6592_LK_FB_ADDR;
    const uint32_t fb_bytes = (uint32_t)MVII_MT6592_LK_FB_PITCH * (uint32_t)MVII_MT6592_LK_FB_HEIGHT;
    volatile uint32_t* pixels = (volatile uint32_t*)(uintptr_t)fb;
    mt6592_panel_bringup_cfg_t cfg;
    int rc;

    /* The logo if the slot carries one, black if it does not. While the DSI host
     * was dead this was a colour, so that "the DDP route works" and "the panel
     * is merely backlit" could not read the same; now that the route is proven,
     * the boot policy below owns the screen and the plugged-with-no-cell case is
     * specified as black.
     *
     * Filled before the bring-up rather than just before the scanout: nothing
     * reads this surface until the OVL is armed at the end of it, and the
     * backlight is the step after that -- so whatever is put here is what the
     * panel lights up on, with no intermediate frame in between. */
    if (!lk_fb_paint_logo()) {
        for (uint32_t i = 0; i < fb_bytes / 4u; ++i) pixels[i] = 0xff000000u;
        mt6592_lcd_clean_fb((uintptr_t)fb, (uintptr_t)fb_bytes);
    }

    mt6592_panel_bringup_defaults(&cfg);
    rc = mt6592_panel_bringup(&cfg, lk_panel_log, 0);
    if (rc != 0) return rc;
    g_fb_live = 1u;

    /* The backlight is the last step of the bring-up, so this is the instant the
     * logo became visible -- not when it was painted, which was before any of
     * this and behind a dark panel. Everything the splash dwell is measured
     * against starts here. */
    g_splash_ticks = mt6592_delay_gpt_ticks();
    g_splash_lit = 1u;

    /* The same twelve DSI words the panel-program failure path dumps, plus the
     * sixteen DDP ones, but taken *after* video mode is armed, which is the
     * only moment they answer the question that matters: mode_ctrl should read
     * the video mode from g_lcm (2), start should have bit 0 set and stay set,
     * phy_lccon should show the clock lane in HS, and com_ctrl must have DSI_EN
     * (bit1) — a dump from before the mode switch cannot distinguish a host
     * that is transmitting from one that emitted a single frame and stopped.
     *
     * Compare the DDP half against the stock profile in mt6592_board_j36.h
     * (MT6592_J36_LK_HANDOFF_*); mutex0 must read back with ACQUIRED (bit1)
     * clear or the path is frozen. */
    mt6592_panel_dump(lk_panel_log, 0);
    return 0;
}

#ifndef MVII_MT6592_LK_RELEASE
/* The two things the console can drive but does not own, in one table shared by
 * both callers: the early boot-time trigger (held combo or eMMC flag) and the
 * charge park's double-tap. It sits at file scope rather than inside either
 * because they hand the console the SAME capabilities and a second copy is a
 * second thing to forget to update.
 *
 * Still the only reference to mvii_debug_console_run() in the image, which is
 * what lets --gc-sections take the console, the MUSB gadget and the Wi-Fi stack
 * out of the release build -- see the #ifdef at the boot-time call site. */
static const mvii_debug_console_hooks_t kConsoleHooks = {
    lk_display_on,
    lk_fb_fill,
};
#endif

/* ── Charge park ──
 *
 * The one piece of policy in this image, and the reason it is policy rather
 * than the stock LK's is worth stating: the stock bootloader parks on a charger
 * power-on cause and its only exits are "unplugged -> power off" and "power key
 * -> reboot", so a board with a flat or missing cell never reaches an OS. This
 * parks on the same condition and exits *forward*, into the boot it was already
 * going to do.
 *
 *   plugged, no cell   plug + bolt, no percentage, wait for the power key
 *   plugged, cell      battery gauge, percentage, plug badge, wait for the key
 *   not plugged        no park at all; boot straight through
 *   unplugged in park  grey gauge for 5 s, then boot -- the same answer as above,
 *                      because it is the same board, five seconds later
 *   MENU in the park   the detail screen: decimal percent + four live gauges
 *
 * The panel runs at half brightness throughout -- the backlight draws from the
 * same node the charger feeds -- and goes to full at exactly one point, the
 * handoff, where the OS is about to own it anyway. See
 * MVII_MT6592_LK_HANDOFF_BACKLIGHT_PCT.
 *
 * "Plugged" is CHRDET, which is a real comparator and is trustworthy. Whether a
 * cell is fitted is not (see mt6592_pmic.c): so presence decides only what is
 * drawn, never whether the board boots or how it exits. An UNKNOWN answer draws
 * the gauge, since UNKNOWN is only reachable while pwrap is still warming up.
 *
 * The cell-less case used to be a black screen with the backlight off, on the
 * argument that the backlight was the largest load left and had nothing to
 * light. That was true and still unhelpful: a dark panel on a board that is
 * plugged in and healthy is indistinguishable from a board that is dead. It now
 * shows the plug, which is the one true statement available in that state.
 *
 * The unplug path exits forward, like every other path out of the park. The
 * frame with the broken cable goes up, it is held long enough to be read, and
 * then the loader boots. It used to drop the backlight and keep looping instead,
 * on the argument that the board should "stay warm" for a power key that would
 * boot it forward later. That was the one place in this loader where reaching
 * the code did not mean reaching an OS: on battery -- which is the only supply
 * an unplugged board has -- it left a dark, awake, draining machine that looked
 * exactly like a dead one, and it needed a key press nobody has a reason to try
 * on something that appears to be off. There is no state in here now that the
 * board does not leave on its own.
 */

/* 3x5 digits plus '%', a lightning bolt, and the five shapes the detail screen
 * needs to label a gauge -- '.', '-', and the unit letters m, V, A. Bit 2
 * leftmost. Small enough to be a table rather than a font, large enough at scale
 * 8 to read across a room.
 *
 * There is deliberately no general alphabet here. The detail screen says what a
 * gauge measures with an ICON -- a plug or a battery -- and only the unit needs
 * letters, so this is five shapes rather than a font nobody asked for. */
static const uint8_t kGlyphs[17][5] = {
    {7u, 5u, 5u, 5u, 7u}, /* 0 */
    {2u, 6u, 2u, 2u, 7u}, /* 1 */
    {7u, 1u, 7u, 4u, 7u}, /* 2 */
    {7u, 1u, 7u, 1u, 7u}, /* 3 */
    {5u, 5u, 7u, 1u, 1u}, /* 4 */
    {7u, 4u, 7u, 1u, 7u}, /* 5 */
    {7u, 4u, 7u, 5u, 7u}, /* 6 */
    {7u, 1u, 1u, 1u, 1u}, /* 7 */
    {7u, 5u, 7u, 5u, 7u}, /* 8 */
    {7u, 5u, 7u, 1u, 7u}, /* 9 */
    {5u, 1u, 2u, 4u, 5u}, /* % */
    {3u, 2u, 7u, 2u, 6u}, /* bolt: mass top-right, crossbar, mass bottom-left */
    {0u, 0u, 0u, 0u, 2u}, /* . — one cell on the baseline                     */
    {0u, 0u, 7u, 0u, 0u}, /* -                                                */
    {0u, 5u, 7u, 7u, 5u}, /* m — two stems and a shared top                   */
    {5u, 5u, 5u, 5u, 2u}, /* V                                                */
    {2u, 5u, 7u, 5u, 5u}, /* A                                                */
};

#define LK_GLYPH_PERCENT 10u
#define LK_GLYPH_BOLT 11u
#define LK_GLYPH_DOT 12u
#define LK_GLYPH_MINUS 13u
#define LK_GLYPH_M 14u
#define LK_GLYPH_V 15u
#define LK_GLYPH_A 16u

/*
 * ══════════════════════════════════════════════════════════════════════════
 * THE SHADOW CANVAS, AND WHY THE SCREEN FLICKERED ANYWAY
 * ══════════════════════════════════════════════════════════════════════════
 *
 * 0x82700000 is not a back buffer. The OVL scans it out continuously, so every
 * store this file makes is on screen the moment it lands, and the panel shows
 * whatever the drawing code happens to be part-way through.
 *
 * That makes clear-then-redraw visible BY CONSTRUCTION. The clear is a real
 * picture -- a black hole where the number was -- and it is displayed for as long
 * as the redraw takes. The MMU is off in this loader (see the note in
 * lk_charge_park's caller), so every one of those stores is Strongly-Ordered:
 * nothing is buffered, nothing coalesces, and a store costs what a store to DRAM
 * costs. Shrinking the cleared area from the whole screen to three boxes cut that
 * interval by about four times and left it plainly visible, because the defect was
 * never the SIZE of the clear. It was that the clear is on the scanned-out surface
 * at all.
 *
 * So compose somewhere nobody is looking, then move finished pixels. g_draw_base
 * points the primitives at the shadow while a frame is being built; the presents
 * below copy the rectangles that changed to the live canvas. A live pixel now goes
 * from its old final value straight to its new one in a single store, and never
 * holds an intermediate. There is no interval left to see.
 *
 * The sweep down the copy is not a tear worth the name: consecutive gauge frames
 * differ only in the digits and the bar, so the top and bottom halves of the copy
 * agree with each other everywhere except the handful of pixels that were supposed
 * to change.
 *
 * This deliberately does NOT flip the OVL's layer base between two buffers, which
 * would be untearable rather than merely invisible. That path needs a register
 * write and a mutex acquire inside the DDP, and mtk_ddp_commit_lk()'s own comment
 * records two different ways this panel has already been blanked by getting that
 * pair wrong. A memcpy cannot blank a panel.
 */
/*
 * ── WHERE THE SHADOW MAY LIVE, AND THE ONE PLACE IT MAY NOT ──
 *
 * This was 0x82900000, which is MVII_ASSET_STAGE_ADDR. The asset container is
 * staged there, and the slot renderer's own first act is a full-screen clear of
 * the compose target -- so it painted 1.2 MB of black over the blob it was about
 * to read its glyphs and its ENTRY TABLE out of, having already taken pointers
 * into that table. Nothing recognisable was drawn, and the splash stayed on the
 * panel: "no battery screen" was this, and only this.
 *
 * 0x82b00000 is the first address the asset stage cannot reach (its cap is 2 MB
 * and an over-large container is refused rather than staged), and it ends at
 * 0x82c2c000 -- clear of the firmware scratch at 0x83000000, and above stage1's
 * RAMDISK_SCAN_END so the ramdisk magic scan cannot mistake it for an image.
 *
 * The check below is the point of the story. A framebuffer-sized scratch buffer
 * chosen by reading a memory map is a buffer that will be chosen wrongly again
 * the next time the map changes, and the failure is silent -- black pixels over
 * live data, no fault, no log.
 */
#ifndef MVII_MT6592_LK_SHADOW_ADDR
#define MVII_MT6592_LK_SHADOW_ADDR 0x82b00000u
#endif

#define MVII_LK_SHADOW_BYTES ((uint32_t)MVII_MT6592_LK_FB_PITCH * (uint32_t)MVII_MT6592_LK_FB_HEIGHT)

/* C99 has no static_assert; a zero-width array does the same job here. Both
 * conditions are "the shadow starts after the other buffer ends", one per
 * direction, which together mean the two ranges are disjoint. */
typedef char mvii_lk_shadow_clears_assets_t
    [(MVII_MT6592_LK_SHADOW_ADDR >= MVII_ASSET_STAGE_ADDR + MVII_ASSET_STAGE_MAX ||
      MVII_MT6592_LK_SHADOW_ADDR + MVII_LK_SHADOW_BYTES <= MVII_ASSET_STAGE_ADDR)
         ? 1
         : -1];
typedef char mvii_lk_shadow_clears_fb_t
    [(MVII_MT6592_LK_SHADOW_ADDR >= (uint32_t)MVII_MT6592_LK_FB_ADDR + MVII_LK_SHADOW_BYTES ||
      MVII_MT6592_LK_SHADOW_ADDR + MVII_LK_SHADOW_BYTES <= (uint32_t)MVII_MT6592_LK_FB_ADDR)
         ? 1
         : -1];
/* And the third claim on this stretch of DRAM: the Mali job buffers, which
 * mvii_lk_linker.ld places at 0x82d00000 for 3 MiB because they are far too big
 * for the LK's own 512 KiB window. Repeated here rather than shared because the
 * linker script cannot be included from C -- so the number is duplicated, and
 * this is what makes the duplicate load-bearing instead of decorative. The GP
 * writes that region while the PP reads it; a shadow canvas overlapping it is
 * torn tiles on screen with nothing in any log. */
#define MVII_LK_GPU_DRAM_ADDR 0x82d00000u
#define MVII_LK_GPU_DRAM_BYTES 0x00300000u
typedef char mvii_lk_shadow_clears_gpu_t
    [(MVII_MT6592_LK_SHADOW_ADDR >= MVII_LK_GPU_DRAM_ADDR + MVII_LK_GPU_DRAM_BYTES ||
      MVII_MT6592_LK_SHADOW_ADDR + MVII_LK_SHADOW_BYTES <= MVII_LK_GPU_DRAM_ADDR)
         ? 1
         : -1];
typedef char mvii_lk_gpu_clears_assets_t
    [(MVII_LK_GPU_DRAM_ADDR >= MVII_ASSET_STAGE_ADDR + MVII_ASSET_STAGE_MAX ||
      MVII_LK_GPU_DRAM_ADDR + MVII_LK_GPU_DRAM_BYTES <= MVII_ASSET_STAGE_ADDR)
         ? 1
         : -1];

/* Where the primitives draw. The live canvas by default, so every existing caller
 * that just wants pixels on screen -- the splash, the solid fills, the boot-status
 * paints -- is unchanged and pays nothing. */
static uint32_t g_draw_base = (uint32_t)MVII_MT6592_LK_FB_ADDR;

static void lk_fb_target(uint32_t base) { g_draw_base = base; }

/* The one copy shadow -> live canvas. Half-open box, already clipped. */
static void lk_fb_blit_live(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    const uint32_t stride = (uint32_t)MVII_MT6592_LK_FB_PITCH / 4u;
    const uint32_t* src = (const uint32_t*)(uintptr_t)MVII_MT6592_LK_SHADOW_ADDR;
    volatile uint32_t* dst = (volatile uint32_t*)(uintptr_t)MVII_MT6592_LK_FB_ADDR;

    for (uint32_t yy = y0; yy < y1; ++yy) {
        const uint32_t row = yy * stride;
        for (uint32_t xx = x0; xx < x1; ++xx) dst[row + xx] = src[row + xx];
    }
}

/*
 * ── HOLDING A COMPOSED FRAME BACK ──
 *
 * Composing off-screen buys one more thing besides an invisible redraw: the
 * finished frame can be made to WAIT. With presents deferred, the whole charge
 * screen is built while the splash is still the picture on the panel, and the
 * flush later moves it across in a single sweep -- so the splash gets its full
 * dwell and the gauge appears the instant that dwell ends, instead of the dwell
 * being dead time and the first frame being drawn after it.
 *
 * The union of the deferred boxes is what gets copied, not the boxes one by one.
 * A held frame is always a full compose in practice (`full' is set on the first
 * paint), and one rectangle costs one pass over the rows rather than three.
 */
static int g_present_deferred;
static uint32_t g_dirty_x0, g_dirty_y0, g_dirty_x1, g_dirty_y1;

static void lk_fb_defer_present(int on) {
    g_present_deferred = on;
    if (on) {
        g_dirty_x0 = 0xffffffffu;
        g_dirty_y0 = 0xffffffffu;
        g_dirty_x1 = 0u;
        g_dirty_y1 = 0u;
    }
}

/* Copy one rectangle from the shadow to the live canvas, or remember it for the
 * flush while presents are deferred. Clipped the same way lk_fb_rect() clips, so
 * a caller may hand it a box that runs off the edge. */
static void lk_fb_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t x1 = x + w;
    uint32_t y1 = y + h;

    if (x1 > (uint32_t)MVII_MT6592_LK_FB_WIDTH) x1 = (uint32_t)MVII_MT6592_LK_FB_WIDTH;
    if (y1 > (uint32_t)MVII_MT6592_LK_FB_HEIGHT) y1 = (uint32_t)MVII_MT6592_LK_FB_HEIGHT;
    if (x >= x1 || y >= y1) return;

    if (!g_present_deferred) {
        lk_fb_blit_live(x, y, x1, y1);
        return;
    }

    if (x < g_dirty_x0) g_dirty_x0 = x;
    if (y < g_dirty_y0) g_dirty_y0 = y;
    if (x1 > g_dirty_x1) g_dirty_x1 = x1;
    if (y1 > g_dirty_y1) g_dirty_y1 = y1;
}

/* Put the held frame on the panel. Does nothing if nothing was composed, which
 * is the case the caller must not have to special-case: a compose that bailed
 * out leaves the splash up rather than a half-picture. */
static void lk_fb_flush_present(void) {
    const uint32_t x0 = g_dirty_x0, y0 = g_dirty_y0;
    const uint32_t x1 = g_dirty_x1, y1 = g_dirty_y1;

    g_dirty_x0 = 0xffffffffu;
    g_dirty_y0 = 0xffffffffu;
    g_dirty_x1 = 0u;
    g_dirty_y1 = 0u;
    if (x0 >= x1 || y0 >= y1) return;

    lk_fb_blit_live(x0, y0, x1, y1);
    mt6592_lcd_clean_fb((uintptr_t)MVII_MT6592_LK_FB_ADDR, (uintptr_t)MVII_LK_SHADOW_BYTES);
}

static void lk_fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb) {
    const uint32_t stride = (uint32_t)MVII_MT6592_LK_FB_PITCH / 4u;
    volatile uint32_t* px = (volatile uint32_t*)(uintptr_t)g_draw_base;
    uint32_t x1 = x + w;
    uint32_t y1 = y + h;

    if (x1 > (uint32_t)MVII_MT6592_LK_FB_WIDTH) x1 = (uint32_t)MVII_MT6592_LK_FB_WIDTH;
    if (y1 > (uint32_t)MVII_MT6592_LK_FB_HEIGHT) y1 = (uint32_t)MVII_MT6592_LK_FB_HEIGHT;

    for (uint32_t yy = y; yy < y1; ++yy) {
        for (uint32_t xx = x; xx < x1; ++xx) px[yy * stride + xx] = argb;
    }
}

static void lk_fb_glyph(uint32_t index, uint32_t x, uint32_t y, uint32_t scale, uint32_t argb) {
    for (uint32_t row = 0; row < 5u; ++row) {
        const uint32_t bits = kGlyphs[index][row];
        for (uint32_t col = 0; col < 3u; ++col) {
            if (bits & (1u << (2u - col))) {
                lk_fb_rect(x + col * scale, y + row * scale, scale, scale, argb);
            }
        }
    }
}

/* Centre "<pct>%" horizontally at row y. Glyph cell is 3*scale wide with one
 * scale of tracking, so the run is n*4*scale - scale wide. */
static void lk_fb_percent(uint32_t pct, uint32_t y, uint32_t scale, uint32_t argb) {
    uint32_t digits[3];
    uint32_t n = 0;
    uint32_t run;
    uint32_t x;

    if (pct > 100u) pct = 100u;
    if (pct == 0u) {
        digits[n++] = 0u;
    } else {
        uint32_t v = pct;
        uint32_t tmp[3];
        uint32_t t = 0;
        while (v > 0u && t < 3u) {
            tmp[t++] = v % 10u;
            v /= 10u;
        }
        while (t > 0u) digits[n++] = tmp[--t];
    }

    run = (n + 1u) * 4u * scale - scale;
    x = ((uint32_t)MVII_MT6592_LK_FB_WIDTH - run) / 2u;
    for (uint32_t i = 0; i < n; ++i) {
        lk_fb_glyph(digits[i], x + i * 4u * scale, y, scale, argb);
    }
    lk_fb_glyph(LK_GLYPH_PERCENT, x + n * 4u * scale, y, scale, argb);
}

/*
 * A RUN OF GLYPH CELLS, MEASURED BEFORE IT IS DRAWN.
 *
 * lk_fb_percent() above centres a number by computing its own span. That works
 * for one number in the middle of a screen and not at all for a column of values
 * that have to line up on their right edge while changing width -- 999 mA to
 * 1000 mA moves every pixel of the run. So a run is built first, measured, and
 * then placed, which is also what lets the clear box be sized from the WIDEST
 * run a field can hold rather than from the one on screen now.
 *
 * A cell is 3*scale wide with one scale of tracking after it, so n cells span
 * n*4*scale - scale -- the same geometry lk_fb_percent() uses, kept identical so
 * the two never disagree about where a digit sits.
 */
enum { LK_RUN_MAX = 12u };

typedef struct {
    uint8_t g[LK_RUN_MAX];
    uint32_t n;
} lk_run_t;

static void lk_run_reset(lk_run_t* r) { r->n = 0u; }

static void lk_run_push(lk_run_t* r, uint32_t glyph) {
    if (r->n < LK_RUN_MAX) r->g[r->n++] = (uint8_t)glyph;
}

/* Decimal digits, most significant first. Zero is "0", not nothing. */
static void lk_run_push_u32(lk_run_t* r, uint32_t v) {
    uint32_t tmp[LK_RUN_MAX];
    uint32_t t = 0u;

    if (v == 0u) {
        lk_run_push(r, 0u);
        return;
    }
    while (v > 0u && t < LK_RUN_MAX) {
        tmp[t++] = v % 10u;
        v /= 10u;
    }
    while (t > 0u) lk_run_push(r, tmp[--t]);
}

static uint32_t lk_run_width(const lk_run_t* r, uint32_t scale) {
    return (r->n == 0u) ? 0u : r->n * 4u * scale - scale;
}

static void lk_run_draw(const lk_run_t* r, uint32_t x, uint32_t y, uint32_t scale,
                        uint32_t argb) {
    for (uint32_t i = 0; i < r->n; ++i) {
        lk_fb_glyph(r->g[i], x + i * 4u * scale, y, scale, argb);
    }
}

/* Right edge @p right, so a field of changing width stays anchored. A run wider
 * than its own right edge would wrap to a negative x, so it is left-clamped. */
static void lk_run_draw_right(const lk_run_t* r, uint32_t right, uint32_t y, uint32_t scale,
                              uint32_t argb) {
    const uint32_t width = lk_run_width(r, scale);
    lk_run_draw(r, (right > width) ? (right - width) : 0u, y, scale, argb);
}

/* Mains plug with a trailing cable, on a 6x10 cell grid @p s pixels per cell.
 * Same silhouette as the OS status bar's drawPlug565(), so the loader and the
 * dashboard never draw two different symbols for one fact.
 *
 * @p connected 0 breaks the cable: a stub off the body, a gap, then the loose
 * end kinked sideways. That is the frame the park puts up when the charger comes
 * out, and it is the last thing on the panel before the backlight goes.
 * @p bolt punches a lightning glyph out of the body in the background colour --
 * a bolt in the body's own ink would not be there at all. */
static void lk_fb_plug(uint32_t x, uint32_t y, uint32_t s, int connected, int bolt,
                       uint32_t argb) {
    lk_fb_rect(x + 1u * s, y, s, 2u * s, argb);      /* left prong  */
    lk_fb_rect(x + 4u * s, y, s, 2u * s, argb);      /* right prong */
    lk_fb_rect(x, y + 2u * s, 6u * s, 4u * s, argb); /* body        */

    if (connected) {
        lk_fb_rect(x + 2u * s, y + 6u * s, 2u * s, 4u * s, argb);
    } else {
        lk_fb_rect(x + 2u * s, y + 6u * s, 2u * s, s, argb);      /* stub      */
        lk_fb_rect(x + 4u * s, y + 8u * s, 2u * s, 2u * s, argb); /* loose end */
    }

    if (bolt) {
        const uint32_t bs = (4u * s - 2u) / 5u;
        if (bs > 0u) {
            lk_fb_glyph(LK_GLYPH_BOLT, x + (6u * s - 3u * bs) / 2u,
                        y + 2u * s + (4u * s - 5u * bs) / 2u, bs, 0xff000000u);
        }
    }
}

/*
 * A battery outline with a proportional fill and the percentage under it.
 *
 * ── THIS IS THE ONLY PARK PICTURE. ──
 *
 * There used to be a second one, lk_draw_mains_screen(): a bare plug, no
 * outline and no percentage, shown whenever the level could not be attributed
 * to a cell. The reasoning was that a gauge drawn at the charger's own output
 * voltage is a lie. It is -- but a blank where the number should be is a worse
 * one, because it is not readable AS a refusal. What the operator saw was a
 * board that would only tell them the charge if they pulled the cable out, and
 * pulling the cable out is the one thing that makes a charge reading urgent.
 *
 * So the number is always drawn. What it is worth is carried by the badge and
 * the ink instead of by withholding it -- see the call sites in lk_charge_park.
 *
 * @p fill_argb  the bar, same colour the centre LED is showing, so the two
 *               never disagree about what they are reporting.
 * @p ink_argb   outline, numerals and badge. Grey is how the park says the
 *               charger is gone and this frame is the last one.
 * @p cable      1 plug with the cable joined, 0 plug with it broken,
 *               -1 no badge at all.
 * @p bolt       punch a lightning glyph out of the plug body. Drawn when the
 *               node sits BELOW the port's baseline, which is the one state
 *               where something is actually taking the charge; withheld at or
 *               above it, where a full pack and an empty holder are the same
 *               node and "charging" would be a claim neither has earned.
 *
 * ── @p full, AND WHY THE SCREEN USED TO FLICKER ──
 *
 * There is one framebuffer and the OVL scans out of it CONTINUOUSLY. Nothing here
 * is double-buffered, so a pixel written is a pixel on the panel, immediately and
 * on whatever scanline the display controller happens to be fetching.
 *
 * This function used to open with lk_fb_fill(0xff000000) unconditionally. That is
 * 307,200 words written by a CPU whose MMU is off -- every store Strongly-Ordered,
 * un-coalesced, un-cached -- and it is written INTO THE SURFACE BEING DISPLAYED.
 * So each one-percent step put a solid black frame on the panel and then let the
 * artwork reappear over it a scanline at a time. The operator reported it as a
 * flicker and guessed the update rate was too slow. The rate was fine, at one
 * repaint per changed percent; the fault was clearing the whole live surface to
 * redraw a number in the middle of it.
 *
 * Confining the clear was not enough on its own -- a smaller blink is still a
 * blink -- so the frame is now composed on the shadow canvas and only finished
 * rectangles are copied to the live one. See the block above lk_fb_rect(). @p full
 * still decides HOW MUCH gets composed and presented: 0 means the three boxes the
 * picture occupies -- the battery, the widest span the number can take, and the
 * badge -- against a background that is already black and stays untouched; 1 means
 * the whole screen, and is required whenever a frame is not a same-layout
 * restatement of its predecessor: the first paint, coming back from the unplugged
 * park, or the grey frame, whose badge is a different asset entirely.
 */
static void lk_draw_charge_screen_builtin(uint32_t pct, uint32_t fill_argb, uint32_t ink_argb,
                                          int cable, int bolt, int full) {
    const uint32_t w = (uint32_t)MVII_MT6592_LK_FB_WIDTH;
    const uint32_t h = (uint32_t)MVII_MT6592_LK_FB_HEIGHT;
    const uint32_t body_w = 300u;
    const uint32_t body_h = 140u;
    const uint32_t border = 6u;
    const uint32_t nub_w = 16u;
    const uint32_t nub_h = 56u;
    const uint32_t body_x = (w - (body_w + nub_w)) / 2u;
    const uint32_t body_y = h / 2u - body_h - 20u;
    const uint32_t inner_x = body_x + border * 2u;
    const uint32_t inner_y = body_y + border * 2u;
    const uint32_t inner_w = body_w - border * 4u;
    const uint32_t inner_h = body_h - border * 4u;
    const uint32_t num_s = 10u;
    const uint32_t num_y = body_y + body_h + 48u;
    /* lk_fb_percent()'s run is (n+1)*4*scale - scale and it centres it, so the
     * three-digit span CONTAINS the one- and two-digit ones. Clearing the widest
     * clears wherever a narrower number could have been. */
    const uint32_t num_w = 15u * num_s;
    const uint32_t plug_s = 9u;
    const uint32_t plug_y = num_y + 5u * num_s + 40u;
    uint32_t fill_w;

    if (pct > 100u) pct = 100u;

    /* Everything from here to the presents at the bottom lands on the shadow. The
     * live canvas is not touched at all while the picture is half-built. */
    lk_fb_target((uint32_t)MVII_MT6592_LK_SHADOW_ADDR);

    if (full) {
        lk_fb_rect(0u, 0u, w, h, 0xff000000u);
    } else {
        /* Only the three boxes the picture actually occupies. */
        lk_fb_rect(body_x, body_y, body_w + nub_w, body_h, 0xff000000u);
        lk_fb_rect((w - num_w) / 2u, num_y, num_w, 5u * num_s, 0xff000000u);
        if (cable >= 0) {
            lk_fb_rect((w - 6u * plug_s) / 2u, plug_y, 6u * plug_s, 10u * plug_s, 0xff000000u);
        }
    }

    /* Outline as four bars rather than a filled-then-hollowed rectangle: the
     * framebuffer is 1.2 MB and every pixel written here is written by a CPU
     * with its D-cache off. */
    lk_fb_rect(body_x, body_y, body_w, border, ink_argb);
    lk_fb_rect(body_x, body_y + body_h - border, body_w, border, ink_argb);
    lk_fb_rect(body_x, body_y, border, body_h, ink_argb);
    lk_fb_rect(body_x + body_w - border, body_y, border, body_h, ink_argb);
    lk_fb_rect(body_x + body_w, body_y + (body_h - nub_h) / 2u, nub_w, nub_h, ink_argb);

    fill_w = inner_w * pct / 100u;
    if (pct > 0u && fill_w == 0u) fill_w = 1u; /* never show an empty cell as flat-zero */
    if (fill_w > 0u) lk_fb_rect(inner_x, inner_y, fill_w, inner_h, fill_argb);

    lk_fb_percent(pct, num_y, num_s, ink_argb);

    if (cable >= 0) {
        lk_fb_plug((w - 6u * plug_s) / 2u, plug_y, plug_s, cable, bolt, ink_argb);
    }

    /* Finished. Hand the primitives back to the live canvas and move the pixels --
     * exactly the boxes that were composed, nothing else. */
    lk_fb_target((uint32_t)MVII_MT6592_LK_FB_ADDR);

    if (full) {
        lk_fb_present(0u, 0u, w, h);
    } else {
        lk_fb_present(body_x, body_y, body_w + nub_w, body_h);
        lk_fb_present((w - num_w) / 2u, num_y, num_w, 5u * num_s);
        if (cable >= 0) {
            lk_fb_present((w - 6u * plug_s) / 2u, plug_y, 6u * plug_s, 10u * plug_s);
        }
    }

    mt6592_lcd_clean_fb((uintptr_t)MVII_MT6592_LK_FB_ADDR,
                        (uintptr_t)((uint32_t)MVII_MT6592_LK_FB_PITCH * h));
}

/*
 * ── THE SAME PICTURE, OUT OF THE ASSET SLOT ──
 *
 * Identical policy to the function above — same states, same colours, same
 * decisions — drawn from anti-aliased coverage masks on the LOGO partition
 * instead of from axis-aligned rectangles and a 3x5 font scaled ten times.
 *
 * NOTHING HERE IS A HARDCODED SIZE. Every dimension is read out of the entries:
 * the stack is centred from the assets' own heights, the fill is placed by the
 * battery's `inset' and revealed by clipping it, and the bolt is centred in the
 * plug's inset (the plug's BODY, not its silhouette — centring on the whole
 * shape puts the bolt in the gap between the prongs). That is what makes new art
 * a flash rather than a build: change the battery on the host, make it rounder
 * or taller or a different aspect, and this lays it out correctly with no edit.
 *
 * Returns -1 if any piece is missing, and the caller falls back whole rather
 * than in part: half a picture from the slot and half from the rectangles would
 * be worse than either.
 *
 * @p tracking, @p gaps  the only numbers this file still owns, because they are
 * typography and layout rather than art.
 */
enum {
    LK_ASSET_TRACKING = 6u,  /* between numerals                              */
    LK_ASSET_GAP_NUM = 40u,  /* battery to percentage                         */
    LK_ASSET_GAP_PLUG = 30u  /* percentage to badge                           */
};

static int lk_draw_charge_screen_slot(uint32_t pct, uint32_t fill_argb, uint32_t ink_argb,
                                      int cable, int bolt, int full) {
    /* The shadow, not the live canvas: the asset blits compose there and the
     * presents at the bottom move the finished boxes across. See the block above
     * lk_fb_rect(). */
    const uint32_t fb = (uint32_t)MVII_MT6592_LK_SHADOW_ADDR;
    const uint32_t pitch = (uint32_t)MVII_MT6592_LK_FB_PITCH;
    const uint32_t w = (uint32_t)MVII_MT6592_LK_FB_WIDTH;
    const uint32_t h = (uint32_t)MVII_MT6592_LK_FB_HEIGHT;
    const uint32_t plug_id = cable ? MVII_ASSET_PLUG : MVII_ASSET_PLUG_BROKEN;
    int whole;
    const mvii_asset_entry_t* bat;
    const mvii_asset_entry_t* fill;
    const mvii_asset_entry_t* zero;
    const mvii_asset_entry_t* plug = 0;
    uint32_t stack;
    uint32_t num_w;
    uint32_t wide;
    uint32_t bx, by, ty, py;
    uint32_t fill_w;

    if (!mvii_assets_ready()) return -1;
    if (pct > 100u) pct = 100u;

    bat = mvii_asset_find(MVII_ASSET_BATTERY);
    fill = mvii_asset_find(MVII_ASSET_BATTERY_FILL);
    zero = mvii_asset_find(MVII_ASSET_DIGIT(0));
    if (!bat || !fill || !zero) return -1;
    if (bat->inset_w == 0u || bat->inset_h == 0u) return -1;

    num_w = mvii_asset_percent_width(pct, LK_ASSET_TRACKING);
    if (num_w == 0u) return -1;

    if (cable >= 0) {
        plug = mvii_asset_find(plug_id);
        if (!plug) return -1;
    }

    stack = (uint32_t)bat->height + LK_ASSET_GAP_NUM + (uint32_t)zero->height;
    if (plug) stack += LK_ASSET_GAP_PLUG + (uint32_t)plug->height;
    if (stack > h) return -1;

    by = (h - stack) / 2u;
    bx = (w - (uint32_t)bat->width) / 2u;
    ty = by + (uint32_t)bat->height + LK_ASSET_GAP_NUM;
    py = ty + (uint32_t)zero->height + LK_ASSET_GAP_PLUG;

    /* mvii_asset_percent_width() shrinks with the digit count and the run is
     * centred, so the widest span -- "100%" -- contains every narrower one. Clear
     * that, not the number's own box, or a 100 -> 99 step would leave the outer
     * digits of the old number standing.
     *
     * Zero means the widest span is unmeasurable (a digit absent from the slot
     * that the CURRENT number happens not to use), and an unmeasurable span is
     * not one this can clear around: fall back to clearing everything. */
    wide = mvii_asset_percent_width(100u, LK_ASSET_TRACKING);
    whole = (full || wide == 0u);

    lk_fb_target((uint32_t)MVII_MT6592_LK_SHADOW_ADDR);

    if (whole) {
        lk_fb_rect(0u, 0u, w, h, 0xff000000u);
    } else {
        lk_fb_rect(bx, by, (uint32_t)bat->width, (uint32_t)bat->height, 0xff000000u);
        lk_fb_rect((w - wide) / 2u, ty, wide, (uint32_t)zero->height, 0xff000000u);
        if (plug) {
            lk_fb_rect((w - (uint32_t)plug->width) / 2u, py, (uint32_t)plug->width,
                       (uint32_t)plug->height, 0xff000000u);
        }
    }

    /* The bar first, then the outline over it. Drawing the outline first and the
     * bar second would put the bar's own anti-aliased corner on top of the
     * outline's inner edge, and two soft edges stacked read as a smudge.
     *
     * Both are redrawn even when only the bar's length changed, because an alpha
     * blit is not idempotent: laying the outline over itself blends its soft edge
     * into itself and hardens it a little more every second. Clearing the box is
     * what makes the blit repeatable, and it is why the battery cannot be narrowed
     * to just the bar. */
    fill_w = (uint32_t)fill->width * pct / 100u;
    if (pct > 0u && fill_w == 0u) fill_w = 1u; /* never show a live cell as flat zero */
    if (fill_w > 0u) {
        (void)mvii_asset_blit_a8_clip(MVII_ASSET_BATTERY_FILL, fb, pitch, w, h,
                                      (int)(bx + bat->inset_x), (int)(by + bat->inset_y), fill_w,
                                      fill_argb);
    }
    (void)mvii_asset_blit_a8(MVII_ASSET_BATTERY, fb, pitch, w, h, (int)bx, (int)by, ink_argb);

    (void)mvii_asset_draw_percent(pct, fb, pitch, w, h, (int)((w - num_w) / 2u), (int)ty,
                                  LK_ASSET_TRACKING, ink_argb);

    if (plug) {
        const uint32_t px = (w - (uint32_t)plug->width) / 2u;
        (void)mvii_asset_blit_a8(plug_id, fb, pitch, w, h, (int)px, (int)py, ink_argb);
        if (bolt) {
            /* Punched out in the background colour: a bolt drawn in the body's
             * own ink would not be there at all. */
            const mvii_asset_entry_t* lit = mvii_asset_find(MVII_ASSET_BOLT);
            if (lit && plug->inset_w >= lit->width && plug->inset_h >= lit->height) {
                (void)mvii_asset_blit_a8(
                    MVII_ASSET_BOLT, fb, pitch, w, h,
                    (int)(px + plug->inset_x + (plug->inset_w - lit->width) / 2u),
                    (int)(py + plug->inset_y + (plug->inset_h - lit->height) / 2u), 0xff000000u);
            }
        }
    }

    /* Composed. Move exactly what was composed, and nothing else. */
    lk_fb_target((uint32_t)MVII_MT6592_LK_FB_ADDR);

    if (whole) {
        lk_fb_present(0u, 0u, w, h);
    } else {
        lk_fb_present(bx, by, (uint32_t)bat->width, (uint32_t)bat->height);
        lk_fb_present((w - wide) / 2u, ty, wide, (uint32_t)zero->height);
        if (plug) {
            lk_fb_present((w - (uint32_t)plug->width) / 2u, py, (uint32_t)plug->width,
                          (uint32_t)plug->height);
        }
    }

    mt6592_lcd_clean_fb((uintptr_t)MVII_MT6592_LK_FB_ADDR, (uintptr_t)(pitch * h));
    return 0;
}

static void lk_draw_charge_screen(uint32_t pct, uint32_t fill_argb, uint32_t ink_argb, int cable,
                                  int bolt, int full) {
    if (lk_draw_charge_screen_slot(pct, fill_argb, ink_argb, cable, bolt, full) == 0) return;
    lk_draw_charge_screen_builtin(pct, fill_argb, ink_argb, cable, bolt, full);
}

/*
 * ══════════════════════════════════════════════════════════════════════════
 * THE DETAIL SCREEN
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Four live gauges and a level with a decimal, on the MENU key, over the top of
 * the charge park. It exists because a percent of this pack is about ninety
 * seconds of charging: the ordinary screen is CORRECT and looks frozen, and the
 * only way to tell those two apart from the outside is to see the inputs.
 *
 * WHAT IT SHOWS, AND WHAT EACH NUMBER IS WORTH:
 *
 *   plug    mV   VCHR. The cable's own voltage, measured, live.
 *   plug    mA   the CS_VTH step CHR_CON4 is programmed to. NOT a measurement --
 *                there is no input-side shunt on this board, so what the port
 *                delivers is not observable. It is what the charger was asked
 *                for, and it moves when BC1.2 re-negotiates.
 *   battery mV   BATSNS. Measured, live, and with a cable in it is the rail as
 *                much as the cell (no power-path FET).
 *   battery mA   the shunt, VSEN - BATSNS. Measured, live, SIGNED -- positive
 *                into the cell, negative out of it, and the bar changes colour
 *                rather than direction so a drain cannot be misread as a charge.
 *
 * There is no text on it beyond the numbers and their units. What a gauge
 * measures is said with an icon, which is also why the glyph table has five new
 * shapes in it and not an alphabet.
 *
 * The two mV gauges are WINDOWED, not zero-based: a cell lives between 3.0 and
 * 4.4 V and a USB supply between 4.0 and 5.5, so a bar from zero would sit at
 * five-sixths full and never visibly move. The window is the range the reading
 * can actually occupy.
 */
enum {
    LK_DET_PCT_SCALE = 12u,
    LK_DET_PCT_Y = 34u,
    LK_DET_PCT_CELLS = 6u, /* "100.0%", the widest the field can hold          */

    LK_DET_ICON_X = 26u,
    LK_DET_BAR_X = 132u,
    LK_DET_BAR_W = 322u,
    LK_DET_BAR_H = 30u,
    LK_DET_BAR_BORDER = 2u,

    LK_DET_VAL_SCALE = 5u,
    LK_DET_VAL_RIGHT = 614u,
    LK_DET_VAL_CELLS = 7u, /* "-1600mA", the widest a field can hold           */

    LK_DET_ROW0 = 150u,   /* charger mV */
    LK_DET_ROW1 = 208u,   /* charger mA */
    LK_DET_RULE_Y = 268u, /* the two groups, kept apart                        */
    LK_DET_ROW2 = 300u,   /* battery mV */
    LK_DET_ROW3 = 358u,   /* battery mA */

    /* Gauge windows. The mV pairs are the range the reading can occupy; the mA
     * pairs are the CS_VTH table's own span, which is the widest current this
     * charger can be set to and therefore the widest it can report. */
    LK_DET_VBUS_LO = 3500,
    LK_DET_VBUS_HI = 5600,
    LK_DET_VBAT_LO = 3000,
    LK_DET_VBAT_HI = 4400,
    LK_DET_MA_LO = 0,
    LK_DET_MA_HI = 1600,

    /* How often a live screen is allowed to be live. The park loop comes round
     * every ~30 ms and every one of these repaints is five uncached band presents
     * plus a cache clean, so drawing on every pass would hold the CPU at full draw
     * off the node the charger is trying to fill -- which is the same argument the
     * backlight's half brightness makes. Four frames a second is what "live gauge"
     * needs: the medians behind these numbers are refilled about that often, and a
     * bar that moves faster than the value under it is moving on noise. */
    LK_DET_REPAINT_MS = 250
};

#define LK_DET_BG 0xff000000u
#define LK_DET_INK 0xffffffffu
#define LK_DET_FRAME 0xff585858u
#define LK_DET_TRACK 0xff262626u  /* the unfilled part of a bar               */
#define LK_DET_USB_FILL 0xff3c9cf0u
#define LK_DET_BAT_FILL 0xff44c060u
#define LK_DET_DRAIN_FILL 0xffe08c30u /* current leaving the cell             */

/* Battery outline with its interior filled to @p pct. Four bars and a nub, the
 * same way lk_draw_charge_screen_builtin() does it and for the same reason: every
 * pixel here is written by a CPU with its D-cache off. */
static void lk_fb_battery_icon(uint32_t x, uint32_t y, uint32_t bw, uint32_t bh, uint32_t border,
                               uint32_t pct, uint32_t ink, uint32_t fill) {
    const uint32_t nub_w = border * 2u;
    const uint32_t nub_h = bh / 3u;
    const uint32_t in_x = x + border * 2u;
    const uint32_t in_y = y + border * 2u;
    const uint32_t in_w = bw - border * 4u;
    const uint32_t in_h = bh - border * 4u;
    uint32_t fill_w;

    lk_fb_rect(x, y, bw, border, ink);
    lk_fb_rect(x, y + bh - border, bw, border, ink);
    lk_fb_rect(x, y, border, bh, ink);
    lk_fb_rect(x + bw - border, y, border, bh, ink);
    lk_fb_rect(x + bw, y + (bh - nub_h) / 2u, nub_w, nub_h, ink);

    if (pct > 100u) pct = 100u;
    lk_fb_rect(in_x, in_y, in_w, in_h, LK_DET_TRACK);
    fill_w = in_w * pct / 100u;
    if (pct > 0u && fill_w == 0u) fill_w = 1u;
    if (fill_w > 0u) lk_fb_rect(in_x, in_y, fill_w, in_h, fill);
}

/* One gauge row: the bar's interior and the value beside it. The frame around the
 * bar is chrome and is drawn once, by lk_det_chrome().
 *
 * @p valid 0 draws a dash and an empty bar rather than a zero -- a zero is a
 * reading, and "nothing measured yet" is not one. */
static void lk_det_row(uint32_t row, int valid, int value, int lo, int hi, uint32_t unit_glyph,
                       uint32_t fill_argb) {
    const uint32_t in_x = LK_DET_BAR_X + LK_DET_BAR_BORDER;
    const uint32_t in_y = row + LK_DET_BAR_BORDER;
    const uint32_t in_w = LK_DET_BAR_W - 2u * LK_DET_BAR_BORDER;
    const uint32_t in_h = LK_DET_BAR_H - 2u * LK_DET_BAR_BORDER;
    const uint32_t val_h = 5u * LK_DET_VAL_SCALE;
    const uint32_t val_w = LK_DET_VAL_CELLS * 4u * LK_DET_VAL_SCALE - LK_DET_VAL_SCALE;
    const uint32_t val_y = row + (LK_DET_BAR_H - val_h) / 2u;
    lk_run_t r;

    /* The value field is cleared across its WIDEST run, not the one about to be
     * drawn: 1000 mA falling to 999 mA is a narrower run right-anchored in the
     * same place, and the digit it no longer needs would otherwise stay lit. */
    lk_fb_rect(LK_DET_VAL_RIGHT - val_w, val_y, val_w, val_h, LK_DET_BG);
    lk_fb_rect(in_x, in_y, in_w, in_h, LK_DET_TRACK);

    lk_run_reset(&r);

    if (!valid) {
        lk_run_push(&r, LK_GLYPH_MINUS);
        lk_run_draw_right(&r, LK_DET_VAL_RIGHT, val_y, LK_DET_VAL_SCALE, LK_DET_FRAME);
        return;
    }

    {
        int mag = (value < 0) ? -value : value;
        int span = hi - lo;
        int clamped = mag;
        uint32_t fill_w;

        if (value < 0) lk_run_push(&r, LK_GLYPH_MINUS);
        lk_run_push_u32(&r, (uint32_t)mag);
        lk_run_push(&r, LK_GLYPH_M);
        lk_run_push(&r, unit_glyph);
        lk_run_draw_right(&r, LK_DET_VAL_RIGHT, val_y, LK_DET_VAL_SCALE, LK_DET_INK);

        if (clamped < lo) clamped = lo;
        if (clamped > hi) clamped = hi;
        fill_w = (span > 0) ? (uint32_t)(((clamped - lo) * (int)in_w) / span) : 0u;
        if (fill_w == 0u && clamped > lo) fill_w = 1u;
        if (fill_w > 0u) lk_fb_rect(in_x, in_y, fill_w, in_h, fill_argb);
    }
}

/* Everything that does not change: the icons, the four bar frames, the rule
 * between the two groups. Drawn on a full repaint only. */
static void lk_det_chrome(uint32_t pct) {
    const uint32_t rows[4] = {LK_DET_ROW0, LK_DET_ROW1, LK_DET_ROW2, LK_DET_ROW3};

    lk_fb_rect(0u, 0u, (uint32_t)MVII_MT6592_LK_FB_WIDTH, (uint32_t)MVII_MT6592_LK_FB_HEIGHT,
               LK_DET_BG);

    for (uint32_t i = 0; i < 4u; ++i) {
        const uint32_t y = rows[i];
        lk_fb_rect(LK_DET_BAR_X, y, LK_DET_BAR_W, LK_DET_BAR_BORDER, LK_DET_FRAME);
        lk_fb_rect(LK_DET_BAR_X, y + LK_DET_BAR_H - LK_DET_BAR_BORDER, LK_DET_BAR_W,
                   LK_DET_BAR_BORDER, LK_DET_FRAME);
        lk_fb_rect(LK_DET_BAR_X, y, LK_DET_BAR_BORDER, LK_DET_BAR_H, LK_DET_FRAME);
        lk_fb_rect(LK_DET_BAR_X + LK_DET_BAR_W - LK_DET_BAR_BORDER, y, LK_DET_BAR_BORDER,
                   LK_DET_BAR_H, LK_DET_FRAME);
    }

    lk_fb_rect(LK_DET_ICON_X, LK_DET_RULE_Y, LK_DET_VAL_RIGHT - LK_DET_ICON_X, 2u, 0xff303030u);

    /* One icon per group, centred on the pair of rows it labels. The plug is
     * drawn joined and with its bolt, because the detail screen is only reachable
     * from the plugged park. */
    lk_fb_plug(LK_DET_ICON_X + 4u, LK_DET_ROW0 - 6u, 7u, 1, 1, LK_DET_INK);
    lk_fb_battery_icon(LK_DET_ICON_X, LK_DET_ROW2 + 26u, 56u, 34u, 3u, pct, LK_DET_INK,
                       LK_DET_BAT_FILL);
}

/*
 * @p full 1 repaints the chrome and presents the whole panel; 0 presents only the
 * five bands the live values live in, which is what the park's repeat calls use.
 * Composed on the shadow either way -- the live canvas is never touched while the
 * picture is half-built. See the block above lk_fb_rect().
 */
/*
 * The driver holds UI-SOC at 99% until the termination ladder latches
 * full.  On this board the shunt includes the system's own draw, so that
 * ladder never saw 150 mA and a pack sitting on the 4.20 V CV rail
 * (4246 mV in the report) was painted 99.0% forever.  Once the node is
 * on CV the cell is full; show it.
 */
static int lk_display_permille(const mt6592_pmic_battery_t* bat) {
    if (!bat || bat->battery_percent < 0)
        return -1;
    if (bat->charger_online == 1 && bat->battery_mv >= 4200 &&
        bat->battery_percent >= 99)
        return 1000;
    if (bat->battery_permille >= 0)
        return (bat->battery_permille > 1000) ? 1000 : bat->battery_permille;
    return bat->battery_percent * 10;
}

static uint32_t lk_display_percent(const mt6592_pmic_battery_t* bat) {
    const int permille = lk_display_permille(bat);
    if (permille < 0)
        return 0u;
    return ((uint32_t)permille + 5u) / 10u;
}

static void lk_draw_detail_screen(const mt6592_pmic_battery_t* bat, int full) {
    const uint32_t w = (uint32_t)MVII_MT6592_LK_FB_WIDTH;
    const uint32_t h = (uint32_t)MVII_MT6592_LK_FB_HEIGHT;
    const uint32_t pct_w = LK_DET_PCT_CELLS * 4u * LK_DET_PCT_SCALE - LK_DET_PCT_SCALE;
    const uint32_t pct_x = (w - pct_w) / 2u;
    const uint32_t pct_h = 5u * LK_DET_PCT_SCALE;
    const uint32_t band_x = LK_DET_BAR_X;
    const uint32_t band_w = LK_DET_VAL_RIGHT - LK_DET_BAR_X;
    const int permille = lk_display_permille(bat);
    const uint32_t level = lk_display_percent(bat);
    const int step_ma = mt6592_pmic_charge_step_ma();
    lk_run_t r;

    lk_fb_target((uint32_t)MVII_MT6592_LK_SHADOW_ADDR);

    if (full) lk_det_chrome(level);

    /* The level, with its tenth. Cleared across the widest run for the same
     * reason the value fields are: "100.0%" is wider than "9.9%". */
    lk_fb_rect(pct_x, LK_DET_PCT_Y, pct_w, pct_h, LK_DET_BG);
    lk_run_reset(&r);
    if (permille < 0) {
        lk_run_push(&r, LK_GLYPH_MINUS);
    } else {
        lk_run_push_u32(&r, (uint32_t)(permille / 10));
        lk_run_push(&r, LK_GLYPH_DOT);
        lk_run_push(&r, (uint32_t)(permille % 10));
        lk_run_push(&r, LK_GLYPH_PERCENT);
    }
    {
        const uint32_t run_w = lk_run_width(&r, LK_DET_PCT_SCALE);
        lk_run_draw(&r, (w - run_w) / 2u, LK_DET_PCT_Y, LK_DET_PCT_SCALE, LK_DET_INK);
    }

    lk_det_row(LK_DET_ROW0, bat->charger_mv >= 0, bat->charger_mv, LK_DET_VBUS_LO, LK_DET_VBUS_HI,
               LK_GLYPH_V, LK_DET_USB_FILL);
    lk_det_row(LK_DET_ROW1, step_ma >= 0, step_ma, LK_DET_MA_LO, LK_DET_MA_HI, LK_GLYPH_A,
               LK_DET_USB_FILL);
    lk_det_row(LK_DET_ROW2, bat->battery_mv >= 0, bat->battery_mv, LK_DET_VBAT_LO, LK_DET_VBAT_HI,
               LK_GLYPH_V, LK_DET_BAT_FILL);
    lk_det_row(LK_DET_ROW3, bat->current_valid, bat->current_ma, LK_DET_MA_LO, LK_DET_MA_HI,
               LK_GLYPH_A,
               (bat->current_valid && bat->current_ma < 0) ? LK_DET_DRAIN_FILL : LK_DET_BAT_FILL);

    lk_fb_target((uint32_t)MVII_MT6592_LK_FB_ADDR);

    if (full) {
        lk_fb_present(0u, 0u, w, h);
    } else {
        lk_fb_present(pct_x, LK_DET_PCT_Y, pct_w, pct_h);
        lk_fb_present(band_x, LK_DET_ROW0, band_w, LK_DET_BAR_H);
        lk_fb_present(band_x, LK_DET_ROW1, band_w, LK_DET_BAR_H);
        lk_fb_present(band_x, LK_DET_ROW2, band_w, LK_DET_BAR_H);
        lk_fb_present(band_x, LK_DET_ROW3, band_w, LK_DET_BAR_H);
    }

    mt6592_lcd_clean_fb((uintptr_t)MVII_MT6592_LK_FB_ADDR,
                        (uintptr_t)((uint32_t)MVII_MT6592_LK_FB_PITCH * h));
}

/* The unplugged frame. Dim enough to read as "not live" against the lit frame
 * that preceded it, bright enough to still be legible in the second and a half
 * it stays up before the backlight goes. The fill is darker than the ink so the
 * bar does not read as a charge level any more -- it is the last one measured. */
#define LK_PARK_GREY_INK 0xff909090u
#define LK_PARK_GREY_FILL 0xff484848u

static uint32_t lk_led_fill_color(mt6592_led_color_t c) {
    switch (c) {
        case MT6592_LED_RED: return 0xffe04030u;
        case MT6592_LED_BLUE: return 0xff3080ffu;
        case MT6592_LED_GREEN: return 0xff40d040u;
        case MT6592_LED_OFF:
        default: return 0xff606060u;
    }
}

/*
 * Park while the charger is the supply. Returns when the power key has been
 * pressed, and the caller then boots normally.
 *
 * The key must be seen *released* before a press counts. A board powered on by
 * holding the power button arrives here with the key still down, and without
 * this the park would exit on the same press that started the boot — which
 * looks exactly like the park not working at all.
 */
/*
 * Hold whatever is on the panel for roughly @p ms while keeping the watchdog fed
 * and the charger attended. One bare delay long enough to read a frame would trip
 * the watchdog, and on a cell-less board a stretch this long with nobody re-arming
 * the input path is the board losing its supply -- which is the whole failure this
 * park exists to survive. Every call underneath is self-rate-limited.
 *
 * THE SLICE IS 1 ms, NOT 30 ms, AND THAT IS THE WHOLE FIX FOR A SLOW GAUGE.
 *
 * mt6592_pmic_service() advances the AUXADC by at most one pwrap transaction per
 * call, so the converter's throughput is precisely this loop's call rate. At one
 * call per 30 ms a conversion took 90 ms and the park's number crawled -- the old
 * comment in the loop below cheerfully recorded "a fresh median about every
 * 1.2 s", which is what "the battery sampling renders slowly" looks like from the
 * outside. It was never the drawing; it was a state machine being clocked by a
 * sleep.
 *
 * 1 ms is the right slice because 1 ms is what the hardware itself needs between
 * the start of conversion and a valid latch (the kernel's own "Duo to HW
 * limitation" msleep(1)). Servicing faster than that would spend pwrap traffic
 * discovering the ADC is still busy; slower wastes silicon that is already done.
 * A round of all three channels now lands in ~9 ms.
 */
static void lk_park_hold_ms(uint32_t ms) {
    uint32_t i;

    for (i = 0u; i < ms; ++i) {
        /* The watchdog wants attention every few tens of milliseconds; the
         * converter wants it every millisecond. Kicking on both cadences at once
         * would put a needless APB write between every pair of ADC transactions. */
        if ((i % 10u) == 0u) mtk_watchdog_kick();
        mt6592_pmic_charger_service();
        mt6592_delay_cycles(33000u);
    }
}

/*
 * ── THE LOGO IS A SPLASH, SO GIVE IT A SPLASH'S DWELL ──
 *
 * The first lit frame is the logo (see lk_fb_paint_logo()), and until now the
 * next thing to touch the panel was the charge park's first paint -- which
 * arrives a few milliseconds later, because the only work between them is a
 * bootstatus write and a breadcrumb read. A picture that appears and is gone
 * inside a frame or two does not read as a splash; it reads as a glitch.
 *
 * Held to a FLOOR measured from when the backlight came up, not as a flat delay
 * bolted on here. The two are different whenever the boot takes real time to get
 * from one to the other -- the breadcrumb read goes to the eMMC -- and a floor
 * is what makes the splash the same length on every boot instead of two seconds
 * plus whatever the rest cost. A boot that somehow spent longer than the whole
 * dwell getting here does not wait at all.
 *
 * The wait costs nothing but the wait: lk_park_hold_ms() spends it feeding the
 * watchdog and clocking the AUXADC a transaction per millisecond, which is
 * ~666 conversions -- forty times what it takes to fill every ring. That is why
 * the park's blocking prime could come out. The splash IS the prime now.
 */
enum {
    LK_SPLASH_DWELL_MS = 4000u,
    /*
     * Where the dwell is interrupted to compose the charge screen off-screen. It
     * is a point in the SAME four seconds, not an addition to them: the picture
     * is built behind the splash and flushed when the four are up, so the gauge
     * costs the boot nothing beyond the dwell itself.
     *
     * Late in the dwell rather than early because everything the frame needs
     * comes from the converter this hold is feeding -- 3.5 s is ~350 rounds of
     * all three channels, every ring filled many times over -- and because the
     * shorter the gap between composing and flushing, the fresher the number on
     * the panel when it appears.
     */
    LK_SPLASH_COMPOSE_AT_MS = 3500u
};

/* Hold the splash until @p target_ms after the backlight came up. A floor, not a
 * delay: a boot that already spent longer than that getting here does not wait. */
static void lk_splash_hold_to(uint32_t target_ms) {
    uint32_t elapsed_ms;

    if (!g_splash_lit) return; /* headless boot; there is nothing on the panel */

    /* Wrap-safe: GPT4 is a free-running 13 MHz up-counter and unsigned
     * subtraction is correct across its ~330 s wrap. If it is not ticking at all
     * the delta stays zero and the splash gets its full dwell, which is the safe
     * way round. */
    elapsed_ms = mt6592_delay_ticks_to_us(mt6592_delay_gpt_ticks() - g_splash_ticks) / 1000u;
    if (elapsed_ms >= target_ms) return;

    lk_park_hold_ms(target_ms - elapsed_ms);
}

/* What the park last put on the panel, so each pass repaints only on a change.
 * Every repaint is a 1.2 MB uncached fill plus a cache clean; doing it every
 * pass would keep the CPU at full draw for the whole park, which is the opposite
 * of what a charge screen is for. */
typedef enum {
    LK_PARK_SCREEN_NONE = 0,
    LK_PARK_SCREEN_GAUGE,     /* cable in: gauge + percent + plug             */
    LK_PARK_SCREEN_DETAIL,    /* MENU: decimal percent + the four live gauges */
    LK_PARK_SCREEN_DETAIL_OUT,/* the detail screen, cable gone, counting down */
    LK_PARK_SCREEN_UNPLUGGED, /* grey, cable broken, still lit, counting down */
} lk_park_screen_t;

/*
 * LK_PARK_SCREEN_BLACK IS GONE, AND WITH IT THE ONLY DEAD END IN THIS LOADER.
 *
 * It was the state the park entered when the unplug window expired: backlight
 * off, loop still running, "board stays warm". The intent was a board that had
 * been left plugged in overnight not sitting behind a lit panel once somebody
 * pulled the cable. What it actually built was a boot that never finishes -- the
 * comment above the loop said so in as many words, "the power key is still the
 * only exit" -- so a board on battery, five seconds after the cable came out,
 * was dark, awake, draining, and to anyone holding it indistinguishable from one
 * that had died. Pressing power was the documented way out and is not a thing an
 * operator knows to try on a machine that is, as far as they can see, off.
 *
 * The file's own contract two hundred lines up is "parks on the same condition
 * and exits FORWARD, into the boot it was already going to do", and the entry
 * gate has always honoured it: not plugged means no park at all, because parking
 * a board running off its own cell drains it in front of a screen nobody asked
 * for. That is the same board and the same reasoning five seconds later. So the
 * window now ends the way the entry gate begins -- it boots.
 */

/* How close together two MENU presses have to be to count as one double-tap.
 *
 * 600 ms, which is loose for a deliberate double-tap and still far tighter than
 * any two presses meant as two. The floor is the sampling rate, not the human:
 * this loop runs a pass every ~30 ms and the KPD block only refreshes its scan
 * words once per debounce interval, so a press has to stay down across at least
 * one pass and be released across at least one more to be seen as a press at
 * all. Two of those plus the gap between them is ~150 ms at the fastest anyone
 * can tap, so 600 leaves room for a tap that is merely quick rather than
 * expert. Erring long costs a rare accidental console entry, which is two taps
 * to leave again; erring short costs the operator the feature. */
enum { LK_PARK_TAP_WINDOW_MS = 600u };

/* Milliseconds since a GPT4 tick reading. Wrap-safe on the same terms as
 * lk_splash_hold_to(): a free-running 13 MHz up-counter, unsigned subtraction,
 * correct across its ~330 s wrap -- and every window measured with this is
 * seconds long, so a wrap cannot be mistaken for one. */
static uint32_t lk_ms_since(uint32_t ticks) {
    return mt6592_delay_ticks_to_us(mt6592_delay_gpt_ticks() - ticks) / 1000u;
}

/*
 * ── THE PARK'S ONE WAY OUT ──
 *
 * All three of the park's exits come through here: the entry gate that never
 * parks at all, the power key, and the unplug window. They had drifted into
 * separate copies of "reassert the backlight and return", which is exactly how
 * the unplug case ended up with no exit at all -- it was the copy nobody ever
 * added the return to (see the window below).
 *
 * The PMIC service either side is the same bracket lk_display_on() gets at the
 * bottom of this file, and it is here for the same reason. The step to full
 * brightness is the largest single load this loader makes; VBAT *is* VSYS on
 * this board -- there is no power-path FET, so the cell and the system rail are
 * the same node (see the top of mt6592_pmic.h) -- and a dip below UVLO_VTHL is
 * a power cut, not a warning. charger_arm() re-writes UVLO_VTHL to its lowest
 * threshold on every service call, before it looks at CHRDET and therefore also
 * when there is no cable at all. Unplugged, that write is the only thing
 * standing between this load step and a dead board, so it is made immediately
 * before the step and again immediately after it.
 *
 * `clear' is 0 for the entry gate, which has the splash up and deliberately
 * leaves it there through the whole boot.img load, and 1 for the two exits
 * taken out of a park that has a gauge on the panel and should not leave it
 * frozen there through the load and the kernel's own bring-up.
 */
static void lk_park_handoff(int clear) {
    if (clear) lk_fb_fill(0xff000000u);
    mt6592_pmic_power_hold();
    mt6592_backlight_reassert(MVII_MT6592_LK_HANDOFF_BACKLIGHT_PCT);
    mt6592_pmic_power_hold();
    mt6592_led_battery_indicate();
}

/*
 * LK_PARK_SCREEN_MAINS AND ITS DWELL TIMER ARE GONE.
 *
 * There were three states here and two of them were pictures that alternated:
 * a gauge when the level could be attributed to a cell, a bare plug when it
 * could not. Two pictures for one fact needed a floor on how fast they could
 * swap -- LK_PARK_CELL_DWELL_MS, 2 s -- because the underlying bit is a live
 * comparison against a baseline and it does not have to hold still.
 *
 * One picture needs no floor. The gauge is now drawn on every plugged pass and
 * the trusted/untrusted distinction moved into the badge, which is repainted
 * with it, so there is nothing left to strobe between. The percentage redraws
 * whenever it changes, exactly as it always did.
 */

static void lk_charge_park(void) {
    mt6592_pmic_battery_t bat;
    int have_release = 0;
    uint32_t last_pct = 0xffffffffu;
    int last_bolt = -1;
    mt6592_led_color_t last_led = MT6592_LED_OFF;
    lk_park_screen_t screen = LK_PARK_SCREEN_NONE;
    /* GPT4 at the moment the cable came out, which is what the unplug window is
     * measured from. Meaningless unless `screen' is UNPLUGGED. */
    uint32_t unplug_ticks = 0u;
    /* MENU's toggle. `menu_release' is the same press-with-prior-release rule the
     * power key below uses: a key that is still down from the previous pass is not
     * a new press, and without that a 30 ms loop would toggle the screen ten times
     * per human keypress. Starts at 1 -- if MENU is somehow already held when the
     * park opens, the first pass is a press and the operator gets what they asked
     * for rather than having to release and try again. */
    uint32_t detail = 0u;
    uint32_t menu_release = 1u;
    uint32_t detail_ticks = 0u;
    /* GPT4 at the previous MENU press edge, and whether there was one recent
     * enough to still count. See the double-tap block in the loop. */
    uint32_t tap_ticks = 0u;
    uint32_t tap_armed = 0u;

    /*
     * The return value decides this, not the struct alone. A non-zero return means
     * the driver has not yet read CHRDET even once, so `charger_online` is -1 and
     * not a measurement.
     *
     * This gate is the whole reason the park is safe. Booting through on a failed
     * read is what the loader did when it last worked; the version that dropped
     * the check parked the board on the strength of an unmeasured field, and a park
     * has no exit but a power-key press. The `!= 1' is deliberate too -- -1 is
     * truthy, so a plain `!bat.charger_online' would read "not determined yet" as
     * "plugged in" and park on it.
     */
    if (mt6592_pmic_battery_read(&bat) != 0 || bat.charger_online != 1) {
        /* Not plugged (or the PMIC cannot say). Boot straight through — this is
         * the "not plugged and battery" case and also the only safe thing to do
         * when the answer is unreadable, since parking a board running off its
         * own cell would drain it in front of a screen nobody asked for. */
        lk_log("lk: on battery or charger unreadable; booting straight through\n");
        /* The other handoff, and the same rule: this returns straight into the
         * boot.img load with the splash still up, so the level the OS inherits is
         * whatever is set here. Full, for the same reason the power-key exit is --
         * there is no park after this to conserve for. Not cleared, so the logo
         * survives the load. */
        lk_park_handoff(0);
        return;
    }

    /*
     * ── HOLD THE SPLASH, PRIME THE GAUGE, AND BUILD THE GAUGE BEHIND IT ──
     *
     * The splash's four seconds are spent here, inside the plugged branch, where
     * there is actually something about to replace it. The unplugged path returns
     * above and leaves the logo up through the whole boot.img load, which is
     * longer than any dwell.
     *
     * The hold doubles as the gauge prime this used to do with a blocking sample.
     * The converter is a state machine whose throughput is its call rate, so
     * arriving here it has nothing published and the first frame would draw a hold
     * rather than a number. lk_park_hold_ms() is a transaction per millisecond --
     * every ring filled many times over, with no blocking call and no time that
     * was not already being spent on the splash.
     *
     * The dwell is then broken in two so the charge screen can be BUILT during it.
     * Composing goes to the shadow canvas (see the block above lk_fb_rect), and
     * with presents deferred nothing reaches the panel, so the logo holds its full
     * four seconds and the finished gauge is moved across in one sweep the moment
     * they are up. There is no "logo, pause, blank, gauge" any more: it is logo,
     * then gauge.
     *
     * There is no presence question here any more, and no verdict to disagree with
     * the console about. The park draws the level it measured and the badge says
     * whether current is going in; what it cannot tell is whether the node it is
     * measuring is a cell or the port, and it no longer claims to (see the top of
     * mt6592_pmic.h).
     */
    lk_splash_hold_to((uint32_t)LK_SPLASH_COMPOSE_AT_MS);
    (void)mt6592_pmic_battery_read(&bat);
    lk_log("lk: charger present; charge park\n");

    if (bat.battery_percent >= 0) {
        const uint32_t seed_pct = lk_display_percent(&bat);
        const int seed_bolt = (bat.status == MT6592_PMIC_STATUS_CHARGING) ? 1 : 0;
        const mt6592_led_color_t seed_led = mt6592_led_battery_color(1, 1, bat.battery_percent);

        lk_fb_defer_present(1);
        lk_draw_charge_screen(seed_pct, lk_led_fill_color(seed_led), 0xffffffffu, 1, seed_bolt, 1);
        lk_fb_defer_present(0);

        /* Seeded so the loop's first pass recognises this frame as already on the
         * panel and repaints only when something in it actually changes. `last_led'
         * is deliberately NOT seeded: the LED is set in one place, by the loop. */
        last_pct = seed_pct;
        last_bolt = seed_bolt;
        screen = LK_PARK_SCREEN_GAUGE;
    }

    /* The remainder of the splash, with the finished gauge waiting behind it. A
     * board whose gauge had nothing to publish yet composed nothing above and
     * simply keeps the logo until the loop's first measured pass. */
    lk_splash_hold_to((uint32_t)LK_SPLASH_DWELL_MS);

    /* The park's brightness before the picture, so the change lands on the splash
     * rather than on the gauge -- the gauge is the frame that should arrive already
     * correct. Idempotent, and a no-op when the two brightnesses agree.
     *
     * This used to draw the mains screen unconditionally first and let the loop
     * repaint over it. That was a workaround for a 2.2 s presence window -- with the
     * window gone it became a straight inversion: a board WITH a cell showed the
     * no-cell plug, then flipped to the gauge, so whichever frame the eye caught was
     * the wrong one. There is nothing left to hide behind a placeholder. */
    mt6592_backlight_reassert(MVII_MT6592_LK_PARK_BACKLIGHT_PCT);

    /* Four seconds are up: the composed gauge replaces the splash here, in one
     * pass over the rows. Nothing was composed on a board with no reading yet, and
     * this then does nothing at all. */
    lk_fb_flush_present();

    /* MENU's pads and the scanner, once, here rather than at the top of the loader:
     * this is the only screen that reads a key other than the power key, and doing
     * it here keeps the two mode writes off every boot that never parks. */
    lk_kpd_menu_arm();

    for (;;) {
        int pressed;
        int online;
        int bolt;
        int have_pct;
        uint32_t pct;

        mtk_watchdog_kick();

        /* The converter is advanced by lk_park_hold_ms() at the bottom of the loop,
         * a transaction per millisecond, so by the time control returns here the
         * medians have already been refilled several times over. This is a read of
         * published state; it forces nothing and waits for nothing. */
        (void)mt6592_pmic_battery_read(&bat);
        /* == 1, not truthy: -1 is "not determined" and must not grey the screen. */
        online = (bat.charger_online == 1);

        /*
         * ── THE LEVEL IS ALWAYS DRAWN. THE BADGE CARRIES WHAT IT IS WORTH. ──
         *
         * There is one picture now. The driver publishes a percentage on every
         * sample -- plugged, unplugged, cell or no cell -- and the park draws it
         * every pass, because a board that will only report its charge once the
         * cable is out is a board that cannot report its charge when it matters.
         *
         * The badge is the measured current, which is the one thing here that can
         * actually distinguish a charge from a plug. Current INTO the node means
         * something is taking it, so the plug gets its bolt; zero or out means the
         * cable is attached and nothing is going in, which is a full pack, an empty
         * holder, or a finished charge -- and those three are indistinguishable on
         * this board, so the badge says the one thing it knows instead of guessing
         * which of them it is looking at.
         *
         * This was a `level_trusted' bit, which answered a question about the
         * NUMBER'S PROVENANCE and was 0 for the whole time a cable was in.
         */
        bolt = (bat.status == MT6592_PMIC_STATUS_CHARGING) ? 1 : 0;
        have_pct = (bat.battery_percent >= 0);
        pct = lk_display_percent(&bat);

        /*
         * ── MENU TOGGLES THE DETAIL SCREEN ──
         *
         * Sampled before the draw decisions below so a press takes effect on the
         * pass that saw it, not the next one. `screen' is knocked back to NONE and
         * the caches invalidated because both directions of the toggle are a
         * different picture on the same panel and neither may be reached by a
         * partial repaint over the other.
         *
         * The detail screen only exists with a cable in -- it is four gauges, two of
         * which are about the port. The unplug branch below keeps it up for the
         * window rather than snapping back to the gauge, and ends it on the same
         * clock, so a cable coming out while it is up gets the same five seconds
         * and the same boot.
         */
        {
            const uint32_t menu = lk_kpd_menu_down();

            if (!menu) {
                menu_release = 1u;
            } else if (menu_release) {
                /*
                 * ── AND A DOUBLE-TAP OPENS THE CONSOLE ──
                 *
                 * The debug console used to be reachable from exactly one instant
                 * per boot: a button held through the early init window, two
                 * seconds wide and hundreds of lines before this screen exists.
                 * Miss it, or have the console time out waiting for a host, or
                 * type `boot`, and the only way back was a power cycle -- which on
                 * a board being debugged over USB is also the thing that drops the
                 * link you were debugging through. So the park, which is the one
                 * screen this board sits on indefinitely, carries the trigger too:
                 * two fast taps of MENU, available on every pass, for as long as
                 * the park is up.
                 *
                 * The first tap still toggles the detail screen. Swallowing it
                 * until the window expired would put a 600 ms lag on the single
                 * press, which is the common case, to smooth the rare one; the
                 * toggle is cheap, visible, and reversed on the line below when
                 * the second tap arrives, so the screen ends up where it started.
                 *
                 * The screen and the caches are NOT reset here. The console
                 * repaints the panel itself (magenta while it waits, green once a
                 * host attaches), so the restore below owns that -- and it has to
                 * run whether or not the toggle did.
                 */
                const int double_tap =
                    (tap_armed != 0u && lk_ms_since(tap_ticks) <= (uint32_t)LK_PARK_TAP_WINDOW_MS);

                menu_release = 0u;
                tap_armed = 1u;
                tap_ticks = mt6592_delay_gpt_ticks();

                if (double_tap) {
                    tap_armed = 0u;
                    detail = !detail; /* undo the first tap's toggle */
#ifdef MVII_MT6592_LK_RELEASE
                    lk_log("lk: menu double-tap; release build has no debug console\n");
#else
                    lk_log("lk: menu double-tap; entering the live console\n");
                    mvii_debug_console_run(&kConsoleHooks);
                    lk_log("lk: console done; back to the charge park\n");

                    /*
                     * Put the park back, in the order the park itself builds it.
                     *
                     * The keypad first, because the console's `kpdmode`, `kpdmux`
                     * and `kpdon` commands reprogram the very pads and the very
                     * KPD_EN bit this loop's key sampler reads -- an operator who
                     * used any of them would otherwise come back to a screen whose
                     * only two controls are dead, with no way to reach the console
                     * again to undo it.
                     *
                     * Then the backlight, which the console left at whatever the
                     * panel bring-up set; then the caches, so the next pass draws
                     * a full frame over the console's flat fill rather than a
                     * partial repaint into it. `menu_release' is cleared rather
                     * than set: the operator's finger may still be on the key from
                     * the tap that got us here, and a fresh release has to be seen
                     * before the next press counts.
                     */
                    lk_kpd_menu_arm();
                    menu_release = 0u;
                    mt6592_backlight_reassert(MVII_MT6592_LK_PARK_BACKLIGHT_PCT);
                    screen = LK_PARK_SCREEN_NONE;
                    last_pct = 0xffffffffu;
                    last_bolt = -1;
                    /* Driven off rather than just marked off. `led` and `ledscan`
                     * leave the channels wherever the operator stopped, and the
                     * loop below only writes the part when it DIFFERS from this
                     * cache -- so a cache set to OFF over a lit part would agree
                     * with itself forever and leave the LED stuck on whatever the
                     * console last set. Driving it makes the cache true. */
                    (void)mt6592_led_set(MT6592_LED_OFF);
                    last_led = MT6592_LED_OFF;
#endif
                } else {
                    detail = !detail;
                    screen = LK_PARK_SCREEN_NONE;
                    last_pct = 0xffffffffu;
                    last_bolt = -1;
                    lk_log(detail ? "lk: menu; detail screen\n" : "lk: menu; back to the gauge\n");
                }
            }
        }

        if (!online) {
            /*
             * ── THE GAUGE SURVIVES THE UNPLUG, THEN THE BOARD BOOTS ──
             *
             * This once greyed the picture, held it 1500 ms and blanked. That made
             * the one moment an operator most wants to read the level the moment it
             * went away, and "the lk battery indicator does not show when the cable
             * is unplugged" was that write and nothing else. It was then replaced by
             * leaving the panel up indefinitely, which fixed the reading and traded
             * it for an unattended board draining behind a lit screen. The third
             * shape -- blank the panel and keep looping -- fixed the drain and traded
             * it for a board that never boots (see the note where
             * LK_PARK_SCREEN_BLACK used to be).
             *
             * The window is the answer to all three, and what closes it is the boot.
             * Full brightness for the acknowledgement, the greyed gauge redrawn on
             * the same terms as the lit one for MVII_MT6592_LK_UNPLUG_BOOT_MS so
             * it tracks the pack DOWN rather than freezing at whatever it read as the
             * cable parted, and then the loader goes and does the job it was on its
             * way to do. Grey is still the whole message: same layout, same number,
             * no colour and no bolt.
             *
             * The window is also the debounce, which is why the exit is at the end of
             * it and not on the edge: a cable that is merely reseated, or a CHRDET
             * comparator that chatters as a barrel jack is nudged, is back inside
             * five seconds and the branch below un-greys the park with nothing lost.
             * Only a removal that is still a removal five seconds later ends it.
             */
            const int first = (screen != LK_PARK_SCREEN_UNPLUGGED &&
                               screen != LK_PARK_SCREEN_DETAIL_OUT);
            uint32_t shown = pct;

            if (!have_pct) shown = (last_pct == 0xffffffffu) ? 0u : last_pct;

            if (first) {
                lk_log("lk: charger removed; gauge greyed, booting shortly\n");
                if (last_led != MT6592_LED_OFF) {
                    (void)mt6592_led_set(MT6592_LED_OFF);
                    last_led = MT6592_LED_OFF;
                }
                unplug_ticks = mt6592_delay_gpt_ticks();
            }

            /* Full clear on the transition only: the badge swaps to a DIFFERENT
             * asset there (plug -> plug-broken) and the broken one need not cover
             * the joined one's silhouette. Afterwards it is the same box repaint
             * the plugged branch uses, because only the digits move. */
            if (detail) {
                /* THE DETAIL SCREEN GETS THE SAME WINDOW, not a snap back to the
                 * gauge. It needs no grey palette to say the cable is gone: the two
                 * USB rows go to a dash and an empty bar on their own the moment
                 * charger_mv reads -1, and the two battery rows are still real
                 * measurements on a board running off its cell. So it stays up, stays
                 * live, and ends on the same clock the gauge does. */
                if (first || lk_ms_since(detail_ticks) >= (uint32_t)LK_DET_REPAINT_MS) {
                    lk_draw_detail_screen(&bat, first);
                    detail_ticks = mt6592_delay_gpt_ticks();
                    screen = LK_PARK_SCREEN_DETAIL_OUT;
                    last_pct = 0xffffffffu;
                    last_bolt = -1;
                }
            } else if (first || shown != last_pct || last_bolt != 0) {
                lk_draw_charge_screen(shown, LK_PARK_GREY_FILL, LK_PARK_GREY_INK, 0, 0, first);
                last_pct = shown;
                last_bolt = 0;
                screen = LK_PARK_SCREEN_UNPLUGGED;
            }

            /* The window closing. The last frame above is still on the panel when
             * this fires, so what the handoff paints over is a picture that had its
             * five seconds -- not a repaint racing the exit. */
            if ((screen == LK_PARK_SCREEN_UNPLUGGED || screen == LK_PARK_SCREEN_DETAIL_OUT) &&
                lk_ms_since(unplug_ticks) >= (uint32_t)MVII_MT6592_LK_UNPLUG_BOOT_MS) {
                lk_log("lk: unplugged window over; leaving the park, booting\n");
                lk_park_handoff(1);
                return;
            }
        } else {
            /* "Is there a number" -- the same argument mt6592_led_battery_color() is
             * given everywhere else now, so the park's LED and the shell's cannot say
             * different things about the same board. See the note in
             * mt6592_led_battery_indicate(). */
            const mt6592_led_color_t led =
                mt6592_led_battery_color(1, (bat.battery_percent >= 0), bat.battery_percent);

            /* Coming back from the unplugged park, inside the window -- past it there
             * is no park to come back to, the loader has gone and booted. The
             * reassert is unconditional rather than skipped as redundant because the
             * panel may be at any level the unplug branch left it at; it is
             * idempotent when it agrees. NONE forces the full repaint below, which is
             * what un-greys the picture. */
            if (screen == LK_PARK_SCREEN_UNPLUGGED || screen == LK_PARK_SCREEN_DETAIL_OUT) {
                lk_log("lk: charger back; un-greying the park\n");
                mt6592_backlight_reassert(MVII_MT6592_LK_PARK_BACKLIGHT_PCT);
                screen = LK_PARK_SCREEN_NONE;
            }

            if (led != last_led) {
                (void)mt6592_led_set(led);
                last_led = led;
            }

            /* Repaint on any of the three things the picture shows. Nothing is
             * rate-limited here any more: there is only one picture, so the worst
             * a fast-moving input can do is redraw the same layout with a
             * different number in it -- which is the point of the screen.
             *
             * A percentage that has not been measured yet holds the previous
             * frame rather than painting a hard 0%. The driver publishes -1 for
             * exactly one case -- before its first median lands, which the prime
             * above normally forecloses -- and 0% is the one reading an operator
             * must never be shown on a guess. */
            if (detail) {
                /* Redrawn on a clock rather than on a change, which is the opposite
                 * of every other screen here and is the point of this one: mV and mA
                 * move continuously, so "repaint when the value differs" would be
                 * every pass. The first frame after the toggle is unconditional and
                 * full; the rest are the five bands. */
                const int first = (screen != LK_PARK_SCREEN_DETAIL);

                if (first || lk_ms_since(detail_ticks) >= (uint32_t)LK_DET_REPAINT_MS) {
                    lk_draw_detail_screen(&bat, first);
                    detail_ticks = mt6592_delay_gpt_ticks();
                    screen = LK_PARK_SCREEN_DETAIL;
                    /* The gauge's caches describe a picture that is no longer on the
                     * panel; leaving them alone would let the toggle back land as a
                     * partial repaint into the detail layout. */
                    last_pct = 0xffffffffu;
                    last_bolt = -1;
                }
            } else if (have_pct && (screen != LK_PARK_SCREEN_GAUGE || pct != last_pct ||
                                    bolt != last_bolt)) {
                lk_draw_charge_screen(pct, lk_led_fill_color(led), 0xffffffffu, 1, bolt,
                                      screen != LK_PARK_SCREEN_GAUGE);
                last_pct = pct;
                last_bolt = bolt;
                screen = LK_PARK_SCREEN_GAUGE;
            }
        }

        /*
         * THREE-VALUED, AND TESTED AS SUCH. mt6592_pmic_pwrkey_pressed() returns
         * 1 down, 0 up and -1 when it could not ask -- pwrap not up, or the
         * CHRSTATUS read failed. This was `if (!pressed) ... else if
         * (have_release)', which folds -1 in with 1 because -1 is truthy: a
         * single failed transaction after any release would hand off as though
         * somebody had pressed the key, and a pwrap that stayed down would keep
         * `have_release' at 0 forever so the real key could never work again.
         * An unreadable pass is neither a press nor a release; it is skipped, and
         * the next pass 30 ms later asks again.
         */
        pressed = mt6592_pmic_pwrkey_pressed();
        if (pressed == 0) {
            have_release = 1;
        } else if (pressed == 1 && have_release) {
            lk_log("lk: power key; handing off, backlight to full\n");
            /* Leave the panel black on the way out so the gauge does not sit
             * frozen on screen through the eMMC load and the kernel's own
             * bring-up -- and at FULL, because this is the handoff. The park's
             * half brightness existed to keep the backlight off the charge
             * current; from here the OS owns the panel. */
            lk_park_handoff(1);
            return;
        }

        /* ~30 ms until the next paint and the next key sample -- but spent
         * ADVANCING THE CONVERTER, one transaction per millisecond, instead of
         * idling. That is ten conversions per pass, so every pass this loop makes
         * reads a fully refilled median of all three channels rather than
         * one-thirtieth of one. */
        lk_park_hold_ms(30u);
    }
}

/* ── boot.img ── */

static uint32_t rd32le(const uint8_t* p, uint32_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1u] << 8) | ((uint32_t)p[off + 2u] << 16) |
           ((uint32_t)p[off + 3u] << 24);
}

static uint32_t round_up(uint32_t value, uint32_t align) {
    return (value + align - 1u) & ~(align - 1u);
}

static int has_mtk_header(const uint8_t* p) {
    return rd32le(p, 0u) == MTK_IMG_MAGIC;
}

static int addr_in_dram(uint32_t addr, uint32_t len) {
    if (addr < DRAM_BASE || len == 0u) return 0;
    if (addr >= DRAM_LIMIT) return 0;
    return len <= (DRAM_LIMIT - addr);
}

typedef struct {
    uint32_t kernel_entry;
    uint32_t ramdisk_addr;
    uint32_t ramdisk_size;
    uint32_t tags_addr;
    char cmdline[512];
} lk_boot_image_t;

/*
 * Read one payload out of the BOOTIMG slot into DRAM, transparently stripping a
 * 512-byte MTK image header if the payload carries one (our packer puts one on
 * the kernel and not on the ramdisk; the stock LK strips it the same way).
 *
 * eMMC reads must be 512-aligned in both offset and length, so the tail read
 * rounds up — the few slack bytes land in the reserved space past the payload.
 */
/*
 * Is the window a payload is about to land in actually backed by DRAM?
 *
 * Worth asking because the alternative explanations for a board that stops
 * mid-load are expensive to chase. The LK writes with the MMU off, so nothing
 * traps a store to an address the EMI does not decode; it either lands
 * somewhere unintended or wedges the bus, and both look identical from here — a
 * boot that simply stops. This settles it before the read rather than after.
 *
 * One word per 4 KiB, written then read back with an address-derived pattern so
 * an aliased region fails as loudly as an absent one. That is ~950 accesses
 * across a 3.8 MB window: enough to find a region that is not there, cheap
 * enough not to change the timing of the boot it is diagnosing. It is not a
 * memory test and is not meant to be one.
 */
static int lk_dram_probe(const char* what, uint32_t base, uint32_t len) {
    const uint32_t step = 4096u;

    for (uint32_t off = 0; off < len; off += step) {
        volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)(base + off);
        const uint32_t pattern = (base + off) ^ 0xa5a55a5au;

        *p = pattern;
        if (*p != pattern) {
            lk_log("lk: ");
            lk_log(what);
            lk_log(" DRAM probe failed\n");
            lk_log_hex("lk: first bad address=", base + off);
            return -1;
        }
    }
    lk_log("lk: ");
    lk_log(what);
    lk_log(" DRAM window ok\n");
    return 0;
}

static int lk_load_payload(const char* what, uint32_t slot_off, uint32_t payload_len, uint32_t dst,
                           uint32_t* loaded_len) {
    uint32_t src = MVII_MT6592_LK_BOOTIMG_OFFSET + slot_off;
    uint32_t len = payload_len;
    int err;

    if (len == 0u) return MT6592_MSDC_OK;
    if (slot_off + len > MVII_MT6592_LK_BOOTIMG_SIZE) {
        lk_log("lk: ");
        lk_log(what);
        lk_log(" overruns the BOOTIMG slot\n");
        return -1;
    }

    err = mt6592_emmc_read_user((uint64_t)src, g_page, 512u);
    if (err != MT6592_MSDC_OK) return err;
    if (has_mtk_header(g_page) && len > 512u) {
        src += 512u;
        len -= 512u;
    }

    if (!addr_in_dram(dst, len)) {
        lk_log("lk: ");
        lk_log(what);
        lk_log(" load address is not DRAM\n");
        return -1;
    }

    /* addr_in_dram() only checks the window this image is willing to use. Ask
     * the hardware whether the memory is really there before reading into it. */
    if (lk_dram_probe(what, dst, round_up(len, 512u)) != 0) return -1;

    /*
     * Megabytes of PIO ahead — the longest uninterrupted load of the whole
     * boot, and the point the board was dying at. Do it in segments rather than
     * one call, for two reasons that happen to point the same way:
     *
     *  - Diagnosis. Every segment logs its running total, and with storage up
     *    every log line is autoflushed to the eMMC record, so a board that stops
     *    mid-read says over USB how many bytes it had managed. A stop that lands
     *    on the same offset every time is a bug in this code; one that wanders
     *    between boots is the rail giving out. Those two want opposite fixes and
     *    from the outside they are indistinguishable.
     *  - Survival. mt6592_emmc_read_part() re-opens the PMIC input path and
     *    disarms the charger watchdog once per call. Across a 3.8 MB read that
     *    was once, at the start. Per segment it is often enough to matter on a
     *    board with no cell, where a bulk burst is already on record as one of
     *    the loads that collapses VSYS.
     *
     * The watchdog is disabled at entry; kicking it here covers a warm path that
     * left it armed with a timeout shorter than the read.
     */
    for (uint32_t done = 0u, total = round_up(len, 512u); done < total;) {
        uint32_t chunk = total - done;

        if (chunk > LK_LOAD_SEGMENT_BYTES) chunk = LK_LOAD_SEGMENT_BYTES;
        mtk_watchdog_kick();
        err = mt6592_emmc_read_user((uint64_t)(src + done), (uint8_t*)(uintptr_t)(dst + done), chunk);
        mtk_watchdog_kick();
        if (err != MT6592_MSDC_OK) return err;
        done += chunk;
        lk_log(what);
        lk_log_hex(" +", done);
    }

    if (loaded_len) *loaded_len = len;
    return MT6592_MSDC_OK;
}

static int lk_load_boot_image(lk_boot_image_t* out) {
    uint32_t page_size;
    uint32_t kernel_size;
    uint32_t kernel_addr;
    uint32_t ramdisk_size;
    uint32_t ramdisk_addr;
    uint32_t tags_addr;
    uint32_t ramdisk_loaded = 0u;
    int err;

    err = mt6592_emmc_read_user((uint64_t)MVII_MT6592_LK_BOOTIMG_OFFSET, g_page, 2048u);
    if (err != MT6592_MSDC_OK) {
        lk_log_hex("lk: BOOTIMG header read failed err=", (uint32_t)err);
        return err;
    }

    if (g_page[0] != 'A' || g_page[1] != 'N' || g_page[2] != 'D' || g_page[3] != 'R' || g_page[4] != 'O' ||
        g_page[5] != 'I' || g_page[6] != 'D' || g_page[7] != '!') {
        lk_log("lk: BOOTIMG is not an Android boot image\n");
        return -1;
    }

    kernel_size = rd32le(g_page, 8u);
    kernel_addr = rd32le(g_page, 12u);
    ramdisk_size = rd32le(g_page, 16u);
    ramdisk_addr = rd32le(g_page, 20u);
    tags_addr = rd32le(g_page, 32u);
    page_size = rd32le(g_page, 36u);

    /* Android header cmdline is 512 bytes at offset 64. Stock's is empty, so
     * lk_build_atags() falls back to the stock LK default; a rebuilt boot.img
     * that actually filled this field still wins. */
    {
        uint32_t i;
        for (i = 0u; i < 511u && g_page[64u + i] != 0; ++i) out->cmdline[i] = (char)g_page[64u + i];
        out->cmdline[i] = 0;
    }

    if (page_size < 512u || page_size > 16384u || (page_size & 511u) != 0u) page_size = 2048u;
    if (!addr_in_dram(kernel_addr, kernel_size) || kernel_size == 0u) {
        lk_log("lk: BOOTIMG kernel geometry is unusable\n");
        return -1;
    }
    if (!addr_in_dram(tags_addr, 256u) || (tags_addr & 3u) != 0u) {
        lk_log("lk: BOOTIMG tags address is unusable\n");
        return -1;
    }

    lk_log_hex("lk: kernel addr=", kernel_addr);
    lk_log_hex("lk: kernel size=", kernel_size);
    lk_log_hex("lk: ramdisk addr=", ramdisk_addr);
    lk_log_hex("lk: ramdisk size=", ramdisk_size);
    lk_mark(MT6592_BOOT_STATUS_STAGE_LK_BOOTIMG_HEADER, "lk: BOOTIMG header accepted\n", LK_BEACON_HEADER);

    err = lk_load_payload("kernel", page_size, kernel_size, kernel_addr, 0);
    if (err != MT6592_MSDC_OK) {
        lk_log_hex("lk: kernel load failed err=", (uint32_t)err);
        return err;
    }
    lk_mark(MT6592_BOOT_STATUS_STAGE_LK_KERNEL_LOADED, "lk: kernel in DRAM\n", LK_BEACON_KERNEL);

    if (ramdisk_size != 0u) {
        if (!addr_in_dram(ramdisk_addr, ramdisk_size)) {
            lk_log("lk: BOOTIMG ramdisk geometry is unusable\n");
            return -1;
        }
        err = lk_load_payload("ramdisk", page_size + round_up(kernel_size, page_size), ramdisk_size,
                              ramdisk_addr, &ramdisk_loaded);
        if (err != MT6592_MSDC_OK) {
            lk_log_hex("lk: ramdisk load failed err=", (uint32_t)err);
            return err;
        }
    }
    lk_mark(MT6592_BOOT_STATUS_STAGE_LK_RAMDISK_LOADED, "lk: ramdisk in DRAM\n", LK_BEACON_RAMDISK);

    out->kernel_entry = kernel_addr;
    out->ramdisk_addr = ramdisk_addr;
    out->ramdisk_size = ramdisk_loaded;
    out->tags_addr = tags_addr;
    return MT6592_MSDC_OK;
}

/*
 * ATAGs the stock 3.10 kernel (and stage1) will actually accept.
 *
 * Stage1 only walks ATAG_CORE / ATAG_MTK_BOOT / ATAG_INITRD2 and skips the
 * rest, so extra tags are free for a card-less board that still has the stock
 * boot.img in the BOOTIMG slot. That kernel has no appended DTB — its own
 * "Warning: Neither atags nor dtb found" string is what you get if r2 is
 * garbage — and mt_fixup() sums ATAG_MEM to decide how much DRAM exists. A
 * list without that tag hands it zero banks and the stock OS never comes up.
 *
 * The stock LK (FUN_81e1955c / Reference/MT6592LK/.../lk/atags.c) also writes
 * ATAG_CMDLINE and ATAG_VIDEOLFB. The cmdline is the kernel's own CONFIG_CMDLINE
 * when boot.img left the field empty; the framebuffer tag is only emitted if
 * this image actually lit the panel, and then names the same 0x82700000 canvas
 * the stock LK scanned out.
 */
static uint32_t lk_atag_puts(uint32_t* dst, const char* text) {
    uint8_t* bytes = (uint8_t*)(void*)dst;
    uint32_t n = 0u;

    while (text[n] != 0) {
        bytes[n] = (uint8_t)text[n];
        ++n;
    }
    bytes[n] = 0;
    return (n + 1u + 3u) / 4u;
}

static void lk_build_atags(const lk_boot_image_t* img) {
    uint32_t* p = (uint32_t*)(uintptr_t)img->tags_addr;
    const char* cmdline = img->cmdline[0] != 0 ? img->cmdline : MVII_MT6592_LK_STOCK_CMDLINE;

    *p++ = 2u;
    *p++ = ATAG_CORE;

    *p++ = 3u;
    *p++ = ATAG_MTK_BOOT;
    *p++ = MTK_BOOT_MODE_NORMAL;

    *p++ = 4u;
    *p++ = ATAG_MEM;
    *p++ = (uint32_t)MVII_MT6592_LK_DRAM_SIZE;
    *p++ = (uint32_t)MVII_MT6592_LK_DRAM_START;

    {
        uint32_t* size_cell = p++;
        uint32_t words;

        *p++ = ATAG_CMDLINE;
        words = lk_atag_puts(p, cmdline);
        *size_cell = 2u + words;
        p += words;
    }

    if (g_fb_live) {
        *p++ = 8u;
        *p++ = ATAG_VIDEOLFB;
        *p++ = ((uint32_t)MVII_MT6592_LK_FB_HEIGHT << 16) | (uint32_t)MVII_MT6592_LK_FB_WIDTH;
        *p++ = ((uint32_t)MVII_MT6592_LK_FB_PITCH << 16) | (uint32_t)MVII_MT6592_LK_FB_BPP;
        *p++ = (uint32_t)MVII_MT6592_LK_FB_ADDR;
        *p++ = (uint32_t)MVII_MT6592_LK_FB_PITCH * (uint32_t)MVII_MT6592_LK_FB_HEIGHT;
        *p++ = 0u;
        *p++ = 0u;
    }

    if (img->ramdisk_size != 0u) {
        *p++ = 4u;
        *p++ = ATAG_INITRD2;
        *p++ = img->ramdisk_addr;
        *p++ = img->ramdisk_size;
    }

    *p++ = 0u;
    *p++ = ATAG_NONE;
    __asm__ volatile("dsb sy" ::: "memory");
}

#ifdef MVII_MT6592_LK_SD_HANDOFF
/* ══════════════════════════════════════════════════════════════════════════
 * SD hand-off — the release build's reason to exist
 *
 * Everything above this point boots the device's own OS out of eMMC. This
 * boots somebody else's, off the card in the slot: a kernel, a DTB and an
 * optional initramfs read out of the card's boot volume, handed over the way
 * Linux asks to be handed over (r0=0, r1=machtype, r2=DTB) rather than the way
 * MediaTek's Android chain does (ATAGs). One use for it is a plain Debian on the
 * card; it is not limited to that and does not know what it is booting.
 *
 * WHY IT IS A SEPARATE IMAGE rather than a runtime choice. There is exactly one
 * UBOOT partition and one resident bootloader on this board. A single LK that
 * tried to be both would have to decide, on every single boot of every device,
 * whether the card in the slot is meant to be booted or is just storage — and
 * it would get that wrong on the first card someone drops in to copy a file
 * off. Which bootloader is resident IS the answer to that question, and
 * `flash -upload release` is how you change it.
 *
 * WHAT IT DOES NOT DO: fail the boot. Every step below logs its reason and
 * returns non-zero, and the caller falls back to the eMMC boot.img. A release
 * board with no card in the slot, or a card with no kernel on it, still comes
 * up in its own OS. That is not a courtesy — it is the difference between a
 * bootloader you can leave installed and one you have to re-flash to recover.
 *
 * THE CONTRACT WITH THE CARD is deliberately small enough to satisfy by hand:
 * a FAT partition with a zImage and a DTB on it boots. /mvii/boot.conf is how
 * you say anything else — filenames, the kernel command line, load addresses.
 * The volume is found by parsing BPBs rather than by trusting a partition type
 * byte, so a card partitioned by any tool works.
 *
 * AND IT ALSO READS A dArkOS CARD AS IT COMES. That is the case this had to grow
 * to cover: an image written by the dArkOS build has a 100 MiB FAT32 partition
 * labelled BOOT holding a kernel, a `uInitrd`, a device tree and a u-boot script
 * called boot.ini, plus a multi-gigabyte second FAT volume (EASYROMS) full of
 * game files. Three things follow, and each one is a way the old code got it
 * wrong:
 *
 *   - "the first FAT volume" is no longer an answer. The volume labelled BOOT is
 *     tried first and the first-that-parses search is the fallback.
 *   - boot.conf is not the only boot script worth reading. When the card has a
 *     boot.ini, the filenames and the command line come out of it, so a card
 *     that boots on the hardware it was built for needs no second file here.
 *     The load addresses in it are NOT used: they are a different SoC's DRAM map.
 *   - `uInitrd` is not an initramfs. It is one behind a 64-byte U-Boot header,
 *     and a kernel handed the header boots and then cannot find /init.
 *
 * One consequence is important enough to state on its own: an R36S dArkOS card
 * carries an ARM64 kernel, and this SoC is ARMv7. The kernel image is checked
 * before the jump, because after the jump there is no fallback left.
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * Load addresses, and why these ones.
 *
 * The kernel goes at the conventional ARM zreladdr, DRAM+0x8000. A zImage
 * decompresses upward from there and relocates itself out of the way; 24 MiB of
 * headroom covers any ARMv7 kernel worth booting and stops short of the LK
 * framebuffer at 0x82700000, which the DTB's reserved-memory node hands to
 * simple-framebuffer and which therefore has to survive the decompression.
 *
 * The DTB goes above the framebuffer with a 256 KiB window around it, not a
 * tight fit: mt6592_fdt_setprop() grows the tree, and a bootloader that ran out
 * of room to write a command line would be a strange thing to build. The initrd
 * goes above that, high enough that nothing below reaches it.
 */
#ifndef MVII_MT6592_LK_SD_KERNEL_ADDR
#define MVII_MT6592_LK_SD_KERNEL_ADDR 0x80008000u
#endif
#ifndef MVII_MT6592_LK_SD_KERNEL_MAX
#define MVII_MT6592_LK_SD_KERNEL_MAX 0x01800000u
#endif
#ifndef MVII_MT6592_LK_SD_DTB_ADDR
#define MVII_MT6592_LK_SD_DTB_ADDR 0x83000000u
#endif
#ifndef MVII_MT6592_LK_SD_DTB_WINDOW
#define MVII_MT6592_LK_SD_DTB_WINDOW 0x00040000u
#endif
#ifndef MVII_MT6592_LK_SD_INITRD_ADDR
#define MVII_MT6592_LK_SD_INITRD_ADDR 0x84000000u
#endif
#ifndef MVII_MT6592_LK_SD_INITRD_MAX
#define MVII_MT6592_LK_SD_INITRD_MAX 0x06000000u
#endif

#define SD_CONF_PATH "/mvii/boot.conf"
#define SD_CONF_MAX 2048u

/* The card's own loader script, and the label of the volume it lives on. Both
 * are what a dArkOS image writes; both are optional. */
#define SD_INI_PATH "/boot.ini"
#define SD_INI_MAX 2048u
#define SD_BOOT_LABEL "BOOT"

/*
 * Defaults that boot a card carrying nothing but a kernel and a tree.
 *
 * The root device is the one guess in here, and it is a guess: which mmcblk
 * index Linux gives the removable card depends on the order its own MSDC driver
 * probes the four controllers, which is a property of the kernel on the card and
 * not of this bootloader. That is precisely what `bootargs=' in boot.conf is
 * for, and the image builder writes it. A kernel that boots to its initramfs and
 * stops with "unable to mount root" is telling you to set it. A card written by
 * the dArkOS build sidesteps the guess entirely: its boot.ini says
 * root=LABEL=ROOTFS, which no controller ordering can get wrong.
 */
static const char kSdDefaultKernel[] = "zImage";
static const char kSdDefaultDtb[] = "mt6592-j36-ultra.dtb";
static const char kSdDefaultInitrd[] = "initrd.img";
static const char kSdDefaultArgs[] = "console=tty0 console=ttyS0,115200n8 "
                                     "earlycon=mtk8250,mmio32,0x11002000 "
                                     "root=/dev/mmcblk1p2 rootwait rw";

/*
 * Other names the same payload goes by, tried only when nothing named it. A
 * kernel built the Debian way is `vmlinuz`, one wrapped for u-boot is `uImage`,
 * one straight out of arch/arm/boot is `Image` or `zImage`; an initramfs is
 * `initrd.img` by Debian convention and `uInitrd` once mkimage has been at it.
 * A name that came from a boot script is never second-guessed — see sd_pick().
 */
static const char* const kSdKernelNames[] = { "zImage", "uImage", "Image", "vmlinuz", 0 };
static const char* const kSdInitrdNames[] = { "initrd.img", "uInitrd", "initramfs.img", 0 };

typedef struct {
    const char* kernel;
    const char* dtb;
    const char* initrd;
    const char* bootargs;
    uint32_t kernel_addr;
    uint32_t dtb_addr;
    uint32_t initrd_addr;
} sd_boot_conf_t;

/* Which fields a boot script set, as opposed to which are still the built-in
 * defaults. Two uses: the name search below only runs for a default, and the log
 * can say where a value came from. */
typedef struct {
    int kernel;
    int dtb;
    int initrd;
    int bootargs;
} sd_conf_origin_t;

/* Backing store for the strings above: both scripts are parsed in place and the
 * conf struct points into them, so these outlive the parse and nothing is
 * copied twice. They are separate buffers because boot.ini is parsed first and
 * boot.conf then overrides it — one buffer would free the values it is
 * overriding only some of. */
static char g_sd_conf[SD_CONF_MAX + 1u];
static char g_sd_ini[SD_INI_MAX + 1u];
static mvii_fat_volume g_sd_fs;

static uint32_t sd_strlen(const char* s) {
    uint32_t n = 0u;

    while (s[n] != 0) ++n;
    return n;
}

static int sd_streq(const char* a, const char* b) {
    while (*a != 0 && *b != 0) {
        if (*a != *b) return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

static int sd_is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* "0x81234567" or plain decimal. Returns 0 on anything it cannot parse whole,
 * which the caller treats as "keep the default" rather than "load at zero". */
static int sd_parse_u32(const char* s, uint32_t* out) {
    uint32_t v = 0u;
    int digits = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s != 0) {
            uint32_t d;

            if (*s >= '0' && *s <= '9') d = (uint32_t)(*s - '0');
            else if (*s >= 'a' && *s <= 'f') d = (uint32_t)(*s - 'a') + 10u;
            else if (*s >= 'A' && *s <= 'F') d = (uint32_t)(*s - 'A') + 10u;
            else return 0;
            v = (v << 4) | d;
            ++digits;
            ++s;
        }
    } else {
        while (*s != 0) {
            if (*s < '0' || *s > '9') return 0;
            v = v * 10u + (uint32_t)(*s - '0');
            ++digits;
            ++s;
        }
    }
    if (digits == 0) return 0;
    *out = v;
    return 1;
}

static char sd_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* Case-insensitive, because the names on a FAT volume are whatever the tool that
 * wrote them felt like and the tree is matched by suffix either way. */
static int sd_ends_with(const char* s, const char* suffix) {
    const uint32_t n = sd_strlen(s);
    const uint32_t m = sd_strlen(suffix);
    uint32_t i;

    if (m == 0u || m > n) return 0;
    for (i = 0u; i < m; ++i) {
        if (sd_lower(s[n - m + i]) != sd_lower(suffix[i])) return 0;
    }
    return 1;
}

static int sd_contains(const char* s, const char* needle) {
    const uint32_t n = sd_strlen(s);
    const uint32_t m = sd_strlen(needle);
    uint32_t i;
    uint32_t j;

    if (m == 0u || m > n) return 0;
    for (i = 0u; i + m <= n; ++i) {
        for (j = 0u; j < m; ++j) {
            if (sd_lower(s[i + j]) != sd_lower(needle[j])) break;
        }
        if (j == m) return 1;
    }
    return 0;
}

static char* sd_skip_space(char* p) {
    while (*p != 0 && sd_is_space(*p)) ++p;
    return p;
}

/* Next whitespace-delimited token, terminated where it sits. Advances *p past
 * it; returns 0 at end of line. */
static char* sd_token(char** p) {
    char* s = sd_skip_space(*p);
    char* e;

    if (*s == 0) {
        *p = s;
        return 0;
    }
    e = s;
    while (*e != 0 && !sd_is_space(*e)) ++e;
    if (*e != 0) {
        *e = 0;
        ++e;
    }
    *p = e;
    return s;
}

/* Everything left on the line as one value, with a surrounding pair of quotes
 * removed. `setenv bootargs "root=... rw"` is one value with spaces in it, which
 * is exactly why boot.ini needs its own parse and not the boot.conf one. */
static char* sd_rest(char* p) {
    char* s = sd_skip_space(p);
    uint32_t n;

    if (*s == '"' || *s == '\'') {
        const char quote = *s++;
        char* e = s;

        while (*e != 0 && *e != quote) ++e;
        *e = 0;
        return s;
    }
    n = sd_strlen(s);
    while (n > 0u && sd_is_space(s[n - 1u])) s[--n] = 0;
    return s;
}

/*
 * U-Boot's legacy image header: 64 big-endian bytes in front of the payload.
 *
 * dArkOS ships its initramfs as `uInitrd`, which is a cpio.gz behind one of
 * these, because u-boot's bootm takes the length and the type from the header
 * rather than from the filesystem. A loader that pointed linux,initrd-start at
 * the header instead of the payload would produce a kernel that boots, fails to
 * recognise its initramfs and panics looking for /init — a failure that looks
 * nothing like its cause, which is why this is detected rather than assumed
 * away.
 *
 * Only the magic is checked. The CRCs cover the whole payload, and re-reading a
 * 12 MiB initramfs to confirm the card read it correctly is a minute of a boot
 * spent on the answer the card already gave.
 */
#define SD_UIMAGE_MAGIC 0x27051956u
#define SD_UIMAGE_HDR 64u
#define SD_UIMAGE_TYPE_KERNEL 2u
#define SD_UIMAGE_TYPE_RAMDISK 3u

static uint32_t sd_rd_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t sd_rd_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int sd_uimage_header(uint32_t addr, uint32_t len, uint32_t* out_size, uint32_t* out_type,
                            uint32_t* out_load) {
    const uint8_t* h = (const uint8_t*)(uintptr_t)addr;
    uint32_t size;

    if (len <= SD_UIMAGE_HDR) return 0;
    if (sd_rd_be32(&h[0]) != SD_UIMAGE_MAGIC) return 0;

    /* A header that disagrees with the file loses: the file length is measured,
     * the header field is copied. */
    size = sd_rd_be32(&h[12]);
    if (size == 0u || size > len - SD_UIMAGE_HDR) size = len - SD_UIMAGE_HDR;

    *out_size = size;
    *out_type = (uint32_t)h[30];
    *out_load = sd_rd_be32(&h[16]);
    return 1;
}

/* Slide a payload down over the header it arrived behind, so the kernel's entry
 * point ends up at the address the tree and the jump both name. Forward copy
 * with dst below src, word-aligned at both ends because the header is 64 bytes. */
static void sd_shift_down(uint32_t dst, uint32_t src, uint32_t len) {
    uint32_t* d = (uint32_t*)(uintptr_t)dst;
    const uint32_t* s = (const uint32_t*)(uintptr_t)src;
    const uint32_t words = len >> 2;
    const uint32_t tail = len & 3u;
    uint32_t i;

    for (i = 0u; i < words; ++i) {
        d[i] = s[i];
        if ((i & 0xffffu) == 0u) mtk_watchdog_kick();
    }
    if (tail != 0u) {
        uint8_t* db = (uint8_t*)(uintptr_t)(dst + (words << 2));
        const uint8_t* sb = (const uint8_t*)(uintptr_t)(src + (words << 2));

        for (i = 0u; i < tail; ++i) db[i] = sb[i];
    }
    mtk_watchdog_kick();
    __asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Is this thing an ARMv7 kernel at all?
 *
 * An ARM64 Linux kernel says so at offset 0x38: the arm64 boot header's magic,
 * "ARM\x64". A dArkOS card built for an R36S carries exactly that, it fits in
 * this device's card slot, and an ARMv7 core told to execute it gets as far as
 * the first instruction it cannot decode — after the jump, with the eMMC
 * fallback already behind us. So the check happens here, where "no" still means
 * the device boots its own OS.
 *
 * A 32-bit zImage answers with 0x016f2818 at 0x24. A raw arch/arm/boot/Image
 * answers with nothing at all and is still bootable at zreladdr, so that case
 * is a line in the log rather than a refusal.
 */
#define SD_ZIMAGE_MAGIC 0x016f2818u
#define SD_ARM64_MAGIC 0x644d5241u

static int sd_kernel_is_armv7(uint32_t addr, uint32_t len) {
    const uint8_t* k = (const uint8_t*)(uintptr_t)addr;

    if (len < 0x40u) {
        lk_log("sd: the kernel file is too short to be a kernel\n");
        return 0;
    }
    if (sd_rd_le32(&k[0x38]) == SD_ARM64_MAGIC) {
        lk_log("sd: that kernel is an arm64 Image and this SoC is ARMv7; booting eMMC\n");
        return 0;
    }
    if (sd_rd_le32(&k[0x24]) == SD_ZIMAGE_MAGIC) {
        lk_log("sd: kernel is a zImage\n");
        return 1;
    }
    lk_log("sd: kernel carries no zImage magic; entering it as a raw Image\n");
    return 1;
}

static int sd_block_read(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    (void)ctx;
    return (mt6592_sd_read(lba, count, buffer) == MT6592_MSDC_OK) ? 0 : -1;
}

/*
 * Called once per cluster of every file read. A multi-megabyte kernel off a
 * slow card is minutes of uninterrupted PIO, which is longer than any watchdog
 * worth arming and — on a board with no cell fitted, where the charger block is
 * the only thing holding VSYS — longer than the charger's own watchdog too. The
 * kick is free and goes every cluster; the charger service is a handful of
 * pwrap transactions and goes every 32nd, which on a 32 KiB cluster is once a
 * megabyte.
 */
static void sd_progress(void* ctx, uint32_t done, uint32_t total) {
    static uint32_t tick;

    (void)ctx;
    (void)total;
    mtk_watchdog_kick();
    if ((++tick & 31u) == 0u) mt6592_pmic_power_hold();
    if ((tick & 255u) == 0u) lk_log_hex("sd:   +", done);
}

static void sd_conf_defaults(sd_boot_conf_t* c) {
    c->kernel = kSdDefaultKernel;
    c->dtb = kSdDefaultDtb;
    c->initrd = kSdDefaultInitrd;
    c->bootargs = kSdDefaultArgs;
    c->kernel_addr = MVII_MT6592_LK_SD_KERNEL_ADDR;
    c->dtb_addr = MVII_MT6592_LK_SD_DTB_ADDR;
    c->initrd_addr = MVII_MT6592_LK_SD_INITRD_ADDR;
}

/*
 * key=value, one per line, '#' to end of line is a comment, blanks ignored.
 *
 * Parsed in place: every value is NUL-terminated where it sits and the conf
 * struct points at it. That is why g_sd_conf is a file-scope buffer and not a
 * local — the pointers outlive this function by design.
 */
static void sd_conf_parse(sd_boot_conf_t* c, char* text, uint32_t len, sd_conf_origin_t* o) {
    uint32_t i = 0u;

    text[len] = 0;
    while (i < len) {
        char* key;
        char* value;
        uint32_t klen;
        uint32_t start = i;

        while (i < len && text[i] != '\n') ++i;
        text[i] = 0;
        if (i < len) ++i;

        key = &text[start];
        while (*key != 0 && sd_is_space(*key)) ++key;
        if (*key == '#' || *key == 0) continue;

        value = key;
        while (*value != 0 && *value != '=') ++value;
        if (*value != '=') continue;
        *value++ = 0;
        while (*value != 0 && sd_is_space(*value)) ++value;

        /* Trailing comment, then trailing whitespace. A path with a '#' in it
         * is not a path anyone should be writing here. */
        {
            char* hash = value;

            while (*hash != 0 && *hash != '#') ++hash;
            *hash = 0;
        }
        klen = sd_strlen(key);
        while (klen > 0u && sd_is_space(key[klen - 1u])) key[--klen] = 0;
        {
            uint32_t vlen = sd_strlen(value);

            while (vlen > 0u && sd_is_space(value[vlen - 1u])) value[--vlen] = 0;
            if (vlen == 0u) continue;
        }

        if (sd_streq(key, "kernel")) { c->kernel = value; o->kernel = 1; }
        else if (sd_streq(key, "dtb")) { c->dtb = value; o->dtb = 1; }
        else if (sd_streq(key, "initrd")) { c->initrd = value; o->initrd = 1; }
        else if (sd_streq(key, "bootargs")) { c->bootargs = value; o->bootargs = 1; }
        else if (sd_streq(key, "kernel_addr")) (void)sd_parse_u32(value, &c->kernel_addr);
        else if (sd_streq(key, "dtb_addr")) (void)sd_parse_u32(value, &c->dtb_addr);
        else if (sd_streq(key, "initrd_addr")) (void)sd_parse_u32(value, &c->initrd_addr);
    }
}

/*
 * boot.ini — the card's own u-boot script, read when the card has one.
 *
 * A dArkOS card carries a boot.ini and no boot.conf, and a card that boots on
 * the hardware it was built for should not need a second configuration file to
 * boot here. Two things in that script are true no matter which bootloader reads
 * it: the kernel command line, and which files to load. Those are taken.
 *
 * The load addresses are deliberately NOT taken. boot.ini asks for the kernel at
 * 0x02000000, which is DRAM on an RK3326 and is not memory at all on an MT6592 —
 * a loader that trusted them would write a kernel into a hole and jump to it.
 * The addresses stay this file's business, which is where the knowledge of this
 * SoC's memory map lives.
 *
 * Which loaded file is which comes from the filename rather than from the u-boot
 * variable it is loaded into: a name ending in `.dtb` is the tree, one containing
 * "initr" is the initramfs, and the first of anything else is the kernel. That
 * reads every boot.ini this has been pointed at without a symbol table for
 * somebody else's environment, and /mvii/boot.conf is parsed afterwards so an
 * operator has the last word over any of it.
 *
 * Returns non-zero when it found something usable, so the caller can tell "the
 * card has a script" from "the card has a script for something else entirely".
 */
static int sd_ini_parse(sd_boot_conf_t* c, char* text, uint32_t len, sd_conf_origin_t* o) {
    uint32_t i = 0u;
    int used = 0;

    text[len] = 0;
    while (i < len) {
        char* line;
        char* cursor;
        char* word;
        const uint32_t start = i;

        while (i < len && text[i] != '\n') ++i;
        text[i] = 0;
        if (i < len) ++i;

        line = &text[start];
        {
            char* hash = line;

            while (*hash != 0 && *hash != '#') ++hash;
            *hash = 0;
        }
        cursor = line;
        word = sd_token(&cursor);
        if (word == 0) continue;

        if (sd_streq(word, "setenv") || sd_streq(word, "env")) {
            char* name = sd_token(&cursor);

            if (name != 0 && sd_streq(name, "bootargs")) {
                c->bootargs = sd_rest(cursor);
                o->bootargs = 1;
                used = 1;
            }
            continue;
        }

        if (sd_streq(word, "load") || sd_streq(word, "fatload") || sd_streq(word, "ext4load") ||
            sd_streq(word, "ext2load")) {
            char* file = 0;
            char* tok;

            /* The filename is the last token whatever the loader's argument
             * order is, and every loader in that list puts it last. */
            while ((tok = sd_token(&cursor)) != 0) file = tok;
            if (file == 0) continue;

            if (sd_ends_with(file, ".dtb")) {
                if (o->dtb == 0) { c->dtb = file; o->dtb = 1; used = 1; }
            } else if (sd_contains(file, "initr")) {
                if (o->initrd == 0) { c->initrd = file; o->initrd = 1; used = 1; }
            } else if (o->kernel == 0) {
                c->kernel = file;
                o->kernel = 1;
                used = 1;
            }
        }
    }
    return used;
}

/*
 * Resolve a payload name against what is actually on the volume.
 *
 * A name that came out of a boot script is used as written, even when it is not
 * there: an operator who typed `kernel=zimage-6.1` wants to be told that file is
 * missing, not to have the loader boot something else it happened to find. The
 * candidate list is only for the built-in defaults, and it exists because the
 * same kernel is called four different things depending on who built it.
 */
static const char* sd_pick(const char* configured, int from_script, const char* const* candidates,
                           const char* what) {
    mvii_fat_dirent entry;
    uint32_t i;

    if (mvii_fat_stat(&g_sd_fs, configured, &entry) == MVII_FAT_OK && entry.is_dir == 0) return configured;
    if (from_script) return configured;

    for (i = 0u; candidates[i] != 0; ++i) {
        if (sd_streq(candidates[i], configured)) continue;
        if (mvii_fat_stat(&g_sd_fs, candidates[i], &entry) == MVII_FAT_OK && entry.is_dir == 0) {
            lk_log("sd: no ");
            lk_log(configured);
            lk_log("; the ");
            lk_log(what);
            lk_log(" on this card is ");
            lk_log(candidates[i]);
            lk_log("\n");
            return candidates[i];
        }
    }
    return configured;
}

static int sd_read_into(const char* what, const char* path, uint32_t addr, uint32_t max, uint32_t* out_len) {
    uint32_t len = 0u;
    int rc;

    if (!addr_in_dram(addr, max)) {
        lk_log("sd: ");
        lk_log(what);
        lk_log(" load address is not DRAM\n");
        return -1;
    }

    mvii_fat_set_progress(&g_sd_fs, sd_progress, 0);
    rc = mvii_fat_read_file(&g_sd_fs, path, (void*)(uintptr_t)addr, max, &len);
    mvii_fat_set_progress(&g_sd_fs, 0, 0);
    mtk_watchdog_kick();
    mt6592_pmic_power_hold();

    if (rc != MVII_FAT_OK) {
        lk_log("sd: ");
        lk_log(path);
        lk_log(": ");
        lk_log(mvii_fat_strerror(rc));
        lk_log("\n");
        if (rc == MVII_FAT_ERR_TOOBIG) lk_log_hex("sd: file size=", len);
        return -1;
    }

    lk_log("sd: ");
    lk_log(what);
    lk_log(" ");
    lk_log(path);
    lk_log_hex(" bytes=", len);
    if (out_len) *out_len = len;
    return 0;
}

/*
 * Hand the loader's facts to the kernel through the tree it is about to be
 * given. Three of them are things only a bootloader can know — the command
 * line the operator configured, and where the initramfs actually landed — and
 * one is provenance, so a board that comes up in Linux can say which loader put
 * it there without anybody having to guess from the console.
 *
 * A failure here is fatal to the SD boot and only to the SD boot: handing a
 * kernel a tree we know is half-patched is worse than not booting it, and the
 * eMMC fallback is right there.
 */
static int sd_patch_fdt(uint32_t dtb_addr, uint32_t dtb_len, const sd_boot_conf_t* conf, uint32_t initrd_addr,
                        uint32_t initrd_size) {
    mt6592_fdt_t fdt;
    int rc;

    (void)dtb_len;
    rc = mt6592_fdt_open(&fdt, (void*)(uintptr_t)dtb_addr, MVII_MT6592_LK_SD_DTB_WINDOW);
    if (rc != MT6592_FDT_OK) {
        lk_log("sd: device tree rejected: ");
        lk_log(mt6592_fdt_strerror(rc));
        lk_log("\n");
        return -1;
    }

    rc = mt6592_fdt_setprop_string(&fdt, "/chosen", "bootargs", conf->bootargs);
    if (rc != MT6592_FDT_OK) goto failed;

    if (initrd_size != 0u) {
        rc = mt6592_fdt_setprop_u32(&fdt, "/chosen", "linux,initrd-start", initrd_addr);
        if (rc != MT6592_FDT_OK) goto failed;
        rc = mt6592_fdt_setprop_u32(&fdt, "/chosen", "linux,initrd-end", initrd_addr + initrd_size);
        if (rc != MT6592_FDT_OK) goto failed;
    }

    rc = mt6592_fdt_setprop_string(&fdt, "/chosen", "mvii,loader", "MVII LK (release, SD hand-off)");
    if (rc != MT6592_FDT_OK) goto failed;

    lk_log("sd: bootargs: ");
    lk_log(conf->bootargs);
    lk_log("\n");
    lk_log_hex("sd: device tree size=", mt6592_fdt_size(&fdt));
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;

failed:
    lk_log("sd: device tree patch failed: ");
    lk_log(mt6592_fdt_strerror(rc));
    lk_log("\n");
    return -1;
}

/*
 * Try to boot the card. Returns 0 only when it is about to jump — it does not
 * return on success, because there is nowhere left to return to.
 */
static int lk_sd_boot(void) {
    sd_boot_conf_t conf;
    sd_conf_origin_t from_script;
    uint32_t kernel_len = 0u;
    uint32_t dtb_len = 0u;
    uint32_t initrd_len = 0u;
    uint32_t initrd_data;
    uint32_t conf_len = 0u;
    uint32_t usize = 0u;
    uint32_t utype = 0u;
    uint32_t uload = 0u;
    int want_initrd;
    int rc;

    sd_conf_defaults(&conf);
    from_script.kernel = 0;
    from_script.dtb = 0;
    from_script.initrd = 0;
    from_script.bootargs = 0;

    if (mt6592_sd_probe() != MT6592_MSDC_OK) {
        lk_log("sd: no card in the slot; booting eMMC\n");
        return -1;
    }
    lk_log_hex("sd: card ready, sectors=", (uint32_t)mt6592_sd_capacity_sectors());

    /*
     * The volume labelled BOOT first. On a dArkOS card the first FAT volume that
     * parses is the right one anyway, but the second one — EASYROMS, several
     * gigabytes of game files — is also FAT, and a card repartitioned in any
     * other order would have the loader hunting for a kernel in the ROM library.
     * The label is what the image builder controls and what the operator can see
     * on a desktop, so the label is what this asks for; the unlabelled search is
     * the fallback, which is what a card someone formatted by hand will hit.
     */
    rc = mvii_fat_mount_labeled(&g_sd_fs, sd_block_read, 0, SD_BOOT_LABEL);
    if (rc != MVII_FAT_OK) {
        lk_log("sd: no volume labelled " SD_BOOT_LABEL "; taking the first FAT volume\n");
        rc = mvii_fat_mount(&g_sd_fs, sd_block_read, 0);
    }
    if (rc != MVII_FAT_OK) {
        lk_log("sd: no FAT boot volume: ");
        lk_log(mvii_fat_strerror(rc));
        lk_log("\n");
        return -1;
    }
    lk_log("sd: boot volume ");
    lk_log(g_sd_fs.source);
    lk_log(" label '");
    lk_log(g_sd_fs.label);
    lk_log("'\n");

    /*
     * The card's own script first, this loader's override second. Neither is
     * required: a hand-made card with a kernel and a tree on it has neither, and
     * that is the case the defaults exist for.
     */
    if (mvii_fat_read_file(&g_sd_fs, SD_INI_PATH, g_sd_ini, SD_INI_MAX, &conf_len) == MVII_FAT_OK) {
        if (sd_ini_parse(&conf, g_sd_ini, conf_len, &from_script)) {
            lk_log("sd: " SD_INI_PATH " applied\n");
        } else {
            lk_log("sd: " SD_INI_PATH " names nothing this loader can use\n");
        }
    }
    if (mvii_fat_read_file(&g_sd_fs, SD_CONF_PATH, g_sd_conf, SD_CONF_MAX, &conf_len) == MVII_FAT_OK) {
        sd_conf_parse(&conf, g_sd_conf, conf_len, &from_script);
        lk_log("sd: " SD_CONF_PATH " applied\n");
    } else if (from_script.kernel == 0 && from_script.bootargs == 0) {
        lk_log("sd: no boot script on the card; using the built-in defaults\n");
    }

    /* A default name is matched against what is on the volume; a name somebody
     * wrote down is used as written. */
    want_initrd = (conf.initrd[0] != 0 && !sd_streq(conf.initrd, "none"));
    conf.kernel = sd_pick(conf.kernel, from_script.kernel, kSdKernelNames, "kernel");
    if (want_initrd) conf.initrd = sd_pick(conf.initrd, from_script.initrd, kSdInitrdNames, "initramfs");

    if (sd_read_into("kernel", conf.kernel, conf.kernel_addr, MVII_MT6592_LK_SD_KERNEL_MAX, &kernel_len) != 0)
        return -1;

    /* A uImage kernel is the payload behind a header, and the payload has to end
     * up at the entry address, so it moves down over the header rather than the
     * jump moving up: a zImage relocates itself relative to where it was entered
     * and the tree's reserved-memory nodes are absolute. */
    if (sd_uimage_header(conf.kernel_addr, kernel_len, &usize, &utype, &uload) &&
        utype == SD_UIMAGE_TYPE_KERNEL) {
        lk_log_hex("sd: kernel is a uImage, header load=", uload);
        sd_shift_down(conf.kernel_addr, conf.kernel_addr + SD_UIMAGE_HDR, usize);
        kernel_len = usize;
    }
    if (!sd_kernel_is_armv7(conf.kernel_addr, kernel_len)) return -1;

    if (sd_read_into("device tree", conf.dtb, conf.dtb_addr, MVII_MT6592_LK_SD_DTB_WINDOW, &dtb_len) != 0)
        return -1;

    /* The initramfs is the only optional payload: a card with a real root
     * filesystem on it does not need one, and one that is named but missing is
     * worth a line rather than a dead boot. */
    initrd_data = conf.initrd_addr;
    if (want_initrd) {
        if (sd_read_into("initramfs", conf.initrd, conf.initrd_addr, MVII_MT6592_LK_SD_INITRD_MAX,
                         &initrd_len) != 0) {
            lk_log("sd: continuing without an initramfs\n");
            initrd_len = 0u;
        } else if (sd_uimage_header(conf.initrd_addr, initrd_len, &usize, &utype, &uload) &&
                   utype == SD_UIMAGE_TYPE_RAMDISK) {
            /* No move here, unlike the kernel: nothing has to start at a
             * particular address, so the tree just points past the header. */
            initrd_data = conf.initrd_addr + SD_UIMAGE_HDR;
            initrd_len = usize;
            lk_log_hex("sd: initramfs is a uImage; payload at ", initrd_data);
            lk_log_hex("sd: initramfs payload bytes=", initrd_len);
        }
    }

    if (sd_patch_fdt(conf.dtb_addr, dtb_len, &conf, initrd_data, initrd_len) != 0) return -1;

    lk_log_hex("sd: kernel entry=", conf.kernel_addr);
    lk_log_hex("sd: device tree=", conf.dtb_addr);

    /* Same re-arm the eMMC path does before the kernel's first load step. */
    mt6592_pmic_power_hold();
    mt6592_pmic_charger_service();

    lk_mark(MT6592_BOOT_STATUS_STAGE_LK_HANDOFF, "lk: jumping to the SD card kernel\n", LK_BEACON_HANDOFF);

    /*
     * Linux, not Android: r0 = 0, r1 = machine type, r2 = the device tree.
     * mvii_lk_jump_to_kernel() zeroes r0 and passes the other two through
     * untouched, which is the same convention the ATAG path uses with a
     * different thing in r2 — the kernel's __vet_atags tells them apart by the
     * magic word it finds there, and ignores r1 entirely once it has a tree.
     */
    mvii_lk_jump_to_kernel(conf.kernel_addr, (uint32_t)MVII_MT6592_LK_MACHTYPE, conf.dtb_addr);
    return 0;
}
#endif /* MVII_MT6592_LK_SD_HANDOFF */

/* ── Entry ── */

void mvii_lk_main(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    lk_boot_image_t img;
    int storage_rc;
    int display_rc;
    int boot_rc;

    (void)r0;
    (void)r1;
    (void)r2;
    (void)r3;

    mtk_watchdog_disable();
    mt6592_uart_init();
    mt6592_bootstatus_init();
    mt6592_bootstatus_console_set_autoflush(0);
    mt6592_bootstatus_set_image_base(0x81e00000u);
    lk_mark(MT6592_BOOT_STATUS_STAGE_LK_ENTRY, "\nlk: MVII minimal LK (UBOOT slot, 0x81e00000)\n", 0u);

    /*
     * Put out the indicator LEDs first, before anything that can take time or
     * fail. They come out of reset all three lit -- pads 47/48/49 at DOUT 0, and
     * they are active low -- which reads as a fault light on a console that is
     * booting perfectly well, and used to stay that way right through to the OS.
     * This costs three GPIO stores and needs no PMIC, so it goes above the pwrap
     * bring-up rather than waiting on it. The charge park and the OS set an
     * honest colour once there is a battery reading to be honest about.
     */
    (void)mt6592_led_init();

    /*
     * Power before pixels. The panel bring-up below is the largest load step
     * this image makes, and on a board with no cell fitted there is nothing
     * between that step and the rail but the charger block — which the
     * preloader leaves with its watchdog armed and its cutoffs at the stock
     * settings. mt6592_pmic_charger_service() disarms that watchdog, widens the
     * UVLO, drops the VBUS high-voltage comparator, and enables the input path,
     * and it now does the first three before it looks at CHRDET so the DC port
     * gets them whether or not it drives the detect comparator.
     */
    if (mt6592_pwrap_init() == MT6592_PWRAP_OK || mt6592_pwrap_is_ready()) {
        mt6592_pmic_charger_service();
        lk_mark(MT6592_BOOT_STATUS_STAGE_LK_POWER_ARMED, "lk: PMIC input path armed\n", 0u);
    } else {
        lk_mark(MT6592_BOOT_STATUS_STAGE_LK_POWER_ARMED, "lk: PWRAP down; PMIC left as the preloader set it\n",
                0u);
    }

    /*
     * Storage before pixels, even though pixels are what the user sees first.
     * eMMC is what carries the boot log off the board, and the display path is
     * both the longest and the least proven thing this image does — running it
     * blind would mean a panel that stops halfway can say nothing at all. Once
     * this succeeds, every line below is on the eMMC status record within
     * milliseconds of being emitted and `flash -mtk-read-boot-status` can read
     * the whole boot back.
     */
    storage_rc = mt6592_emmc_user_init();
    if (storage_rc == MT6592_MSDC_OK) {
        g_storage_up = 1u;
        mt6592_bootstatus_console_set_autoflush(1);
        lk_mark(MT6592_BOOT_STATUS_STAGE_LK_STORAGE_READY, "lk: eMMC ready\n", 0u);

        /*
         * The asset slot, read here and only here: the LOGO partition is on the
         * device that just came up, this is the first moment it can be read, and
         * both consumers want it before they draw. The park below is one of them;
         * stage1 is the other, and it adopts the staged copy out of DRAM rather
         * than reading eMMC a second time (see mvii_assets_adopt()).
         *
         * A failure is not a boot failure and never can be. Every drawing path
         * that uses the slot keeps the rectangles-and-3x5-font version it had
         * before as its fallback, so a board whose LOGO partition has never been
         * written -- which is every board until the first `-assets` flash --
         * boots and parks exactly as it did.
         */
        {
            const int assets_rc = mvii_assets_load();
            if (assets_rc == 0) {
                lk_log("lk: asset slot loaded\n");
            } else {
                lk_log_hex("lk: no asset slot (rc=", (uint32_t)assets_rc);
                lk_log("); using the built-in drawing\n");
            }
        }
    } else {
        lk_log_hex("lk: eMMC init failed err=", (uint32_t)storage_rc);
    }

#ifdef MVII_MT6592_LK_RELEASE
    /*
     * The release bootloader has no debug mode, and this is where it would have
     * been. Not "declines to enter one": mvii_debug_console_run() is called from
     * exactly one place -- the block below -- and with that call gone the linker
     * (--gc-sections, over -ffunction-sections objects) drops the console, the
     * MUSB peripheral gadget that carries it, and the Wi-Fi stack it is the only
     * caller of. There is nothing in this image for a held button or a flag
     * sector to reach, so neither is read.
     *
     * A boot log that simply skipped from the asset slot to the display would
     * leave an operator wondering whether the console failed to open or was
     * never there. One line settles it.
     */
    lk_log("lk: release build; no debug console in this image\n");
#else
    /*
     * Debug mode: held buttons first, eMMC flag second.
     *
     * This is the earliest point either can run, and it is deliberately *before*
     * the display, so the panel bring-up is something the operator triggers and
     * watches from the console rather than something that has already happened by
     * the time the console exists.
     *
     * POWER + VOL_UP held at this instant is the primary trigger and the flag is
     * the fallback, which is the opposite of how this started. The flag has to
     * survive a BROM write, a watchdog reset, the stock preloader, and a second
     * eMMC init before the LK can read it back -- four things we do not control,
     * any one of which silently yields a normal boot. The buttons are read from
     * two registers on the same die, in this function, microseconds before the
     * decision. When a trigger's whole job is to be believed, fewer moving parts
     * beats more.
     *
     * Both are logged either way, unconditionally. A boot that does *not* enter
     * the console must still say why, or the next iteration is another guess:
     * `flash -mtk-read-boot-status` then shows whether the keys were seen, what
     * the flag sector actually held, and which of the two paths declined.
     */
    {
        const uint32_t combo = lk_debug_combo_held();
        uint32_t flag = MT6592_DBG_MODE_NORMAL;

        if (g_storage_up != 0u) flag = mt6592_dbgflag_take();

        lk_log_hex("lk: dbg combo=", combo);
        lk_log_hex(" flag=", flag);
        lk_log("\n");

        if (combo != 0u || flag == MT6592_DBG_MODE_CONSOLE) {
            lk_log(combo != 0u ? "lk: a button is held; entering the live console\n"
                               : "lk: debug flag found; entering the live console\n");
            mvii_debug_console_run(&kConsoleHooks);
            lk_log("lk: console done; resuming the normal boot\n");
        }
    }
#endif /* MVII_MT6592_LK_RELEASE */

    /*
     * Bracket the display in PMIC service calls. Between these two there is a
     * few hundred milliseconds of panel reset timing and DCS programming during
     * which nothing touches the charger, and on a battery-less J36 an
     * unserviced stretch is the thing that ends boots — the input path wants
     * re-opening before a long quiet period and again the moment it is over.
     */
    mt6592_pmic_power_hold();
    display_rc = lk_display_on();
    mt6592_pmic_power_hold();
    if (display_rc == 0) {
        mt6592_bootstatus_set_framebuffer(MVII_MT6592_LK_FB_ADDR, MVII_MT6592_LK_FB_WIDTH,
                                          MVII_MT6592_LK_FB_HEIGHT, MVII_MT6592_LK_FB_PITCH,
                                          MVII_MT6592_LK_FB_BPP, 1u);
        lk_mark(MT6592_BOOT_STATUS_STAGE_LK_SCANOUT, "lk: scanout live\n", LK_BEACON_SCANOUT);
    } else {
        /* Not fatal, and deliberately so: a dark panel is a bad boot, a boot
         * that stops here is a brick. */
        mt6592_bootstatus_set_error((uint32_t)display_rc);
        lk_mark(MT6592_BOOT_STATUS_STAGE_LK_DISPLAY_FAILED, "lk: display bring-up failed; booting headless\n",
                0u);
    }

    /*
     * Charge park, before the boot.img load rather than after it.
     *
     * Either order boots the same; this one puts the charge screen on the panel
     * a couple of seconds sooner and leaves the eMMC idle for the whole park,
     * which on a cell-less board is the difference between a park that sips
     * from the charger and one that keeps a storage controller awake for
     * nothing. The cost is that the key press is followed by the same load a
     * normal boot pays for anyway.
     */
    /*
     * The breadcrumb read, before the park -- for the LOG, not for the park.
     *
     * The park used to branch on presence and this call was what gave it an answer.
     * It does not any more: presence is undecidable on this board and the driver has
     * no property for it, so the park assumes a cell is fitted (see lk_charge_park()
     * and mt6592_pmic.h). What survives here is the breadcrumb, which is where an
     * operator's `bat probe' result lands; reading it on the way up is what clears
     * the dead-man's switch and puts the verdict in the boot log. It writes no PMIC
     * register and never probes, so it cannot cost a boot.
     */
    (void)mvii_battery_boot_policy();

    /* CHRDET set means "plugged in", so a cable-powered board with no cell
     * would sit on the charge screen until the power key. The PMIC service
     * above disables the charger watchdog and widens UVLO. */
    if (MVII_WITHOUT_BATTERY) {
        lk_log("lk: batteryless; charge park skipped\n");
        mt6592_pmic_power_hold();
    } else {
        lk_charge_park();
    }

#ifdef MVII_MT6592_LK_SD_HANDOFF
    /*
     * The card gets first refusal, and only here — after the console, after the
     * display, after the charge park. Those three are what makes a board
     * recoverable, and a hand-off that ran before them would mean a bad card
     * could take the debug console away with it.
     *
     * This does not return when it works.
     *
     * Serviced first, and this is a release-build-only step so it is the one load
     * the debug image never pays: lk_sd_boot() opens with mt6592_sd_probe(),
     * which powers MSDC1 up, and it lands directly on top of the park's exit --
     * on a board that just took the backlight to full, with no cable, on a rail
     * that is the cell itself. Same reasoning as the bracket around
     * lk_display_on() below; see lk_park_handoff().
     */
    mt6592_pmic_power_hold();
    (void)lk_sd_boot();
    lk_log("lk: SD hand-off declined; falling back to the eMMC boot image\n");
#endif

    boot_rc = storage_rc == MT6592_MSDC_OK ? lk_load_boot_image(&img) : storage_rc;
    if (boot_rc != MT6592_MSDC_OK) {
        mt6592_bootstatus_set_error((uint32_t)boot_rc);
        lk_mark(MT6592_BOOT_STATUS_STAGE_LK_BOOTIMG_FAILED, "lk: no bootable image; halting\n",
                LK_BEACON_FAILED);
        for (;;) {
            __asm__ volatile("wfi");
        }
    }

    lk_build_atags(&img);
    lk_log_hex("lk: kernel entry=", img.kernel_entry);
    lk_log_hex("lk: atags=", img.tags_addr);
    lk_log("lk: cmdline ");
    lk_log(img.cmdline[0] != 0 ? img.cmdline : MVII_MT6592_LK_STOCK_CMDLINE);
    lk_log("\n");

    /* Last thing before the load steps the kernel is about to make: force the
     * charger service past its rate limiter and re-arm. */
    mt6592_pmic_power_hold();
    mt6592_pmic_charger_service();

    /*
     * The hand-off milestone goes after the last log line, not before it. With
     * the console mirror autoflushing, a completed line rewrites the stage to
     * its RUNTIME heartbeat and appends to the message field, so a stage set
     * any earlier is gone by the time storage sees it. Nothing here replaces
     * the message field wholesale either: a board that stops downstream still
     * has this LK's entire boot log in the record, ending with the hand-off.
     */
    lk_mark(MT6592_BOOT_STATUS_STAGE_LK_HANDOFF, "lk: jumping to the boot.img kernel\n", LK_BEACON_HANDOFF);
    mvii_lk_jump_to_kernel(img.kernel_entry, (uint32_t)MVII_MT6592_LK_MACHTYPE, img.tags_addr);
}
