/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "terminal.h"

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "joypad.h"
#include "keyboard.h"
#include "theme.h"

namespace {

const int kMaxScrollback = 600;
/* Bytes taken off the pty per tick.  A command that produces output faster than
 * this -- `yes', a kernel build -- then falls behind rather than freezing the
 * event loop inside one drain(), which is the trade that keeps the pad alive. */
const int kDrainBudget = 48 * 1024;
const int kHintH = 20;

QString shellPath()
{
    /* The user's shell if the passwd entry is honest about it, then the usual
     * suspects, then sh, which POSIX says is there. */
    const QByteArray env = qgetenv("SHELL");
    if (!env.isEmpty() && QFileInfo(QString::fromLocal8Bit(env)).isExecutable())
        return QString::fromLocal8Bit(env);

    static const char *kCandidates[] = { "/bin/bash", "/usr/bin/bash", "/bin/sh" };
    for (size_t i = 0; i < sizeof(kCandidates) / sizeof(kCandidates[0]); ++i)
        if (QFileInfo(QString::fromLatin1(kCandidates[i])).isExecutable())
            return QString::fromLatin1(kCandidates[i]);
    return QStringLiteral("/bin/sh");
}

/*
 * The xterm 256-colour palette, computed rather than tabulated.  0..15 are the
 * ANSI colours, taken here from MVII's own accents so `ls --color' looks like the
 * rest of the shell instead of like a 1990s VGA console; 16..231 are the 6x6x6
 * cube; 232..255 are the grey ramp.
 */
QColor paletteColour(int index)
{
    static const QColor kBase[16] = {
        QColor( 32,  34,  44), QColor(255,  95,  86), QColor( 40, 200,  64), QColor(254, 188,  46),
        QColor( 10, 132, 255), QColor(148, 112, 219), QColor( 48, 176, 199), QColor(200, 205, 216),
        QColor( 90,  96, 112), QColor(255, 140, 130), QColor(110, 226, 128), QColor(255, 214, 110),
        QColor( 96, 172, 255), QColor(184, 156, 236), QColor(112, 210, 226), QColor(240, 244, 252)
    };

    if (index < 0)
        return QColor();
    if (index < 16)
        return kBase[index];
    if (index < 232) {
        const int n = index - 16;
        static const int kSteps[6] = { 0, 95, 135, 175, 215, 255 };
        return QColor(kSteps[(n / 36) % 6], kSteps[(n / 6) % 6], kSteps[n % 6]);
    }
    if (index < 256) {
        const int v = 8 + (index - 232) * 10;
        return QColor(v, v, v);
    }
    return QColor();
}

int paramOr(const QVector<int> &params, int index, int fallback)
{
    if (index >= params.size())
        return fallback;
    /* A CSI parameter that was written as an empty field means "default", and
     * the parser stores that as zero. */
    return params.at(index) == 0 ? fallback : params.at(index);
}

} /* namespace */

TerminalPage::TerminalPage(QWidget *parent)
    : PageWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    m_poll = new QTimer(this);
    m_poll->setInterval(25);
    connect(m_poll, &QTimer::timeout, this, &TerminalPage::drain);

    resizeGrid(m_cols, m_rows);
}

TerminalPage::~TerminalPage()
{
    stopChild();
}

QString TerminalPage::title() const
{
    if (!m_childTitle.isEmpty())
        return m_childTitle;
    return QStringLiteral("Terminal");
}

/* ── the child ───────────────────────────────────────────────────────────── */

bool TerminalPage::startChild(const QString &command)
{
    if (m_fd >= 0)
        return true;

    struct winsize ws;
    ::memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)m_cols;
    ws.ws_row = (unsigned short)m_rows;
    ws.ws_xpixel = (unsigned short)(m_cols * m_cellW);
    ws.ws_ypixel = (unsigned short)(m_rows * m_cellH);

    /*
     * Both of these are worked out BEFORE the fork.  Between fork and exec only
     * async-signal-safe calls are legal, and QString, QFileInfo and getenv all
     * allocate: if another thread held the malloc lock at the moment of the fork,
     * the child would deadlock inside it and this terminal would simply never
     * open, intermittently and unreproducibly.
     */
    const QByteArray shell = shellPath().toLocal8Bit();
    const QByteArray cmd = command.toLocal8Bit();

    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        m_dead = true;
        m_exitNote = QString("forkpty failed: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        update();
        return false;
    }

    if (pid == 0) {
        /*
         * The child.  Everything here has to be async-signal-safe in spirit --
         * this is after a fork in a Qt process, so no allocation and no Qt.
         *
         * Closing the inherited descriptors matters more than usual on this
         * device: mixdash holds /dev/fb0 and every /dev/input node open, and a
         * shell that inherits them is a shell whose children can scribble on the
         * panel this terminal is being drawn into.
         */
        for (int fd = 3; fd < 256; ++fd)
            ::close(fd);

        ::signal(SIGPIPE, SIG_DFL);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGALRM, SIG_DFL);   /* main.cpp's watchdog must not fire here */
        ::signal(SIGSEGV, SIG_DFL);
        ::signal(SIGABRT, SIG_DFL);

        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 1);
        /* Qt's own environment would follow the shell into anything it launches,
         * and a child that then tried linuxfb would fight for the framebuffer. */
        ::unsetenv("QT_QPA_PLATFORM");
        ::unsetenv("QT_QPA_FB_DISABLE_INPUT");
        ::unsetenv("QT_QPA_FONTDIR");
        ::unsetenv("LD_PRELOAD");

        const char *home = ::getenv("HOME");
        if (home && *home)
            (void)::chdir(home);
        else
            (void)::chdir("/root");

        if (cmd.isEmpty()) {
            /* A login shell, so /etc/profile and the user's rc files run and the
             * prompt looks like the one they get over ssh. */
            ::execl(shell.constData(), "-", (char *)nullptr);
            ::execl(shell.constData(), shell.constData(), "-l", (char *)nullptr);
        } else {
            ::execl(shell.constData(), shell.constData(), "-lc", cmd.constData(), (char *)nullptr);
        }
        ::_exit(127);
    }

    m_pid = pid;
    m_fd = master;
    m_dead = false;
    m_exitNote.clear();

    /* Non-blocking, because drain() reads until EAGAIN and a blocking read on an
     * idle shell would stop the dashboard dead. */
    const int flags = ::fcntl(m_fd, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
    ::fcntl(m_fd, F_SETFD, FD_CLOEXEC);

    m_poll->start();
    emit titleChanged();
    return true;
}

void TerminalPage::stopChild()
{
    if (m_poll)
        m_poll->stop();

    if (m_pid > 0) {
        /* SIGHUP is what a closing terminal sends, and it is what a shell expects:
         * it runs its exit trap and takes its job control children with it. */
        ::kill(-m_pid, SIGHUP);
        ::kill(m_pid, SIGHUP);

        /* Give it a moment, then insist.  Sleeping in 20 ms slices rather than one
         * 200 ms one so a shell that exits promptly does not hold the UI. */
        for (int i = 0; i < 10; ++i) {
            int status = 0;
            if (::waitpid(m_pid, &status, WNOHANG) == m_pid) {
                m_pid = -1;
                break;
            }
            ::usleep(20 * 1000);
        }
        if (m_pid > 0) {
            ::kill(-m_pid, SIGKILL);
            ::kill(m_pid, SIGKILL);
            int status = 0;
            ::waitpid(m_pid, &status, 0);
            m_pid = -1;
        }
    }

    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

void TerminalPage::onEnter()
{
    /*
     * The child is started here and NOT stopped in onLeave().  Backing out of the
     * terminal while `apt install' is running has to leave apt running -- and the
     * poll timer has to keep draining, because a pty whose master is not read
     * fills up in a few kilobytes and then blocks the writer.
     */
    if (m_fd < 0)
        startChild(m_pendingCommand);
    else if (!m_pendingCommand.isEmpty())
        send(m_pendingCommand.toLocal8Bit() + "\n");
    m_pendingCommand.clear();

    m_view = 0;
    update();
}

void TerminalPage::onLeave()
{
    /* Deliberately empty.  See onEnter(). */
}

void TerminalPage::runCommand(const QString &command)
{
    if (m_fd >= 0 && !m_dead) {
        m_view = 0;
        send(command.toLocal8Bit() + "\n");
    } else {
        m_pendingCommand = command;
    }
}

void TerminalPage::send(const QByteArray &bytes)
{
    if (m_fd < 0 || bytes.isEmpty())
        return;

    /* Typing scrolls back to the live screen, the way every terminal does -- the
     * alternative is a keystroke that seems to do nothing. */
    if (m_view != 0) {
        m_view = 0;
        update();
    }

    int written = 0;
    while (written < bytes.size()) {
        const ssize_t n = ::write(m_fd, bytes.constData() + written, bytes.size() - written);
        if (n > 0) {
            written += (int)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        /* EAGAIN means the child is not reading -- it is busy, or it is stopped.
         * Dropping the rest is better than blocking the dashboard for it. */
        break;
    }
}

void TerminalPage::drain()
{
    if (m_fd < 0)
        return;

    char buf[4096];
    int budget = kDrainBudget;
    bool got = false;

    while (budget > 0) {
        const ssize_t n = ::read(m_fd, buf, sizeof(buf));
        if (n > 0) {
            feed(QByteArray(buf, (int)n));
            budget -= (int)n;
            got = true;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;

        /* Zero, or EIO: the slave side is gone.  The child has exited or is about
         * to; reap it and stop. */
        int status = 0;
        if (m_pid > 0 && ::waitpid(m_pid, &status, WNOHANG) == m_pid) {
            if (WIFEXITED(status))
                m_exitNote = QString("shell exited %1").arg(WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                m_exitNote = QString("shell killed by signal %1").arg(WTERMSIG(status));
            m_pid = -1;
        } else if (m_exitNote.isEmpty()) {
            m_exitNote = QStringLiteral("shell exited");
        }
        m_dead = true;
        ::close(m_fd);
        m_fd = -1;
        m_poll->stop();
        got = true;
        emit titleChanged();
        break;
    }

    if (got && isVisible())
        update();
}

/* ── the grid ────────────────────────────────────────────────────────────── */

TerminalPage::Cell *TerminalPage::cellAt(int col, int row)
{
    if (col < 0 || col >= m_cols || row < 0 || row >= m_rows)
        return nullptr;
    return &m_grid[row * m_cols + col];
}

void TerminalPage::resizeGrid(int cols, int rows)
{
    cols = qMax(8, cols);
    rows = qMax(2, rows);
    if (cols == m_cols && rows == m_rows && m_grid.size() == cols * rows)
        return;

    QVector<Cell> fresh(cols * rows);
    /* Keep what fits, anchored top-left.  Anchoring to the bottom would be kinder
     * to a shell prompt, but it moves text the user is looking at, and this panel
     * only ever resizes once at startup anyway. */
    for (int r = 0; r < qMin(rows, m_rows); ++r)
        for (int c = 0; c < qMin(cols, m_cols); ++c)
            fresh[r * cols + c] = m_grid[r * m_cols + c];

    m_grid = fresh;
    if (m_altActive)
        m_alt = QVector<Cell>(cols * rows);
    else
        m_alt.clear();

    if (cols != m_cols) {
        m_scrollback.clear();
        m_scrollbackLines = 0;
        m_view = 0;
    }

    m_cols = cols;
    m_rows = rows;
    m_top = 0;
    m_bottom = rows - 1;
    clampCursor();
}

void TerminalPage::syncWindowSize()
{
    if (m_fd < 0)
        return;
    struct winsize ws;
    ::memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)m_cols;
    ws.ws_row = (unsigned short)m_rows;
    ws.ws_xpixel = (unsigned short)(m_cols * m_cellW);
    ws.ws_ypixel = (unsigned short)(m_rows * m_cellH);
    /* The child gets SIGWINCH from the kernel as a side effect, which is how a
     * full-screen program like nano learns to redraw. */
    ::ioctl(m_fd, TIOCSWINSZ, &ws);
}

void TerminalPage::clampCursor()
{
    m_cx = qBound(0, m_cx, m_cols - 1);
    m_cy = qBound(0, m_cy, m_rows - 1);
    m_top = qBound(0, m_top, m_rows - 1);
    m_bottom = qBound(m_top, m_bottom, m_rows - 1);
}

void TerminalPage::pushScrollback(const Cell *line)
{
    if (m_altActive)
        return;   /* The alternate screen has no history by definition. */

    for (int c = 0; c < m_cols; ++c)
        m_scrollback.append(line[c]);
    ++m_scrollbackLines;

    if (m_scrollbackLines > kMaxScrollback) {
        const int drop = m_scrollbackLines - kMaxScrollback;
        m_scrollback.remove(0, drop * m_cols);
        m_scrollbackLines -= drop;
    }

    /* Somebody looking at history stays where they are while new lines arrive
     * underneath -- until the history itself scrolls out from under them. */
    if (m_view > 0)
        m_view = qMin(m_view + 1, m_scrollbackLines);
}

void TerminalPage::scrollUp(int top, int bottom, int count)
{
    if (count <= 0 || top > bottom)
        return;
    count = qMin(count, bottom - top + 1);

    /* Only a full-height region loses its top line to history; a region set by
     * DECSTBM is a pane inside the screen and its top line is not history. */
    const bool keeps = (top == 0 && bottom == m_rows - 1);

    for (int i = 0; i < count; ++i)
        if (keeps)
            pushScrollback(&m_grid[(top + i) * m_cols]);

    for (int r = top; r <= bottom - count; ++r)
        ::memmove(&m_grid[r * m_cols], &m_grid[(r + count) * m_cols],
                  sizeof(Cell) * m_cols);

    Cell blank;
    blank.bg = m_bg;
    for (int r = bottom - count + 1; r <= bottom; ++r)
        for (int c = 0; c < m_cols; ++c)
            m_grid[r * m_cols + c] = blank;
}

void TerminalPage::scrollDown(int top, int bottom, int count)
{
    if (count <= 0 || top > bottom)
        return;
    count = qMin(count, bottom - top + 1);

    for (int r = bottom; r >= top + count; --r)
        ::memmove(&m_grid[r * m_cols], &m_grid[(r - count) * m_cols],
                  sizeof(Cell) * m_cols);

    Cell blank;
    blank.bg = m_bg;
    for (int r = top; r < top + count; ++r)
        for (int c = 0; c < m_cols; ++c)
            m_grid[r * m_cols + c] = blank;
}

void TerminalPage::newline()
{
    if (m_cy == m_bottom)
        scrollUp(m_top, m_bottom, 1);
    else if (m_cy < m_rows - 1)
        ++m_cy;
}

void TerminalPage::eraseInLine(int mode)
{
    Cell blank;
    blank.bg = m_bg;
    int from = 0;
    int to = m_cols - 1;
    if (mode == 0)
        from = m_cx;
    else if (mode == 1)
        to = m_cx;
    for (int c = from; c <= to && c < m_cols; ++c)
        m_grid[m_cy * m_cols + c] = blank;
}

void TerminalPage::eraseInDisplay(int mode)
{
    Cell blank;
    blank.bg = m_bg;

    if (mode == 2 || mode == 3) {
        /*
         * A full clear on the primary screen sends the visible rows to history
         * first.  `clear' is otherwise the one command that silently destroys
         * everything the user might have wanted to scroll back to.
         */
        if (!m_altActive) {
            for (int r = 0; r < m_rows; ++r) {
                bool blankRow = true;
                for (int c = 0; c < m_cols && blankRow; ++c)
                    blankRow = (m_grid[r * m_cols + c].ch == ' ');
                if (!blankRow)
                    pushScrollback(&m_grid[r * m_cols]);
            }
        }
        m_grid.fill(blank);
        return;
    }

    if (mode == 0) {
        for (int c = m_cx; c < m_cols; ++c)
            m_grid[m_cy * m_cols + c] = blank;
        for (int r = m_cy + 1; r < m_rows; ++r)
            for (int c = 0; c < m_cols; ++c)
                m_grid[r * m_cols + c] = blank;
    } else if (mode == 1) {
        for (int r = 0; r < m_cy; ++r)
            for (int c = 0; c < m_cols; ++c)
                m_grid[r * m_cols + c] = blank;
        for (int c = 0; c <= m_cx && c < m_cols; ++c)
            m_grid[m_cy * m_cols + c] = blank;
    }
}

void TerminalPage::insertLines(int n)
{
    if (m_cy < m_top || m_cy > m_bottom)
        return;
    scrollDown(m_cy, m_bottom, n);
}

void TerminalPage::deleteLines(int n)
{
    if (m_cy < m_top || m_cy > m_bottom)
        return;
    scrollUp(m_cy, m_bottom, n);
}

void TerminalPage::insertChars(int n)
{
    n = qBound(1, n, m_cols - m_cx);
    Cell *row = &m_grid[m_cy * m_cols];
    ::memmove(row + m_cx + n, row + m_cx, sizeof(Cell) * (m_cols - m_cx - n));
    Cell blank;
    blank.bg = m_bg;
    for (int c = m_cx; c < m_cx + n; ++c)
        row[c] = blank;
}

void TerminalPage::deleteChars(int n)
{
    n = qBound(1, n, m_cols - m_cx);
    Cell *row = &m_grid[m_cy * m_cols];
    ::memmove(row + m_cx, row + m_cx + n, sizeof(Cell) * (m_cols - m_cx - n));
    Cell blank;
    blank.bg = m_bg;
    for (int c = m_cols - n; c < m_cols; ++c)
        row[c] = blank;
}

void TerminalPage::eraseChars(int n)
{
    n = qBound(1, n, m_cols - m_cx);
    Cell blank;
    blank.bg = m_bg;
    for (int c = m_cx; c < m_cx + n; ++c)
        m_grid[m_cy * m_cols + c] = blank;
}

/* ── the parser ──────────────────────────────────────────────────────────── */

void TerminalPage::putChar(uint ch)
{
    if (m_wrapPending) {
        m_cx = 0;
        newline();
        m_wrapPending = false;
    }

    Cell *cell = cellAt(m_cx, m_cy);
    if (!cell)
        return;
    /* Anything outside the BMP is stored as the replacement character: Cell::ch is
     * a ushort, and a device that has no CJK font staged would draw a box for a
     * surrogate pair either way. */
    cell->ch = (ch > 0xFFFF) ? (ushort)0xFFFD : (ushort)ch;
    cell->fg = m_fg;
    cell->bg = m_bg;
    cell->flags = m_flags;

    if (m_cx + 1 >= m_cols)
        m_wrapPending = true;   /* DEC wrap: the cursor stays put until the next glyph */
    else
        ++m_cx;
}

void TerminalPage::execute(uchar c)
{
    switch (c) {
    case 0x07:   /* BEL -- there is no buzzer wired on this board. */
        break;
    case 0x08:   /* BS */
        if (m_wrapPending)
            m_wrapPending = false;
        else if (m_cx > 0)
            --m_cx;
        break;
    case 0x09: { /* HT */
        const int next = ((m_cx / 8) + 1) * 8;
        m_cx = qMin(next, m_cols - 1);
        m_wrapPending = false;
        break;
    }
    case 0x0A:   /* LF */
    case 0x0B:   /* VT */
    case 0x0C:   /* FF */
        newline();
        m_wrapPending = false;
        break;
    case 0x0D:   /* CR */
        m_cx = 0;
        m_wrapPending = false;
        break;
    default:
        break;
    }
}

void TerminalPage::sgr()
{
    if (m_params.isEmpty())
        m_params.append(0);

    for (int i = 0; i < m_params.size(); ++i) {
        const int p = m_params.at(i);
        switch (p) {
        case 0:  m_fg = -1; m_bg = -1; m_flags = 0; break;
        case 1:  m_flags |= FlagBold; break;
        case 2:  m_flags |= FlagDim; break;
        case 4:  m_flags |= FlagUnderline; break;
        case 7:  m_flags |= FlagReverse; break;
        case 21:
        case 22: m_flags &= ~(FlagBold | FlagDim); break;
        case 24: m_flags &= ~FlagUnderline; break;
        case 27: m_flags &= ~FlagReverse; break;
        case 39: m_fg = -1; break;
        case 49: m_bg = -1; break;
        case 38:
        case 48: {
            /*
             * 38;5;N is one of 256; 38;2;R;G;B is direct colour, which this grid
             * has no room to store -- it is folded onto the nearest cube entry,
             * which on a 640x480 panel nobody can tell from the real thing.
             */
            const bool fg = (p == 38);
            if (i + 1 < m_params.size() && m_params.at(i + 1) == 5) {
                if (i + 2 < m_params.size()) {
                    const short v = (short)qBound(0, m_params.at(i + 2), 255);
                    if (fg) m_fg = v; else m_bg = v;
                }
                i += 2;
            } else if (i + 1 < m_params.size() && m_params.at(i + 1) == 2) {
                if (i + 4 < m_params.size()) {
                    const int r = qBound(0, m_params.at(i + 2), 255);
                    const int g = qBound(0, m_params.at(i + 3), 255);
                    const int b = qBound(0, m_params.at(i + 4), 255);
                    const short v = (short)(16 + 36 * (r * 5 / 255) + 6 * (g * 5 / 255) + (b * 5 / 255));
                    if (fg) m_fg = v; else m_bg = v;
                }
                i += 4;
            }
            break;
        }
        default:
            if (p >= 30 && p <= 37)
                m_fg = (short)(p - 30);
            else if (p >= 40 && p <= 47)
                m_bg = (short)(p - 40);
            else if (p >= 90 && p <= 97)
                m_fg = (short)(p - 90 + 8);
            else if (p >= 100 && p <= 107)
                m_bg = (short)(p - 100 + 8);
            break;
        }
    }
}

void TerminalPage::csiDispatch(uchar final)
{
    switch (final) {
    case 'A': m_cy = qMax(m_top, m_cy - paramOr(m_params, 0, 1)); m_wrapPending = false; break;
    case 'B': m_cy = qMin(m_bottom, m_cy + paramOr(m_params, 0, 1)); m_wrapPending = false; break;
    case 'C': m_cx = qMin(m_cols - 1, m_cx + paramOr(m_params, 0, 1)); m_wrapPending = false; break;
    case 'D': m_cx = qMax(0, m_cx - paramOr(m_params, 0, 1)); m_wrapPending = false; break;
    case 'E': m_cx = 0; m_cy = qMin(m_bottom, m_cy + paramOr(m_params, 0, 1)); break;
    case 'F': m_cx = 0; m_cy = qMax(m_top, m_cy - paramOr(m_params, 0, 1)); break;
    case 'G':
    case '`': m_cx = qBound(0, paramOr(m_params, 0, 1) - 1, m_cols - 1); m_wrapPending = false; break;
    case 'd': m_cy = qBound(0, paramOr(m_params, 0, 1) - 1, m_rows - 1); break;
    case 'H':
    case 'f':
        m_cy = qBound(0, paramOr(m_params, 0, 1) - 1, m_rows - 1);
        m_cx = qBound(0, paramOr(m_params, 1, 1) - 1, m_cols - 1);
        m_wrapPending = false;
        break;
    case 'J': eraseInDisplay(m_params.isEmpty() ? 0 : m_params.at(0)); break;
    case 'K': eraseInLine(m_params.isEmpty() ? 0 : m_params.at(0)); break;
    case 'L': insertLines(paramOr(m_params, 0, 1)); break;
    case 'M': deleteLines(paramOr(m_params, 0, 1)); break;
    case 'P': deleteChars(paramOr(m_params, 0, 1)); break;
    case '@': insertChars(paramOr(m_params, 0, 1)); break;
    case 'X': eraseChars(paramOr(m_params, 0, 1)); break;
    case 'S': scrollUp(m_top, m_bottom, paramOr(m_params, 0, 1)); break;
    case 'T': scrollDown(m_top, m_bottom, paramOr(m_params, 0, 1)); break;
    case 'm': sgr(); break;
    case 'r':
        m_top = qBound(0, paramOr(m_params, 0, 1) - 1, m_rows - 1);
        m_bottom = qBound(m_top, paramOr(m_params, 1, m_rows) - 1, m_rows - 1);
        m_cx = 0;
        m_cy = m_top;
        break;
    case 's': m_savedCx = m_cx; m_savedCy = m_cy; break;
    case 'u': m_cx = m_savedCx; m_cy = m_savedCy; clampCursor(); break;
    case 'n':
        /* Device status report.  `6n' is the cursor position, and enough shells
         * ask for it at startup that not answering leaves them waiting. */
        if (!m_params.isEmpty() && m_params.at(0) == 6)
            send(QString("\033[%1;%2R").arg(m_cy + 1).arg(m_cx + 1).toLatin1());
        else if (!m_params.isEmpty() && m_params.at(0) == 5)
            send(QByteArray("\033[0n"));
        break;
    case 'c':
        /* Primary device attributes: "a VT102 with colour", which is close enough
         * to be believed and modest enough not to be asked for sixel. */
        send(QByteArray("\033[?6c"));
        break;
    case 'h':
    case 'l': {
        const bool set = (final == 'h');
        for (int i = 0; i < m_params.size(); ++i) {
            const int p = m_params.at(i);
            if (!m_privateParam)
                continue;
            if (p == 25) {
                m_cursorVisible = set;
            } else if (p == 1) {
                m_appCursorKeys = set;
            } else if (p == 1049 || p == 47 || p == 1047) {
                if (set && !m_altActive) {
                    m_alt = m_grid;
                    m_grid.fill(Cell());
                    m_altActive = true;
                    m_savedCx = m_cx;
                    m_savedCy = m_cy;
                    m_cx = 0;
                    m_cy = 0;
                    m_view = 0;
                } else if (!set && m_altActive) {
                    m_grid = m_alt;
                    m_alt.clear();
                    m_altActive = false;
                    m_cx = m_savedCx;
                    m_cy = m_savedCy;
                    clampCursor();
                }
            }
        }
        break;
    }
    default:
        /* Unrecognised: swallowed.  Printing the bytes would corrupt the screen
         * far more visibly than the missing feature. */
        break;
    }
    clampCursor();
}

void TerminalPage::feed(const QByteArray &bytes)
{
    for (int i = 0; i < bytes.size(); ++i) {
        const uchar c = (uchar)bytes.at(i);

        switch (m_state) {
        case Ground:
            if (c == 0x1B) {
                m_state = Escape;
                m_params.clear();
                m_intermediate.clear();
                m_privateParam = false;
                break;
            }
            if (c < 0x20 || c == 0x7F) {
                m_utf8.clear();
                execute(c);
                break;
            }
            if (c < 0x80) {
                m_utf8.clear();
                putChar(c);
                break;
            }
            /*
             * UTF-8.  Accumulated rather than decoded byte by byte because a read
             * can split a multi-byte character in half, and this is the only place
             * that can hold the halves together.
             */
            m_utf8.append((char)c);
            if (m_utf8.size() >= 4 || (c & 0xC0) != 0x80) {
                /* Either a full sequence or a new lead byte after a broken one. */
                if ((c & 0xC0) == 0xC0 && m_utf8.size() > 1) {
                    /* A lead byte arrived while a sequence was open: the old one
                     * was truncated.  Drop it and start over with this byte. */
                    m_utf8 = QByteArray(1, (char)c);
                }
            }
            {
                /* How many bytes the lead byte promised. */
                const uchar lead = (uchar)m_utf8.at(0);
                int want = 1;
                if ((lead & 0xE0) == 0xC0) want = 2;
                else if ((lead & 0xF0) == 0xE0) want = 3;
                else if ((lead & 0xF8) == 0xF0) want = 4;
                else want = 0;   /* a stray continuation byte */

                if (want == 0) {
                    m_utf8.clear();
                } else if (m_utf8.size() >= want) {
                    const QString s = QString::fromUtf8(m_utf8);
                    for (int k = 0; k < s.size(); ++k)
                        putChar(s.at(k).unicode());
                    m_utf8.clear();
                }
            }
            break;

        case Escape:
            if (c == '[') {
                m_state = Csi;
                m_params.clear();
                m_intermediate.clear();
                m_privateParam = false;
            } else if (c == ']') {
                m_state = Osc;
                m_oscBuffer.clear();
            } else if (c == '(' || c == ')' || c == '*' || c == '+') {
                m_state = Charset;   /* the next byte names a character set; ignored */
            } else if (c == 'M') {
                /* Reverse index. */
                if (m_cy == m_top)
                    scrollDown(m_top, m_bottom, 1);
                else if (m_cy > 0)
                    --m_cy;
                m_state = Ground;
            } else if (c == 'D') {
                newline();
                m_state = Ground;
            } else if (c == 'E') {
                m_cx = 0;
                newline();
                m_state = Ground;
            } else if (c == '7') {
                m_savedCx = m_cx;
                m_savedCy = m_cy;
                m_state = Ground;
            } else if (c == '8') {
                m_cx = m_savedCx;
                m_cy = m_savedCy;
                clampCursor();
                m_state = Ground;
            } else if (c == 'c') {
                /* Full reset. */
                m_grid.fill(Cell());
                m_cx = m_cy = 0;
                m_fg = m_bg = -1;
                m_flags = 0;
                m_top = 0;
                m_bottom = m_rows - 1;
                m_state = Ground;
            } else {
                m_state = Ground;
            }
            break;

        case Charset:
            m_state = Ground;
            break;

        case Csi:
            if (c >= '0' && c <= '9') {
                if (m_params.isEmpty())
                    m_params.append(0);
                m_params.last() = qMin(m_params.last() * 10 + (c - '0'), 65535);
            } else if (c == ';') {
                m_params.append(0);
            } else if (c == '?' || c == '>' || c == '!' || c == '<') {
                m_privateParam = true;
            } else if (c >= 0x20 && c <= 0x2F) {
                m_intermediate.append((char)c);
            } else if (c >= 0x40 && c <= 0x7E) {
                csiDispatch(c);
                m_state = Ground;
            } else if (c < 0x20) {
                execute(c);
            } else {
                m_state = Ground;
            }
            break;

        case Osc:
            /* OSC ends at BEL or at ST (ESC \).  The only one acted on is the
             * window title, which the status line at the foot shows. */
            if (c == 0x07 || c == 0x9C) {
                const int semi = m_oscBuffer.indexOf(';');
                const int kind = m_oscBuffer.left(qMax(semi, 0)).toInt();
                if (semi >= 0 && (kind == 0 || kind == 2)) {
                    m_childTitle = QString::fromUtf8(m_oscBuffer.mid(semi + 1)).left(48);
                    emit titleChanged();
                }
                m_state = Ground;
            } else if (c == 0x1B) {
                /* Probably ST; the '\' that follows is eaten by the Escape state
                 * falling through to its default. */
                const int semi = m_oscBuffer.indexOf(';');
                const int kind = m_oscBuffer.left(qMax(semi, 0)).toInt();
                if (semi >= 0 && (kind == 0 || kind == 2)) {
                    m_childTitle = QString::fromUtf8(m_oscBuffer.mid(semi + 1)).left(48);
                    emit titleChanged();
                }
                m_state = Escape;
            } else if (m_oscBuffer.size() < 512) {
                m_oscBuffer.append((char)c);
            }
            break;

        default:
            m_state = Ground;
            break;
        }
    }
}

/* ── typing ──────────────────────────────────────────────────────────────── */

void TerminalPage::sendKey(int code, int modifiers)
{
    const bool ctrl = (modifiers & Joypad::ModCtrl) != 0;
    const bool alt = (modifiers & Joypad::ModAlt) != 0;

    QByteArray out;

    /* The keys that are a sequence rather than a character. */
    switch (code) {
    case KEY_ENTER:
    case KEY_KPENTER:
        out = "\r";
        break;
    case KEY_BACKSPACE:
        /* DEL, not BS.  It is what Debian's terminfo for xterm says, and sending
         * BS is why backspace deletes forwards in half the world's terminals. */
        out = "\177";
        break;
    case KEY_TAB:
        out = (modifiers & Joypad::ModShift) ? QByteArray("\033[Z") : QByteArray("\t");
        break;
    case KEY_ESC:
        out = "\033";
        break;
    case KEY_UP:    out = m_appCursorKeys ? "\033OA" : "\033[A"; break;
    case KEY_DOWN:  out = m_appCursorKeys ? "\033OB" : "\033[B"; break;
    case KEY_RIGHT: out = m_appCursorKeys ? "\033OC" : "\033[C"; break;
    case KEY_LEFT:  out = m_appCursorKeys ? "\033OD" : "\033[D"; break;
    case KEY_HOME:  out = "\033[H"; break;
    case KEY_END:   out = "\033[F"; break;
    case KEY_PAGEUP:   out = "\033[5~"; break;
    case KEY_PAGEDOWN: out = "\033[6~"; break;
    case KEY_INSERT:   out = "\033[2~"; break;
    case KEY_DELETE:   out = "\033[3~"; break;
    case KEY_F1:  out = "\033OP"; break;
    case KEY_F2:  out = "\033OQ"; break;
    case KEY_F3:  out = "\033OR"; break;
    case KEY_F4:  out = "\033OS"; break;
    case KEY_F5:  out = "\033[15~"; break;
    case KEY_F6:  out = "\033[17~"; break;
    case KEY_F7:  out = "\033[18~"; break;
    case KEY_F8:  out = "\033[19~"; break;
    case KEY_F9:  out = "\033[20~"; break;
    case KEY_F10: out = "\033[21~"; break;
    case KEY_F11: out = "\033[23~"; break;
    case KEY_F12: out = "\033[24~"; break;
    default:
        break;
    }

    if (out.isEmpty()) {
        /* Control characters are computed from the UNSHIFTED character: ^C is
         * ctrl and the C key, whether or not shift happens to be down. */
        const QString plain = KeyMap::character(code, ctrl ? 0 : modifiers);
        if (plain.isEmpty())
            return;

        if (ctrl) {
            const QChar ch = plain.at(0).toLower();
            const ushort u = ch.unicode();
            if (u >= 'a' && u <= 'z')
                out.append((char)(u - 'a' + 1));
            else if (u == '[')  out.append((char)0x1B);
            else if (u == '\\') out.append((char)0x1C);
            else if (u == ']')  out.append((char)0x1D);
            else if (u == '^')  out.append((char)0x1E);
            else if (u == '_' || u == '/') out.append((char)0x1F);
            else if (u == ' ' || u == '@') out.append('\0');
            else out = plain.toUtf8();
        } else {
            out = plain.toUtf8();
        }
    }

    if (alt)
        out.prepend('\033');   /* meta-sends-escape, which is what bash expects */

    send(out);
}

void TerminalPage::keyPressed(int code, bool pressed, int modifiers)
{
    if (!pressed)
        return;
    if (m_dead) {
        /* Any key restarts the shell.  A dead terminal that cannot be revived
         * without leaving the page and coming back is a bug people report. */
        m_dead = false;
        m_exitNote.clear();
        m_grid.fill(Cell());
        m_cx = m_cy = 0;
        startChild(QString());
        update();
        return;
    }
    sendKey(code, modifiers);
}

void TerminalPage::textEntered(const QString &text, bool accepted)
{
    if (!accepted || text.isEmpty())
        return;
    if (m_dead) {
        m_dead = false;
        m_exitNote.clear();
        startChild(QString());
    }
    send(text.toUtf8() + "\r");
}

bool TerminalPage::handleNav(int action)
{
    switch (action) {
    case Joypad::NavUp:
        if (m_altActive) {
            /* nano and top want the arrow key, not our scrollback. */
            send(m_appCursorKeys ? "\033OA" : "\033[A");
        } else if (m_view < m_scrollbackLines) {
            ++m_view;
            update();
        }
        return true;
    case Joypad::NavDown:
        if (m_altActive) {
            send(m_appCursorKeys ? "\033OB" : "\033[B");
        } else if (m_view > 0) {
            --m_view;
            update();
        }
        return true;
    case Joypad::NavLeft:
        send(m_appCursorKeys ? "\033OD" : "\033[D");
        return true;
    case Joypad::NavRight:
        send(m_appCursorKeys ? "\033OC" : "\033[C");
        return true;
    case Joypad::NavOk:
        if (m_dead) {
            m_dead = false;
            m_exitNote.clear();
            m_grid.fill(Cell());
            m_cx = m_cy = 0;
            startChild(QString());
            update();
        } else {
            send(QByteArray("\r"));
        }
        return true;
    case Joypad::NavMenu:
        /* The on-screen keyboard, for the case this device is normally in: no USB
         * keyboard attached and a command to type anyway. */
        emit textRequested(QStringLiteral("Command"), QString(), false);
        return true;
    case Joypad::NavBack:
        return false;   /* Leaves the page.  The shell keeps running. */
    default:
        return false;
    }
}

/* ── painting ────────────────────────────────────────────────────────────── */

void TerminalPage::recomputeMetrics()
{
    QFont f = Theme::font(13);
    f.setStyleHint(QFont::Monospace, QFont::PreferMatch);
    f.setFamily(QStringLiteral("monospace"));
    f.setFixedPitch(true);

    /* DejaVu Sans Mono if build-in-vm.sh staged it, and whatever the fontconfig
     * fallback is if it did not.  Named explicitly because Qt's "monospace" alias
     * resolves through fontconfig, and this image has a very short font list. */
    const QStringList preferred = QStringList()
                                  << "DejaVu Sans Mono" << "Liberation Mono"
                                  << "Noto Sans Mono" << "monospace";
    for (const QString &family : preferred) {
        f.setFamily(family);
        QFontInfo info(f);
        if (info.family().compare(family, Qt::CaseInsensitive) == 0)
            break;
    }

    QFontMetrics fm(f);
    m_cellW = qMax(4, fm.horizontalAdvance(QLatin1Char('M')));
    m_cellH = qMax(6, fm.height());
    m_baseline = fm.ascent();

    /* Whether a run of characters can be drawn in one call, which is worth about
     * a factor of ten on a Cortex-A7.  If the resolved face is proportional after
     * all, ten M's are not ten cells wide and the runs would drift. */
    m_monospace = (fm.horizontalAdvance(QStringLiteral("MMMMMMMMMM")) == 10 * m_cellW)
                  && (fm.horizontalAdvance(QStringLiteral("iiiiiiiiii")) == 10 * m_cellW);

    setFont(f);
}

void TerminalPage::resizeEvent(QResizeEvent *event)
{
    recomputeMetrics();

    const int usableW = width() - 2 * 6;
    const int usableH = height() - kHintH - 2 * 4;
    resizeGrid(qMax(20, usableW / m_cellW), qMax(4, usableH / m_cellH));
    syncWindowSize();

    QWidget::resizeEvent(event);
}

QColor TerminalPage::colourFor(short index, bool foreground, uchar flags) const
{
    QColor c;
    if (index >= 0)
        c = paletteColour(index);
    else
        c = foreground ? QColor(216, 220, 232) : QColor(18, 19, 26);

    if (foreground && (flags & FlagBold) && index >= 0 && index < 8)
        c = paletteColour(index + 8);
    if (foreground && (flags & FlagDim))
        c = c.darker(160);
    return c;
}

void TerminalPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);

    const QColor back(18, 19, 26);
    p.fillRect(rect(), back);

    const int originX = 6;
    const int originY = 4;

    p.setFont(font());

    for (int r = 0; r < m_rows; ++r) {
        /*
         * Which line this row shows.  With m_view lines of scrollback in front of
         * it, the top rows come out of history and the rest out of the live grid.
         */
        const int histIndex = m_scrollbackLines - m_view + r;
        const Cell *line = nullptr;
        if (m_view > 0 && histIndex < m_scrollbackLines) {
            if (histIndex >= 0)
                line = &m_scrollback[histIndex * m_cols];
        } else {
            const int gridRow = r - m_view;
            if (gridRow >= 0 && gridRow < m_rows)
                line = &m_grid[gridRow * m_cols];
        }
        if (!line)
            continue;

        const int y = originY + r * m_cellH;

        int c = 0;
        while (c < m_cols) {
            /* One run of identical attributes, drawn as one rectangle and one
             * string.  Trailing blanks in the default background are skipped
             * entirely, which is most of a terminal screen. */
            const short fg = line[c].fg;
            const short bg = line[c].bg;
            const uchar fl = line[c].flags;

            int end = c;
            while (end < m_cols && line[end].fg == fg && line[end].bg == bg
                   && line[end].flags == fl)
                ++end;

            QString text;
            text.reserve(end - c);
            for (int k = c; k < end; ++k)
                text.append(QChar(line[k].ch));

            QColor ink = colourFor(fg, true, fl);
            QColor paper = colourFor(bg, false, fl);
            if (fl & FlagReverse) {
                const QColor t = ink;
                ink = paper;
                paper = t;
            }

            const QRect runRect(originX + c * m_cellW, y, (end - c) * m_cellW, m_cellH);
            if (bg >= 0 || (fl & FlagReverse))
                p.fillRect(runRect, paper);

            if (!text.trimmed().isEmpty()) {
                p.setPen(ink);
                if (m_monospace) {
                    p.drawText(runRect.x(), y + m_baseline, text);
                } else {
                    for (int k = 0; k < text.size(); ++k)
                        p.drawText(originX + (c + k) * m_cellW, y + m_baseline,
                                   QString(text.at(k)));
                }
                if (fl & FlagUnderline) {
                    p.setPen(QPen(ink, 1));
                    p.drawLine(runRect.left(), y + m_baseline + 2,
                               runRect.right(), y + m_baseline + 2);
                }
            }

            c = end;
        }
    }

    /* The cursor: a filled block with the cell's glyph knocked out of it, which is
     * what a text console looks like and is legible at 13 px in a way an outline
     * is not.  Hidden while scrolled back -- it is not where the user is looking. */
    if (m_cursorVisible && m_view == 0 && !m_dead) {
        const QRect cur(originX + m_cx * m_cellW, originY + m_cy * m_cellH, m_cellW, m_cellH);
        p.fillRect(cur, QColor(120, 200, 255, 210));
        const Cell *cell = &m_grid[m_cy * m_cols + m_cx];
        if (cell->ch != ' ') {
            p.setPen(QColor(12, 14, 20));
            p.drawText(cur.x(), originY + m_cy * m_cellH + m_baseline, QString(QChar(cell->ch)));
        }
    }

    /* The foot: what the buttons do, and what went wrong if anything did. */
    const QRect hint(0, height() - kHintH, width(), kHintH);
    p.fillRect(hint, QColor(28, 30, 40));
    p.setPen(QColor(60, 64, 78));
    p.drawLine(hint.topLeft(), hint.topRight());

    QString foot;
    if (m_dead)
        foot = m_exitNote + "  --  A or any key restarts it";
    else if (m_view > 0)
        foot = QString("scrolled back %1 lines  --  Down returns").arg(m_view);
    else
        foot = QString("%1x%2  --  B leaves, Menu types, A is Enter")
                   .arg(m_cols).arg(m_rows);

    p.setFont(Theme::font(11));
    p.setPen(m_dead ? Theme::orange() : Theme::ink3());
    p.drawText(hint.adjusted(8, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, foot);

    if (!m_childTitle.isEmpty()) {
        p.setPen(Theme::ink3());
        p.drawText(hint.adjusted(8, 0, -8, 0), Qt::AlignRight | Qt::AlignVCenter, m_childTitle);
    }
}
