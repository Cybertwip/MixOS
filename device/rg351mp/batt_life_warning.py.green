#!/usr/bin/env python3
#
# Battery life indicator.
#
# WHY THIS IS NOT THE THREE-BRANCH LOOP IT USED TO BE.  The original opened two
# files by absolute path on every pass and let a missing one raise:
#
#     batt_life = "/sys/class/power_supply/battery/capacity"
#     pwr_led   = "/sys/class/gpio/gpio77/value"
#
# Both of those are RK3326 facts.  On the J36 Ultra the capacity file exists --
# the MT6592 PMIC driver registers its supply under exactly that name -- but
# there is no sysfs GPIO export and no gpio77, and EVERY branch of the old loop
# read the LED, including the "battery is fine" one.  So the first pass raised
# FileNotFoundError and exited 1, and batt_led.service is Restart=always with
# RestartSec=2 and StartLimitIntervalSec=0 -- explicitly unbounded -- which put
# an eight-line Python traceback on the console every 2.3 seconds for as long as
# the machine was up.  On a board whose console IS the panel, that is the splash
# screen being overdrawn with a traceback forever.
#
# So: no path is assumed to exist, and nothing here is fatal.  A missing LED
# means the indicator has nothing to blink and the daemon keeps reading the
# gauge; a missing gauge means the driver has not loaded yet and it is worth
# waiting for.  Neither is a reason to exit and be restarted.
#
# The RK3326 behaviour is unchanged: gpio77 is found first, the thresholds are
# the same 10% and 20%, and the sleeps are the same.

import glob
import os
import time

SYSFS_SUPPLY = "/sys/class/power_supply"

# The RK3326 power LED, and then whatever this kernel does expose.  Ordered, not
# searched in parallel: the first entry is the one this script was written for.
LED_CANDIDATES = [
    "/sys/class/gpio/gpio77/value",
]

# THE ONE LINE THAT DIFFERS BETWEEN THE .red AND .green COPIES OF THIS FILE.
# "Change LED to Red.sh" and "Change LED to Green.sh" install one or the other
# over this script, and the two used to be a nine-hunk diff of inverted
# comparisons.  Keeping the polarity in a constant means the variants are this
# line and nothing else, so a fix here does not have to be made three times.
LED_ON = 1
LED_OFF = 1 - LED_ON


def read_int(path):
    """The integer in a sysfs file, or None if it is not there or not one."""
    try:
        with open(path, "r") as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def write_int(path, value):
    """True if it was written.  False is the answer for a node that vanished."""
    try:
        with open(path, "w") as f:
            f.write(str(value))
        return True
    except OSError:
        return False


def find_capacity():
    """
    The first power supply that says it is a battery.

    By type rather than by name, because "battery" is the RK3326 driver's name
    for it and not a guarantee -- and because a supply that reports a capacity
    but calls itself USB is a charger, whose percentage means nothing here.
    """
    for base in sorted(glob.glob(os.path.join(SYSFS_SUPPLY, "*"))):
        capacity = os.path.join(base, "capacity")
        if not os.path.exists(capacity):
            continue
        try:
            with open(os.path.join(base, "type"), "r") as f:
                if f.read().strip() != "Battery":
                    continue
        except OSError:
            continue
        return capacity
    return None


def find_led():
    """The node to blink, or None.  None is a normal answer, not an error."""
    for path in LED_CANDIDATES:
        if os.path.exists(path):
            return path
    # Any class LED will do -- on a board with no gpio export there may still be
    # one registered by a driver.  Sorted so the choice is the same every boot.
    for path in sorted(glob.glob("/sys/class/leds/*/brightness")):
        return path
    return None


def main():
    capacity_path = None
    led_path = None
    # Said once each, so a board with neither is one line in the journal rather
    # than a line every pass.
    warned_capacity = False
    warned_led = False

    while True:
        if capacity_path is None or not os.path.exists(capacity_path):
            capacity_path = find_capacity()
            if capacity_path is None:
                if not warned_capacity:
                    warned_capacity = True
                    print("batt_life_warning: no battery in %s yet; waiting"
                          % SYSFS_SUPPLY, flush=True)
                time.sleep(30)
                continue
            warned_capacity = False
            print("batt_life_warning: reading %s" % capacity_path, flush=True)

        if led_path is None or not os.path.exists(led_path):
            led_path = find_led()
            if led_path is None and not warned_led:
                warned_led = True
                print("batt_life_warning: no indicator LED on this board; "
                      "watching the gauge without one", flush=True)
            elif led_path is not None:
                warned_led = False
                print("batt_life_warning: indicator is %s" % led_path,
                      flush=True)

        capacity = read_int(capacity_path)
        if capacity is None:
            # The supply went away under us -- a module unload, or a driver that
            # is still coming up.  Look for it again next pass.
            capacity_path = None
            time.sleep(5)
            continue

        # With no LED there is nothing to do but keep the gauge in the journal,
        # which is still worth having: it is the cheapest standing check that
        # the power_supply registration survived a kernel bump.
        if led_path is None:
            time.sleep(30)
            continue

        led = read_int(led_path)
        if led is None:
            led_path = None
            continue

        if capacity <= 10:
            # Flat: blink at 1 Hz.
            write_int(led_path, LED_OFF if led == LED_ON else LED_ON)
            time.sleep(1)
        elif capacity <= 20:
            # Low: on, and left on.
            if led != LED_ON:
                write_int(led_path, LED_ON)
            time.sleep(30)
        else:
            # Fine: off, and left off.
            if led != LED_OFF:
                write_int(led_path, LED_OFF)
            time.sleep(30)


if __name__ == "__main__":
    main()
