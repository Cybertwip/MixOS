#include "mt6592_bootinfo.h"

#include "mt6592_board_j36.h"
#include "mt6592_uart.h"

static mt6592_boot_handoff_t g_handoff;
static const mt6592_boot_handoff_t* g_active_handoff;

static void zero_bytes(void* ptr, uint32_t len) {
    uint8_t* p = (uint8_t*)ptr;
    while (len--) *p++ = 0;
}

static void copy_label(char dst[12], const char src[12]) {
    for (uint32_t i = 0; i < 12u; ++i) dst[i] = src ? src[i] : 0;
}

static int handoff_pointer_is_plausible(const mt6592_boot_handoff_t* handoff) {
    uintptr_t p = (uintptr_t)handoff;
    if ((p & 3u) != 0u) return 0;
    if (p >= 0x10000000u && p < 0x12000000u) return 1;
    if (p >= 0x81e00000u && p < 0x82000000u) return 1;
    return 0;
}

static int readable_bootarg_pointer(uint32_t addr, uint32_t size) {
    if ((addr & 3u) != 0u || size < 4u || size > 4096u) return 0;
    if (addr >= 0x10000000u && addr < 0x12000000u && size <= 0x02000000u - (addr - 0x10000000u)) return 1;
    if (addr >= 0x80000000u && addr < 0xc0000000u && size <= 0x40000000u - (addr - 0x80000000u)) return 1;
    return 0;
}

static int framebuffer_addr_is_plausible(uint32_t addr) {
    if ((addr & 0xfffu) != 0u) return 0;
    if (addr >= 0x10000000u && addr < 0x12000000u) return 1;
    if (addr >= 0x80000000u && addr < 0xc0000000u) return 1;
    return 0;
}

static uint32_t word_at(const volatile uint32_t* words, uint32_t count, uint32_t index) {
    if (index >= count) return 0;
    return words[index];
}

static int word_in_window(const volatile uint32_t* words, uint32_t count, uint32_t center, uint32_t radius,
                          uint32_t value) {
    uint32_t begin = (center > radius) ? (center - radius) : 0u;
    uint32_t end = center + radius + 1u;
    if (end < center || end > count) end = count;
    for (uint32_t i = begin; i < end; ++i) {
        if (word_at(words, count, i) == value) return 1;
    }
    return 0;
}

static uint32_t pitch_in_window(const volatile uint32_t* words, uint32_t count, uint32_t center, uint32_t radius,
                                uint32_t width) {
    uint32_t begin = (center > radius) ? (center - radius) : 0u;
    uint32_t end = center + radius + 1u;
    uint32_t min_pitch = width * 2u;
    uint32_t max_pitch = width * 8u;
    if (end < center || end > count) end = count;
    for (uint32_t i = begin; i < end; ++i) {
        uint32_t value = word_at(words, count, i);
        if (value >= min_pitch && value <= max_pitch && (value & 3u) == 0u) return value;
    }
    return 0;
}

static uint32_t bpp_in_window(const volatile uint32_t* words, uint32_t count, uint32_t center, uint32_t radius) {
    uint32_t begin = (center > radius) ? (center - radius) : 0u;
    uint32_t end = center + radius + 1u;
    if (end < center || end > count) end = count;
    for (uint32_t i = begin; i < end; ++i) {
        uint32_t value = word_at(words, count, i);
        if (value == 16u || value == 24u || value == 32u) return value;
    }
    return 0;
}

const mt6592_boot_handoff_t* mt6592_bootinfo_init(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    zero_bytes(&g_handoff, sizeof(g_handoff));
    g_handoff.magic = MT6592_BOOT_HANDOFF_MAGIC;
    g_handoff.version = MT6592_BOOT_HANDOFF_VERSION;
    g_handoff.entry_r0 = r0;
    g_handoff.entry_r1 = r1;
    g_handoff.entry_r2 = r2;
    g_handoff.entry_r3 = r3;
    return &g_handoff;
}

int mt6592_bootinfo_is_valid(const mt6592_boot_handoff_t* handoff) {
    return handoff_pointer_is_plausible(handoff) && handoff->magic == MT6592_BOOT_HANDOFF_MAGIC &&
           handoff->version >= 1u && handoff->version <= MT6592_BOOT_HANDOFF_VERSION;
}

void mt6592_bootinfo_set_framebuffer(uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp) {
    g_handoff.fb_addr = addr;
    g_handoff.fb_width = width;
    g_handoff.fb_height = height;
    g_handoff.fb_pitch = pitch;
    g_handoff.fb_bpp = bpp;
    if (addr != 0u && width != 0u && height != 0u && pitch != 0u && bpp != 0u) {
        g_handoff.flags |= MT6592_BOOT_HANDOFF_FLAG_FRAMEBUFFER;
    } else {
        g_handoff.flags &= ~MT6592_BOOT_HANDOFF_FLAG_FRAMEBUFFER;
    }
}

int mt6592_bootinfo_probe_bootargs_for_framebuffer(uint32_t bootarg_addr, uint32_t bootarg_size) {
    if (!readable_bootarg_pointer(bootarg_addr, bootarg_size)) return 0;

    volatile const uint32_t* words = (volatile const uint32_t*)(uintptr_t)bootarg_addr;
    uint32_t count = bootarg_size / 4u;
    if (count > 256u) count = 256u;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t addr = word_at(words, count, i);
        if (!framebuffer_addr_is_plausible(addr)) continue;
        if (!word_in_window(words, count, i, 10u, MT6592_J36_PANEL_WIDTH) ||
            !word_in_window(words, count, i, 10u, MT6592_J36_PANEL_HEIGHT)) {
            continue;
        }

        uint32_t bpp = bpp_in_window(words, count, i, 10u);
        uint32_t pitch = pitch_in_window(words, count, i, 10u, MT6592_J36_PANEL_WIDTH);
        if (bpp == 0u && pitch != 0u) {
            bpp = (pitch >= MT6592_J36_PANEL_WIDTH * 4u) ? 32u : 16u;
        }
        if (pitch == 0u && bpp != 0u) {
            pitch = MT6592_J36_PANEL_WIDTH * (bpp / 8u);
        }
        if (pitch == 0u || bpp == 0u) continue;

        mt6592_bootinfo_set_framebuffer(addr, MT6592_J36_PANEL_WIDTH, MT6592_J36_PANEL_HEIGHT, pitch, bpp);
        return 1;
    }
    return 0;
}

int mt6592_bootinfo_framebuffer(const mt6592_boot_handoff_t* handoff, uint32_t* addr, uint32_t* width,
                                uint32_t* height, uint32_t* pitch, uint32_t* bpp) {
    if (!mt6592_bootinfo_is_valid(handoff) || handoff->version < 2u ||
        (handoff->flags & MT6592_BOOT_HANDOFF_FLAG_FRAMEBUFFER) == 0u) {
        return 0;
    }
    if (addr) *addr = handoff->fb_addr;
    if (width) *width = handoff->fb_width;
    if (height) *height = handoff->fb_height;
    if (pitch) *pitch = handoff->fb_pitch;
    if (bpp) *bpp = handoff->fb_bpp;
    return 1;
}

void mt6592_bootinfo_set_boot_volume(uint32_t serial, const char label[12]) {
    g_handoff.boot_volume_serial = serial;
    copy_label(g_handoff.boot_volume_label, label);
    if (serial != 0u || g_handoff.boot_volume_label[0] != 0) {
        g_handoff.flags |= MT6592_BOOT_HANDOFF_FLAG_BOOT_VOLUME;
    } else {
        g_handoff.flags &= ~MT6592_BOOT_HANDOFF_FLAG_BOOT_VOLUME;
    }
}

int mt6592_bootinfo_boot_volume(const mt6592_boot_handoff_t* handoff, uint32_t* serial, char label[12]) {
    if (!mt6592_bootinfo_is_valid(handoff) || handoff->version < 2u ||
        (handoff->flags & MT6592_BOOT_HANDOFF_FLAG_BOOT_VOLUME) == 0u) {
        return 0;
    }
    if (serial) *serial = handoff->boot_volume_serial;
    if (label) copy_label(label, handoff->boot_volume_label);
    return 1;
}

void mt6592_bootinfo_set_joystick_centers(const uint32_t centers[MT6592_BOOT_JOY_CHANNEL_COUNT]) {
    uint32_t any = 0u;
    for (uint32_t i = 0; i < MT6592_BOOT_JOY_CHANNEL_COUNT; ++i) {
        g_handoff.joy_center[i] = centers ? centers[i] : 0u;
        if (g_handoff.joy_center[i] != 0u) any = 1u;
    }
    if (any) {
        g_handoff.flags |= MT6592_BOOT_HANDOFF_FLAG_JOYSTICK_CENTERS;
    } else {
        g_handoff.flags &= ~MT6592_BOOT_HANDOFF_FLAG_JOYSTICK_CENTERS;
    }
}

int mt6592_bootinfo_joystick_centers(const mt6592_boot_handoff_t* handoff,
                                     uint32_t centers[MT6592_BOOT_JOY_CHANNEL_COUNT]) {
    if (!mt6592_bootinfo_is_valid(handoff) || handoff->version < 3u ||
        (handoff->flags & MT6592_BOOT_HANDOFF_FLAG_JOYSTICK_CENTERS) == 0u) {
        return 0;
    }
    if (centers) {
        for (uint32_t i = 0; i < MT6592_BOOT_JOY_CHANNEL_COUNT; ++i) centers[i] = handoff->joy_center[i];
    }
    return 1;
}

void mt6592_bootinfo_set_battery(uint32_t mv, uint32_t pct, uint32_t state, uint32_t raw) {
    g_handoff.battery_mv = mv;
    g_handoff.battery_pct = pct;
    g_handoff.battery_state = state;
    g_handoff.battery_raw = raw;
    /* The flag means "stage1 ran the measurement", not "there is a battery" --
     * an ABSENT verdict is a result, and a consumer that cannot tell ABSENT
     * from "never measured" would print 0% on a battery-less board. */
    g_handoff.flags |= MT6592_BOOT_HANDOFF_FLAG_BATTERY;
}

int mt6592_bootinfo_battery(const mt6592_boot_handoff_t* handoff, uint32_t* mv, uint32_t* pct,
                            uint32_t* state, uint32_t* raw) {
    if (!mt6592_bootinfo_is_valid(handoff) || handoff->version < 4u ||
        (handoff->flags & MT6592_BOOT_HANDOFF_FLAG_BATTERY) == 0u) {
        return 0;
    }
    if (mv) *mv = handoff->battery_mv;
    if (pct) *pct = handoff->battery_pct;
    if (state) *state = handoff->battery_state;
    if (raw) *raw = handoff->battery_raw;
    return 1;
}

void mt6592_bootinfo_adopt(const mt6592_boot_handoff_t* handoff) {
    g_active_handoff = mt6592_bootinfo_is_valid(handoff) ? handoff : 0;
}

const mt6592_boot_handoff_t* mt6592_bootinfo_active(void) {
    return g_active_handoff;
}

void mt6592_bootinfo_uart_dump(const mt6592_boot_handoff_t* handoff) {
    if (!mt6592_bootinfo_is_valid(handoff)) {
        mt6592_uart_puts("  preloader handoff: not provided\n");
        return;
    }
    mt6592_uart_puts("  preloader/LK entry regs: r0=");
    mt6592_uart_put_hex32(handoff->entry_r0);
    mt6592_uart_puts(" r1=");
    mt6592_uart_put_hex32(handoff->entry_r1);
    mt6592_uart_puts(" r2=");
    mt6592_uart_put_hex32(handoff->entry_r2);
    mt6592_uart_puts(" r3=");
    mt6592_uart_put_hex32(handoff->entry_r3);
    mt6592_uart_puts("\n");
    if (handoff->version >= 2u && (handoff->flags & MT6592_BOOT_HANDOFF_FLAG_FRAMEBUFFER) != 0u) {
        mt6592_uart_puts("  handoff framebuffer: addr=");
        mt6592_uart_put_hex32(handoff->fb_addr);
        mt6592_uart_puts(" ");
        mt6592_uart_put_dec(handoff->fb_width);
        mt6592_uart_puts("x");
        mt6592_uart_put_dec(handoff->fb_height);
        mt6592_uart_puts(" pitch=");
        mt6592_uart_put_dec(handoff->fb_pitch);
        mt6592_uart_puts(" bpp=");
        mt6592_uart_put_dec(handoff->fb_bpp);
        mt6592_uart_puts("\n");
    }
    if (handoff->version >= 3u && (handoff->flags & MT6592_BOOT_HANDOFF_FLAG_JOYSTICK_CENTERS) != 0u) {
        mt6592_uart_puts("  handoff joystick centers:");
        for (uint32_t i = 0; i < MT6592_BOOT_JOY_CHANNEL_COUNT; ++i) {
            mt6592_uart_puts(" c");
            mt6592_uart_put_dec(MT6592_BOOT_JOY_FIRST_CHANNEL + i);
            mt6592_uart_puts("=");
            mt6592_uart_put_dec(handoff->joy_center[i]);
        }
        mt6592_uart_puts("\n");
    }
    if (handoff->version >= 4u && (handoff->flags & MT6592_BOOT_HANDOFF_FLAG_BATTERY) != 0u) {
        mt6592_uart_puts("  handoff battery: ");
        switch (handoff->battery_state) {
        case MT6592_BOOT_BATTERY_PRESENT: mt6592_uart_puts("present"); break;
        case MT6592_BOOT_BATTERY_ABSENT:  mt6592_uart_puts("absent"); break;
        default:                          mt6592_uart_puts("unknown"); break;
        }
        mt6592_uart_puts(" ");
        mt6592_uart_put_dec(handoff->battery_pct);
        mt6592_uart_puts("% ");
        mt6592_uart_put_dec(handoff->battery_mv);
        mt6592_uart_puts("mV chrstatus=");
        mt6592_uart_put_hex32(handoff->battery_raw);
        mt6592_uart_puts("\n");
    }
}
