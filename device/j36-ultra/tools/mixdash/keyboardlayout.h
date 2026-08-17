/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * The keyboard contract shared by mixdash's linuxfb surface and j36-padx's X
 * surface.  They cannot share a native window -- they belong to different display
 * servers -- but they do share this layout and the small state record below.  The
 * foreground hand-off ensures that only one surface is live at a time.
 */
#ifndef J36_KEYBOARD_LAYOUT_H
#define J36_KEYBOARD_LAYOUT_H

#define J36_KBD_CHAR_ROWS 4
#define J36_KBD_ROWS 5
#define J36_KBD_MAX_COLS 12

static const char *const j36_kbd_lower[J36_KBD_CHAR_ROWS] = {
    "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"
};

static const char *const j36_kbd_upper[J36_KBD_CHAR_ROWS] = {
    "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"
};

/* Keep this byte-oriented and ASCII: the X surface uses the server's core font
 * and keymap, while Qt uses UTF-8.  Every symbol here is available from both. */
static const char *const j36_kbd_symbols[J36_KBD_CHAR_ROWS] = {
    "!@#$%^&*()", "-_=+[]{}|\\", ":;'\",.<>/?", "`~"
};

#define J36_KBD_SHIFT_SPAN  1.5
#define J36_KBD_BACK_SPAN   1.5
#define J36_KBD_SYMBOL_SPAN 1.6
#define J36_KBD_ARROW_SPAN  0.9
#define J36_KBD_SPACE_SPAN  4.0
#define J36_KBD_CANCEL_SPAN 1.6
#define J36_KBD_ACCEPT_SPAN 1.6

/* Selection and layer follow the user between the two display surfaces.  Text is
 * deliberately absent: it belongs to the focused field, never to the keyboard. */
#define J36_KBD_STATE_PATH "/run/j36/keyboard.state"
#define J36_KBD_STATE_VERSION 1

struct j36_keyboard_state {
    int version;
    int row;
    int col;
    int layer;          /* 0 lower, 1 upper, 2 symbols */
    int shift_latched;  /* layer 1 is one-shot when true, caps-lock otherwise */
};

#endif /* J36_KEYBOARD_LAYOUT_H */
