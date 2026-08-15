/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * files.cpp -- the four panes described in files.h.
 */
#include "files.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFontMetrics>
#include <QItemSelectionModel>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

#include "joypad.h"
#include "theme.h"
#include "trace.h"
#include "disks.h"

namespace {

const int kPad = 8;
const int kGap = 6;
const int kFieldH = 24;
const int kPlacesW = 116;
const int kInfoW = 138;
const int kPlaceRowH = 22;

/* Bytes, in the four units anybody reading a file listing wants and no more.  One
 * decimal above a kilobyte, none below it -- "1.0 B" is noise. */
QString humanSize(qint64 bytes)
{
    if (bytes < 1024)
        return FilesPage::tr("%1 B").arg(bytes);
    double v = bytes / 1024.0;
    if (v < 1024.0)
        return FilesPage::tr("%1 kB").arg(v, 0, 'f', 1);
    v /= 1024.0;
    if (v < 1024.0)
        return FilesPage::tr("%1 MB").arg(v, 0, 'f', 1);
    v /= 1024.0;
    return FilesPage::tr("%1 GB").arg(v, 0, 'f', 1);
}

} /* namespace */

/*
 * ANNOUNCED STEP BY STEP, and here more than anywhere else in this program.  This
 * page is the only one that hands a Qt class a path off the SD card and lets it go
 * and look: QFileSystemModel does its work on a thread of its own
 * (QFileInfoGatherer), so a throw inside it aborts the process while the main
 * thread is still building widgets, and the last thing printed is the only evidence
 * of where that was.  Every other page is arithmetic and QPainter calls.
 */
FilesPage::FilesPage(QWidget *parent)
    : PageWidget(parent)
{
    /*
     * /run/j36/card first: that is the card's own mount, put there by the
     * initramfs, and the only directory here whose contents the operator chose.
     * /home/virtua is the login user's home -- a plain directory since the data
     * partition went -- and /home/ark is what a card written before the rename
     * calls it.  / is the answer for a rootfs with none of the three.
     */
    Trace::step("FilesPage: choosing a base directory");
    m_base = QFileInfo::exists("/run/j36/card") ? QString("/run/j36/card")
           : QFileInfo::exists("/home/virtua")  ? QString("/home/virtua")
           : QFileInfo::exists("/home/ark")     ? QString("/home/ark")
           : QFileInfo::exists("/root")         ? QString("/root")
                                                : QString("/");

    Trace::step("FilesPage: QFileSystemModel");
    m_model = new QFileSystemModel(this);
    m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);

    /* Starts the gatherer thread walking the card. */
    Trace::step("FilesPage: setRootPath -- starts the gatherer thread on the card");
    m_model->setRootPath(m_base);

    Trace::step("FilesPage: QListView");
    m_view = new QListView(this);
    m_view->setModel(m_model);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setUniformItemSizes(true);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    m_view->viewport()->setAutoFillBackground(false);

    /*
     * :!active is not optional.  The dashboard drives this view from evdev rather
     * than through Qt's focus, so the window may never be "active" as Qt counts it,
     * and without that selector the selection is drawn in the inactive palette --
     * grey on grey, which reads as nothing being selected at all.
     */
    Trace::step("FilesPage: stylesheet");
    m_view->setStyleSheet(
        "QListView { background: transparent; border: none; color: #E8EAF2;"
        "            font-size: 13px; outline: none; }"
        "QListView::item { height: 24px; padding-left: 6px; border-radius: 6px; }"
        "QListView::item:selected, QListView::item:selected:!active {"
        "            background: #0A84FF; color: #FFFFFF; }"
        "QScrollBar:vertical { background: transparent; width: 6px; margin: 0; }"
        "QScrollBar::handle:vertical { background: #5C606C; border-radius: 3px;"
        "            min-height: 24px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "            background: transparent; }");

    /*
     * QFileSystemModel populates on a worker thread, so the row count is zero for
     * a moment after every setRootPath and selecting row 0 straight away selects
     * nothing.  This puts the cursor on the first entry as soon as the directory
     * actually arrives.
     */
    connect(m_model, &QFileSystemModel::directoryLoaded, this, [this](const QString &path) {
        if (QDir::cleanPath(path) != m_root)
            return;
        if (!m_view->currentIndex().isValid()) {
            const QModelIndex root = m_model->index(m_root);
            if (m_model->rowCount(root) > 0)
                m_view->setCurrentIndex(m_model->index(0, 0, root));
        }
        update();
    });

    /* The info panel is about whatever is highlighted, so it repaints whenever
     * that changes -- including when the change came from the pointer, which is
     * the one path this page does not drive itself. */
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &, const QModelIndex &) { update(); });
    connect(m_view, &QAbstractItemView::clicked, this, [this](const QModelIndex &) {
        setPane(PaneList);
    });
    connect(m_view, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex &) {
        enter();
    });

    /*
     * A stick plugged in while this page is open adds a row to the places panel
     * without anybody asking, which is the entire reason the panel is on the glass
     * rather than behind a button.  A stick pulled out while it is being BROWSED is
     * the other half: the page is closed rather than left pointing at a mount point
     * that is not there any more.
     */
    connect(&Disks::instance(), &Disks::changed, this, [this]() {
        rebuildPlaces();
        if (!m_scope.isEmpty() && !QFileInfo::exists(m_scope)) {
            emit toastRequested(tr("%1 was removed").arg(m_scopeName), 2600);
            emit closeRequested();
            return;
        }
        update();
    });

    Trace::step("FilesPage: setRoot -- reads the directory");
    rebuildPlaces();
    setRoot(m_base);
}

/* ── geometry ────────────────────────────────────────────────────────────── */

QRectF FilesPage::bodyRect() const
{
    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    /* 35 is paintSheet's title bar plus its hairline; the two have to agree, and
     * paintSheet is the one that decides. */
    return QRectF(card.x(), card.y() + 35, card.width(), card.height() - 35);
}

QRectF FilesPage::addressRect() const
{
    const QRectF b = bodyRect();
    return QRectF(b.x() + kPad, b.y() + kPad, b.width() - 2 * kPad, kFieldH);
}

QRectF FilesPage::placesRect() const
{
    const QRectF b = bodyRect();
    const qreal top = addressRect().bottom() + kGap;
    return QRectF(b.x() + kPad, top, kPlacesW, b.bottom() - kPad - top);
}

QRectF FilesPage::infoRect() const
{
    const QRectF b = bodyRect();
    const qreal top = addressRect().bottom() + kGap;
    return QRectF(b.right() - kPad - kInfoW, top, kInfoW, b.bottom() - kPad - top);
}

QRectF FilesPage::searchRect() const
{
    const qreal x = placesRect().right() + kGap;
    const qreal w = infoRect().x() - kGap - x;
    return QRectF(x, placesRect().y(), qMax(qreal(40), w), kFieldH);
}

QRectF FilesPage::listRect() const
{
    const QRectF s = searchRect();
    const qreal top = s.bottom() + kGap;
    return QRectF(s.x(), top, s.width(), placesRect().bottom() - top);
}

QRectF FilesPage::placeRowRect(int i) const
{
    const QRectF r = placesRect();
    return QRectF(r.x() + 4, r.y() + 6 + i * kPlaceRowH, r.width() - 8, kPlaceRowH);
}

void FilesPage::resizeEvent(QResizeEvent *event)
{
    /* Four pixels in from the panel it sits in, so the selection highlight does not
     * touch the border that frames it. */
    m_view->setGeometry(listRect().adjusted(4, 4, -4, -4).toRect());
    PageWidget::resizeEvent(event);
}

/* ── places ──────────────────────────────────────────────────────────────── */

void FilesPage::rebuildPlaces()
{
    m_places.clear();

    if (!m_scope.isEmpty()) {
        /* Scoped to one volume: the panel lists that volume and nothing else.  It
         * is one row, and it is still worth drawing -- it is what says on the glass
         * that this page is inside a disk rather than on the card. */
        Place p;
        p.label = m_scopeName;
        p.path = m_scope;
        p.glyph = GlyphDrive;
        p.volume = true;
        const Disk *v = nullptr;
        for (const Disk &candidate : Disks::instance().list()) {
            if (candidate.mountPoint == m_scope)
                v = &candidate;
        }
        if (v)
            p.readOnly = v->readOnly;
        m_places.append(p);
        m_place = 0;
        return;
    }

    Place home;
    home.label = tr("Home");
    home.path = QFileInfo::exists("/home/virtua") ? QStringLiteral("/home/virtua")
              : QFileInfo::exists("/home/ark")    ? QStringLiteral("/home/ark")
                                                  : QStringLiteral("/root");
    home.glyph = GlyphFiles;
    m_places.append(home);

    if (QFileInfo::exists("/run/j36/card")) {
        Place card;
        card.label = tr("Card");
        card.path = QStringLiteral("/run/j36/card");
        card.glyph = GlyphChip;
        m_places.append(card);
    }

    Place root;
    root.label = tr("System");
    root.path = QStringLiteral("/");
    root.glyph = GlyphSettings;
    m_places.append(root);

    const QVector<Disk> disks = Disks::instance().list();
    for (const Disk &v : disks) {
        Place p;
        p.label = v.name();
        p.path = v.mountPoint;
        p.glyph = GlyphDrive;
        p.volume = true;
        p.readOnly = v.readOnly;
        m_places.append(p);
    }

    /*
     * ── THE BACKSTOP, FOR WHEN THE DISK IS THERE AND WE DID NOT SEE IT ───────
     *
     * Everything above this line depends on Disks recognising the volume, and
     * Disks is a reader of two reports -- the mounter's state files and
     * /proc/self/mountinfo.  Both of them can be silent about a filesystem that is
     * genuinely mounted: a card whose payload predates the automount units writes
     * no state file, and a mount performed in another namespace is not in the
     * mountinfo this process opened.  The symptom is exact and was reported as
     * such -- a stick that the Terminal card can `ls' and this page cannot show.
     *
     * So when nothing was recognised and /media has something in it anyway, the
     * directory itself goes on the panel.  It is one row and it is not a volume:
     * no eject, no read-only badge, nothing claimed about it that a stat of a
     * directory cannot support.  Browsing into it lists the mount points, which is
     * the whole of what was missing.
     *
     * Only when the list is empty, deliberately.  A "Media" row NEXT TO the disks
     * it contains is two ways to the same files and a question about which one is
     * right; this is a fallback, and a fallback that shows up while the real thing
     * is working is just clutter.
     */
    if (disks.isEmpty()) {
        const QDir media(QStringLiteral("/media"));
        if (media.exists()
            && !media.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
            Place p;
            p.label = tr("Media");
            p.path = media.absolutePath();
            p.glyph = GlyphDrive;
            m_places.append(p);
        }
    }

    m_place = qBound(0, m_place, qMax(0, m_places.size() - 1));
}

/* ── navigation ──────────────────────────────────────────────────────────── */

QString FilesPage::title() const
{
    if (!m_scopeName.isEmpty())
        return m_scopeName;
    return tr("Files");
}

void FilesPage::onEnter()
{
    rebuildPlaces();

    /* The directory this page was left in may have gone away while it was closed --
     * an unmounted volume, a directory deleted from the Terminal card. */
    if (m_root.isEmpty() || !QFileInfo::exists(m_root))
        setRoot(m_scope.isEmpty() ? m_base : m_scope);

    setPane(PaneList);
    update();
}

void FilesPage::openAt(const QString &path, const QString &scope)
{
    const QString wasScope = m_scope;
    m_scope = scope.isEmpty() ? QString() : QDir::cleanPath(scope);
    m_scopeName.clear();
    if (!m_scope.isEmpty()) {
        m_scopeName = m_scope.section(QLatin1Char('/'), -1);
        for (const Disk &v : Disks::instance().list()) {
            if (v.mountPoint == m_scope)
                m_scopeName = v.name();
        }
    }

    /* A filter left over from the last volume is a listing with things missing in
     * it, on a page the user has only just opened. */
    m_search.clear();
    applyFilter();

    rebuildPlaces();

    /*
     * An empty path means "wherever this page was", which is what the Files card
     * asks for: a browser that forgot the directory every time it was closed would
     * be four presses back to where you were on every trip out to open something.
     * A CHANGE OF SCOPE overrides that -- coming out of a volume and into the
     * filesystem must not leave the page sitting inside the volume.
     */
    QString target = path;
    if (target.isEmpty()) {
        const bool kept = m_scope == wasScope && !m_root.isEmpty()
                          && QFileInfo::exists(m_root) && withinScope(m_root);
        target = kept ? m_root : (m_scope.isEmpty() ? m_base : m_scope);
    }
    setRoot(target);
    setPane(PaneList);
    emit titleChanged();
}

bool FilesPage::withinScope(const QString &path) const
{
    if (m_scope.isEmpty())
        return true;
    const QString clean = QDir::cleanPath(path);
    return clean == m_scope || clean.startsWith(m_scope + QLatin1Char('/'));
}

void FilesPage::navigateTo(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    const QFileInfo info(clean);
    if (!info.exists()) {
        emit toastRequested(tr("No such directory"), 2200);
        return;
    }
    if (!withinScope(clean)) {
        emit toastRequested(tr("Only %1 can be browsed here").arg(m_scopeName), 2600);
        return;
    }
    if (info.isDir())
        setRoot(clean);
    else
        emit openRequested(clean);
}

void FilesPage::setRoot(const QString &path)
{
    m_root = QDir::cleanPath(path);
    m_model->setRootPath(m_root);
    const QModelIndex root = m_model->index(m_root);
    m_view->setRootIndex(root);
    m_view->setCurrentIndex(m_model->index(0, 0, root));
    emit titleChanged();
    update();
}

void FilesPage::applyFilter()
{
    /*
     * A glob, because that is what QFileSystemModel speaks, and the plain substring
     * anybody types is turned into one here rather than being explained on the
     * glass.  setNameFilterDisables(false) is what makes a non-matching file
     * disappear instead of being drawn greyed out -- a list of grey names is not a
     * search result.
     *
     * DIRECTORIES ARE NOT FILTERED, and that is Qt's rule rather than a choice: a
     * name filter in QFileSystemModel applies to files alone.  It is also the more
     * useful behaviour here, because a search that hid the folders would hide the
     * only way to search anywhere else.
     */
    m_model->setNameFilterDisables(false);
    if (m_search.trimmed().isEmpty())
        m_model->setNameFilters(QStringList());
    else
        m_model->setNameFilters(QStringList() << (QLatin1Char('*') + m_search.trimmed()
                                                  + QLatin1Char('*')));
}

void FilesPage::step(int delta)
{
    const QModelIndex root = m_model->index(m_root);
    const int rows = m_model->rowCount(root);
    if (rows <= 0)
        return;

    const QModelIndex current = m_view->currentIndex();
    int row = current.isValid() ? current.row() + delta : 0;
    row = qBound(0, row, rows - 1);
    const QModelIndex next = m_model->index(row, 0, root);
    m_view->setCurrentIndex(next);
    m_view->scrollTo(next);
}

void FilesPage::enter()
{
    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid())
        return;
    const QString path = m_model->filePath(current);
    if (m_model->isDir(current))
        navigateTo(path);
    else
        emit openRequested(path);
}

bool FilesPage::leave()
{
    if (m_root == QLatin1String("/") || (!m_scope.isEmpty() && m_root == m_scope))
        return false;
    QDir dir(m_root);
    if (!dir.cdUp())
        return false;
    const QString up = dir.absolutePath();
    if (!withinScope(up))
        return false;

    const QString child = m_root;
    setRoot(up);
    /* Come back to the directory we just left, not to the top of its parent. */
    const QModelIndex idx = m_model->index(child);
    if (idx.isValid()) {
        m_view->setCurrentIndex(idx);
        m_view->scrollTo(idx);
    }
    return true;
}

/* ── panes ───────────────────────────────────────────────────────────────── */

void FilesPage::setPane(int pane)
{
    if (m_pane == pane)
        return;
    m_pane = pane;
    update();
}

void FilesPage::cyclePane(int delta)
{
    setPane((m_pane + delta + PaneCount) % PaneCount);
}

bool FilesPage::handleNav(int action)
{
    /* The shoulders are the pane switch.  Free here: the shell uses them on the
     * card grid and nowhere else, and a pushed page is asked first. */
    if (action == Joypad::NavPrevPage) {
        cyclePane(-1);
        return true;
    }
    if (action == Joypad::NavNextPage) {
        cyclePane(1);
        return true;
    }

    switch (m_pane) {
    case PaneAddress:
        switch (action) {
        case Joypad::NavDown:
            setPane(PaneSearch);
            return true;
        case Joypad::NavOk:
            m_asking = AskAddress;
            emit textRequested(tr("Go to"), m_root, false);
            return true;
        case Joypad::NavUp:
        case Joypad::NavLeft:
        case Joypad::NavRight:
            return true;
        default:
            return false;
        }

    case PaneSearch:
        switch (action) {
        case Joypad::NavUp:
            setPane(PaneAddress);
            return true;
        case Joypad::NavDown:
            setPane(PaneList);
            return true;
        case Joypad::NavLeft:
            setPane(PanePlaces);
            return true;
        case Joypad::NavOk:
            m_asking = AskSearch;
            emit textRequested(tr("Search"), m_search, false);
            return true;
        case Joypad::NavRight:
            return true;
        default:
            return false;
        }

    case PanePlaces:
        switch (action) {
        case Joypad::NavUp:
            if (m_place > 0) {
                --m_place;
                update();
            } else {
                setPane(PaneAddress);
            }
            return true;
        case Joypad::NavDown:
            if (m_place + 1 < m_places.size()) {
                ++m_place;
                update();
            }
            return true;
        case Joypad::NavRight:
        case Joypad::NavOk:
            if (m_place >= 0 && m_place < m_places.size()) {
                navigateTo(m_places[m_place].path);
                setPane(PaneList);
            }
            return true;
        case Joypad::NavLeft:
            return true;
        default:
            return false;
        }

    case PaneList:
    default:
        switch (action) {
        case Joypad::NavUp: {
            const QModelIndex current = m_view->currentIndex();
            /* Off the top of the listing and onto the search box, which is where
             * the eye goes anyway: it is the thing directly above. */
            if (current.isValid() && current.row() == 0)
                setPane(PaneSearch);
            else
                step(-1);
            return true;
        }
        case Joypad::NavDown:
            step(1);
            return true;
        case Joypad::NavRight:
        case Joypad::NavOk:
            enter();
            return true;
        case Joypad::NavLeft:
            /* Up a directory, and at the top of what this page is allowed to see,
             * sideways into the places panel instead.  B still leaves the page from
             * any depth, which is why this one never has to. */
            if (!leave())
                setPane(PanePlaces);
            return true;
        case Joypad::NavBack:
            /*
             * B leaves the page, from any depth, and never changes directory.
             *
             * It used to climb one directory per press and only pop the page once
             * it reached the top, so the number of presses it took to get back to
             * the dashboard was however deep the browsing had gone.  One button
             * doing two jobs also meant a press could not be predicted without
             * knowing where in the tree you were.
             */
            return false;
        default:
            return false;
        }
    }
}

void FilesPage::textEntered(const QString &text, bool accepted)
{
    const int asked = m_asking;
    m_asking = AskNothing;
    if (!accepted)
        return;

    if (asked == AskAddress) {
        navigateTo(text);
        setPane(PaneList);
    } else if (asked == AskSearch) {
        m_search = text;
        applyFilter();
        /* The filter changes what row 0 is, so the cursor goes back to the top
         * rather than to whatever has inherited the old row number. */
        const QModelIndex root = m_model->index(m_root);
        if (m_model->rowCount(root) > 0)
            m_view->setCurrentIndex(m_model->index(0, 0, root));
        setPane(PaneList);
        update();
    }
}

/* ── pointer ─────────────────────────────────────────────────────────────── */

void FilesPage::mousePressEvent(QMouseEvent *event)
{
    const QPointF at = event->pos();

    if (addressRect().contains(at)) {
        setPane(PaneAddress);
        m_asking = AskAddress;
        emit textRequested(tr("Go to"), m_root, false);
        event->accept();
        return;
    }
    if (searchRect().contains(at)) {
        setPane(PaneSearch);
        m_asking = AskSearch;
        emit textRequested(tr("Search"), m_search, false);
        event->accept();
        return;
    }
    if (placesRect().contains(at)) {
        setPane(PanePlaces);
        for (int i = 0; i < m_places.size(); ++i) {
            if (placeRowRect(i).contains(at)) {
                m_place = i;
                navigateTo(m_places[i].path);
                setPane(PaneList);
                break;
            }
        }
        update();
        event->accept();
        return;
    }
    event->accept();
}

void FilesPage::mouseDoubleClickEvent(QMouseEvent *event)
{
    /* The list has its own double click -- see the constructor.  This one is for
     * the places panel, where a double click should not be a second navigation. */
    event->accept();
}

/* ── painting ────────────────────────────────────────────────────────────── */

void FilesPage::paintField(QPainter &p, const QRectF &r, const QString &text,
                           const QString &placeholder, bool focused) const
{
    Theme::vgrad(p, r, Theme::deskLow(), Theme::desk(), 7);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(focused ? Theme::blue() : Theme::border(), focused ? 1.6 : 1.0));
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);

    const QFont f = Theme::font(12);
    const QFontMetrics fm(f);
    p.setFont(f);
    const QRectF inner = r.adjusted(9, 0, -9, 0);
    const bool empty = text.isEmpty();
    p.setPen(empty ? Theme::ink3() : Theme::ink());
    p.drawText(inner, Qt::AlignVCenter | Qt::AlignLeft,
               fm.elidedText(empty ? placeholder : text, Qt::ElideMiddle, (int)inner.width()));
}

void FilesPage::paintPlaces(QPainter &p)
{
    const QRectF r = placesRect();
    const bool focused = m_pane == PanePlaces;

    Theme::vgrad(p, r, Theme::deskLow(), Theme::desk(), 9);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(focused ? Theme::blue() : Theme::border(), focused ? 1.6 : 1.0));
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 9, 9);

    const QFont f = Theme::font(12);
    const QFontMetrics fm(f);
    p.setFont(f);

    for (int i = 0; i < m_places.size(); ++i) {
        const QRectF row = placeRowRect(i);
        if (row.bottom() > r.bottom() - 4)
            break;

        const bool here = QDir::cleanPath(m_places[i].path) == m_root;
        const bool cursor = focused && i == m_place;
        if (cursor || here) {
            QColor fill = cursor ? Theme::blue() : Theme::glass();
            fill.setAlpha(cursor ? 200 : 60);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(row, 6, 6);
            p.setBrush(Qt::NoBrush);
        }

        const QRectF icon(row.x() + 5, row.center().y() - 8, 16, 16);
        paintGlyph(p, icon, m_places[i].glyph,
                   cursor ? Theme::ink() : (m_places[i].volume ? Theme::teal() : Theme::ink2()));

        QString label = m_places[i].label;
        if (m_places[i].readOnly)
            label += QStringLiteral("  *");
        const QRectF text(icon.right() + 6, row.y(), row.right() - icon.right() - 10, row.height());
        p.setPen(cursor ? Theme::ink() : Theme::ink2());
        p.drawText(text, Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(label, Qt::ElideRight, (int)text.width()));
    }

    /* What the asterisk means, once, at the foot of the panel -- rather than a
     * "read-only" badge on every row of a panel 116 px wide. */
    bool anyRo = false;
    for (const Place &pl : m_places)
        anyRo = anyRo || pl.readOnly;
    if (anyRo) {
        p.setFont(Theme::font(10));
        p.setPen(Theme::ink3());
        p.drawText(QRectF(r.x() + 6, r.bottom() - 20, r.width() - 12, 16),
                   Qt::AlignVCenter | Qt::AlignLeft, tr("* read-only"));
    }
}

void FilesPage::paintInfo(QPainter &p)
{
    const QRectF r = infoRect();

    Theme::vgrad(p, r, Theme::deskLow(), Theme::desk(), 9);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Theme::border(), 1.0));
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 9, 9);

    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid()) {
        p.setFont(Theme::font(12));
        p.setPen(Theme::ink3());
        p.drawText(r.adjusted(10, 0, -10, 0), Qt::AlignCenter, tr("Nothing selected"));
        return;
    }

    const QFileInfo info(m_model->filePath(current));

    /*
     * NO PATH IN HERE.  It is in the address bar, in full, in a field that can be
     * edited -- see the header.  What follows is what the listing cannot say.
     */
    qreal y = r.y() + 10;
    const qreal x = r.x() + 10;
    const qreal w = r.width() - 20;

    const QFont nameFont = Theme::font(13, true);
    p.setFont(nameFont);
    p.setPen(Theme::ink());
    QRectF nameBox(x, y, w, 34);
    p.drawText(nameBox, Qt::AlignTop | Qt::AlignLeft | Qt::TextWrapAnywhere, info.fileName());
    y += 40;

    struct Field { QString label; QString value; };
    QVector<Field> fields;

    QString kind;
    if (info.isDir())
        kind = tr("Folder");
    else if (info.isSymLink())
        kind = tr("Link");
    else if (!info.suffix().isEmpty())
        kind = info.suffix().toUpper();
    else
        kind = tr("File");
    fields.append({ tr("Kind"), kind });

    if (!info.isDir())
        fields.append({ tr("Size"), humanSize(info.size()) });

    const QDateTime when = info.lastModified();
    if (when.isValid()) {
        fields.append({ tr("Modified"), when.toString(QStringLiteral("yyyy-MM-dd")) });
        fields.append({ QString(), when.toString(QStringLiteral("HH:mm")) });
    }

    fields.append({ tr("Access"), info.isWritable() ? tr("Read and write") : tr("Read only") });

    const QFont labelFont = Theme::font(10);
    const QFont valueFont = Theme::font(12);
    const QFontMetrics vm(valueFont);
    for (const Field &f : fields) {
        if (y > r.bottom() - 24)
            break;
        if (!f.label.isEmpty()) {
            p.setFont(labelFont);
            p.setPen(Theme::ink3());
            p.drawText(QRectF(x, y, w, 13), Qt::AlignVCenter | Qt::AlignLeft, f.label.toUpper());
            y += 14;
        }
        p.setFont(valueFont);
        p.setPen(Theme::ink2());
        p.drawText(QRectF(x, y, w, 16), Qt::AlignVCenter | Qt::AlignLeft,
                   vm.elidedText(f.value, Qt::ElideRight, (int)w));
        y += f.label.isEmpty() ? 18 : 20;
    }
}

void FilesPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);

    const QModelIndex root = m_model->index(m_root);
    const int rows = m_model->rowCount(root);
    paintSheet(p, card, title(), tr("%1 items").arg(rows));

    paintField(p, addressRect(), m_root, tr("Path"), m_pane == PaneAddress);
    paintField(p, searchRect(), m_search, tr("Search"), m_pane == PaneSearch);
    paintPlaces(p);
    paintInfo(p);

    /* The frame around the listing.  The view itself is a child widget with a
     * transparent background, so this is drawn under it and shows through. */
    const QRectF lr = listRect();
    Theme::vgrad(p, lr, Theme::deskLow(), Theme::desk(), 9);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(m_pane == PaneList ? Theme::blue() : Theme::border(),
                  m_pane == PaneList ? 1.6 : 1.0));
    p.drawRoundedRect(lr.adjusted(0.5, 0.5, -0.5, -0.5), 9, 9);
}
