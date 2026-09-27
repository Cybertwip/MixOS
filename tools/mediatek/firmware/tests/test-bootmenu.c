/* Host test for the LK boot-choice countdown (lk_bootmenu.h).
 *
 * The header is pure arithmetic over milliseconds -- no registers, no DRAM --
 * so the target and the workstation compile the same logic. Run with:
 *
 *     cc -std=c99 -Wall -Wextra -Werror tools/mediatek/firmware/tests/test-bootmenu.c \
 *         -o /tmp/j36-bootmenu-test && /tmp/j36-bootmenu-test
 */
#include <assert.h>
#include <stdio.h>

#include "../Drivers/lk_bootmenu.h"
#include "../Drivers/lk_menu_ui.h"

static const char kTestPrompt[] = "PRESS ANY BUTTON TO BOOT INTO ANDROID";
static const char kTestDetail[] = "Booting MixOS in 5";
static const char kTestBadge[] = "BOOTING ANDROID";

static void check_save_covers(uint32_t w, uint32_t h) {
    lk_menu_box_t save = lk_menu_save_box(w, h);
    lk_menu_box_t spin = lk_menu_spinner_box(w, h);
    lk_menu_box_t stage = lk_menu_text_bounds(w / 2u, lk_menu_stage_y(h), kTestPrompt, 2u);
    lk_menu_box_t detail = lk_menu_text_bounds(w / 2u, lk_menu_detail_y(h), kTestDetail, 1u);
    lk_menu_box_t badge = lk_menu_text_bounds(w / 2u, lk_menu_stage_y(h), kTestBadge, 3u);
    lk_menu_box_t bar = lk_menu_bar_box(w, h);
    assert(lk_menu_box_covers(save, spin));
    assert(lk_menu_box_covers(save, stage));
    assert(lk_menu_box_covers(save, detail));
    assert(lk_menu_box_covers(save, badge));
    assert(lk_menu_box_covers(save, bar));
    assert(lk_menu_text_width(kTestPrompt, 2u) < w);
}

int main(void) {
    /* The window the operator was promised. */
    assert(LK_BOOTMENU_WINDOW_MS == 5000u);

    /* Whole seconds left, counting 5..1, then 0 once the window is over. */
    assert(lk_bootmenu_remaining_s(0u) == 5u);
    assert(lk_bootmenu_remaining_s(1u) == 5u);
    assert(lk_bootmenu_remaining_s(999u) == 5u);
    assert(lk_bootmenu_remaining_s(1000u) == 4u);
    assert(lk_bootmenu_remaining_s(4999u) == 1u);
    assert(lk_bootmenu_remaining_s(5000u) == 0u);
    assert(lk_bootmenu_remaining_s(6000u) == 0u);

    /* The bar drains full-to-empty across the window and parks at empty. */
    assert(lk_bootmenu_bar_permille(0u) == 1000u);
    assert(lk_bootmenu_bar_permille(2500u) == 500u);
    assert(lk_bootmenu_bar_permille(4999u) == 0u);
    assert(lk_bootmenu_bar_permille(5000u) == 0u);
    assert(lk_bootmenu_bar_permille(6000u) == 0u);

    /* A press anywhere in the window tags Android; silence tags MixOS. */
    assert(lk_bootmenu_pick(1u) == LK_BOOTMENU_ANDROID);
    assert(lk_bootmenu_pick(0u) == LK_BOOTMENU_MIXOS);

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
    assert(lk_menu_text_width("AB", 2u) == 22u);

    /* The colour helpers: mixsplash's formulas, lifted to opaque ARGB. */
    assert(lk_menu_mix(0xff112233u, 0x445566u, 0u) == 0xff112233u);
    assert(lk_menu_mix(0xff112233u, 0x445566u, 255u) == 0xff445566u);
    assert(lk_menu_mix(0xffffffffu, 0x000000u, 96u) == 0xff9f9f9fu);
    assert(lk_menu_mix(0xff000000u, 0x0078d4u, 235u) == 0xff006ec3u);
    assert(lk_menu_add(0xff000000u, 72, 90, 90) == 0xff485a5au);
    assert(lk_menu_add(0xffffffffu, 10, 10, 10) == 0xffffffffu);
    assert(lk_menu_add(0xff000000u, -5, -5, -5) == 0xff000000u);

    /* Fixed-point sine: cardinals, wrap, symmetry, quadrant shape. */
    assert(lk_sin256(0u) == 0);
    assert(lk_sin256(64u) == 256);
    assert(lk_sin256(128u) == 0);
    assert(lk_sin256(192u) == -256);
    assert(lk_sin256(256u) == lk_sin256(0u));
    assert(lk_sin256(511u) == lk_sin256(255u));
    {
        uint32_t i;
        for (i = 0u; i < 256u; ++i) {
            int32_t v = lk_sin256(i);
            assert(v >= -256 && v <= 256);
            assert(v == -lk_sin256((i + 128u) & 255u));
            assert(v == lk_sin256((128u - i) & 255u));
            if (i < 64u) assert(lk_sin256(i + 1u) >= v);
            if (i >= 64u && i < 128u) assert(lk_sin256(i + 1u) <= v);
        }
    }

    /* The easing: fixed at the cardinals, monotone, ±31 at most. */
    assert(lk_spin_ease256(0u) == 0);
    assert(lk_spin_ease256(128u) == 128);
    assert(lk_spin_ease256(64u) == 95);
    assert(lk_spin_ease256(192u) == 161);
    {
        uint32_t p;
        for (p = 0u; p < 255u; ++p) {
            int32_t e0 = lk_spin_ease256(p), e1 = lk_spin_ease256(p + 1u);
            assert(e1 >= e0);
            assert(e0 - (int32_t)p >= -31 && e0 - (int32_t)p <= 31);
        }
    }

    /* Dot phases: period wrap, stagger offset, negative-time wrap. */
    assert(lk_spin_phase256(0u, 0u) == 0u);
    assert(lk_spin_phase256(2000u, 0u) == 0u);
    assert(lk_spin_phase256(100u, 1u) == 0u);
    assert(lk_spin_phase256(0u, 1u) == 243u);
    assert(lk_spin_phase256(50u, 1u) == 249u);

    /* Orbit: top at phase 0, right at the quarter, radius held. */
    {
        int32_t dx, dy;
        uint32_t e;
        lk_spin_offset(0u, 26u, &dx, &dy);
        assert(dx == 0 && dy == -26);
        lk_spin_offset(64u, 26u, &dx, &dy);
        assert(dx == 26 && dy == 0);
        for (e = 0u; e < 256u; e += 7u) {
            int32_t rr;
            lk_spin_offset(e, 26u, &dx, &dy);
            rr = dx * dx + dy * dy;
            assert(rr >= 24 * 24 && rr <= 28 * 28);
        }
    }

    /* Geometry: reference radius, and the save box covering every paint box
     * at 640x480, 1920x1080 and a portrait 1080x2400. */
    assert(lk_menu_spin_radius(640u, 480u) == 26u);
    assert(lk_menu_spin_dot(640u, 480u) == 3u);
    assert(lk_menu_spin_radius(1920u, 1080u) == 59u);
    check_save_covers(640u, 480u);
    check_save_covers(1920u, 1080u);
    check_save_covers(1080u, 2400u);

    printf("bootmenu: window, countdown, bar, pick, blend and menu-ui passed\n");
    return 0;
}
