/* Boot selection: MixOS by default; a debounced MENU press selects Android. */
#ifndef MVII_LK_BOOTMENU_H
#define MVII_LK_BOOTMENU_H

#include <stdint.h>

enum {
    LK_BOOTMENU_MIXOS = 0u,
    LK_BOOTMENU_ANDROID = 1u,
    LK_BOOTMENU_WINDOW_MS = 5000u
};

/* Require release after entering the menu, then three consecutive pressed
 * samples (60 ms). A held power-on key or one noisy scan cannot select Android. */
typedef struct {
    uint32_t released;
    uint32_t down_samples;
} lk_bootmenu_key_t;

static inline uint32_t lk_bootmenu_sample(lk_bootmenu_key_t *key, uint32_t down) {
    if (!down) {
        key->released = 1u;
        key->down_samples = 0u;
        return 0u;
    }
    if (!key->released) return 0u;
    if (key->down_samples < 3u) ++key->down_samples;
    return key->down_samples >= 3u;
}

/* The tag: a press anywhere in the window means Android, silence means MixOS. */
static inline uint32_t lk_bootmenu_pick(uint32_t pressed) {
    return pressed != 0u ? LK_BOOTMENU_ANDROID : LK_BOOTMENU_MIXOS;
}

/* BOOTIMG payload sniff: stock Android or a MixOS image in Android's slot?
 *
 * The stock image carries a numeric build id in the board field (offset 48,
 * 16 bytes) and an empty cmdline (offset 64, 512 bytes) -- see the Y01
 * firmware's boot.img. Our packer writes "j36-ultra" and a cmdline full of
 * j36.* words, at the same addresses as stock, so the magic and geometry
 * checks alone cannot tell them apart -- and booting a MixOS image as
 * Android jumps a DTB-era kernel through ATAGs, which is the black screen.
 * Either mark means MixOS. Pure over the header buffer, so the host test
 * pins it; the loader calls it after the ANDROID! check. Needs no includes:
 * the loops below are bytes, not library calls. */
static inline uint32_t lk_bootmenu_is_mixos_payload(const uint8_t* hdr) {
    static const char kJ36Board[] = "j36-ultra";
    uint32_t i;

    for (i = 0u; i < 9u; ++i) {
        if (hdr[48u + i] != (uint8_t)kJ36Board[i]) break;
    }
    if (i == 9u) return 1u;
    for (i = 0u; i + 4u < 512u; ++i) {
        if (hdr[64u + i] == 0u) break;
        if (hdr[64u + i] == (uint8_t)'j' && hdr[64u + i + 1u] == (uint8_t)'3' &&
            hdr[64u + i + 2u] == (uint8_t)'6' && hdr[64u + i + 3u] == (uint8_t)'.')
            return 1u;
    }
    return 0u;
}

/* One src-over pixel blend for the overlay: the banner background carries an
 * alpha and composites over the splashed snapshot underneath, while the ink,
 * frame and bar stay opaque and short-circuit to a plain write. The composite
 * is always opaque -- the panel never sees the alpha, only the blend. */
static inline uint32_t lk_blend_pixel(uint32_t dst, uint32_t src) {
    const uint32_t a = (src >> 24u) & 0xffu;
    uint32_t r, g, b;

    if (a >= 0xffu) return src;
    if (a == 0u) return dst;
    r = (((src >> 16u) & 0xffu) * a + ((dst >> 16u) & 0xffu) * (255u - a) + 127u) / 255u;
    g = (((src >> 8u) & 0xffu) * a + ((dst >> 8u) & 0xffu) * (255u - a) + 127u) / 255u;
    b = ((src & 0xffu) * a + (dst & 0xffu) * (255u - a) + 127u) / 255u;
    return 0xff000000u | ((r & 0xffu) << 16u) | ((g & 0xffu) << 8u) | (b & 0xffu);
}

#endif /* MVII_LK_BOOTMENU_H */
