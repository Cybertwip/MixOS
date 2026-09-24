#ifndef MT6592_BOOTINFO_H
#define MT6592_BOOTINFO_H

#include <stdint.h>

enum {
    MT6592_BOOT_HANDOFF_MAGIC = 0x4d37484fu, /* M7HO */
    MT6592_BOOT_HANDOFF_VERSION = 4u,
    MT6592_BOOT_HANDOFF_FLAG_FRAMEBUFFER = 1u << 0,
    MT6592_BOOT_HANDOFF_FLAG_BOOT_VOLUME = 1u << 1,
    MT6592_BOOT_HANDOFF_FLAG_JOYSTICK_CENTERS = 1u << 2,
    MT6592_BOOT_HANDOFF_FLAG_BATTERY = 1u << 3,
};

/*
 * Battery state as stage1 measured it, so stage2 can print it on the splash.
 *
 * Stage2 cannot measure it for itself at the moment it draws: mt6592_pmic.c
 * holds the AUXADC off for the first three seconds precisely so nothing sits
 * between the engine and its first presented frame, and the splash is drawn
 * inside that window. stage1 has already taken the reading (it needs one for
 * the survival hold anyway) at the only moment on the whole boot where the
 * system load is low enough for it to mean anything.
 *
 * PRESENT/ABSENT come from the PMIC's BATON comparator, which is the one signal
 * on this board that looks at the battery connector rather than at VBAT --
 * with no power path, VBAT *is* the system node, so it reads "fitted" off the
 * charger alone. UNKNOWN is what an unreadable or un-enabled comparator gets,
 * and callers must treat it as "do not act on this".
 */
enum {
    MT6592_BOOT_BATTERY_UNKNOWN = 0u,
    MT6592_BOOT_BATTERY_ABSENT = 1u,
    MT6592_BOOT_BATTERY_PRESENT = 2u,
};

/*
 * Analog-stick rest positions captured by stage1. The OS cannot sample these
 * itself without a race it always loses: the OS keypad driver only reaches its lazy
 * seeding path once the frame pump is running, which is seconds after the user
 * has had the device in hand, so a thumb resting on a stick during boot becomes
 * the locked center and the cursor drifts forever. stage1 samples them before
 * the splash instead, which is the earliest moment the AUXADC is usable.
 *
 * Raw 12-bit AUXADC counts, indexed by (channel - MT6592_BOOT_JOY_FIRST_CHANNEL)
 * so the array covers ch12..ch15: both sticks, both axes.
 */
enum {
    MT6592_BOOT_JOY_FIRST_CHANNEL = 12u,
    MT6592_BOOT_JOY_CHANNEL_COUNT = 4u,
};

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_r0;
    uint32_t entry_r1;
    uint32_t entry_r2;
    uint32_t entry_r3;
    uint32_t flags;
    uint32_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;
    uint32_t boot_volume_serial;
    char boot_volume_label[12];
    /* version >= 3, valid only with MT6592_BOOT_HANDOFF_FLAG_JOYSTICK_CENTERS */
    uint32_t joy_center[MT6592_BOOT_JOY_CHANNEL_COUNT];
    /* version >= 4, valid only with MT6592_BOOT_HANDOFF_FLAG_BATTERY */
    uint32_t battery_mv;      /* BATSNS, under charge -- a TERMINAL voltage    */
    /* mt6592_battery_percent_from_mv(battery_mv, measured charge current): the
     * IR drop is taken back out of battery_mv before the vendor's table is read,
     * so this is a state of charge and not the charger's setpoint. battery_mv
     * itself is left as measured -- it is what the pin reads. */
    uint32_t battery_pct;
    uint32_t battery_state;   /* MT6592_BOOT_BATTERY_*                         */
    uint32_t battery_raw;     /* raw PMIC CHRSTATUS behind battery_state       */
} mt6592_boot_handoff_t;

const mt6592_boot_handoff_t* mt6592_bootinfo_init(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3);
int mt6592_bootinfo_is_valid(const mt6592_boot_handoff_t* handoff);
void mt6592_bootinfo_set_framebuffer(uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp);
int mt6592_bootinfo_probe_bootargs_for_framebuffer(uint32_t bootarg_addr, uint32_t bootarg_size);
int mt6592_bootinfo_framebuffer(const mt6592_boot_handoff_t* handoff, uint32_t* addr, uint32_t* width,
                                uint32_t* height, uint32_t* pitch, uint32_t* bpp);
void mt6592_bootinfo_set_boot_volume(uint32_t serial, const char label[12]);
int mt6592_bootinfo_boot_volume(const mt6592_boot_handoff_t* handoff, uint32_t* serial, char label[12]);
void mt6592_bootinfo_set_joystick_centers(const uint32_t centers[MT6592_BOOT_JOY_CHANNEL_COUNT]);
int mt6592_bootinfo_joystick_centers(const mt6592_boot_handoff_t* handoff,
                                     uint32_t centers[MT6592_BOOT_JOY_CHANNEL_COUNT]);
void mt6592_bootinfo_set_battery(uint32_t mv, uint32_t pct, uint32_t state, uint32_t raw);
int mt6592_bootinfo_battery(const mt6592_boot_handoff_t* handoff, uint32_t* mv, uint32_t* pct,
                            uint32_t* state, uint32_t* raw);

/*
 * Stage2 receives the handoff in r0 and threads it explicitly to the few
 * consumers that take it as a parameter. Drivers initialised later (input, in
 * particular) have no such parameter and no way back to it, so stage2 publishes
 * the pointer once and they read it from here. Returns 0 until adopted, and
 * only ever returns a record that passed mt6592_bootinfo_is_valid().
 */
void mt6592_bootinfo_adopt(const mt6592_boot_handoff_t* handoff);
const mt6592_boot_handoff_t* mt6592_bootinfo_active(void);

void mt6592_bootinfo_uart_dump(const mt6592_boot_handoff_t* handoff);

#endif
