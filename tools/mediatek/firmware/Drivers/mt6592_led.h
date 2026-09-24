#ifndef MT6592_LED_H
#define MT6592_LED_H

/*
 * mt6592_led.h — the three indicator LEDs, on plain SoC GPIOs.
 *
 * WIRING (verified on hardware, one pad at a time, with the console's `pin`):
 *
 *     GPIO 47 = red    GPIO 48 = green    GPIO 49 = blue
 *     mode 0 (GPIO), direction out, no pull, and DOUT 0 LIGHTS IT.
 *
 * The transcript that settled it:
 *     pin #48 0 1 0 1   -> green off      pin #48 0 1 0 0 -> green on
 *     pin #49 0 1 0 0   -> blue on
 *     pin #47 0 1 0 1   -> red off
 * Active low, i.e. the pad sinks the LED's current and a 1 is dark. Corroborated
 * from the stock ramdisk: ueventd.rc chowns
 * /sys/devices/platform/leds-mt65xx/leds/{green,red,blue}, so the stock kernel
 * drives these as three separate one-colour LEDs, not as an RGB part.
 *
 * WHAT THIS FILE USED TO BE, AND WHY THAT WAS WRONG
 *   It was an MT6323 ISINK driver: ungate the 32 kHz driver clock, program
 *   current step and dim duty on channels 0/1/2, enable via 0x0356. Plausible --
 *   that is how MT65xx reference boards wire an indicator -- and wrong. Two
 *   independent checks killed it:
 *     - the console's `ledscan` walked all four ISINK channels with current and
 *       duty programmed, and the LEDs never changed;
 *     - the whole stock LK decompile contains no access anywhere in
 *       0x0330..0x0356, so nothing before the kernel touches that block at all.
 *   The "every colour is lit at power-on" that started the ISINK theory is just
 *   three pads left at DOUT 0 by reset -- which, active low, is all three on.
 *   A later guess of pads 6/112/113 was wrong too: those were picked only for
 *   being the three pads that boot in mode 0 as outputs, and 112 is panel power.
 *
 * Nothing here can fail: a GPIO write is a store to a register that is always
 * there. The int returns are kept because callers (and the console's `led`)
 * already check them, and because the pin numbers can be overridden at build
 * time -- an out-of-range override is the one error this can report.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MT6592_LED_OFF = 0,
    MT6592_LED_RED = 1,
    MT6592_LED_GREEN = 2,
    MT6592_LED_BLUE = 3,
} mt6592_led_color_t;

/* Take the three pads to mode 0, output, no pull, all dark. Idempotent;
 * mt6592_led_set() calls it on first use, so callers only need it to put out the
 * boot-time all-three-lit state early. Returns 0 on success, non-zero only if a
 * build-time pin override is outside the pad space. */
int mt6592_led_init(void);

/* Drive the three pads directly. Non-zero lights a colour (the active-low
 * inversion happens here, not in the caller). This is the primitive the
 * console's `led` command and the colour policy both go through. */
int mt6592_led_set_channels(uint32_t red_on, uint32_t green_on, uint32_t blue_on);

/* One of the four named states. Returns 0 on success. */
int mt6592_led_set(mt6592_led_color_t color);

/* The battery/charge indicator policy, as a pure function so it can be reasoned
 * about (and unit-checked) without a PMIC:
 *
 *   plugged in   : green, and blue once the cell is at or past 80%
 *   on battery   : blue at full, green from 80% down, red at 20% or less
 *   no cell / no reading while plugged: green (the charger is the supply)
 *   no cell / no reading on battery   : off (nothing truthful to say)
 */
mt6592_led_color_t mt6592_led_battery_color(int charger_online, int battery_valid,
                                            int battery_percent);

/* Sample the PMIC and apply the policy above. Cheap enough to call from a park
 * loop; the underlying battery read is itself rate-limited. */
void mt6592_led_battery_indicate(void);

#ifdef __cplusplus
}
#endif

#endif /* MT6592_LED_H */
