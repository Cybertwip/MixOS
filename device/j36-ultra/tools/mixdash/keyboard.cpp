/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "keyboard.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>

#include <linux/input.h>

#include "joypad.h"
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

const char *kLower[4] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm"
};

const char *kUpper[4] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM"
};

/* The symbols a passphrase, a URL and an apt package name are actually made of.
 * Four rows of ten so the grid geometry does not change between layers. */
const char *kSymbols[4] = {
    "!@#$%^&*()",
    "-_=+[]{}|\\",
    ":;'\",.<>/?",
    "`~\xC2\xA3\xE2\x82\xAC"
};

} /* namespace */

Keyboard::Keyboard(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    hide();
    buildLayout();
}

void Keyboard::open(const QString &prompt, const QString &initial, bool password)
{
    m_prompt = prompt;
    m_text = initial;
    m_caret = m_text.size();
    m_password = password;
    m_layer = 0;
    m_shiftLatched = false;
    m_row = 1;
    m_col = 0;
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
    hide();
    emit finished(m_text, accepted);
}

void Keyboard::buildLayout()
{
    m_rows.clear();

    const char **layer = m_layer == 1 ? kUpper : m_layer == 2 ? kSymbols : kLower;

    for (int r = 0; r < 4; ++r) {
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
            shift.label = m_layer == 1 ? "shift" : "SHIFT";
            shift.special = KeyShift;
            shift.span = 1.5;
            row.prepend(shift);

            Cap back;
            back.label = "back";
            back.special = KeyBackspace;
            back.span = 1.5;
            row.append(back);
        }
        m_rows.append(row);
    }

    QVector<Cap> bottom;
    Cap sym;
    sym.label = m_layer == 2 ? "abc" : "?123";
    sym.special = KeySymbols;
    sym.span = 1.6;
    bottom.append(sym);

    Cap left;
    left.label = "<";
    left.special = KeyLeft;
    left.span = 0.9;
    bottom.append(left);

    Cap space;
    space.label = "space";
    space.special = KeySpace;
    space.span = 4.0;
    bottom.append(space);

    Cap right;
    right.label = ">";
    right.special = KeyRight;
    right.span = 0.9;
    bottom.append(right);

    Cap cancel;
    cancel.label = "cancel";
    cancel.special = KeyCancel;
    cancel.span = 1.6;
    bottom.append(cancel);

    Cap ok;
    ok.label = "done";
    ok.special = KeyAccept;
    ok.span = 1.6;
    bottom.append(ok);

    m_rows.append(bottom);

    if (m_row >= m_rows.size())
        m_row = m_rows.size() - 1;
    if (m_col >= m_rows[m_row].size())
        m_col = m_rows[m_row].size() - 1;
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
    if (m_caret <= 0 || m_text.isEmpty())
        return;
    m_text.remove(m_caret - 1, 1);
    --m_caret;
    update();
}

void Keyboard::pressCap(const Cap &cap)
{
    switch (cap.special) {
    case KeyChar:
        insert(cap.value);
        return;
    case KeyShift:
        m_layer = (m_layer == 1) ? 0 : 1;
        m_shiftLatched = (m_layer == 1);
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
    if (!isVisible())
        return false;

    switch (action) {
    case Joypad::NavUp:
        if (m_row > 0) {
            /* Keep the horizontal position across rows of different lengths by
             * matching the centre of the cap, not its index -- otherwise moving
             * up from the space bar lands on the first key of the row above. */
            const qreal cx = m_rows[m_row][m_col].rect.center().x();
            --m_row;
            m_col = 0;
            for (int c = 0; c < m_rows[m_row].size(); ++c)
                if (m_rows[m_row][c].rect.contains(cx, m_rows[m_row][c].rect.center().y()))
                    m_col = c;
            update();
        }
        return true;
    case Joypad::NavDown:
        if (m_row < m_rows.size() - 1) {
            const qreal cx = m_rows[m_row][m_col].rect.center().x();
            ++m_row;
            m_col = 0;
            for (int c = 0; c < m_rows[m_row].size(); ++c)
                if (m_rows[m_row][c].rect.contains(cx, m_rows[m_row][c].rect.center().y()))
                    m_col = c;
            update();
        }
        return true;
    case Joypad::NavLeft:
        m_col = (m_col - 1 + m_rows[m_row].size()) % m_rows[m_row].size();
        update();
        return true;
    case Joypad::NavRight:
        m_col = (m_col + 1) % m_rows[m_row].size();
        update();
        return true;
    case Joypad::NavOk:
        pressCap(m_rows[m_row][m_col]);
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
        if (m_caret < m_text.size()) {
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
    default:
        break;
    }

    const QString s = KeyMap::character(code, modifiers);
    if (!s.isEmpty())
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
    if (hit >= 0) {
        m_row = hit / 100;
        m_col = hit % 100;
        update();
    }
    event->accept();
}

void Keyboard::mousePressEvent(QMouseEvent *event)
{
    m_pressed = capAt(event->pos());
    if (m_pressed >= 0) {
        m_row = m_pressed / 100;
        m_col = m_pressed % 100;
        update();
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

void Keyboard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    /* The panel itself: solid enough to read a passphrase against whatever the
     * page underneath happens to be. */
    QColor back = Theme::window();
    back.setAlpha(244);
    p.setPen(Qt::NoPen);
    p.setBrush(back);
    p.drawRoundedRect(QRectF(0, 0, width(), height() + Theme::Radius), Theme::Radius,
                      Theme::Radius);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Theme::border(), 1.0));
    p.drawLine(QPointF(0, 0.5), QPointF(width(), 0.5));

    p.setFont(Theme::font(12));
    p.setPen(Theme::ink3());
    p.drawText(QRectF(kPad + 6, kPad - 2, width() - 2 * kPad, 20),
               Qt::AlignLeft | Qt::AlignVCenter, m_prompt);

    /* The field. */
    const QRectF field = fieldRect();
    Theme::vgrad(p, field, Theme::glass().lighter(140), Theme::glass().lighter(120), 8);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Theme::blue(), 1.4));
    p.drawRoundedRect(field.adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);

    const QString shown = m_password ? QString(m_text.size(), QChar(0x2022)) : m_text;
    const QFont fieldFont = Theme::font(14);
    const QFontMetrics ffm(fieldFont);
    p.setFont(fieldFont);
    p.setPen(Theme::ink());

    /* Scrolled so the caret is always on screen, which for a 63-character key is
     * the difference between usable and not. */
    const qreal inner = field.width() - 20;
    qreal caretX = ffm.horizontalAdvance(shown.left(m_caret));
    qreal offset = 0;
    if (caretX > inner)
        offset = caretX - inner;
    p.save();
    p.setClipRect(field.adjusted(8, 0, -8, 0));
    p.drawText(QRectF(field.x() + 10 - offset, field.y(), qMax(inner, caretX + 20), field.height()),
               Qt::AlignLeft | Qt::AlignVCenter, shown);
    p.setPen(QPen(Theme::blue(), 1.6));
    p.drawLine(QPointF(field.x() + 10 - offset + caretX, field.y() + 6),
               QPointF(field.x() + 10 - offset + caretX, field.bottom() - 6));
    p.restore();

    /* The caps. */
    const QFont capFont = Theme::font(15, true);
    const QFont wideFont = Theme::font(12, true);
    for (int r = 0; r < m_rows.size(); ++r) {
        for (int c = 0; c < m_rows[r].size(); ++c) {
            const Cap &cap = m_rows[r][c];
            const bool focused = (r == m_row && c == m_col);
            const bool down = (m_pressed == r * 100 + c);
            const bool accent = (cap.special == KeyAccept);

            QColor top = accent ? Theme::blue() : Theme::card();
            QColor bottom = accent ? Theme::blueLow() : Theme::cardLow();
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
