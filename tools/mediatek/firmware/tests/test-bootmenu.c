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

    printf("bootmenu: window, countdown, bar and pick passed\n");
    return 0;
}
