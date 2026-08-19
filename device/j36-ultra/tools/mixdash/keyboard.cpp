/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "keyboard.h"

#include <QFontMetrics>
#include <QFile>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QRegion>
#include <QResizeEvent>
#include <QSaveFile>
#include <QTextStream>

#include <linux/input.h>

#include "joypad.h"
#include "keyboardlayout.h"
#include "theme.h"

namespace KeyMap {

namespace {

/*
 * A US layout, and only the part of it a passphrase or a shell command can be
 * made of.  Written as a table rather than derived from a keymap file because
 * this image has no console keymaps in it -- /etc/console-setup is not installed
 * -- and because the table is shorter than the code that would read one.
 */
struct Entry {
    int code;
    const char *plain;
    const char *shifted;
};

const Entry kTable[] = {
    { KEY_A, "a", "A" }, { KEY_B, "b", "B" }, { KEY_C, "c", "C" }, { KEY_D, "d", "D" },
    { KEY_E, "e", "E" }, { KEY_F, "f", "F" }, { KEY_G, "g", "G" }, { KEY_H, "h", "H" },
    { KEY_I, "i", "I" }, { KEY_J, "j", "J" }, { KEY_K, "k", "K" }, { KEY_L, "l", "L" },
    { KEY_M, "m", "M" }, { KEY_N, "n", "N" }, { KEY_O, "o", "O" }, { KEY_P, "p", "P" },
    { KEY_Q, "q", "Q" }, { KEY_R, "r", "R" }, { KEY_S, "s", "S" }, { KEY_T, "t", "T" },
    { KEY_U, "u", "U" }, { KEY_V, "v", "V" }, { KEY_W, "w", "W" }, { KEY_X, "x", "X" },
    { KEY_Y, "y", "Y" }, { KEY_Z, "z", "Z" },

    { KEY_1, "1", "!" }, { KEY_2, "2", "@" }, { KEY_3, "3", "#" }, { KEY_4, "4", "$" },
    { KEY_5, "5", "%" }, { KEY_6, "6", "^" }, { KEY_7, "7", "&" }, { KEY_8, "8", "*" },
    { KEY_9, "9", "(" }, { KEY_0, "0", ")" },

    { KEY_MINUS, "-", "_" }, { KEY_EQUAL, "=", "+" },
    { KEY_LEFTBRACE, "[", "{" }, { KEY_RIGHTBRACE, "]", "}" },
    { KEY_BACKSLASH, "\\", "|" }, { KEY_SEMICOLON, ";", ":" },
    { KEY_APOSTROPHE, "'", "\"" }, { KEY_GRAVE, "`", "~" },
    { KEY_COMMA, ",", "<" }, { KEY_DOT, ".", ">" }, { KEY_SLASH, "/", "?" },
    { KEY_SPACE, " ", " " },

    { KEY_KP0, "0", "0" }, { KEY_KP1, "1", "1" }, { KEY_KP2, "2", "2" },
    { KEY_KP3, "3", "3" }, { KEY_KP4, "4", "4" }, { KEY_KP5, "5", "5" },
    { KEY_KP6, "6", "6" }, { KEY_KP7, "7", "7" }, { KEY_KP8, "8", "8" },
    { KEY_KP9, "9", "9" }, { KEY_KPDOT, ".", "." }, { KEY_KPPLUS, "+", "+" },
    { KEY_KPMINUS, "-", "-" }, { KEY_KPASTERISK, "*", "*" }, { KEY_KPSLASH, "/", "/" }
};

} /* namespace */

QString character(int code, int modifiers)
{
    const bool shift = (modifiers & Joypad::ModShift) != 0;
    for (size_t i = 0; i < sizeof(kTable) / sizeof(kTable[0]); ++i) {
        if (kTable[i].code != code)
            continue;
        return QString::fromLatin1(shift ? kTable[i].shifted : kTable[i].plain);
    }
    return QString();
}

} /* namespace KeyMap */

namespace {

const int kRowGap = 4;
const int kFieldH = 34;
const int kPad = 8;

} /* namespace */

Keyboard::Keyboard(QWidget *parent)
    : QWidget(parent)
{
    /* linuxfb has no compositor.  A translucent top-level child makes every
     * D-pad focus change read and blend the page below again; on the uncached
     * panel that was slow and successive partial updates visibly ate the text
     * field.  The keyboard owns its rectangle while it is open, so say so to Qt
     * and paint it opaquely.  Its colours, gradients and layout stay unchanged. */
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setMouseTracking(true);
    hide();
    buildLayout();
}

void Keyboard::loadSharedState()
{
    /* Safe defaults are also the first-open position on a fresh boot. */
    m_row = 1;
    m_col = 0;
    m_layer = 0;
    m_shiftLatched = false;

    QFile file(QString::fromLatin1(J36_KBD_STATE_PATH));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    struct j36_keyboard_state state = {};
    QTextStream in(&file);
    in >> state.version >> state.row >> state.col
       >> state.layer >> state.shift_latched;
    if (in.status() != QTextStream::Ok ||
        state.version != J36_KBD_STATE_VERSION ||
        state.row < 0 || state.row >= J36_KBD_ROWS ||
        state.col < 0 || state.col >= J36_KBD_MAX_COLS ||
        state.layer < 0 || state.layer > 2 ||
        (state.shift_latched != 0 && state.shift_latched != 1))
        return;

    m_row = state.row;
    m_col = state.col;
    m_layer = state.layer;
    m_shiftLatched = state.layer == 1 && state.shift_latched;
}

void Keyboard::saveSharedState() const
{
    QSaveFile file(QString::fromLatin1(J36_KBD_STATE_PATH));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;

    QTextStream out(&file);
    out << J36_KBD_STATE_VERSION << ' ' << m_row << ' ' << m_col << ' '
        << m_layer << ' ' << (m_shiftLatched ? 1 : 0) << '\n';
    out.flush();
    if (out.status() == QTextStream::Ok)
        file.commit();
    else
        file.cancelWriting();
}

void Keyboard::updateCaps(int oldRow, int oldCol, int newRow, int newCol)
{
    (void)oldRow;
    (void)oldCol;
    (void)newRow;
    (void)newCol;
    /* Full widget, not a pair of caps.  Up/down then left dirties the field
     * plus two distant keys; linuxfb unions that into a tall rectangle, the
     * page underneath paints into it, and a partial update cannot put the
     * typed text back.  The keyboard is opaque, so a complete fill is cheap. */
    update();
}

void Keyboard::open(const QString &prompt, const QString &initial, bool password)
{
    m_prompt = prompt;
    m_text = initial;
    m_caret = m_text.size();
    m_password = password;
    loadSharedState();
    m_pressed = -1;
    buildLayout();
    relayout();
    show();
    raise();
    update();
}

void Keyboard::dismiss(bool accepted)
{
    if (!isVisible())
        return;
    saveSharedState();
    hide();
    emit finished(m_text, accepted);
}

void Keyboard::buildLayout()
{
    m_rows.clear();

    const char *const *layer = m_layer == 1 ? j36_kbd_upper
                             : m_layer == 2 ? j36_kbd_symbols : j36_kbd_lower;

    for (int r = 0; r < J36_KBD_CHAR_ROWS; ++r) {
        QVector<Cap> row;
        const QString chars = QString::fromUtf8(layer[r]);
        for (int i = 0; i < chars.size(); ++i) {
            Cap c;
            c.label = chars.mid(i, 1);
            c.value = c.label;
            row.append(c);
        }
        if (r == 3) {
            /* The short row gets the two edits, so backspace is never more than
             * two presses from wherever the cursor is. */
            Cap shift;
            /*
             * The label names the state the keyboard is IN, and it used to name the
             * state the next press would put it in.  That was reported as "caps does
             * not lock", and the report was fair: locked upper drew the cap as
             * "shift" in lower case and one-shot upper drew it as "CAPS", so the one
             * state that does lock looked like the one that is off, and the state
             * that drops after a single letter looked like the lock.  Anyone who
             * tapped once, read "CAPS", typed a letter and watched it go back to
             * lower case had every reason to call it a bug.
             *
             * Every keyboard anyone has used names the current state on this key --
             * outlined arrow off, filled arrow for one shot, barred arrow for the
             * lock -- so this one does too, in the three words it has room for, and
             * `lit' puts the two engaged states in the accent colour so the lock is
             * visible without reading the cap at all.
             */
            shift.label = m_layer != 1     ? tr("shift")   /* off               */
                        : m_shiftLatched   ? tr("SHIFT")   /* on, one character */
                                           : tr("CAPS");   /* locked            */
            shift.special = KeyShift;
            shift.span = J36_KBD_SHIFT_SPAN;
            shift.lit = (m_layer == 1);
            row.prepend(shift);

            Cap back;
            back.label = tr("back");
            back.special = KeyBackspace;
            back.span = J36_KBD_BACK_SPAN;
            row.append(back);
        }
        m_rows.append(row);
    }

    QVector<Cap> bottom;
    Cap sym;
    sym.label = m_layer == 2 ? "abc" : "?123";
    sym.special = KeySymbols;
    sym.span = J36_KBD_SYMBOL_SPAN;
    bottom.append(sym);

    Cap left;
    left.label = "<";
    left.special = KeyLeft;
    left.span = J36_KBD_ARROW_SPAN;
    bottom.append(left);

    Cap space;
    space.label = tr("space");
    space.special = KeySpace;
    space.span = J36_KBD_SPACE_SPAN;
    bottom.append(space);

    Cap right;
    right.label = ">";
    right.special = KeyRight;
    right.span = J36_KBD_ARROW_SPAN;
    bottom.append(right);

    Cap cancel;
    cancel.label = tr("cancel");
    cancel.special = KeyCancel;
    cancel.span = J36_KBD_CANCEL_SPAN;
    bottom.append(cancel);

    Cap ok;
    ok.label = tr("done");
    ok.special = KeyAccept;
    ok.span = J36_KBD_ACCEPT_SPAN;
    bottom.append(ok);

    m_rows.append(bottom);

    if (m_row < 0)
        m_row = 0;
    if (m_row >= m_rows.size())
        m_row = m_rows.size() - 1;
    if (!m_rows.isEmpty()) {
        if (m_col < 0)
            m_col = 0;
        if (m_col >= m_rows[m_row].size())
            m_col = m_rows[m_row].size() - 1;
    }
}

QRectF Keyboard::fieldRect() const
{
    return QRectF(kPad + 6, kPad + 22, width() - 2 * (kPad + 6), kFieldH);
}

void Keyboard::relayout()
{
    if (m_rows.isEmpty())
        return;

    const qreal top = fieldRect().bottom() + 10;
    const qreal avail = height() - top - kPad;
    const qreal rowH = qMax(20.0, (avail - kRowGap * (m_rows.size() - 1)) / m_rows.size());

    for (int r = 0; r < m_rows.size(); ++r) {
        qreal spans = 0;
        for (int c = 0; c < m_rows[r].size(); ++c)
            spans += m_rows[r][c].span;
        const qreal usable = width() - 2 * kPad - kRowGap * (m_rows[r].size() - 1);
        const qreal unit = usable / qMax(1.0, spans);

        qreal x = kPad;
        const qreal y = top + r * (rowH + kRowGap);
        for (int c = 0; c < m_rows[r].size(); ++c) {
            const qreal w = unit * m_rows[r][c].span;
            m_rows[r][c].rect = QRectF(x, y, w, rowH);
            x += w + kRowGap;
        }
    }
}

void Keyboard::resizeEvent(QResizeEvent *event)
{
    relayout();
    QWidget::resizeEvent(event);
}

void Keyboard::insert(const QString &s)
{
    if (s.isEmpty())
        return;
    m_caret = qBound(0, m_caret, m_text.size());
    m_text.insert(m_caret, s);
    m_caret += s.size();

    /* A latched shift is for one character, which is what makes typing a
     * capitalised SSID two presses instead of three. */
    if (m_layer == 1 && m_shiftLatched) {
        m_layer = 0;
        m_shiftLatched = false;
        buildLayout();
        relayout();
    }
    update();
}

void Keyboard::backspace()
{
    m_caret = qBound(0, m_caret, m_text.size());
    if (m_caret <= 0 || m_text.isEmpty())
        return;
    m_text.remove(m_caret - 1, 1);
    --m_caret;
    update();
}

/*
 * The lock, set from somewhere that is not the shift cap -- which for now is a real
 * keyboard's Caps Lock key.  It goes through the same two variables the cap uses
 * rather than adding a third, so the on-screen shift lights up when the physical key
 * is pressed and the two can never disagree about what case the next letter is.
 *
 * Turning it off returns to lower case rather than to one-shot upper: the symbols
 * layer is somewhere Caps Lock has no opinion about, so it is left alone.
 */
void Keyboard::setCapsLocked(bool on)
{
    if (capsLocked() == on)
        return;
    if (on) {
        m_layer = 1;
        m_shiftLatched = false;
    } else if (m_layer == 1) {
        m_layer = 0;
        m_shiftLatched = false;
    }
    buildLayout();
    relayout();
    update();
}

void Keyboard::pressCap(Cap cap)
{
    switch (cap.special) {
    case KeyChar:
        insert(cap.value);
        return;
    case KeyShift:
        /*
         * Three states, cycled by tapping, which is what every phone keyboard
         * does and what typing a WPA passphrase on this thing needs.
         *
         *     lower  --tap-->  one-shot upper  --tap-->  CAPS LOCK  --tap-->  lower
         *
         * It used to be two, and the middle one was missing: shift capitalised
         * exactly one character and then dropped back, so a key that is genuinely
         * all upper case -- a network name, a serial, a password somebody wrote
         * down in capitals -- cost one shift press PER LETTER, with the D-pad
         * travelling back to the shift cap between each one.  Twelve characters
         * was twenty-four presses and a wrong one anywhere was invisible in a
         * password field.
         *
         * The two upper states differ only in m_shiftLatched, which insert()
         * already reads: latched means "for one character" and it drops the layer
         * after that character; unlatched means the layer stays where it is,
         * which IS caps lock and needs no other machinery.
         */
        if (m_layer != 1) {
            m_layer = 1;
            m_shiftLatched = true;      /* one-shot */
        } else if (m_shiftLatched) {
            m_shiftLatched = false;     /* locked: stays until tapped again */
        } else {
            m_layer = 0;                /* back to lower */
        }
        buildLayout();
        relayout();
        update();
        return;
    case KeySymbols:
        m_layer = (m_layer == 2) ? 0 : 2;
        m_shiftLatched = false;
        buildLayout();
        relayout();
        update();
        return;
    case KeyBackspace:
        backspace();
        return;
    case KeySpace:
        insert(QStringLiteral(" "));
        return;
    case KeyLeft:
        m_caret = qMax(0, m_caret - 1);
        update();
        return;
    case KeyRight:
        m_caret = qMin(m_text.size(), m_caret + 1);
        update();
        return;
    case KeyAccept:
        dismiss(true);
        return;
    case KeyCancel:
        dismiss(false);
        return;
    default:
        return;
    }
}

bool Keyboard::handleNav(int action)
{
    if (!isVisible() || m_rows.isEmpty())
        return false;

    const int oldRow = m_row;
    const int oldCol = m_col;

    switch (action) {
    case Joypad::NavUp:
        if (m_row > 0) {
            /* Keep the horizontal position across rows of different lengths by
             * matching the centre of the cap, not its index -- otherwise moving
             * up from the space bar lands on the first key of the row above. */
            const qreal cx = (m_row < m_rows.size() && m_col >= 0 && m_col < m_rows[m_row].size())
                           ? m_rows[m_row][m_col].rect.center().x()
                           : 0.0;
            --m_row;
            m_col = 0;
            qreal bestDist = 1e9;
            for (int c = 0; c < m_rows[m_row].size(); ++c) {
                const qreal d = qAbs(m_rows[m_row][c].rect.center().x() - cx);
                if (d < bestDist) {
                    bestDist = d;
                    m_col = c;
                }
            }
            updateCaps(oldRow, oldCol, m_row, m_col);
        }
        return true;
    case Joypad::NavDown:
        if (m_row < m_rows.size() - 1) {
            const qreal cx = (m_row >= 0 && m_row < m_rows.size() && m_col >= 0 && m_col < m_rows[m_row].size())
                           ? m_rows[m_row][m_col].rect.center().x()
                           : 0.0;
            ++m_row;
            m_col = 0;
            qreal bestDist = 1e9;
            for (int c = 0; c < m_rows[m_row].size(); ++c) {
                const qreal d = qAbs(m_rows[m_row][c].rect.center().x() - cx);
                if (d < bestDist) {
                    bestDist = d;
                    m_col = c;
                }
            }
            updateCaps(oldRow, oldCol, m_row, m_col);
        }
        return true;
    case Joypad::NavLeft:
        if (m_row >= 0 && m_row < m_rows.size() && !m_rows[m_row].isEmpty()) {
            m_col = (m_col - 1 + m_rows[m_row].size()) % m_rows[m_row].size();
            updateCaps(oldRow, oldCol, m_row, m_col);
        }
        return true;
    case Joypad::NavRight:
        if (m_row >= 0 && m_row < m_rows.size() && !m_rows[m_row].isEmpty()) {
            m_col = (m_col + 1) % m_rows[m_row].size();
            updateCaps(oldRow, oldCol, m_row, m_col);
        }
        return true;
    case Joypad::NavOk:
        if (m_row >= 0 && m_row < m_rows.size() && m_col >= 0 && m_col < m_rows[m_row].size()) {
            const Cap cap = m_rows[m_row][m_col];
            pressCap(cap);
        }
        return true;
    case Joypad::NavBack:
        dismiss(false);
        return true;
    case Joypad::NavMenu:
        dismiss(true);
        return true;
    case Joypad::NavPrevPage:
        backspace();
        return true;
    case Joypad::NavNextPage:
        insert(QStringLiteral(" "));
        return true;
    default:
        return true; /* Swallow everything while the keyboard is up. */
    }
}

void Keyboard::keyPressed(int code, bool pressed, int modifiers)
{
    if (!pressed || !isVisible())
        return;

    switch (code) {
    case KEY_ENTER:
    case KEY_KPENTER:
        dismiss(true);
        return;
    case KEY_ESC:
        dismiss(false);
        return;
    case KEY_BACKSPACE:
        backspace();
        return;
    case KEY_DELETE:
        m_caret = qBound(0, m_caret, m_text.size());
        if (m_caret >= 0 && m_caret < m_text.size()) {
            m_text.remove(m_caret, 1);
            update();
        }
        return;
    case KEY_LEFT:
        m_caret = qMax(0, m_caret - 1);
        update();
        return;
    case KEY_RIGHT:
        m_caret = qMin(m_text.size(), m_caret + 1);
        update();
        return;
    case KEY_HOME:
        m_caret = 0;
        update();
        return;
    case KEY_END:
        m_caret = m_text.size();
        update();
        return;
    case KEY_CAPSLOCK:
        /* It did nothing at all before this: the modifier set joypad.cpp tracks is
         * shift, control and alt, and KeyMap::character() reads only the first of
         * them, so a USB keyboard's Caps Lock typed lower case for ever.  Toggling
         * the on-screen lock is the whole fix, because the case is applied below
         * from that state rather than from a modifier bit. */
        setCapsLocked(!capsLocked());
        return;
    default:
        break;
    }

    QString s = KeyMap::character(code, modifiers);
    if (s.isEmpty())
        return;

    /*
     * Caps Lock is LETTERS ONLY, which is the one thing about it everybody knows and
     * the reason it cannot be implemented as a permanently held shift: locked caps
     * plus 1 is 1, not an exclamation mark.  isLetter() is the test, and shift while
     * the lock is on inverts rather than adds, exactly as it does on a real keyboard.
     */
    if (capsLocked() && s.size() == 1 && s.at(0).isLetter())
        s = (modifiers & Joypad::ModShift) ? s.toLower() : s.toUpper();

    insert(s);
}

int Keyboard::capAt(const QPoint &p) const
{
    for (int r = 0; r < m_rows.size(); ++r)
        for (int c = 0; c < m_rows[r].size(); ++c)
            if (m_rows[r][c].rect.contains(QPointF(p)))
                return r * 100 + c;
    return -1;
}

void Keyboard::mouseMoveEvent(QMouseEvent *event)
{
    const int hit = capAt(event->pos());
    if (hit >= 0 && (m_row != hit / 100 || m_col != hit % 100)) {
        const int oldRow = m_row;
        const int oldCol = m_col;
        m_row = hit / 100;
        m_col = hit % 100;
        updateCaps(oldRow, oldCol, m_row, m_col);
    }
    event->accept();
}

void Keyboard::mousePressEvent(QMouseEvent *event)
{
    const int oldRow = m_row;
    const int oldCol = m_col;
    m_pressed = capAt(event->pos());
    if (m_pressed >= 0) {
        m_row = m_pressed / 100;
        m_col = m_pressed % 100;
        updateCaps(oldRow, oldCol, m_row, m_col);
    }
    /* Accepted even on a miss: the keyboard is modal, and a press on its
     * background must not reach the page underneath. */
    event->accept();
}

void Keyboard::mouseReleaseEvent(QMouseEvent *event)
{
    const int hit = capAt(event->pos());
    if (hit >= 0 && hit == m_pressed) {
        const int r = hit / 100;
        const int c = hit % 100;
        /* Copied, because pressCap can rebuild m_rows out from under a reference. */
        const Cap cap = m_rows[r][c];
        m_pressed = -1;
        pressCap(cap);
        event->accept();
        return;
    }
    m_pressed = -1;
    update();
    event->accept();
}

void Keyboard::paintEvent(QPaintEvent *event)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRegion dirty = event->region();

    /* The panel itself.  Clip the fill to the dirty rect so a partial
     * event cannot paint background over the field and then skip it. */
    const QColor back = Theme::window();
    p.setPen(Qt::NoPen);
    p.setBrush(back);
    p.setClipRegion(dirty);
    p.drawRoundedRect(QRectF(0, 0, width(), height() + Theme::Radius), Theme::Radius,
                      Theme::Radius);
    p.setClipping(false);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Theme::border(), 1.0));
    p.drawLine(QPointF(0, 0.5), QPointF(width(), 0.5));

    const QRectF promptRect(kPad + 6, kPad - 2, width() - 2 * kPad, 20);
    if (dirty.intersects(promptRect.toAlignedRect())) {
        p.setFont(Theme::font(12));
        p.setPen(Theme::ink3());
        p.drawText(promptRect, Qt::AlignLeft | Qt::AlignVCenter, m_prompt);
    }

    /* The field.  Always redrawn: linuxfb clips poorly, and the background
     * fill above would otherwise leave an empty box after a D-pad step. */
    const QRectF field = fieldRect();
    Theme::vgrad(p, field, Theme::glass().lighter(140),
                 Theme::glass().lighter(120), 8);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Theme::blue(), 1.4));
    p.drawRoundedRect(field.adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);

    const QString shown = m_password ? QString(m_text.size(), QChar(0x2022)) : m_text;
    const QFont fieldFont = Theme::font(14);
    const QFontMetrics ffm(fieldFont);
    p.setFont(fieldFont);
    p.setPen(Theme::ink());

    /* Scrolled so the caret is always on screen, which for a 63-character key
     * is the difference between usable and not. */
    const qreal inner = qMax(1.0, field.width() - 20);
    const int safeCaret = qBound(0, m_caret, shown.size());
    qreal caretX = ffm.horizontalAdvance(shown.left(safeCaret));
    qreal offset = 0;
    if (caretX > inner)
        offset = caretX - inner;
    p.save();
    p.setClipRect(field.adjusted(8, 0, -8, 0));
    p.drawText(QRectF(field.x() + 10 - offset, field.y(),
                      qMax(inner, caretX + 20), field.height()),
               Qt::AlignLeft | Qt::AlignVCenter, shown);
    p.setPen(QPen(Theme::blue(), 1.6));
    p.drawLine(QPointF(field.x() + 10 - offset + caretX, field.y() + 6),
               QPointF(field.x() + 10 - offset + caretX, field.bottom() - 6));
    p.restore();

    /* The caps.  Always drawn: a clip-only skip after a tall D-pad update
     * left blank keys, and the next Left then looked like the field had
     * been erased because the strip above it was gone too. */
    const QFont capFont = Theme::font(15, true);
    const QFont wideFont = Theme::font(12, true);
    for (int r = 0; r < m_rows.size(); ++r) {
        for (int c = 0; c < m_rows[r].size(); ++c) {
            const Cap &cap = m_rows[r][c];
            const bool focused = (r == m_row && c == m_col);
            const bool down = (m_pressed == r * 100 + c);
            const bool accent = (cap.special == KeyAccept);

            /* An engaged modifier is drawn like the accept key so that "the shift
             * is on" is a colour rather than a word -- and teal rather than blue so
             * it is not mistaken for the one cap that ends the typing. */
            QColor top = accent ? Theme::blue() : cap.lit ? Theme::teal() : Theme::card();
            QColor bottom = accent ? Theme::blueLow()
                          : cap.lit ? Theme::teal().darker(130)
                                    : Theme::cardLow();
            if (down) {
                top = top.darker(120);
                bottom = bottom.darker(120);
            } else if (focused) {
                top = top.lighter(126);
                bottom = bottom.lighter(118);
            }
            Theme::vgrad(p, cap.rect, top, bottom, 7);

            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(focused ? Theme::blue() : Theme::border(), focused ? 1.8 : 1.0));
            p.drawRoundedRect(cap.rect.adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);

            p.setFont(cap.special == KeyChar ? capFont : wideFont);
            p.setPen(cap.special == KeyCancel ? Theme::ink2() : Theme::ink());
            p.drawText(cap.rect, Qt::AlignCenter, cap.label);
        }
    }
}
