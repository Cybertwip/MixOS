/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * terminal.h -- a real shell, on the panel, without leaving the dashboard.
 *
 * WHY NOT JUST SPAWN A GETTY.  Because this dashboard owns /dev/fb0 and puts the
 * console into KD_GRAPHICS to keep the kernel from printing over it.  A getty or
 * an agetty on tty1 would want KD_TEXT and the same pixels, and the two of them
 * would take turns until one of them lost.  Drawing the terminal into the same
 * framebuffer everything else is drawn into is the only version of this that can
 * coexist with the shell around it.
 *
 * WHY NOT QProcess.  QProcess gives a pipe, and a pipe is not a terminal: bash
 * turns off its prompt and its line editing when isatty(0) is false, `top' and
 * `nano' refuse to start at all, and nothing that matters on a bring-up console
 * behaves the way it does over ssh.  forkpty(3) gives a real pty pair, so the
 * child gets a controlling terminal, a window size, job control and signals from
 * ^C -- and in exchange this file has to interpret the escape sequences that come
 * back.  That interpreter is most of what is below.
 *
 * WHAT IS IMPLEMENTED is the subset that bash, ls --color, top, nano, dmesg and
 * apt actually emit: cursor motion, erase, insert and delete of lines and
 * characters, a scrolling region, SGR colour including the 256-colour form, and
 * the alternate screen.  Sixel, mouse reporting and double-width characters are
 * not here.  When something unrecognised arrives it is swallowed rather than
 * printed, because a stray `[?25l' on the screen is worse than a missing hidden
 * cursor.
 *
 * TYPING.  A USB keyboard goes through Joypad::key() and KeyMap; with no keyboard
 * plugged in the on-screen Keyboard is put up with the Menu button and types into
 * the same place.  The pad itself keeps working as navigation, so B always leaves.
 */
#ifndef MIXDASH_TERMINAL_H
#define MIXDASH_TERMINAL_H

#include <QString>
#include <QVector>

#include "widgets.h"

class QTimer;

class TerminalPage : public PageWidget
{
    Q_OBJECT

public:
    explicit TerminalPage(QWidget *parent = nullptr);
    ~TerminalPage() override;

    QString title() const override;
    bool handleNav(int action) override;
    void onEnter() override;
    void onLeave() override;
    bool wantsFullscreen() const override { return true; }
    bool wantsKeys() const override { return true; }
    void keyPressed(int code, bool pressed, int modifiers) override;
    void textEntered(const QString &text, bool accepted) override;

    /* Used by the Packages page: open the terminal already running a command,
     * which is how `apt install' gets to show its own progress instead of this
     * shell pretending to know what apt is doing. */
    void runCommand(const QString &command);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void drain();

private:
    /* One character cell.  Packed small on purpose: 80x24 is 1920 of these and
     * the whole grid is copied when the screen scrolls. */
    struct Cell {
        ushort ch = ' ';
        /* Palette indices, 0..255, or -1 for "the default". */
        short fg = -1;
        short bg = -1;
        uchar flags = 0;
    };

    enum CellFlag { FlagBold = 1, FlagUnderline = 2, FlagReverse = 4, FlagDim = 8 };

    /* Where the parser is in an escape sequence. */
    enum State { Ground = 0, Escape, Csi, Osc, Charset };

    bool startChild(const QString &command);
    void stopChild();
    void send(const QByteArray &bytes);
    void sendKey(int code, int modifiers);

    void resizeGrid(int cols, int rows);
    void syncWindowSize();

    void feed(const QByteArray &bytes);
    void putChar(uint ch);
    void execute(uchar c);
    void csiDispatch(uchar final);
    void sgr();

    void newline();
    void scrollUp(int top, int bottom, int count);
    void scrollDown(int top, int bottom, int count);
    void eraseInLine(int mode);
    void eraseInDisplay(int mode);
    void insertLines(int n);
    void deleteLines(int n);
    void insertChars(int n);
    void deleteChars(int n);
    void eraseChars(int n);
    void clampCursor();

    Cell *cellAt(int col, int row);
    QColor colourFor(short index, bool foreground, uchar flags) const;
    void recomputeMetrics();
    /* A line pushed off the top of the primary screen.  Kept because the pad is
     * the only input this device is guaranteed to have, and without scrollback the
     * D-pad's up and down would do nothing at all on a page full of text. */
    void pushScrollback(const Cell *line);

    int m_fd = -1;
    int m_pid = -1;
    QTimer *m_poll = nullptr;
    /* Set when the child exits, so the page can say so instead of looking hung. */
    bool m_dead = false;
    QString m_exitNote;

    /* The screen, row-major, m_cols wide.  A flat vector rather than a vector of
     * vectors: scrolling is then one memmove of a contiguous run. */
    QVector<Cell> m_grid;
    /* The alternate screen, swapped in by 1049h -- what nano and top run on. */
    QVector<Cell> m_alt;
    bool m_altActive = false;

    /* Lines that have scrolled off the top, oldest first, m_cols wide.  Dropped
     * on a resize rather than reflowed: reflowing wrapped lines correctly needs
     * to know where the wraps were, and this grid does not record that. */
    QVector<Cell> m_scrollback;
    int m_scrollbackLines = 0;
    /* How far back the view is, in lines.  Zero is live. */
    int m_view = 0;
    /* False when the measured font turned out not to be monospace, which turns
     * the fast per-run text drawing off in favour of one call per cell. */
    bool m_monospace = true;

    int m_cols = 80;
    int m_rows = 24;
    int m_cx = 0;
    int m_cy = 0;
    int m_savedCx = 0;
    int m_savedCy = 0;
    /* DECSTBM.  Inclusive, zero-based. */
    int m_top = 0;
    int m_bottom = 23;
    bool m_wrapPending = false;   /* DEC's "cursor is past the last column" state */
    bool m_cursorVisible = true;
    bool m_appCursorKeys = false; /* DECCKM: arrows send SS3 instead of CSI */

    /* Current pen. */
    short m_fg = -1;
    short m_bg = -1;
    uchar m_flags = 0;

    /* Parser. */
    int m_state = Ground;
    QVector<int> m_params;
    QByteArray m_intermediate;
    QByteArray m_oscBuffer;
    bool m_privateParam = false;
    /* A UTF-8 sequence can straddle two reads. */
    QByteArray m_utf8;

    /* Font metrics, measured once per resize. */
    int m_cellW = 8;
    int m_cellH = 14;
    int m_baseline = 11;

    QString m_pendingCommand;
    /* The title an OSC 0/2 asked for, shown in the status line at the foot. */
    QString m_childTitle;
};

#endif /* MIXDASH_TERMINAL_H */
