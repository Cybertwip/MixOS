/* Host test for the mt68xx LK UI core (lk_bootmenu.h + lk_menu_ui.h).
 *
 * Derived from tools/mediatek/firmware/tests/test-bootmenu.c. The headers
 * are pure logic -- no registers, no DRAM -- so the target and the
 * workstation compile the same code. Two deliberate deltas against the j36
 * test: the payload sniff asserts the phone mark ("mixos-" image name)
 * instead of the j36 marks, and the save-box geometry is pinned at the
 * phone panels (480x960, 720x1612) as well as the 640x480 reference.
 *
 * The prompt strings below are the j36's, kept as geometry fuel: the menu
 * does not run yet (LK-BRINGUP step 4 picks the phone copy), and the test's
 * job is that the save box covers WHATEVER copy lands. If a j36 UI change
 * breaks the shared-math asserts here, re-derive lk_menu_ui.h (diff it --
 * the copy must stay byte-identical) instead of weakening the asserts.
 *
 * Run with:
 *
 *     cc -std=c99 -Wall -Wextra -Werror tools/mediatek/mt68xx/firmware/tests/test-lk-ui.c \
 *         -o /tmp/mt68xx-lk-ui-test && /tmp/mt68xx-lk-ui-test
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../Drivers/lk_bootmenu.h"
#include "../Drivers/lk_menu_ui.h"

static const char kTestPrompt[] = "PRESS MENU FOR ANDROID";
static const char kTestErrSd[] = "MIXOS SD BOOT FAILED";
static const char kTestErrSlot[] = "BOOTIMG HOLDS MIXOS IMAGE";
static const char kTestErrNone[] = "NO ANDROID IMAGE";

static void check_save_covers(uint32_t w, uint32_t h) {
    lk_menu_box_t save = lk_menu_save_box(w, h);
    lk_menu_box_t spin = lk_menu_spinner_box(w, h);
    lk_menu_box_t stage = lk_menu_text_bounds(w / 2u, lk_menu_stage_y(h), kTestPrompt, 2u);
    lk_menu_box_t err_sd = lk_menu_text_bounds(w / 2u, lk_menu_stage_y(h), kTestErrSd, 2u);
    lk_menu_box_t err_slot =
        lk_menu_text_bounds(w / 2u, lk_menu_stage_y(h), kTestErrSlot, 2u);
    lk_menu_box_t err_none =
        lk_menu_text_bounds(w / 2u, lk_menu_stage_y(h), kTestErrNone, 2u);
    assert(lk_menu_box_covers(save, spin));
    assert(lk_menu_box_covers(save, stage));
    assert(lk_menu_box_covers(save, err_sd));
    assert(lk_menu_box_covers(save, err_slot));
    assert(lk_menu_box_covers(save, err_none));
    assert(lk_menu_text_width(kTestPrompt, 2u) < w);
}

int main(void) {
    /* The window the operator was promised. */
    assert(LK_BOOTMENU_WINDOW_MS == 5000u);

    /* Debounce: a release arms the key, then three consecutive pressed
     * samples select. A key held at entry and lone bounces never do. */
    {
        lk_bootmenu_key_t k = { 0u, 0u };
        assert(lk_bootmenu_sample(&k, 1u) == 0u);
        assert(lk_bootmenu_sample(&k, 1u) == 0u);
        assert(lk_bootmenu_sample(&k, 0u) == 0u);
        assert(lk_bootmenu_sample(&k, 1u) == 0u);
        assert(lk_bootmenu_sample(&k, 1u) == 0u);
        assert(lk_bootmenu_sample(&k, 1u) == 1u);
        assert(lk_bootmenu_sample(&k, 1u) == 1u);
        assert(lk_bootmenu_sample(&k, 0u) == 0u);
        assert(lk_bootmenu_sample(&k, 1u) == 0u);
    }
    {
        lk_bootmenu_key_t k = { 1u, 0u };
        assert(lk_bootmenu_sample(&k, 1u) == 0u);
        assert(lk_bootmenu_sample(&k, 0u) == 0u);
        assert(lk_bootmenu_sample(&k, 1u) == 0u);
        assert(lk_bootmenu_sample(&k, 0u) == 0u);
    }

    /* A press anywhere in the window tags Android; silence tags MixOS. */
    assert(lk_bootmenu_pick(1u) == LK_BOOTMENU_ANDROID);
    assert(lk_bootmenu_pick(0u) == LK_BOOTMENU_MIXOS);

    /* Payload sniff: stock passes, the "mixos-" name mark trips. */
    {
        uint8_t hdr[1024];
        uint32_t i;

        for (i = 0u; i < 1024u; ++i) hdr[i] = 0u;
        memcpy(hdr + 48u, "1778287588", 10u);
        assert(lk_bootmenu_is_mixos_payload(hdr) == 0u);
        for (i = 0u; i < 1024u; ++i) hdr[i] = 0u;
        memcpy(hdr + 48u, "mixos-cph2381", 13u);
        assert(lk_bootmenu_is_mixos_payload(hdr) == 1u);
        for (i = 0u; i < 1024u; ++i) hdr[i] = 0u;
        assert(lk_bootmenu_is_mixos_payload(hdr) == 0u);
        /* The dash is part of the mark: "mixos" without it is stock. */
        for (i = 0u; i < 1024u; ++i) hdr[i] = 0u;
        memcpy(hdr + 48u, "mixosX", 6u);
        assert(lk_bootmenu_is_mixos_payload(hdr) == 0u);
    }

    /* The overlay blend: opaque ink wins outright, transparent keeps the
     * splash, and a real alpha lands strictly between the two. */
    assert(lk_blend_pixel(0xff000000u, 0xffffffffu) == 0xffffffffu);
    assert(lk_blend_pixel(0xff112233u, 0x00112233u) == 0xff112233u);
    assert(lk_blend_pixel(0xff000000u, 0x80ffffffu) == 0xff808080u);
    {
        const uint32_t navy_over_white = lk_blend_pixel(0xffffffffu, 0xd90c1420u);
        assert((navy_over_white & 0xff000000u) == 0xff000000u);
        assert(((navy_over_white >> 16u) & 0xffu) > 0x0cu &&
               ((navy_over_white >> 16u) & 0xffu) < 0xffu);
        assert(((navy_over_white >> 8u) & 0xffu) > 0x14u &&
               ((navy_over_white >> 8u) & 0xffu) < 0xffu);
        assert((navy_over_white & 0xffu) > 0x20u &&
               (navy_over_white & 0xffu) < 0xffu);
    }

    /* The font table: space empty, A and tilde spot-checked. */
    assert(LK_FONT5X7[0][0] == 0x00u && LK_FONT5X7[0][4] == 0x00u);
    assert(LK_FONT5X7['A' - 0x20][0] == 0x7Eu && LK_FONT5X7['A' - 0x20][4] == 0x7Eu);
    assert(LK_FONT5X7['~' - 0x20][2] == 0x2Au);

    /* Text metrics: 5 columns + 1 gap a cell, less the last gap. */
    assert(lk_menu_text_width("", 2u) == 0u);
    assert(lk_menu_text_width("A", 1u) == 5u);
    assert(lk_menu_text_width("AB", 1u) == 11u);
    assert(lk_menu_text_width("A", 2u) == 10u);

    /* The spinner easing: pinned values plus monotonicity and the +/-31
     * overshoot bound, straight from the j36 test. */
    assert(lk_spin_ease256(0u) == 0);
    assert(lk_spin_ease256(128u) == 128);
    assert(lk_spin_ease256(64u) == 95);
    assert(lk_spin_ease256(192u) == 161);
    {
        uint32_t p;
        int32_t prev = lk_spin_ease256(0u);
        for (p = 1u; p < 256u; ++p) {
            int32_t e0 = lk_spin_ease256(p);
            assert(e0 >= prev);
            prev = e0;
            assert(e0 - (int32_t)p >= -31 && e0 - (int32_t)p <= 31);
        }
    }

    /* Dot phases and orbit offsets. */
    assert(lk_spin_phase256(0u, 0u) == 0u);
    assert(lk_spin_phase256(2000u, 0u) == 0u);
    assert(lk_spin_phase256(100u, 1u) == 0u);
    assert(lk_spin_phase256(0u, 1u) == 243u);
    assert(lk_spin_phase256(50u, 1u) == 249u);
    {
        int32_t dx = 0, dy = 0;
        lk_spin_offset(0u, 26u, &dx, &dy);
        assert(dx == 0 && dy == -26);
        lk_spin_offset(64u, 26u, &dx, &dy);
        assert(dx == 26 && dy == 0);
    }

    /* Spinner proportions: reference size plus the phone panels. */
    assert(lk_menu_spin_radius(640u, 480u) == 26u);
    assert(lk_menu_spin_dot(640u, 480u) == 3u);
    assert(lk_menu_spin_radius(480u, 960u) == 26u);
    assert(lk_menu_spin_dot(480u, 960u) == 3u);
    assert(lk_menu_spin_radius(720u, 1612u) == 39u);
    assert(lk_menu_spin_dot(720u, 1612u) == 5u);

    /* The save box covers every paint box at all three sizes. */
    check_save_covers(640u, 480u);
    check_save_covers(480u, 960u);
    check_save_covers(720u, 1612u);

    printf("PASS: mt68xx LK UI core (bootmenu + menu geometry)\n");
    return 0;
}
