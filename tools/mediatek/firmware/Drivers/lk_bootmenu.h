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

#endif /* MVII_LK_BOOTMENU_H */
