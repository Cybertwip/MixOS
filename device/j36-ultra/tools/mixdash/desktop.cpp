/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
#include "desktop.h"

#include <QFile>
#include <QResizeEvent>
#include <QTimer>

#include "theme.h"

namespace {
const char kWindowList[] = "/run/j36/xsession.windows";
}

DesktopPage::DesktopPage(QWidget *parent)
    : PageWidget(parent)
{
    m_grid = new CardGrid(this);
    m_grid->setPageTitle(tr("Desktop"));
    /* This is a live view, not a launcher arrangement.  Its order belongs to X. */
    m_grid->setRearrangeable(false);
    connect(m_grid, &CardGrid::activated, this, &DesktopPage::activate);
    connect(m_grid, &CardGrid::indexChanged, this, [this](int) {
        emit titleChanged();
    });

    m_timer = new QTimer(this);
    m_timer->setInterval(750);
    connect(m_timer, &QTimer::timeout, this, &DesktopPage::refresh);
}

QString DesktopPage::title() const
{
    if (m_windows.isEmpty())
        return tr("Desktop");
    const int at = m_grid->index();
    if (at >= 0 && at < m_windows.size())
        return tr("Desktop -- %1").arg(m_windows[at].title);
    return tr("Desktop -- %1 windows").arg(m_windows.size());
}

QVector<DesktopPage::Window> DesktopPage::readWindows() const
{
    QVector<Window> out;
    QFile f(QString::fromLatin1(kWindowList));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;

    const QByteArray blob = f.read(4096);
    for (const QByteArray &line : blob.split('\n')) {
        const int tab = line.indexOf('\t');
        if (tab <= 0)
            continue;
        bool ok = false;
        const qint64 pid = line.left(tab).trimmed().toLongLong(&ok);
        const QString name = QString::fromUtf8(line.mid(tab + 1)).trimmed();
        if (!ok || pid <= 0 || name.isEmpty())
            continue;
        Window w;
        w.pid = pid;
        w.title = name;
        out.append(w);
    }
    return out;
}

void DesktopPage::rebuild()
{
    QVector<AppEntry> cards;
    if (m_windows.isEmpty()) {
        AppEntry empty;
        empty.key = QStringLiteral("desktop:open");
        empty.title = tr("Open desktop");
        empty.glyph = GlyphDisplay;
        empty.accent = Theme::blue();
        cards.append(empty);
    } else {
        const QColor accents[] = {
            Theme::blue(), Theme::teal(), Theme::purple(), Theme::orange()
        };
        for (int i = 0; i < m_windows.size(); ++i) {
            AppEntry card;
            card.key = QStringLiteral("desktop:window:%1").arg(m_windows[i].pid);
            card.title = m_windows[i].title;
            card.glyph = GlyphDisplay;
            card.accent = accents[i % 4];
            cards.append(card);
        }
    }

    const int keep = m_grid->index();
    m_grid->setEntries(cards);
    m_grid->setIndex(qBound(0, keep, qMax(0, cards.size() - 1)));
    emit titleChanged();
}

void DesktopPage::refresh()
{
    const QVector<Window> next = readWindows();
    bool same = next.size() == m_windows.size();
    for (int i = 0; same && i < next.size(); ++i)
        same = next[i].pid == m_windows[i].pid && next[i].title == m_windows[i].title;
    if (same)
        return;
    m_windows = next;
    rebuild();
}

void DesktopPage::activate(int index)
{
    if (m_windows.isEmpty()) {
        emit windowRequested(0);
        return;
    }
    if (index >= 0 && index < m_windows.size())
        emit windowRequested(m_windows[index].pid);
}

bool DesktopPage::handleNav(int action)
{
    return m_grid->handleNav(action);
}

void DesktopPage::handleNavRelease(int action)
{
    m_grid->handleNavRelease(action);
}

void DesktopPage::onEnter()
{
    m_windows = readWindows();
    rebuild();
    m_timer->start();
}

void DesktopPage::onLeave()
{
    m_timer->stop();
    m_grid->onLeave();
}

void DesktopPage::resizeEvent(QResizeEvent *event)
{
    m_grid->setGeometry(rect());
    PageWidget::resizeEvent(event);
}
