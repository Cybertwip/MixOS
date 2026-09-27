/*
 * mt6592_led.c — the three indicator LEDs, driven as GPIOs 47/48/49.
 *
 * See mt6592_led.h for the wiring, how it was found, and what this file used to
 * be (an MT6323 ISINK driver, disproved on hardware).
 *
 * GPIO register map, base 0x10005000. Every register is a 16-pin bank at a 0x10
 * stride, and +4 / +8 off a bank are its SET and RST aliases -- writing a 1 to a
 * bit there sets or clears that bit and leaves the other fifteen alone, so no
 * read-modify-write and no chance of stepping on a pad this file does not own:
 *
 *   0x000  DIR      1 = output
 *   0x100  PULLEN   1 = pull resistor connected
 *   0x400  DOUT     output level
 *   0x600  MODE     five 3-bit fields per bank -- NOT sixteen, and no SET/RST
 *
 * MODE is the odd one out and is the only read-modify-write here. Its indexing
 * is base + 0x600 + (pin / 5) * 0x10, field (pin % 5) * 3, which is verbatim
 * what the preloader's mt_set_gpio_mode (0x4d90 in
 * Reference/J36-ULTRA/preloader_sf6592_wet_l.bin) and stock LK's own copy
 * (FUN_81e12e7c) both compute. Both also reject any pin above 0xa8, which is
 * where MT6592_J36_GPIO_MAX comes from.
 *
 * Order matters. DOUT is written before DIR and DIR before MODE, so the pad is
 * already holding the level this file intends by the time it becomes an output,
 * and is not a GPIO at all until both are settled. Getting that backwards
 * flashes the LED on every call.
 */

#include "mt6592_led.h"

#include "mt6592_board_j36.h"
#include "mt6592_pmic.h"

/* ── Board wiring (override from CMake only if a `pin` probe says otherwise) ── */

#ifndef MVII_MT6592_LED_GPIO_RED
#define MVII_MT6592_LED_GPIO_RED MT6592_J36_LED_RED_GPIO
#endif
#ifndef MVII_MT6592_LED_GPIO_GREEN
#define MVII_MT6592_LED_GPIO_GREEN MT6592_J36_LED_GREEN_GPIO
#endif
#ifndef MVII_MT6592_LED_GPIO_BLUE
#define MVII_MT6592_LED_GPIO_BLUE MT6592_J36_LED_BLUE_GPIO
#endif

/* ── MT6592 GPIO controller ── */

#define GPIO_BASE 0x10005000u
#define GPIO_DIR 0x0000u
#define GPIO_PULLEN 0x0100u
#define GPIO_DOUT 0x0400u
#define GPIO_MODE 0x0600u
#define GPIO_BANK_STRIDE 0x0010u
#define GPIO_BANK_SET 0x0004u
#define GPIO_BANK_RST 0x0008u
#define GPIO_PINS_PER_BANK 16u
#define GPIO_PINS_PER_MODE_REG 5u
#define GPIO_MODE_FIELD_BITS 3u
#define GPIO_MODE_GPIO 0u

/* ── Battery colour thresholds (mt6592_led.h documents the policy) ── */

#define LED_PCT_LOW 20u  /* at or below this, on battery: red */
#define LED_PCT_HIGH 80u /* at or above this, plugged: blue */
#define LED_PCT_FULL 95u /* at or above this, on battery: blue */

/* mt6592_pmic.c is not linked into the resident BROM flash payload, which still
 * wants mt6592_led_set() for its live `led` command. Weak so the payload links
 * with a null here and simply has no battery-driven mode; the LK and the OS both
 * link the real one. */
extern int mt6592_pmic_battery_read(mt6592_pmic_battery_t* out) __attribute__((weak));

static const uint32_t k_led_pins[3] = {
    MVII_MT6592_LED_GPIO_RED,
    MVII_MT6592_LED_GPIO_GREEN,
    MVII_MT6592_LED_GPIO_BLUE,
};

static uint32_t g_led_ready;
static uint32_t g_led_lit; /* bit per k_led_pins[] entry, so set() can skip no-ops */

static inline void mmio_write32(uint32_t addr, uint32_t value) {
    *(volatile uint32_t*)(uintptr_t)addr = value;
}

static inline uint32_t mmio_read32(uint32_t addr) {
    return *(volatile uint32_t*)(uintptr_t)addr;
}

/* Bank-relative SET/RST write: one bit, no read, no neighbours touched. */
static void gpio_bank_bit(uint32_t reg, uint32_t pin, uint32_t on) {
    const uint32_t bank = (pin / GPIO_PINS_PER_BANK) * GPIO_BANK_STRIDE;
    const uint32_t alias = on ? GPIO_BANK_SET : GPIO_BANK_RST;
    mmio_write32(GPIO_BASE + reg + bank + alias, 1u << (pin % GPIO_PINS_PER_BANK));
}

static void gpio_mode_set(uint32_t pin, uint32_t mode) {
    const uint32_t addr =
        GPIO_BASE + GPIO_MODE + (pin / GPIO_PINS_PER_MODE_REG) * GPIO_BANK_STRIDE;
    const uint32_t shift = (pin % GPIO_PINS_PER_MODE_REG) * GPIO_MODE_FIELD_BITS;
    const uint32_t cur = mmio_read32(addr);
    mmio_write32(addr, (cur & ~(7u << shift)) | ((mode & 7u) << shift));
}

/* Non-zero `lit` means the operator should see light, which on this board means
 * driving the pad LOW. The inversion lives here and nowhere else. */
static void led_pin_drive(uint32_t pin, uint32_t lit) {
    gpio_bank_bit(GPIO_DOUT, pin, MT6592_J36_LED_ACTIVE_LOW ? !lit : !!lit);
}

int mt6592_led_init(void) {
    uint32_t i;

    if (g_led_ready) return 0;

    for (i = 0u; i < 3u; ++i) {
        const uint32_t pin = k_led_pins[i];

        /* The only failure this driver has: a build-time override past the last
         * pad the hardware decodes. Writing it would land in another bank's
         * register and disturb a pad that belongs to something else. */
        if (pin > MT6592_J36_GPIO_MAX) return -1;

        led_pin_drive(pin, 0u);              /* dark before it can drive */
        gpio_bank_bit(GPIO_PULLEN, pin, 0u); /* an output needs no pull */
        gpio_bank_bit(GPIO_DIR, pin, 1u);    /* output */
        gpio_mode_set(pin, GPIO_MODE_GPIO);  /* last: now it is really a GPIO */
    }

    /* All three dark. Whatever the LEDs showed before this ran was reset state,
     * and reset state on this board -- three pads at DOUT 0, active low -- is
     * every colour at once. */
    g_led_ready = 1u;
    g_led_lit = 0u;
    return 0;
}

int mt6592_led_set_channels(uint32_t red_on, uint32_t green_on, uint32_t blue_on) {
    uint32_t want = 0u;
    uint32_t i;

    if (mt6592_led_init() != 0) return -1;

    if (red_on) want |= 1u << 0;
    if (green_on) want |= 1u << 1;
    if (blue_on) want |= 1u << 2;

    /* Called from park loops many times a second with an answer that changes
     * once a minute. A GPIO store is cheap, but so is not making it. */
    if (want == g_led_lit) return 0;

    for (i = 0u; i < 3u; ++i) {
        led_pin_drive(k_led_pins[i], want & (1u << i));
    }
    g_led_lit = want;
    return 0;
}

int mt6592_led_set(mt6592_led_color_t color) {
    switch (color) {
        case MT6592_LED_RED: return mt6592_led_set_channels(1u, 0u, 0u);
        case MT6592_LED_GREEN: return mt6592_led_set_channels(0u, 1u, 0u);
        case MT6592_LED_BLUE: return mt6592_led_set_channels(0u, 0u, 1u);
        case MT6592_LED_OFF:
        default: return mt6592_led_set_channels(0u, 0u, 0u);
    }
}

mt6592_led_color_t mt6592_led_battery_color(int charger_online, int battery_valid,
                                            int battery_percent) {
    uint32_t pct;

    if (!battery_valid || battery_percent < 0) {
        /* No usable reading. Plugged, that is the no-cell case and the honest
         * signal is "running on the charger" — green. Unplugged it cannot
         * happen (no cell and no charger is an unpowered board), so if it
         * somehow does, say nothing rather than something wrong. */
        return charger_online ? MT6592_LED_GREEN : MT6592_LED_OFF;
    }

    pct = battery_percent > 100 ? 100u : (uint32_t)battery_percent;

    if (charger_online) return pct >= LED_PCT_HIGH ? MT6592_LED_BLUE : MT6592_LED_GREEN;
    if (pct >= LED_PCT_FULL) return MT6592_LED_BLUE;
    if (pct <= LED_PCT_LOW) return MT6592_LED_RED;
    return MT6592_LED_GREEN;
}

void mt6592_led_battery_indicate(void) {
    mt6592_pmic_battery_t bat;

    if (&mt6592_pmic_battery_read == 0) return;
    if (mt6592_pmic_battery_read(&bat) != 0) return;

    /*
     * "Is there a number" -- and nothing more, because nothing more is knowable.
     *
     * This argument has been wrong twice in opposite directions. A hard-coded 1
     * asserted a charge level the driver had not claimed, so a board with no cell
     * lit blue for "nearly full". A `level_trusted' bit fixed that and produced the
     * operator's next report, "I can only see the battery reads when the cable is
     * unplugged": that bit was 0 for the whole time the node sat at the charger's
     * setpoint, so with a cable in the LED said "no usable reading" and went flat
     * green whatever the pack was doing. Both were the same mistake -- asking about
     * the NUMBER'S PROVENANCE in order to decide whether A BATTERY EXISTS.
     *
     * That question is not answerable on this board (see the top of mt6592_pmic.h)
     * and the driver no longer pretends otherwise. What it does report is whether
     * the gauge has a percentage at all: -1 until the ADC's first median lands, and
     * on a board with an empty holder that percentage describes the rail, which is
     * the honest thing to show for a rail that is what the board runs on. The
     * plugged-in branch above already draws that case green.
     */
    (void)mt6592_led_set(mt6592_led_battery_color(bat.charger_online,
                                                  (bat.battery_percent >= 0),
                                                  bat.battery_percent));
}
