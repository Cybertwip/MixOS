/* Host test for the LK boot selection (lk_bootmenu.h).
 *
 * The header is pure logic -- no registers, no DRAM -- so the target and
 * the workstation compile the same code. Run with:
 *
 *     cc -std=c99 -Wall -Wextra -Werror tools/mediatek/mt65xx/firmware/tests/test-bootmenu.c \
 *         -o /tmp/j36-bootmenu-test && /tmp/j36-bootmenu-test
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

    /* Payload sniff: stock passes, MixOS marks trip. */
    {
        uint8_t hdr[1024];
        uint32_t i;

        for (i = 0u; i < 1024u; ++i) hdr[i] = 0u;
        memcpy(hdr + 48u, "1778287588", 10u);
        assert(lk_bootmenu_is_mixos_payload(hdr) == 0u);
        memcpy(hdr + 48u, "j36-ultra", 9u);
        assert(lk_bootmenu_is_mixos_payload(hdr) == 1u);
        for (i = 0u; i < 1024u; ++i) hdr[i] = 0u;
        memcpy(hdr + 64u, "console=tty0 j36.audio=speaker", 29u);
        assert(lk_bootmenu_is_mixos_payload(hdr) == 1u);
        for (i = 0u; i < 1024u; ++i) hdr[i] = 0u;
        memcpy(hdr + 64u, "console=tty0 j36x", 17u);
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

    printf("bootmenu: window, debounce, pick, payload, blend and menu-ui passed\n");
    return 0;
}
