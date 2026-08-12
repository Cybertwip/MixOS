/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * keyboard.h -- typing on a device with eleven buttons.
 *
 * A Wi-Fi passphrase is the reason this exists.  Every other input on this shell
 * is a choice between things already on the screen; a WPA key is 8 to 63
 * characters the device cannot know in advance, and without a way to enter one
 * the Wi-Fi page can only ever join open networks.  The package search field and
 * the Terminal's own typing then came for free.
 *
 * THREE WAYS IN, and they are all live at once because on a handheld you cannot
 * know which one the user has to hand:
 *   - the D-pad walks the key grid and A presses,
 *   - the pointer clicks a key,
 *   - a real USB keyboard types straight through, which is what bring-up
 *     actually uses.
 *
 * It is an overlay rather than a page: the thing being typed into stays on the
 * screen above it, which for a passphrase means the network name is still there
 * to check against.
 */
#ifndef MIXDASH_KEYBOARD_H
#define MIXDASH_KEYBOARD_H

#include <QString>
#include <QVector>
#include <QWidget>

/*
 * Shared with terminal.cpp: the character a US layout produces for an evdev key
 * code.  Empty for keys that produce no character (arrows, function keys, the
 * modifiers themselves), which is how a caller tells the two apart.
 */
namespace KeyMap {
QString character(int code, int modifiers);
}

class Keyboard : public QWidget
{
    Q_OBJECT

public:
    explicit Keyboard(QWidget *parent);

    void open(const QString &prompt, const QString &initial, bool password);
    void dismiss(bool accepted);
    bool isOpen() const { return isVisible(); }

    /* True when the action was consumed.  The shell asks the keyboard first while
     * it is up, so Back closes the keyboard rather than the page under it. */
    bool handleNav(int action);
    /* A real keyboard's evdev codes, routed here by the shell while this is up. */
    void keyPressed(int code, bool pressed, int modifiers);

    QString text() const { return m_text; }

signals:
    void finished(const QString &text, bool accepted);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    /* Negative codes are the keys that do something rather than type something. */
    enum Special {
        KeyChar = 0,
        KeyShift = -1,
        KeyBackspace = -2,
        KeySpace = -3,
        KeySymbols = -4,
        KeyAccept = -5,
        KeyCancel = -6,
        KeyLeft = -7,
        KeyRight = -8
    };

    struct Cap {
        QString label;
        QString value;   /* what it types; empty for a Special */
        int special = KeyChar;
        qreal span = 1.0;
        QRectF rect;
    };

    void buildLayout();
    void relayout();
    void pressCap(const Cap &cap);
    void insert(const QString &s);
    void backspace();
    int capAt(const QPoint &p) const;
    QRectF fieldRect() const;

    /* Rows of caps, rebuilt whenever the layer changes. */
    QVector<QVector<Cap> > m_rows;
    int m_row = 0;
    int m_col = 0;
    int m_pressed = -1;      /* flat index, row * 100 + col */

    int m_layer = 0;         /* 0 lower, 1 upper, 2 symbols */
    bool m_shiftLatched = false;

    QString m_prompt;
    QString m_text;
    int m_caret = 0;
    bool m_password = false;
};

#endif /* MIXDASH_KEYBOARD_H */
