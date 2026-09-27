/* The boot-choice countdown, as pure arithmetic.
 *
 * The LK paints "PRESS ANY BUTTON TO BOOT INTO ANDROID" over the splash and
 * gives the operator five seconds: a press anywhere in the window tags the
 * eMMC Android image, silence tags the SD MixOS hand-off. The window, the
 * whole-seconds digit and the draining bar are computed here, with no
 * registers and no DRAM, so the firmware and the host test
 * (../tests/test-bootmenu.c) compile the same logic.
 */
#ifndef MVII_LK_BOOTMENU_H
#define MVII_LK_BOOTMENU_H

#include <stdint.h>

enum {
    LK_BOOTMENU_MIXOS = 0u,
    LK_BOOTMENU_ANDROID = 1u,
    LK_BOOTMENU_WINDOW_MS = 5000u
};

/* Whole seconds left on the panel: 5..1 across the window, 0 once over. */
static inline uint32_t lk_bootmenu_remaining_s(uint32_t elapsed_ms) {
    if (elapsed_ms >= LK_BOOTMENU_WINDOW_MS) return 0u;
    return (LK_BOOTMENU_WINDOW_MS - elapsed_ms + 999u) / 1000u;
}

/* Bar fill in permille: 1000 at the first frame, 0 at the last and after. */
static inline uint32_t lk_bootmenu_bar_permille(uint32_t elapsed_ms) {
    if (elapsed_ms >= LK_BOOTMENU_WINDOW_MS) return 0u;
    return (uint32_t)(((uint64_t)(LK_BOOTMENU_WINDOW_MS - elapsed_ms) * 1000u) /
                      (uint64_t)LK_BOOTMENU_WINDOW_MS);
}

/* The tag: a press anywhere in the window means Android, silence means MixOS. */
static inline uint32_t lk_bootmenu_pick(uint32_t pressed) {
    return pressed != 0u ? LK_BOOTMENU_ANDROID : LK_BOOTMENU_MIXOS;
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
