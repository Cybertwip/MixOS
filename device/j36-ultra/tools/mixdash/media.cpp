/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * media.cpp -- the browser, the queue, the transport and the two ffmpeg chains.
 * media.h says why any of this is shaped the way it is; this file is the how.
 */
#include "media.h"

#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QImageReader>
#include <QPainter>
#include <QProcess>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QTimer>

#include <signal.h>

#include "glvideo.h"
#include "joypad.h"
#include "settings.h"
#include "shell.h"
#include "theme.h"
#include "volume.h"

namespace {

const char *kAudioExt = "mp3 flac ogg oga opus wav m4a aac wma aiff mid";
const char *kVideoExt = "mp4 mkv avi webm mov m4v mpg mpeg wmv flv ts 3gp ogv";
const char *kImageExt = "jpg jpeg png bmp gif webp pbm pgm ppm xbm xpm tif tiff";

/* The one sample rate the whole page uses.  See media.h: 48 kHz is the family the
 * MT6592's audio PLL runs natively, and every divider not taken is a thing that
 * cannot be subtly wrong. */
const int kRate = 48000;

bool extIn(const char *list, const QString &suffix)
{
    if (suffix.isEmpty())
        return false;
    const QString needle = " " + suffix.toLower() + " ";
    const QString haystack = " " + QString::fromLatin1(list) + " ";
    return haystack.contains(needle);
}

QString firstExisting(const QStringList &paths)
{
    for (const QString &p : paths)
        if (QFileInfo(p).isExecutable())
            return p;
    return QString();
}

/*
 * Kill a child and forget it, in the one order that works.
 *
 * disconnect() FIRST, because everything that calls this is either tearing the
 * chain down deliberately or replacing it, and in both cases the finished signal
 * that terminate() is about to cause would be read as "the track ended" by
 * onMusicFinished() -- which would then advance the queue into the next track,
 * from inside the code that was stopping it.  That was the old page's "the music
 * does not stop" in its purest form.
 *
 * SIGCONT SECOND, because a process stopped by the pause button ignores SIGTERM
 * until something lets it run again, and a paused player that is then closed would
 * otherwise sit in the process table holding the sound card open for ever.
 */
void endProcess(QProcess *&p)
{
    if (!p)
        return;
    QProcess *const doomed = p;
    p = nullptr;

    doomed->disconnect();
    if (doomed->state() != QProcess::NotRunning) {
        if (doomed->processId() > 0)
            ::kill((pid_t)doomed->processId(), SIGCONT);
        doomed->terminate();
        /*
         * QProcess's own wait and NOT Shell's, which is the only wait in this
         * program that is deliberately left blind to the console.  Shell repaints
         * the window when it has to take the mode back, and one caller of this
         * function is ~MediaPage -- where painting would run this page's
         * paintEvent over members that have already been destroyed, for exactly
         * the reason the destructor gives for not calling stopMusic() either.
         * It is 400 ms on a child that has just been sent SIGTERM.
         */
        if (!doomed->waitForFinished(400))
            doomed->kill();
    }
    doomed->deleteLater();
}

/* aplay says "aplay: main:834: audio open error: No such file or directory".  The
 * file and line are ours to drop -- the sentence after them is the whole message,
 * and the screen this lands on is 640 px wide. */
QString tidyChildError(const QString &raw)
{
    QString line = raw.trimmed().section('\n', 0, 0).trimmed();
    if (line.startsWith(QStringLiteral("aplay: "))) {
        const int colon = line.indexOf(QStringLiteral(": "), 7);
        if (colon > 0)
            line = QStringLiteral("aplay: ") + line.mid(colon + 2).trimmed();
    }
    return line;
}

/*
 * WHERE THE BROWSER STARTS WHEN NOBODY HAS SAID
 *
 * It used to start nowhere: Settings::mediaRoot() is empty until someone picks a
 * directory, m_dir took that empty string, and populate() handed it to QDir("")
 * -- which is not "no directory", it is the PROCESS's working directory.  That is
 * WorkingDirectory=/opt/mixos/bin out of mixdash.service, so the media browser
 * opened on the payload directory, next to doom-j36, and every row in it is a
 * binary that rebuild() marks unselectable.  Worse, an empty m_dir has no
 * QFileInfo::absolutePath(), so the `..' row's target was empty too and Back's
 * "go up a directory" guard was false from the start: the page had no way out of
 * a directory with nothing in it to open.  Hence "it does not navigate".
 *
 * /home/virtua first because that is the card's writable partition -- p3, mounted
 * over the virtua user's home by the initramfs -- and it is where anything the
 * user copied onto the card actually lands.  The rest are for a machine that
 * booted without a card: the home directory of whoever we are, then the
 * read-only card mount if the initramfs made one, then /media.  Root last so the
 * page always has SOMEWHERE, even on a system missing all of the above.
 */
QString defaultMediaRoot()
{
    const QStringList candidates = QStringList()
        << QStringLiteral("/home/virtua")
        << QDir::homePath()
        << QStringLiteral("/run/j36/card")
        << QStringLiteral("/media");
    for (const QString &c : candidates)
        if (!c.isEmpty() && QFileInfo(c).isDir())
            return c;
    return QDir::rootPath();
}

/*
 * ── THE TOP OF THE TREE, AND WHY THERE IS ONE ────────────────────────────────
 *
 * This page is the MEDIA browser and /home/virtua is the media directory: it is
 * the DATA partition, it is the only place on the card the user can put anything,
 * and it is the only place anything they put is going to be.  Above it is a
 * Debian rootfs -- /usr/lib full of shared objects, /proc, /sys -- none of which
 * has a single playable file in it and all of which is reachable by holding Up on
 * the `..' row.  Walking out into that was not a feature, it was a way to get
 * lost in a directory of device nodes with nothing on screen to explain how you
 * got there, on a handheld with no path bar to type your way back from.
 *
 * So there is a ceiling and this is it.  It is not a security boundary -- the
 * Files page still browses the whole disk, and that page is the one whose job
 * that is -- it is a browser that stays inside what it browses.
 *
 * mediaCeiling() is defaultMediaRoot() and not the remembered directory: the
 * remembered one is where you WERE, which is somewhere under the ceiling, not
 * the ceiling itself.
 */
QString mediaCeiling()
{
    return QDir::cleanPath(defaultMediaRoot());
}

/* True if `dir' is the ceiling or anything beneath it.  Compared with a trailing
 * separator so that /home/virtua-old is not mistaken for a child of
 * /home/virtua, which a bare startsWith() would do. */
bool underCeiling(const QString &dir)
{
    const QString top = mediaCeiling();
    const QString d = QDir::cleanPath(dir);

    if (d == top)
        return true;
    return d.startsWith(top.endsWith(QLatin1Char('/')) ? top : top + QLatin1Char('/'));
}

/* Whatever was asked for if it is inside the tree, the ceiling if it is not.
 * Every path that reaches m_dir goes through here, so there is one place that
 * decides and no way round it. */
QString clampToCeiling(const QString &dir)
{
    if (dir.isEmpty())
        return mediaCeiling();
    return underCeiling(dir) ? QDir::cleanPath(dir) : mediaCeiling();
}

/* The remembered root if it is still a directory, the default if not.  Checked
 * again here and not just at load time because the card it named can be pulled.
 * Clamped as well: a settings file written before the ceiling existed can name
 * / or /usr, and that would put the page outside the tree on the first frame. */
QString mediaStartDir()
{
    const QString remembered = Settings::instance().mediaRoot();
    return QFileInfo(remembered).isDir() ? clampToCeiling(remembered)
                                         : mediaCeiling();
}

} /* namespace */

MediaPage::MediaPage(QWidget *parent)
    : PageWidget(parent)
{
    m_list = new ListPane(this);
    m_list->setRowHeight(30);
    /* Names the row, because the row is now the only way up and an empty
     * directory is exactly where somebody needs telling. */
    m_list->setPlaceholder(tr("Nothing playable here.\nThe .. row goes up, B leaves."));
    connect(m_list, &ListPane::activated, this, &MediaPage::onActivated);
    connect(m_list, &ListPane::valueChanged, this, &MediaPage::onValueChanged);

    m_ui = new QTimer(this);
    m_ui->setInterval(500);
    connect(m_ui, &QTimer::timeout, this, &MediaPage::tick);

    m_seekTimer = new QTimer(this);
    m_seekTimer->setSingleShot(true);
    m_seekTimer->setInterval(350);
    connect(m_seekTimer, &QTimer::timeout, this, &MediaPage::commitSeek);

    /* Both remembered, because a handheld that forgets it was shuffling is a
     * handheld you set up again every boot. */
    m_repeat = Settings::instance().mediaRepeat();
    m_shuffle = Settings::instance().mediaShuffle();

    m_dir = mediaStartDir();
}

MediaPage::~MediaPage()
{
    /* Not stopMusic()/stopVideo(): those rebuild rows and touch the list pane,
     * which is a child being destroyed underneath us.  The processes are all this
     * destructor owes anybody. */
    endProcess(m_decoder);
    endProcess(m_videoAudio);
    endProcess(m_videoAplay);
    endProcess(m_music);
    endProcess(m_aplay);
}

QString MediaPage::title() const
{
    if (m_view == ViewPlayer) {
        const Entry *t = queuedTrack();
        if (t)
            return m_trackTitle.isEmpty() ? t->name : m_trackTitle;
    }
    if (m_view == ViewImage || m_view == ViewVideo)
        return m_showing.name;
    return tr("Media");
}

bool MediaPage::wantsFullscreen() const
{
    /* Only while a picture or a film is on the glass.  The browser and the player
     * both want the status bar -- one to say where you are, the other to keep the
     * clock and the battery in sight while a record plays. */
    return m_view == ViewImage || m_view == ViewVideo;
}

void MediaPage::layOutList()
{
    const QRect card(Theme::Margin, Theme::Margin,
                     width() - 2 * Theme::Margin, height() - 2 * Theme::Margin);
    const int foot = noteText().isEmpty() ? 10 : 30;
    m_list->setGeometry(card.x() + 6, card.y() + 40, card.width() - 12,
                        qMax(24, card.height() - 40 - foot));
}

void MediaPage::resizeEvent(QResizeEvent *event)
{
    layOutList();
    QWidget::resizeEvent(event);
}

void MediaPage::onEnter()
{
    if (m_dir.isEmpty() || !QFileInfo(m_dir).isDir())
        m_dir = mediaStartDir();

    /* A picture or a film that was up when the page was left is gone -- both were
     * stopped by onLeave().  Music is not, so the view it left in is still the
     * view it should come back to. */
    if (m_view == ViewPlayer && !musicLive())
        m_view = ViewBrowse;
    if (m_view == ViewImage || m_view == ViewVideo)
        m_view = ViewBrowse;
    m_list->setVisible(true);

    populate(m_dir);
    m_ui->start();
}

void MediaPage::onLeave()
{
    m_ui->stop();
    /*
     * Video and pictures stop; MUSIC KEEPS PLAYING.  That asymmetry is the whole
     * reason the player does not take the screen: a handheld that stops the album
     * the moment you go and look at something else is a handheld nobody listens to
     * music on.  A film you cannot see is just a fan spinning, so it goes.
     */
    if (m_view == ViewVideo || m_view == ViewImage) {
        stopVideo();
        dropFrame();
        setView(ViewBrowse);
    }
}

void MediaPage::setView(int view)
{
    if (m_view == view)
        return;

    /* Leaving the browser: remember WHICH ENTRY was selected, not which row.  The
     * now-playing row appears and disappears above the entries, so a row index
     * saved here would come back one off whenever the music state changed while
     * the player view was up. */
    if (m_view == ViewBrowse) {
        const ListRow *r = m_list->currentRow();
        m_browseAt = (r && r->id >= 0) ? r->id : 0;
    }

    m_view = view;
    /* The list is a real child widget; behind a full-screen picture it would paint
     * over the picture rather than under it. */
    m_list->setVisible(view == ViewBrowse || view == ViewPlayer);

    rebuild();

    if (view == ViewBrowse) {
        const QVector<ListRow> &rows = m_list->rows();
        for (int i = 0; i < rows.size(); ++i) {
            if (rows[i].id != m_browseAt)
                continue;
            m_list->setCurrent(i);
            break;
        }
    } else if (view == ViewPlayer) {
        /* Land on Pause, which is what the button under your thumb should do the
         * moment the player opens. */
        const QVector<ListRow> &rows = m_list->rows();
        for (int i = 0; i < rows.size(); ++i) {
            if (rows[i].id != RowPause)
                continue;
            m_list->setCurrent(i);
            break;
        }
    }

    emit titleChanged();
    refresh();
}

/* ── the browser ─────────────────────────────────────────────────────────── */

int MediaPage::kindFor(const QFileInfo &info)
{
    if (info.isDir())
        return KindDir;
    const QString suffix = info.suffix();
    if (extIn(kAudioExt, suffix))
        return KindAudio;
    if (extIn(kVideoExt, suffix))
        return KindVideo;
    if (extIn(kImageExt, suffix))
        return KindImage;
    return KindOther;
}

QString MediaPage::humanSize(qint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 1) + " GB";
    if (bytes >= 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024), 'f', 1) + " MB";
    if (bytes >= 1024)
        return QString::number(bytes / 1024) + " kB";
    return QString::number(bytes) + " B";
}

QString MediaPage::humanTime(int seconds)
{
    if (seconds < 0)
        return QStringLiteral("--:--");
    const int h = seconds / 3600;
    const int m = (seconds / 60) % 60;
    const int s = seconds % 60;
    if (h > 0)
        return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

/* dir is taken BY VALUE -- see media.h.  The clear() below would otherwise be
 * destroying the string this function is still reading. */
void MediaPage::populate(QString dir)
{
    /* Absolute before anything stores it.  m_dir is what both ways up out of a
     * directory are computed from -- the `..' row's target and NavBack's parent --
     * and neither an empty nor a relative path has an absolutePath() to climb to,
     * so a page that got one could be entered and never left.  See
     * defaultMediaRoot() above for how it used to get one. */
    QDir d(clampToCeiling(dir));
    m_dir = d.absolutePath();
    m_entries.clear();

    if (!d.exists()) {
        rebuild();
        emit titleChanged();
        return;
    }

    /*
     * `..' only while there is somewhere above to go that is still inside the
     * tree.  The test used to be !d.isRoot(), which meant the row was on every
     * directory but / and was the way out of the media tree entirely.  Dropping
     * the row rather than leaving it and refusing the press is deliberate: a row
     * that is on the list and does nothing reads as a bug, and the absence of the
     * row is itself the message that this is the top.
     */
    if (m_dir != mediaCeiling() && underCeiling(QFileInfo(m_dir).absolutePath())) {
        Entry up;
        up.kind = KindUp;
        up.name = QStringLiteral("..");
        up.path = QFileInfo(m_dir).absolutePath();
        m_entries.append(up);
    }

    const QFileInfoList infos = d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                                QDir::Name | QDir::DirsFirst
                                                    | QDir::IgnoreCase);
    for (const QFileInfo &info : infos) {
        if (info.fileName().startsWith('.'))
            continue;
        Entry e;
        e.path = info.absoluteFilePath();
        e.name = info.fileName();
        e.kind = kindFor(info);
        e.size = info.isFile() ? info.size() : 0;
        /* Files that are not media are listed but not selectable: seeing that a
         * directory has things in it is worth more than a list that looks empty. */
        m_entries.append(e);
    }

    /* The browser IS the navigation, so where it got to is what gets remembered --
     * and it is remembered per directory entered, not once when the page closes,
     * because the way this device stops is that somebody holds the power button. */
    Settings::instance().setMediaRoot(m_dir);

    rebuild();
    emit titleChanged();
}

bool MediaPage::openPath(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return false;

    if (info.isDir()) {
        setView(ViewBrowse);
        populate(info.absoluteFilePath());
        m_list->setCurrent(0);
        return true;
    }

    const int kind = kindFor(info);
    if (kind != KindAudio && kind != KindVideo && kind != KindImage)
        return false;

    /* Browse to the containing directory first: opening a file with no list
     * behind it means Back from the picture lands on an empty page, the queue has
     * nothing to walk, and the next/previous image walk has nothing either. */
    setView(ViewBrowse);
    populate(info.absolutePath());
    const QString want = info.absoluteFilePath();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].path != want)
            continue;
        const QVector<ListRow> &rows = m_list->rows();
        for (int r = 0; r < rows.size(); ++r) {
            if (rows[r].id != i)
                continue;
            m_list->setCurrent(r);
            break;
        }
        open(m_entries[i]);
        return true;
    }
    return false;
}

/* ── the rows ────────────────────────────────────────────────────────────── */

void MediaPage::rebuild()
{
    QVector<ListRow> rows;
    if (m_view == ViewPlayer)
        buildPlayerRows(rows);
    else
        buildBrowseRows(rows);

    const int keep = m_list->current();
    m_list->setRows(rows);
    if (keep >= 0 && keep < rows.size())
        m_list->setCurrent(keep);
    layOutList();
    refresh();
}

void MediaPage::buildBrowseRows(QVector<ListRow> &rows) const
{
    /*
     * The now-playing row is PINNED AT THE TOP and it is a row like any other, not
     * a strip painted over the foot of the sheet the way the old page did it.  A
     * strip is furniture: you cannot select it, so the only way into the transport
     * was a button chord nobody was told about.  A row you can land on and press
     * is the whole discovery mechanism.
     */
    const Entry *track = queuedTrack();
    if (musicLive() && track) {
        ListRow r;
        r.kind = ListRow::Item;
        r.id = RowNowPlaying;
        r.glyph = GlyphMusic;
        r.accent = m_paused ? Theme::orange() : Theme::green();
        r.text = m_trackTitle.isEmpty() ? track->name : m_trackTitle;

        QString clock = humanTime((int)position());
        if (m_duration > 0.0)
            clock += " / " + humanTime((int)m_duration);
        r.detail = clock + "   " + tr("open the player");
        r.badge = m_paused ? tr("paused") : tr("playing");
        r.badgeColour = r.accent;
        rows.append(r);
    }

    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &e = m_entries[i];
        ListRow r;
        r.kind = ListRow::Item;
        r.text = e.name;
        r.id = i;
        r.key = e.path;

        switch (e.kind) {
        case KindUp:
            r.glyph = GlyphBack;
            r.accent = Theme::ink3();
            r.detail = tr("up one level");
            break;
        case KindDir:
            r.glyph = GlyphFiles;
            r.accent = Theme::blue();
            break;
        case KindAudio:
            r.glyph = GlyphMusic;
            r.accent = Theme::purple();
            r.detail = humanSize(e.size);
            break;
        case KindVideo:
            r.glyph = GlyphVideo;
            r.accent = Theme::pink();
            r.detail = humanSize(e.size);
            break;
        case KindImage:
            r.glyph = GlyphImage;
            r.accent = Theme::teal();
            r.detail = humanSize(e.size);
            break;
        default:
            r.glyph = GlyphFiles;
            r.accent = Theme::ink3();
            r.detail = humanSize(e.size);
            r.enabled = false;
            break;
        }

        /* The track that is playing is marked HERE too, so the directory listing
         * and the player never disagree about what is on. */
        if (track && e.kind == KindAudio && e.path == track->path && musicLive()) {
            r.badge = m_paused ? tr("paused") : tr("playing");
            r.badgeColour = m_paused ? Theme::orange() : Theme::green();
        }
        rows.append(r);
    }
}

void MediaPage::buildPlayerRows(QVector<ListRow> &rows) const
{
    const Entry *track = queuedTrack();
    if (!track)
        return;

    {
        ListRow head;
        head.kind = ListRow::Header;
        head.id = RowInert;
        head.text = m_trackTitle.isEmpty() ? track->name : m_trackTitle;
        rows.append(head);
    }

    {
        ListRow r;
        r.id = RowSeek;
        r.text = tr("Position");
        r.accent = Theme::blue();
        if (m_duration > 0.0) {
            r.kind = ListRow::Slider;
            r.minimum = 0;
            r.maximum = qMax(1, (int)m_duration);
            r.value = qBound(0, (int)position(), r.maximum);
            r.stepSize = 10;
            r.valueText = humanTime(r.value) + " / " + humanTime(r.maximum);
        } else {
            /* No duration means no slider that could mean anything.  A disabled row
             * still shows the clock and still cannot be landed on, which is exactly
             * right for a control with nothing behind it. */
            r.kind = ListRow::Item;
            r.enabled = false;
            r.detail = humanTime((int)position());
        }
        rows.append(r);
    }

    {
        ListRow r;
        r.kind = ListRow::Action;
        r.id = RowPause;
        r.glyph = GlyphPower;
        r.accent = m_paused ? Theme::green() : Theme::orange();
        r.text = m_paused ? tr("Resume") : tr("Pause");
        rows.append(r);
    }

    {
        ListRow r;
        r.kind = ListRow::Action;
        r.id = RowNext;
        r.glyph = GlyphMusic;
        r.accent = Theme::purple();
        r.text = tr("Next track");
        /* What Next will actually play, which on a shuffled queue is the only way
         * to know before pressing it. */
        if (m_orderAt >= 0 && m_orderAt + 1 < m_order.size())
            r.detail = m_queue[m_order[m_orderAt + 1]].name;
        else if (m_repeat != RepeatOff && !m_order.isEmpty())
            r.detail = m_queue[m_order[0]].name;
        else
            r.detail = tr("last in this folder");
        rows.append(r);
    }

    {
        ListRow r;
        r.kind = ListRow::Action;
        r.id = RowPrev;
        r.glyph = GlyphBack;
        r.accent = Theme::purple();
        r.text = tr("Previous track");
        if (m_orderAt > 0)
            r.detail = m_queue[m_order[m_orderAt - 1]].name;
        else
            r.detail = tr("first in this folder");
        rows.append(r);
    }

    {
        ListRow r;
        r.kind = ListRow::Action;
        r.id = RowStop;
        r.glyph = GlyphPower;
        r.accent = Theme::pink();
        r.text = tr("Stop");
        r.detail = tr("end playback and go back to the folder");
        rows.append(r);
    }

    {
        ListRow head;
        head.kind = ListRow::Header;
        head.id = RowInert;
        head.text = tr("Queue");
        rows.append(head);
    }

    {
        ListRow r;
        r.kind = ListRow::Item;
        r.id = RowRepeat;
        r.glyph = GlyphSettings;
        r.accent = Theme::teal();
        r.text = tr("Repeat");
        switch (m_repeat) {
        case RepeatAll:
            r.badge = tr("folder");
            r.badgeColour = Theme::teal();
            r.detail = tr("play the folder round and round");
            break;
        case RepeatOne:
            r.badge = tr("one");
            r.badgeColour = Theme::teal();
            r.detail = tr("play this track again when it ends");
            break;
        default:
            r.badge = tr("off");
            r.badgeColour = Theme::separator();
            r.detail = tr("stop at the end of the folder");
            break;
        }
        rows.append(r);
    }

    {
        ListRow r;
        r.kind = ListRow::Toggle;
        r.id = RowShuffle;
        r.glyph = GlyphChip;
        r.accent = Theme::orange();
        r.text = tr("Shuffle");
        r.on = m_shuffle;
        r.detail = tr("%1 tracks in this folder").arg(m_queue.size());
        rows.append(r);
    }

    {
        ListRow r;
        r.kind = ListRow::Action;
        r.id = RowReveal;
        r.glyph = GlyphFiles;
        r.accent = Theme::blue();
        r.text = tr("Show in folder");
        r.detail = QFileInfo(track->path).absolutePath();
        rows.append(r);
    }

    {
        /*
         * The output device, disabled, at the foot.  Not decoration: "there is no
         * sound" and "there is no sound FROM plughw:0,0 at 48 kHz" are different
         * bug reports, and only one of them can be acted on.
         */
        ListRow head;
        head.kind = ListRow::Header;
        head.id = RowInert;
        head.text = tr("Output");
        rows.append(head);

        ListRow d;
        d.kind = ListRow::Item;
        d.id = RowInert;
        d.enabled = false;
        d.glyph = GlyphInfo;
        d.accent = Theme::ink3();
        d.text = m_device.isEmpty() ? tr("no sound card on this device") : m_device;
        d.detail = QString("s16le  %1 Hz  %2").arg(kRate).arg(tr("stereo"));
        rows.append(d);
    }
}

/* ── activation ──────────────────────────────────────────────────────────── */

void MediaPage::onActivated(int index)
{
    const QVector<ListRow> &rows = m_list->rows();
    if (index < 0 || index >= rows.size())
        return;
    const int id = rows[index].id;

    /* Non-negative is an index into m_entries; everything the player owns is
     * negative and named.  One switch, no mapping table. */
    if (id >= 0) {
        if (id < m_entries.size())
            open(m_entries[id]);
        return;
    }

    switch (id) {
    case RowNowPlaying:
        setView(ViewPlayer);
        return;
    case RowPause:
        togglePause();
        return;
    case RowNext:
        advance(1, false);
        return;
    case RowPrev:
        /*
         * Within the first three seconds Previous means "the one before"; after
         * that it means "start this one again", which is what every player has
         * done since the CD transport and is what a thumb reaches for when it
         * missed the start of a song.
         */
        if (position() > 3.0)
            playQueued(0.0);
        else
            advance(-1, false);
        return;
    case RowStop:
        stopMusic();
        return;
    case RowRepeat: {
        m_repeat = (m_repeat + 1) % 3;
        Settings::instance().setMediaRepeat(m_repeat);
        rebuild();
        return;
    }
    case RowReveal: {
        const Entry *track = queuedTrack();
        if (!track)
            return;
        const QString file = track->path;
        setView(ViewBrowse);
        populate(QFileInfo(file).absolutePath());
        const QVector<ListRow> &browse = m_list->rows();
        for (int i = 0; i < browse.size(); ++i) {
            if (browse[i].id < 0 || browse[i].key != file)
                continue;
            m_list->setCurrent(i);
            break;
        }
        return;
    }
    default:
        return;
    }
}

void MediaPage::onValueChanged(int index, int value)
{
    const QVector<ListRow> &rows = m_list->rows();
    if (index < 0 || index >= rows.size())
        return;

    switch (rows[index].id) {
    case RowSeek:
        seekTo(value);
        return;
    case RowShuffle:
        m_shuffle = (value != 0);
        Settings::instance().setMediaShuffle(m_shuffle);
        /* Re-derive the order around whatever is playing, so turning shuffle on
         * mid-album does not restart the track you are listening to. */
        reshuffle();
        rebuild();
        return;
    default:
        return;
    }
}

/*
 * Put the cursor on the row that shows `path', if this listing has one.
 *
 * Rows are not entries and the index of one is not the index of the other:
 * buildBrowseRows() pins a now-playing row above the listing while music is
 * live, so every entry row is shifted down by one whenever that is true.  The
 * row's `id' IS the entry index -- that is what onActivated() looks it up by --
 * so this searches the rows for the id rather than doing arithmetic on an
 * offset it would have to keep in step with buildBrowseRows() by hand.
 */
bool MediaPage::selectPath(const QString &path)
{
    int entry = -1;

    if (path.isEmpty())
        return false;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].path == path) {
            entry = i;
            break;
        }
    }
    if (entry < 0)
        return false;

    const QVector<ListRow> &rows = m_list->rows();
    for (int r = 0; r < rows.size(); ++r) {
        if (rows[r].kind == ListRow::Item && rows[r].id == entry) {
            m_list->setCurrent(r);
            return true;
        }
    }
    return false;
}

void MediaPage::open(Entry entry)
{
    switch (entry.kind) {
    case KindUp: {
        /*
         * Going up lands the cursor on the directory just left, not on row 0.
         *
         * `..' is the only way up now that B leaves the page, so this is the
         * whole of the "where was I" mechanism.  Without it, climbing out of a
         * directory puts the selection back on `..' -- the row at index 0 of the
         * parent -- and the very next press climbs again, so one press too many
         * walks you to the top of the tree instead of one level up.
         */
        const QString child = m_dir;
        populate(entry.path);
        if (!selectPath(child))
            m_list->setCurrent(0);
        return;
    }
    case KindDir:
        populate(entry.path);
        m_list->setCurrent(0);
        return;
    case KindImage:
        openImage(entry);
        return;
    case KindVideo:
        openVideo(entry);
        return;
    case KindAudio:
        /*
         * Starting a track builds the queue out of the directory it came from and
         * STAYS IN THE BROWSER.  Taking the screen here would be the old page's
         * mistake in a new place: you started one song and lost the folder.  The
         * now-playing row at the top is the way into the transport.
         */
        queueDirectory(m_dir, entry.path);
        playQueued(0.0);
        return;
    default:
        emit toastRequested(tr("Nothing here plays %1").arg(entry.name.section('.', -1)), 2500);
        return;
    }
}

/* ── pictures ────────────────────────────────────────────────────────────── */

void MediaPage::openImage(const Entry &entry)
{
    QImageReader reader(entry.path);
    reader.setAutoTransform(true);   /* honour the EXIF rotation a phone camera writes */

    /*
     * Scaled ON THE WAY IN rather than after loading.  A 12 megapixel JPEG is 48 MB
     * as an ARGB32 QImage and this board has no business allocating that to show it
     * at 640x480; QImageReader::setScaledSize lets the JPEG decoder do it during the
     * DCT pass instead.
     */
    const QSize full = reader.size();
    if (full.isValid() && (full.width() > width() || full.height() > height())) {
        QSize target = full;
        target.scale(size(), Qt::KeepAspectRatio);
        reader.setScaledSize(target);
    }

    const QImage image = reader.read();
    if (image.isNull()) {
        const QList<QByteArray> supported = QImageReader::supportedImageFormats();
        const QString suffix = QFileInfo(entry.path).suffix().toLower();
        if (!supported.contains(suffix.toLatin1())
            && !(suffix == "jpg" && supported.contains("jpeg"))) {
            /* The honest diagnosis: the plugin is missing, not the file. */
            emit toastRequested(tr("No %1 image plugin is staged.\nQt here reads: %2")
                                    .arg(suffix.toUpper(),
                                         QString::fromLatin1(supported.join(", "))),
                                5000);
        } else {
            emit toastRequested(tr("Could not read %1: %2").arg(entry.name, reader.errorString()),
                                4000);
        }
        return;
    }

    /*
     * A PICTURE DOES NOT STOP THE MUSIC.  The old page called stopPlayback() here,
     * which killed the album to show a photograph -- the single least defensible
     * thing the old card did.  Only a film stops music, because only a film needs
     * the sound card.
     */
    stopVideo();
    /* A picture is the software path always, so whatever the GPU was holding for
     * a film goes now -- and it has to go before m_frame is set, because that is
     * the one thing dropFrame() clears which is about to be wanted. */
    dropFrame();
    m_frame = image;
    m_showing = entry;
    m_note.clear();
    setView(ViewImage);
    refresh();
}

void MediaPage::stepImage(int delta)
{
    /* Next and previous picture in the same directory, skipping everything that
     * is not one. */
    int at = -1;
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].path == m_showing.path)
            at = i;
    if (at < 0)
        return;

    for (int i = at + delta; i >= 0 && i < m_entries.size(); i += delta) {
        if (m_entries[i].kind != KindImage)
            continue;
        openImage(m_entries[i]);
        return;
    }
}

/* ── ffmpeg, aplay and the card ──────────────────────────────────────────── */

QString MediaPage::ffmpegPath() const
{
    static const QString path = firstExisting(QStringList()
                                              << "/usr/bin/ffmpeg" << "/bin/ffmpeg"
                                              << "/usr/local/bin/ffmpeg");
    return path;
}

QString MediaPage::aplayPath() const
{
    static const QString path = firstExisting(QStringList()
                                              << "/usr/bin/aplay" << "/bin/aplay");
    return path;
}

QString MediaPage::alsaDevice() const
{
    /*
     * THE LOWEST-NUMBERED PLAYBACK PCM, BY NAME, AND NOT `default'.
     *
     * `default' on this image is a trap: finishing_touches.sh links
     * /etc/asound.conf to /home/virtua/.asoundrc, and that file is the RG351MP's
     * -- plug over dmix over hw:0,0, with an RK3326-era 44100/1024/4096 geometry
     * hard-coded into it.  dmix exists so several processes can share one card.
     * This handheld has exactly one audio consumer, which is this page, so the
     * layer bought nothing and stood between the player and the only DAC on the
     * machine.  `plughw:C,D' is resolved inside alsa-lib's own definitions, so
     * nothing in /etc/asound.conf can redirect it, and it still does the rate,
     * format and channel conversion `default' would have done.
     *
     * Read from /dev/snd rather than assumed to be card 0: a USB headset or an
     * HDMI adapter that enumerated first would make card 0 something else, and the
     * answer is wanted before the pipeline is built, not found out afterwards.
     *
     * Not cached.  The card is a module the boot word may or may not have loaded,
     * and it can go away with the adapter it came in on.  One readdir on a devtmpfs
     * per track is not worth a stale answer.
     */
    const QDir snd(QStringLiteral("/dev/snd"));
    if (!snd.exists())
        return QString();

    /* pcmC0D0p -- the trailing p is playback; capture-only devices end in c. */
    const QStringList nodes = snd.entryList(QStringList() << QStringLiteral("pcmC*D*p"),
                                            QDir::System | QDir::NoDotAndDotDot,
                                            QDir::Name);
    int bestCard = -1;
    int bestDev = -1;
    for (const QString &node : nodes) {
        const int cAt = node.indexOf('C');
        const int dAt = node.indexOf('D');
        if (cAt < 0 || dAt <= cAt || dAt + 2 > node.size())
            continue;
        bool okCard = false;
        bool okDev = false;
        const int card = node.mid(cAt + 1, dAt - cAt - 1).toInt(&okCard);
        const int dev = node.mid(dAt + 1, node.size() - dAt - 2).toInt(&okDev);
        if (!okCard || !okDev)
            continue;
        if (bestCard < 0 || card < bestCard || (card == bestCard && dev < bestDev)) {
            bestCard = card;
            bestDev = dev;
        }
    }

    if (bestCard < 0)
        return QString();
    return QString("plughw:%1,%2").arg(bestCard).arg(bestDev);
}

QString MediaPage::mixerComplaint() const
{
    /*
     * A MUTED CARD IS THE OTHER WAY TO HAVE NO SOUND, and it is not the player's
     * to fix silently: unmuting behind the user's back is how a device ends up
     * blaring in a quiet room.  Say it, on the row under the track, and leave the
     * two volume keys to do the fixing.
     */
    int percent = -1;
    bool muted = false;
    if (!Volume::read(&percent, &muted))
        return QString();
    if (muted)
        return tr("the output is muted -- press VOL+");
    if (percent == 0)
        return tr("the volume is at zero -- press VOL+");
    return QString();
}

bool MediaPage::refreshMixerNote()
{
    const QString was = m_mixerNote;
    m_mixerNote = mixerComplaint();
    /*
     * Stamped AFTER the derive, because the derive can move the counter itself:
     * mixerComplaint() goes through Volume::read(), which forks amixer and hands
     * the answer to remember(), and remember() counts an answer that differs from
     * the cached one as a change -- which, on the first look after somebody else
     * moved the mixer, it is.  Stamping beforehand would leave that move
     * outstanding and buy a second fork on the next tick to discover the same
     * thing.  Reading it here takes both moves at once.
     */
    m_volGeneration = Volume::generation();
    return m_mixerNote != was;
}

bool MediaPage::ffmpegHasAlsa() const
{
    /*
     * Probed once and cached.  Debian's ffmpeg is built with the alsa indev and
     * outdev, but this is exactly the sort of thing that a stripped rebuild drops,
     * and the failure without the check is silent video with no sound and no
     * message.
     *
     * THE TIMEOUTS ARE NOT ARBITRARY AND THEY WERE TOO SHORT.  They were 1.5 s to
     * start and 3 s to finish, which is generous on a desktop and not nearly
     * enough here: ffmpeg drags in libavdevice, libavfilter, libavformat and
     * something like two hundred shared objects, and this is an A7 reading them
     * off an SD card with a cold page cache.  A first play after boot could
     * overrun that easily -- and the old code then cached the timeout as a
     * definite `no alsa muxer', for the lifetime of the process, on an ffmpeg
     * that has the muxer.
     *
     * So: a long window, and an inconclusive probe is not an answer.  A timeout
     * leaves the cache unset and the next attempt asks again, by which point the
     * binary and its libraries are in page cache and it returns immediately.
     */
    static int cached = -1;
    if (cached >= 0)
        return cached != 0;

    if (ffmpegPath().isEmpty())
        return false;

    QProcess p;
    p.start(ffmpegPath(), QStringList() << "-hide_banner" << "-devices");
    if (!Shell::waitForStarted(p, 8000))
        return false;
    if (!Shell::waitForFinished(p, 15000)) {
        p.kill();
        Shell::waitForFinished(p, 500);
        return false;
    }

    cached = 0;

    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput())
                        + QString::fromLocal8Bit(p.readAllStandardError());
    const QStringList lines = out.split('\n');
    for (const QString &line : lines) {
        /* " DE alsa            ALSA audio output" -- the E is what matters. */
        const QString t = line.trimmed();
        if (!t.contains(QStringLiteral(" alsa")) && !t.startsWith(QStringLiteral("DE alsa"))
            && !t.startsWith(QStringLiteral(" E alsa")))
            continue;
        const QString flags = t.left(3);
        if (flags.contains('E')) {
            cached = 1;
            break;
        }
    }
    return cached != 0;
}

double MediaPage::probeDuration(const QString &path) const
{
    const QString ffprobe = firstExisting(QStringList()
                                          << "/usr/bin/ffprobe" << "/bin/ffprobe");
    if (!ffprobe.isEmpty()) {
        QProcess p;
        p.start(ffprobe, QStringList()
                             << "-v" << "error"
                             << "-show_entries" << "format=duration"
                             << "-of" << "default=noprint_wrappers=1:nokey=1"
                             << path);
        if (Shell::waitForStarted(p, 1500) && Shell::waitForFinished(p, 4000)) {
            bool ok = false;
            const double d = QString::fromLatin1(p.readAllStandardOutput()).trimmed().toDouble(&ok);
            if (ok && d > 0.0)
                return d;
        } else {
            p.kill();
            Shell::waitForFinished(p, 500);
        }
    }

    /* No ffprobe: ask ffmpeg to open the file and say what it found.  It exits 1
     * for having no output, which is expected and not an error here. */
    if (ffmpegPath().isEmpty())
        return 0.0;
    QProcess p;
    p.start(ffmpegPath(), QStringList() << "-hide_banner" << "-i" << path);
    if (!Shell::waitForStarted(p, 1500) || !Shell::waitForFinished(p, 4000)) {
        p.kill();
        Shell::waitForFinished(p, 500);
        return 0.0;
    }
    const QString err = QString::fromLocal8Bit(p.readAllStandardError());
    const int at = err.indexOf(QStringLiteral("Duration:"));
    if (at < 0)
        return 0.0;
    const QString stamp = err.mid(at + 9, 11).trimmed();   /* HH:MM:SS.ss */
    const QStringList parts = stamp.split(':');
    if (parts.size() != 3)
        return 0.0;
    return parts.at(0).toDouble() * 3600.0 + parts.at(1).toDouble() * 60.0
           + parts.at(2).toDouble();
}

bool MediaPage::probeHasAudio(const QString &path) const
{
    /*
     * Asked because the film's sound is a SEPARATE ffmpeg now, and a process that
     * is started on a silent clip does not sit there quietly: it exits at once with
     * "Stream map '0:a:0' matches no streams", which this page would then paint
     * under the picture as though something had broken.  The old single-process
     * form got this for free from the `?' on its map, and the `?' cannot be used
     * here -- an output with no streams in it is its own error.
     *
     * TRUE WHEN THERE IS NO FFPROBE, which is the useful way to be wrong: nearly
     * every film has sound, so assuming it plays the ones that do and costs a note
     * on the ones that do not.  Assuming the other way would silence everything on
     * an image that happens to ship ffmpeg without ffprobe.
     *
     * Probed alongside the duration and kept for the same lifetime, so seeking --
     * which is openVideo() again with a start time -- does not pay for it twice.
     */
    const QString ffprobe = firstExisting(QStringList()
                                          << "/usr/bin/ffprobe" << "/bin/ffprobe");
    if (ffprobe.isEmpty())
        return true;

    QProcess p;
    p.start(ffprobe, QStringList()
                         << "-v" << "error"
                         << "-select_streams" << "a:0"
                         << "-show_entries" << "stream=index"
                         << "-of" << "default=noprint_wrappers=1:nokey=1"
                         << path);
    if (!Shell::waitForStarted(p, 1500) || !Shell::waitForFinished(p, 4000)) {
        p.kill();
        Shell::waitForFinished(p, 500);
        return true;
    }
    return !QString::fromLatin1(p.readAllStandardOutput()).trimmed().isEmpty();
}

QString MediaPage::probeTitle(const QString &path) const
{
    /*
     * The tags, so the player shows "Blue Monday" and not "03 - bm (1).mp3".
     *
     * WITH the key printed and parsed by name, not with nokey=1: ffprobe emits
     * only the tags that exist, in the order the container stores them, so two
     * bare lines cannot be told apart -- a file with an artist and no title would
     * put the artist where the title goes.
     */
    const QString ffprobe = firstExisting(QStringList()
                                          << "/usr/bin/ffprobe" << "/bin/ffprobe");
    if (ffprobe.isEmpty())
        return QString();

    QProcess p;
    p.start(ffprobe, QStringList()
                         << "-v" << "error"
                         << "-show_entries" << "format_tags=title,artist"
                         << "-of" << "default=noprint_wrappers=1"
                         << path);
    if (!Shell::waitForStarted(p, 1500) || !Shell::waitForFinished(p, 4000)) {
        p.kill();
        Shell::waitForFinished(p, 500);
        return QString();
    }

    QString title;
    QString artist;
    const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split('\n');
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (t.startsWith(QStringLiteral("TAG:title=")))
            title = t.mid(10).trimmed();
        else if (t.startsWith(QStringLiteral("TAG:artist=")))
            artist = t.mid(11).trimmed();
    }

    if (title.isEmpty())
        return QString();
    if (artist.isEmpty())
        return title;
    return artist + QStringLiteral(" - ") + title;
}

QStringList MediaPage::audioDecodeArgs(const QString &path, double startAt,
                                       const QString &alsaSink) const
{
    QStringList args;
    args << "-nostdin" << "-hide_banner" << "-loglevel" << "error";
    /*
     * -ss BEFORE -i is the fast form: it jumps by the container index rather than
     * decoding and throwing away everything up to the point, which on this CPU is
     * the difference between instant and half a minute.
     */
    if (startAt > 0.0)
        args << "-ss" << QString::number(startAt, 'f', 2);
    args << "-i" << path
         << "-vn"                       /* cover art is a video stream, and it is not wanted */
         << "-map" << "0:a:0"
         << "-ar" << QString::number(kRate) << "-ac" << "2";

    /*
     * THE RATE AND THE CHANNEL COUNT ARE FORCED IN BOTH FORMS, and that matters
     * more in the alsa one.  plughw would convert a 44.1 kHz file to whatever the
     * AFE is running at, but it would do it inside alsa-lib on the very thread
     * that is feeding the card -- and that thread has a deadline, because this
     * AFE has no playback interrupt and the driver polls the DL1 cursor from a
     * work item.  ffmpeg's resampler runs before the write instead, where being
     * late costs a buffer rather than a gap.
     */
    if (alsaSink.isEmpty())
        args << "-f" << "s16le" << "-";
    else
        args << "-f" << "alsa" << alsaSink;
    return args;
}

/* ── video ───────────────────────────────────────────────────────────────── */

void MediaPage::openVideo(const Entry &entry, double startAt)
{
    /* By value before anything is torn down: commitSeek() passes m_showing, and
     * the assignment below would otherwise be reading a member it is writing. */
    const Entry item = entry;

    if (ffmpegPath().isEmpty()) {
        emit toastRequested(tr("ffmpeg is not installed.\nInstall it from Packages."), 4000);
        return;
    }

    /* A film takes the sound card, so the record has to come off first.  This is
     * the one direction the asymmetry in onLeave() runs the other way. */
    stopMusic();
    stopVideo();

    m_showing = item;
    m_buffer.clear();
    m_framesShown = 0;
    m_framesDropped = 0;
    m_paused = false;
    m_note.clear();
    if (m_videoDuration <= 0.0 || startAt <= 0.0) {
        m_videoDuration = probeDuration(item.path);
        m_videoHasAudio = probeHasAudio(item.path);
    }

    /* Even dimensions: several of the scalers and every yuv420 path want them, and
     * an odd width is a whole class of "ffmpeg exited 1" that is not worth having. */
    const int wasW = m_frameW, wasH = m_frameH;
    m_frameW = (width() / 2) * 2;
    m_frameH = (height() / 2) * 2;
    if (m_frameW < 16 || m_frameH < 16) {
        m_frameW = 640;
        m_frameH = 480;
    }

    /*
     * stopVideo() deliberately keeps the last frame so that a seek does not blank
     * the screen while ffmpeg loads libavcodec.  That only holds while the frame
     * still describes itself: m_planes carries no dimensions of its own, and a
     * geometry that has changed under it would be read with the new strides and
     * come out as a diagonal smear.  Cheaper to lose one frame than to show that.
     */
    if (m_frameW != wasW || m_frameH != wasH) {
        m_planes.clear();
        m_glShown = false;
    }

    /*
     * ── WHICH OF THE TWO PATHS THIS FILM TAKES, decided here and not changed ──
     *
     * With the GPU: ffmpeg hands over the planar frame the decoder already
     * produced, lima does the colour conversion in a fragment shader and writes
     * the result straight into the memory the panel is scanning.  Without it:
     * swscale converts to bgra on the CPU and the frame is copied four more times
     * on its way to /dev/fb0.  The difference per frame at 640x480 is 460 KB
     * against seven and a half megabytes -- see tools/mixdash/glvideo.h.
     *
     * The window has to be the whole framebuffer, because the GPU is given
     * framebuffer coordinates and there is no compositor here to translate them.
     * That is the normal case on this board and the HDMI mirror is a separate
     * process, so this is a guard rather than a limitation.
     */
    m_gl = nullptr;
    if (!m_glOff && window() && window()->size() == GlVideo::instance()->size() &&
        GlVideo::instance()->available())
        m_gl = GlVideo::instance();

    const QString filter = QString("scale=w=%1:h=%2:force_original_aspect_ratio=decrease,"
                                   "pad=%1:%2:(ow-iw)/2:(oh-ih)/2,format=%3")
                               .arg(m_frameW).arg(m_frameH)
                               .arg(m_gl ? "yuv420p" : "bgra");

    QStringList args;
    args << "-nostdin" << "-hide_banner" << "-loglevel" << "error" << "-re";
    if (startAt > 0.0)
        args << "-ss" << QString::number(startAt, 'f', 2);
    args << "-i" << item.path
         << "-map" << "0:v:0"
         << "-vf" << filter
         << "-f" << "rawvideo" << "-pix_fmt" << (m_gl ? "yuv420p" : "bgra")
         << "-an" << "pipe:1";

    /*
     * The card is asked about FIRST and separately, because the two failures want
     * different words.  No card is a boot-word or a driver matter and no ffmpeg
     * will fix it; no muxer is an ffmpeg matter and the card is fine.  Reporting
     * either as the other is what sends someone rebuilding the wrong half.
     */
    m_device = alsaDevice();
    const bool alsa = !m_device.isEmpty() && ffmpegHasAlsa();

    m_decoder = new QProcess(this);
    m_decoder->setReadChannel(QProcess::StandardOutput);
    connect(m_decoder, &QProcess::readyReadStandardOutput, this, &MediaPage::readFrames);
    connect(m_decoder, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, &MediaPage::onDecoderFinished);
    m_decoder->start(ffmpegPath(), args);

    /*
     * ── THE SOUND IS ITS OWN PROCESS, AND THAT IS THE FIX FOR CHOPPY FILMS ──
     *
     * It used to be the same ffmpeg: one command with two outputs, rawvideo on
     * pipe:1 and the sound on `-f alsa'.  That reads well and it starves the DAC.
     *
     * A frame here is 640x480x4 -- 1.2 MB -- and a pipe holds 64 KiB, so ffmpeg is
     * blocked in write(2) for almost the whole of every frame, waiting for this
     * process to come round the event loop and drain it.  A muxer is one thread:
     * while it is blocked on the pipe it is not calling snd_pcm_writei either.  The
     * card's ring is 64 KiB, which at 48 kHz stereo is 341 ms, so any single stall
     * longer than that -- one full-screen repaint that runs long, one directory
     * listing, one page of the card read cold -- is a hole in the sound.  That is
     * exactly what "the audio is extremely choppy" sounds like, and it gets worse
     * the more the picture costs, which is backwards.
     *
     * Two processes cannot do that to each other.  The decoder blocks on the pipe
     * as before and nothing else is behind it; the sound has its own demux, its own
     * decode -- cheap, `-vn' and one stream -- and blocks only on the card.
     *
     * WHAT IT COSTS is a shared clock: the picture is paced by ffmpeg's `-re' off
     * the wall clock and the sound by the AFE's own 48 kHz, so they drift by
     * whatever those two disagree by.  That is parts per million on this SoC, it
     * starts from the same timestamp because both are given the same `startAt', and
     * every seek restarts both.  A few milliseconds an hour against a hole in the
     * sound every few seconds is not a close call.
     */
    if (m_device.isEmpty()) {
        /*
         * Nothing to play into, so no sound chain is started at all -- an ffmpeg
         * that cannot open a device is a process failing in the background while
         * the screen says nothing.  The note is the whole of the response.
         */
        m_note = tr("no sound card on this device");
    } else if (!m_videoHasAudio) {
        m_note.clear();                 /* a silent clip, and that is not a fault */
        refreshMixerNote();
    } else if (alsa) {
        m_videoAudio = new QProcess(this);
        connect(m_videoAudio, &QProcess::readyReadStandardError,
                this, &MediaPage::onChildStderr);
        m_videoAudio->start(ffmpegPath(), audioDecodeArgs(item.path, startAt, m_device));
        m_note.clear();
        refreshMixerNote();
    } else {
        /*
         * No alsa outdev in this ffmpeg, so the sound goes down a pipe into aplay
         * instead.  Same shape, one more process, and the same drift.
         */
        const QString aplay = aplayPath();
        if (!aplay.isEmpty()) {
            m_videoAudio = new QProcess(this);
            m_videoAplay = new QProcess(this);
            m_videoAudio->setStandardOutputProcess(m_videoAplay);
            connect(m_videoAplay, &QProcess::readyReadStandardError,
                    this, &MediaPage::onChildStderr);
            m_videoAudio->start(ffmpegPath(), audioDecodeArgs(item.path, startAt, QString()));
            m_videoAplay->start(aplay, QStringList()
                                           << "-q" << "-D" << m_device
                                           << "-t" << "raw" << "-f" << "S16_LE"
                                           << "-r" << QString::number(kRate) << "-c" << "2"
                                           << "-");
            m_note = tr("audio through aplay -- no alsa muxer in ffmpeg");
        } else {
            m_note = tr("aplay is not installed (alsa-utils)");
        }
    }

    m_clock.restart();
    m_pausedAt = (qint64)(startAt * 1000.0);
    setView(ViewVideo);
    refresh();
}

void MediaPage::readFrames()
{
    if (!m_decoder)
        return;

    /*
     * yuv420p is twelve bits a pixel, bgra is thirty-two.  That ratio is most of
     * what the GPU path buys before a single instruction runs on the GPU: it is
     * the size of every write ffmpeg makes into a 64 KiB pipe and of every read
     * this slot makes out of it.
     */
    const int frameBytes = m_gl ? (m_frameW * m_frameH * 3) / 2
                                : m_frameW * m_frameH * 4;
    if (frameBytes <= 0)
        return;

    m_buffer.append(m_decoder->readAllStandardOutput());
    if (m_buffer.size() < frameBytes)
        return;

    /*
     * DROP TO THE NEWEST.  If more than one whole frame is waiting, the decoder ran
     * ahead of the paint -- which on this CPU happens the moment anything else asks
     * for time.  Painting the older ones would put the picture further behind the
     * sound with every frame; skipping them keeps the two together and the count of
     * what was skipped goes on the screen instead of being hidden.
     */
    const int whole = m_buffer.size() / frameBytes;
    if (whole > 1)
        m_framesDropped += whole - 1;

    const int offset = (whole - 1) * frameBytes;

    if (m_gl) {
        /* Kept rather than consumed: a paused film and a repaint forced from
         * outside both have to put this same frame back, and there is nowhere
         * else to get it from once the GPU has drawn it into scanout memory. */
        m_planes = m_buffer.mid(offset, frameBytes);
        m_buffer.remove(0, whole * frameBytes);
        ++m_framesShown;
        present();
        return;
    }

    m_frame = QImage((const uchar *)m_buffer.constData() + offset,
                     m_frameW, m_frameH, m_frameW * 4, QImage::Format_RGB32).copy();
    m_buffer.remove(0, whole * frameBytes);
    ++m_framesShown;

    if (isVisible())
        update();
}

void MediaPage::refresh()
{
    /*
     * The GPU path repaints by re-presenting: the same frame goes back up with
     * whatever the strip says now, which is how the clock ticks and the note
     * appears without Qt ever touching the film.  Everywhere else this is the
     * update() it replaced.
     */
    if (glOwnsScreen())
        present();
    else
        update();
}

void MediaPage::present()
{
    if (!m_gl || m_view != ViewVideo || !isVisible() || m_planes.isEmpty())
        return;

    const int w = m_frameW, h = m_frameH;
    const int ysize = w * h;
    const int csize = (w / 2) * (h / 2);
    if (w <= 0 || h <= 0 || m_planes.size() < ysize + 2 * csize)
        return;

    /*
     * Framebuffer coordinates, taken fresh every frame rather than cached.  This
     * page moves: it is inset below the status bar while the browser is up and
     * takes the whole screen when a film starts, and applyChrome() does that to it
     * from outside.  A cached rectangle would put the film in the wrong place for
     * exactly one frame after every one of those, which is the kind of thing that
     * shows as a flicker nobody can reproduce.
     */
    const QRect into(mapToGlobal(QPoint(0, 0)), size());
    if (!QRect(QPoint(0, 0), m_gl->size()).contains(into))
        return;

    refreshChrome(into);

    const uchar *const p = (const uchar *)m_planes.constData();
    if (m_gl->drawFrame(p, w,
                        p + ysize, w / 2,
                        p + ysize + csize, w / 2,
                        w, h, into)) {
        m_glShown = true;
        return;
    }

    /*
     * The driver gave up.  ffmpeg is emitting planes the software painter has no
     * use for, so there is nothing to fall back TO without restarting it -- and
     * that cannot happen from inside a readyRead slot on the very process it would
     * be killing.  Queued, with the position it had reached.
     */
    m_glOff = true;
    m_glShown = false;
    m_glRestartAt = position();
    QTimer::singleShot(0, this, &MediaPage::restartWithoutGl);
}

void MediaPage::restartWithoutGl()
{
    if (m_view != ViewVideo || m_showing.path.isEmpty())
        return;
    openVideo(m_showing, m_glRestartAt);
    /* After, not before: openVideo() clears the note and then writes its own
     * account of the sound into it, and that one matters more than this one. */
    if (m_note.isEmpty())
        m_note = tr("the GPU stopped answering -- back on the software path");
}

void MediaPage::onDecoderFinished()
{
    if (!m_decoder || sender() != m_decoder)
        return;

    const QString err = QString::fromLocal8Bit(m_decoder->readAllStandardError()).trimmed();
    const int code = m_decoder->exitCode();

    if (code != 0 && !err.isEmpty())
        m_note = tidyChildError(err);
    else if (code != 0)
        m_note = tr("ffmpeg exited %1").arg(code);
    else
        m_note = tr("end of file");

    /* The picture stays on the glass with the note under it, rather than dropping
     * back to the list -- which is what the old card did, and why the only thing
     * anybody ever saw of a failure was a toast that said "exited 1". */
    refresh();
}

/* ── the queue ───────────────────────────────────────────────────────────── */

void MediaPage::queueDirectory(const QString &dir, const QString &startWith)
{
    /* Both taken by value at the top: populate() is not called from here, but
     * startWith is an m_entries path in every current caller and a future one
     * that repopulates first would find it dangling. */
    const QString want = startWith;

    m_queue.clear();
    m_order.clear();
    m_orderAt = -1;

    QDir d(dir);
    if (!d.exists())
        return;

    const QFileInfoList infos = d.entryInfoList(QDir::Files | QDir::NoDotAndDotDot,
                                                QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &info : infos) {
        if (info.fileName().startsWith('.'))
            continue;
        if (kindFor(info) != KindAudio)
            continue;
        Entry e;
        e.path = info.absoluteFilePath();
        e.name = info.fileName();
        e.kind = KindAudio;
        e.size = info.size();
        m_queue.append(e);
    }
    if (m_queue.isEmpty())
        return;

    reshuffle();   /* builds the order, identity or permuted */

    for (int i = 0; i < m_order.size(); ++i) {
        if (m_queue[m_order[i]].path != want)
            continue;
        m_orderAt = i;
        break;
    }
    if (m_orderAt < 0)
        m_orderAt = 0;
}

void MediaPage::reshuffle()
{
    /* Which track is playing, as an index into m_queue -- read BEFORE the order is
     * rebuilt, because m_orderAt means nothing across the rebuild. */
    const int playing = queuedIndex();

    m_order.clear();
    m_order.reserve(m_queue.size());
    for (int i = 0; i < m_queue.size(); ++i)
        m_order.append(i);

    if (m_shuffle) {
        /* Fisher-Yates, with Qt's generator rather than <random>: it is seeded
         * already, it is the one this program uses everywhere else, and a
         * shuffle is not worth an extra header. */
        for (int i = m_order.size() - 1; i > 0; --i) {
            const int j = (int)QRandomGenerator::global()->bounded(i + 1);
            const int t = m_order[i];
            m_order[i] = m_order[j];
            m_order[j] = t;
        }
    }

    /* Land back on the same track wherever the permutation put it.  Turning
     * shuffle on mid-album must not restart what you are listening to. */
    m_orderAt = -1;
    if (playing < 0)
        return;
    for (int i = 0; i < m_order.size(); ++i) {
        if (m_order[i] != playing)
            continue;
        m_orderAt = i;
        break;
    }
}

int MediaPage::queuedIndex() const
{
    if (m_orderAt < 0 || m_orderAt >= m_order.size())
        return -1;
    const int i = m_order[m_orderAt];
    return (i >= 0 && i < m_queue.size()) ? i : -1;
}

const MediaPage::Entry *MediaPage::queuedTrack() const
{
    const int i = queuedIndex();
    return i < 0 ? nullptr : &m_queue[i];
}

void MediaPage::playQueued(double startAt)
{
    const Entry *at = queuedTrack();
    if (!at)
        return;
    /* By value: everything below can touch m_queue, and a pointer into a QVector
     * that reallocates is the same use-after-free the header warns about. */
    const Entry track = *at;

    const QString ff = ffmpegPath();
    if (ff.isEmpty()) {
        emit toastRequested(tr("ffmpeg is not installed"), 4000);
        return;
    }

    m_device = alsaDevice();
    if (m_device.isEmpty()) {
        m_note = tr("no sound card on this device");
        emit toastRequested(m_note, 4000);
        rebuild();
        return;
    }

    /*
     * ONE PROCESS, AND APLAY IS THE FALLBACK RATHER THAN THE PATH.
     *
     * A film has always played its sound by handing ffmpeg the card and letting it
     * write with `-f alsa'.  A song went through a pipe into aplay instead, for no
     * reason beyond the order the two were written -- and that difference is what
     * the board reported: films had sound and music did not, because aplay on this
     * rootfs dies before it opens anything with
     *
     *     aplay: symbol lookup error: undefined symbol: snd_pcm_subformat_value
     *
     * which is an alsa-utils newer than the libasound.so.2 it resolves against.
     * Nothing in this page can fix that pairing.  What it can do is stop depending
     * on it, and there was never anything to gain: aplay's only job here was to
     * hold a 64 KiB pipe and call snd_pcm_writei, which is exactly what ffmpeg's
     * alsa outdev does one buffer earlier and in the process that already has the
     * samples.  One process, one fewer buffer between the decoder and the DAC, and
     * one fewer binary that has to be installed and has to work.
     *
     * The old chain stays for an ffmpeg built without the alsa outdev, because
     * that is a real build and the answer for it is still a pipe into aplay.
     */
    const bool direct = ffmpegHasAlsa();
    const QString ap = direct ? QString() : aplayPath();
    if (!direct && ap.isEmpty()) {
        emit toastRequested(tr("aplay is not installed (alsa-utils)"), 4000);
        return;
    }

    /* Replace the chain without touching the queue position -- this is a restart,
     * not a stop, and stopMusic() would clear the very index we are playing. */
    m_stopping = true;
    endProcess(m_music);
    endProcess(m_aplay);
    m_stopping = false;

    m_paused = false;
    m_childSaid.clear();
    m_note.clear();
    refreshMixerNote();
    if (startAt <= 0.0 || m_duration <= 0.0) {
        m_duration = probeDuration(track.path);
        m_trackTitle = probeTitle(track.path);
    }

    m_music = new QProcess(this);
    connect(m_music, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, &MediaPage::onMusicFinished);
    /*
     * THE STDERR OF WHATEVER OPENS THE CARD IS READ.  It was not, which is why a
     * card that would not open produced silence and no message anywhere: the
     * process died, its partner took EPIPE, and the only trace of the reason was
     * in a pipe nobody was holding.
     */
    connect(m_music, &QProcess::readyReadStandardError, this, &MediaPage::onChildStderr);

    if (!direct) {
        m_aplay = new QProcess(this);
        /*
         * Raw s16le into `aplay -t raw' rather than a WAV stream: a WAV header
         * written to a pipe has to lie about its length, and while aplay copes with
         * that, the raw form has no header to be wrong.  Every field aplay needs is
         * on its own command line instead.
         */
        m_music->setStandardOutputProcess(m_aplay);
        /*
         * BOTH ends of the chain report, into the same slot.  ffmpeg's exit is the
         * interesting one when it fails; aplay's is the interesting one when it does
         * not, because aplay is still a third of a second of pipe behind ffmpeg when
         * ffmpeg says it is done.  onMusicFinished() sorts out which is which.
         */
        connect(m_aplay, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this, &MediaPage::onMusicFinished);
        connect(m_aplay, &QProcess::readyReadStandardError, this, &MediaPage::onChildStderr);
    }

    m_music->start(ff, audioDecodeArgs(track.path, startAt, direct ? m_device : QString()));
    if (m_aplay)
        m_aplay->start(ap, QStringList()
                               << "-q" << "-D" << m_device
                               << "-t" << "raw" << "-f" << "S16_LE"
                               << "-r" << QString::number(kRate) << "-c" << "2"
                               << "-");

    m_clock.restart();
    m_pausedAt = (qint64)(startAt * 1000.0);

    rebuild();
    emit titleChanged();
}

void MediaPage::advance(int delta, bool automatic)
{
    if (m_order.isEmpty()) {
        stopMusic();
        return;
    }

    /* Repeat-one acts on a track that ENDED BY ITSELF and on nothing else --
     * pressing Next on a repeat-one track has to move, or the button is a lie. */
    if (automatic && m_repeat == RepeatOne) {
        playQueued(0.0);
        return;
    }

    int next = m_orderAt + delta;
    if (next < 0 || next >= m_order.size()) {
        if (automatic && m_repeat == RepeatOff) {
            stopMusic();
            m_note = tr("end of folder");
            rebuild();
            return;
        }
        /* Off either end, wrap.  A deliberate press at the last track wraps even
         * with repeat off: the alternative is a button that does nothing and says
         * nothing, and the queue is one directory, not a lifetime of listening. */
        next = (next < 0) ? m_order.size() - 1 : 0;
    }

    m_orderAt = next;
    playQueued(0.0);
}

void MediaPage::stopMusic()
{
    m_stopping = true;
    endProcess(m_music);
    endProcess(m_aplay);
    m_stopping = false;

    /* The queue is kept, the POSITION is not: nothing is playing, so nothing is
     * "now playing", and every row and badge that asks queuedTrack() goes quiet in
     * one place.  This is the stop the old page did not have. */
    m_orderAt = -1;
    m_paused = false;
    m_pausedAt = 0;
    m_duration = 0.0;
    m_trackTitle.clear();
    m_seekTimer->stop();
    m_seekTarget = -1.0;

    if (m_view == ViewPlayer)
        setView(ViewBrowse);
    else
        rebuild();
    emit titleChanged();
}

void MediaPage::stopVideo()
{
    endProcess(m_decoder);
    endProcess(m_videoAudio);
    endProcess(m_videoAplay);
    m_buffer.clear();
    m_framesShown = 0;
    m_framesDropped = 0;
    m_seekTimer->stop();
    m_seekTarget = -1.0;
    /*
     * m_videoDuration is deliberately KEPT.  Seeking is stopVideo() followed by
     * openVideo() with a start time, and openVideo only re-probes when it is
     * opening from the beginning -- so clearing it here would put an ffprobe of the
     * whole container in front of every ten-second nudge of the D-pad.  A stale
     * value cannot leak into the next film: that one opens at 0 and re-probes.
     *
     * NOR IS THE PICTURE DROPPED, for the same reason: the frame already on the
     * glass -- m_frame on the software path, m_planes on the GPU one -- stays
     * there while the new ffmpeg loads libavcodec, which on this CPU is a visible
     * fraction of a second.  dropFrame() is for leaving a film, not restarting it.
     */
}

void MediaPage::dropFrame()
{
    m_frame = QImage();
    m_planes.clear();
    m_chromeKey.clear();
    m_glShown = false;
    if (m_gl)
        m_gl->clearOverlays();
    m_gl = nullptr;
}

/* ── the transport ───────────────────────────────────────────────────────── */

void MediaPage::togglePause()
{
    /*
     * SIGSTOP and SIGCONT rather than a pause protocol, because neither ffmpeg on a
     * pipe nor aplay has one.  Stopping the writer stops the pipe filling; stopping
     * aplay as well stops the sound card being fed.  It is exactly what ^Z does in
     * a shell, and it resumes with the buffers intact.
     */
    const int sig = m_paused ? SIGCONT : SIGSTOP;
    QProcess *const all[] = { m_decoder, m_videoAudio, m_videoAplay, m_aplay, m_music };
    bool any = false;
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        QProcess *p = all[i];
        if (!p || p->state() != QProcess::Running || p->processId() <= 0)
            continue;
        ::kill((pid_t)p->processId(), sig);
        any = true;
    }
    if (!any)
        return;

    if (m_paused)
        m_clock.restart();
    else
        m_pausedAt += m_clock.elapsed();
    m_paused = !m_paused;

    rebuild();
    refresh();
}

double MediaPage::position() const
{
    return (m_pausedAt + (m_paused ? 0 : m_clock.elapsed())) / 1000.0;
}

void MediaPage::seekTo(double seconds)
{
    if (!musicLive() && !videoLive())
        return;

    const double total = videoLive() ? m_videoDuration : m_duration;
    double target = seconds;
    if (total > 0.0)
        target = qBound(0.0, target, qMax(0.0, total - 1.0));
    else
        target = qMax(0.0, target);

    /*
     * The clock moves NOW and the process restarts in a third of a second.  Both
     * halves matter: without the first the slider snaps back under the thumb that
     * is dragging it, and without the second a held D-pad spawns one ffmpeg per
     * key repeat on a CPU that takes a visible fraction of a second to load
     * libavcodec, throwing all but the last away before it decodes a frame.
     */
    m_seekTarget = target;
    m_pausedAt = (qint64)(target * 1000.0);
    m_clock.restart();
    m_seekTimer->start();

    if (m_view == ViewVideo)
        refresh();
}

void MediaPage::seekBy(int seconds)
{
    /* From where the slider says we are, which during a pending seek is the target
     * and not the process's real position -- so two quick presses of Right move
     * twenty seconds rather than ten. */
    const double from = (m_seekTarget >= 0.0 && m_seekTimer->isActive()) ? m_seekTarget
                                                                        : position();
    seekTo(from + seconds);
}

void MediaPage::commitSeek()
{
    if (m_seekTarget < 0.0)
        return;
    const double target = m_seekTarget;
    m_seekTarget = -1.0;

    /* A pipe cannot seek, so seeking is opening the file again somewhere else --
     * which is exactly what the openers already do, given a start time. */
    if (videoLive()) {
        const Entry film = m_showing;
        openVideo(film, target);
    } else if (musicLive()) {
        playQueued(target);
    }
}

void MediaPage::onMusicFinished(int code)
{
    if (m_stopping)
        return;
    QProcess *const who = qobject_cast<QProcess *>(sender());
    if (!who || (who != m_music && who != m_aplay))
        return;

    /*
     * FFMPEG FINISHING CLEANLY IS NOT THE END OF THE TRACK.  It exits as soon as the
     * last sample is in the pipe, and at that moment aplay is still holding a
     * pipe-full -- 64 KiB, which at 48 kHz stereo s16 is a third of a second -- plus
     * whatever is left in the ALSA ring.  Tearing the pair down here, which is what
     * a single handler on ffmpeg's finished() would do, cuts the last half second
     * off every song in the album and then does it again on the next one.
     *
     * So the clean case answers on APLAY's finished() instead: ffmpeg's exit closes
     * the write end, aplay reads EOF, drains, and exits by itself.  ffmpeg is only
     * disconnected here, not reaped, because it has nothing left to say.
     *
     * WITH NO APLAY THERE IS NOTHING TO WAIT FOR, and the test is on m_aplay rather
     * than on which branch playQueued() took, because that pointer IS the answer.
     * When ffmpeg writes the card itself the draining happens inside it -- the alsa
     * outdev's writes are blocking, so the last period is in the ring before it
     * returns -- and its clean exit is the end of the track.  Waiting here for a
     * second process that was never started would leave the album stopped on its
     * first song with the transport still showing it as playing.
     */
    if (m_aplay && who == m_music && code == 0 && m_music->exitStatus() == QProcess::NormalExit) {
        /* Only the connections from this process to this page.  disconnect() with
         * no arguments would be aimed at the same place but is a blunter tool than
         * the situation needs. */
        disconnect(m_music, nullptr, this, nullptr);
        return;
    }

    /* Everything both children said, read BEFORE either is reaped. */
    QString err = QString::fromLocal8Bit(who->readAllStandardError()).trimmed();
    if (m_aplay && who != m_aplay) {
        const QString ap = QString::fromLocal8Bit(m_aplay->readAllStandardError()).trimmed();
        if (!ap.isEmpty())
            err = ap;   /* aplay's complaint is the interesting one; ffmpeg only got EPIPE */
    }
    const QString whoName = (who == m_aplay) ? QStringLiteral("aplay")
                                             : QStringLiteral("ffmpeg");
    /* A child killed by a signal reports exitCode 0 -- SIGPIPE from an aplay that
     * died first is the case that matters, and reading it as a clean end of track
     * would advance the queue through every file in the folder at pipe speed. */
    const bool crashed = (who->exitStatus() != QProcess::NormalExit);

    endProcess(m_music);
    endProcess(m_aplay);

    if (code != 0 || crashed) {
        /* What it said now, else what it said earlier and onChildStderr kept, else
         * the bare fact that it is gone. */
        if (!err.isEmpty())
            m_note = tidyChildError(err);
        else if (!m_childSaid.isEmpty())
            m_note = m_childSaid;
        else
            m_note = tr("%1 exited %2").arg(whoName).arg(code);
        /*
         * A FAILURE STOPS THE QUEUE.  Advancing would run the same broken pipeline
         * over every track in the directory, one process pair per file, and the
         * message on the glass would be replaced by the next identical one before
         * anybody read it.
         */
        m_orderAt = -1;
        m_paused = false;
        if (m_view == ViewPlayer)
            setView(ViewBrowse);
        else
            rebuild();
        emit titleChanged();
        return;
    }

    advance(1, true);
}

void MediaPage::onChildStderr()
{
    QProcess *const p = qobject_cast<QProcess *>(sender());
    if (!p)
        return;
    const QString text = QString::fromLocal8Bit(p->readAllStandardError()).trimmed();
    if (text.isEmpty())
        return;
    /* Straight onto the glass.  This is the message that did not exist.  The copy
     * is for onMusicFinished, which arrives after this buffer has been drained. */
    m_childSaid = tidyChildError(text);
    m_note = m_childSaid;
    layOutList();
    refresh();
}

void MediaPage::tick()
{
    /*
     * ── THE NOTE HAS TO STOP BEING TRUE WHEN IT STOPS BEING TRUE ──
     *
     * "the output is muted -- press VOL+" is set once, where the sound chain is
     * started, and pressing VOL+ was the one thing that could not clear it: the
     * volume keys are handled by the shell and no page hears about them, which is
     * deliberate and right, and it left this page with a note telling the user to
     * do something they had already done.
     *
     * Watching a counter rather than the mixer is what makes this affordable.
     * Volume::read() forks amixer, and doing that twice a second for the length
     * of a film -- on an A7 that is also decoding one -- to re-derive a line of
     * text that is usually empty would be absurd.  The counter moves only when
     * the level or the mute actually changed, so the fork happens on the press
     * and not on the tick.
     */
    if (m_volGeneration != Volume::generation() && refreshMixerNote()) {
        layOutList();           /* the strip appearing or going costs the list a row */
        refresh();              /* the chrome key includes the note; this re-renders it */
    }

    if (m_view == ViewVideo) {
        refresh();
        return;
    }

    if (m_view == ViewPlayer) {
        /*
         * The seek row IN PLACE rather than a whole rebuild: setRows() resets the
         * scroll, and doing that twice a second under a thumb that is holding the
         * list still is the sort of thing that makes a UI feel broken without
         * anybody being able to say why.
         */
        const QVector<ListRow> &rows = m_list->rows();
        for (int i = 0; i < rows.size(); ++i) {
            if (rows[i].id != RowSeek)
                continue;
            ListRow r = rows[i];
            if (r.kind == ListRow::Slider) {
                r.value = qBound(r.minimum, (int)position(), r.maximum);
                r.valueText = humanTime(r.value) + " / " + humanTime(r.maximum);
            } else {
                r.detail = humanTime((int)position());
            }
            m_list->updateRow(i, r);
            break;
        }
        layOutList();
        return;
    }

    if (m_view == ViewBrowse && musicLive()) {
        const QVector<ListRow> &rows = m_list->rows();
        if (!rows.isEmpty() && rows[0].id == RowNowPlaying) {
            const Entry *track = queuedTrack();
            if (track) {
                ListRow r = rows[0];
                QString clock = humanTime((int)position());
                if (m_duration > 0.0)
                    clock += " / " + humanTime((int)m_duration);
                r.detail = clock + "   " + tr("open the player");
                m_list->updateRow(0, r);
            }
        }
    }
    layOutList();
}

/* ── input ───────────────────────────────────────────────────────────────── */

bool MediaPage::handleNav(int action)
{
    if (m_view == ViewImage) {
        switch (action) {
        case Joypad::NavLeft:  stepImage(-1); return true;
        case Joypad::NavRight: stepImage(1); return true;
        case Joypad::NavBack:
        case Joypad::NavOk:
            dropFrame();
            setView(ViewBrowse);
            return true;
        default:
            return true;   /* Nothing else does anything while a picture is up. */
        }
    }

    if (m_view == ViewVideo) {
        switch (action) {
        case Joypad::NavOk:
        case Joypad::NavMenu:
            togglePause();
            return true;
        case Joypad::NavLeft:  seekBy(-10); return true;
        case Joypad::NavRight: seekBy(10); return true;
        case Joypad::NavUp:    seekBy(60); return true;
        case Joypad::NavDown:  seekBy(-60); return true;
        case Joypad::NavBack:
            stopVideo();
            dropFrame();
            m_note.clear();
            setView(ViewBrowse);
            return true;
        default:
            return true;
        }
    }

    if (m_view == ViewPlayer) {
        switch (action) {
        case Joypad::NavUp:    m_list->step(-1); return true;
        case Joypad::NavDown:  m_list->step(1); return true;
        case Joypad::NavOk:    m_list->press(); return true;
        case Joypad::NavLeft:
        case Joypad::NavRight: {
            /*
             * The row first -- the seek slider and the shuffle switch both want
             * Left/Right, and adjust() says whether it took them.  Anywhere else
             * on the list they seek anyway, so the transport is never more than
             * one press away wherever the selection happens to be.
             */
            const int delta = (action == Joypad::NavLeft) ? -1 : 1;
            if (!m_list->adjust(delta))
                seekBy(delta * 10);
            return true;
        }
        case Joypad::NavMenu:
            togglePause();
            return true;
        case Joypad::NavBack:
            setView(ViewBrowse);
            return true;
        default:
            return false;
        }
    }

    /* Browsing.  LEFT AND RIGHT ARE NOT TAKEN HERE: they are how the shell moves
     * between root pages, and the old card stole them for a transport whose
     * process had usually already exited.  The transport lives in the player view,
     * one press away on the row at the top. */
    switch (action) {
    case Joypad::NavUp:    m_list->step(-1); return true;
    case Joypad::NavDown:  m_list->step(1); return true;
    case Joypad::NavOk:    m_list->press(); return true;
    case Joypad::NavMenu:
        if (musicLive() && queuedTrack()) {
            setView(ViewPlayer);
            return true;
        }
        return false;
    case Joypad::NavBack:
        /*
         * ── B IS THE WAY OUT, NOT THE WAY UP ────────────────────────────────
         *
         * B used to climb a directory and only leave the page once it ran out of
         * parents.  That made the cost of leaving depend on how deep you had
         * browsed: four directories in, getting back to the dashboard was four
         * presses, and each one redrew a directory listing nobody wanted to look
         * at on the way past.  It also meant B did two unrelated jobs, so it was
         * never possible to know what a press would do without first knowing
         * where you were.
         *
         * Now it does one job, from anywhere: return false, the shell pops the
         * page, and the dashboard is one press away from every depth.  Going UP
         * is the `..' row -- manual navigation, the same list, in the same
         * direction as going down.  Nothing is lost: `..' was always there, and
         * it is now the only thing that moves the directory.
         *
         * m_dir is untouched on the way out and populate() writes it to Settings
         * on every entry, so coming back lands where you left rather than at the
         * top -- which is what makes one-press exit cheap instead of destructive.
         */
        return false;
    default:
        return false;
    }
}

/* ── painting ────────────────────────────────────────────────────────────── */

QString MediaPage::chromeRight() const
{
    if (m_view == ViewVideo) {
        QString right = humanTime((int)position());
        if (m_videoDuration > 0.0)
            right += " / " + humanTime((int)m_videoDuration);
        if (m_paused)
            right = tr("paused") + "  " + right;
        if (m_framesDropped > 0)
            right += "  " + tr("%1 dropped").arg(m_framesDropped);
        return right;
    }

    int at = 0;
    int of = 0;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].kind != KindImage)
            continue;
        ++of;
        if (m_entries[i].path == m_showing.path)
            at = of;
    }
    return QString("%1 / %2   %3x%4").arg(at).arg(of)
               .arg(m_frame.width()).arg(m_frame.height());
}

QRect MediaPage::chromeRect() const
{
    const int barH = 34;
    const int noteH = noteText().isEmpty() ? 0 : 22;
    return QRect(0, height() - barH - noteH, width(), barH + noteH);
}

void MediaPage::paintChrome(QPainter &p) const
{
    /* A strip along the foot with the name, the clock and any complaint. */
    const int barH = 34;
    const QRect bar(0, height() - barH, width(), barH);
    p.fillRect(bar, QColor(8, 9, 14, 205));

    p.setFont(Theme::font(12));
    p.setPen(Theme::ink());
    p.drawText(bar.adjusted(10, 0, -10, 0), Qt::AlignLeft | Qt::AlignVCenter,
               m_showing.name);

    p.setPen(Theme::ink3());
    p.drawText(bar.adjusted(10, 0, -10, 0), Qt::AlignRight | Qt::AlignVCenter,
               chromeRight());

    const QString note = noteText();
    if (!note.isEmpty()) {
        const QRect noteRect(0, bar.top() - 22, width(), 22);
        p.fillRect(noteRect, QColor(8, 9, 14, 180));
        p.setFont(Theme::font(11));
        p.setPen(Theme::orange());
        p.drawText(noteRect.adjusted(10, 0, -10, 0),
                   Qt::AlignLeft | Qt::AlignVCenter, note);
    }
}

void MediaPage::refreshChrome(const QRect &into)
{
    if (!m_gl)
        return;

    const QRect cr = chromeRect();
    if (cr.isEmpty()) {
        m_gl->clearOverlay(GlVideo::ChromeLayer);
        m_chromeKey.clear();
        return;
    }

    /*
     * The whole reason there is a key at all.  Rendering this strip is QPainter
     * laying out three runs of text with antialiasing, which is not free on a
     * Cortex-A7 -- and its content changes once a second, when the clock ticks.
     * Doing it per frame would put back a good part of what the GPU path just
     * saved.  Everything the strip draws goes into the key, geometry included,
     * so a resize cannot leave a stale texture behind.
     */
    const QString key = m_showing.name + QChar(0x1f) + chromeRight() + QChar(0x1f) +
                        noteText() + QChar(0x1f) + QString::number(cr.width()) + "x" +
                        QString::number(cr.height());
    if (key == m_chromeKey)
        return;
    m_chromeKey = key;

    /*
     * Premultiplied because that is QPainter's own working format -- painting into
     * plain ARGB32 makes Qt convert every span twice.  setOverlay() un-multiplies
     * on the way to the GPU, where fixed-function blending wants straight alpha.
     */
    QImage strip(cr.size(), QImage::Format_ARGB32_Premultiplied);
    strip.fill(Qt::transparent);
    {
        QPainter sp(&strip);
        sp.setRenderHint(QPainter::Antialiasing, true);
        /* paintChrome() works in widget coordinates, so the image is shifted
         * under it rather than the other way about.  One translate here is the
         * price of the software path and this one sharing a painter. */
        sp.translate(-cr.topLeft());
        paintChrome(sp);
    }
    m_gl->setOverlay(GlVideo::ChromeLayer, strip, cr.translated(into.topLeft()));
}

bool MediaPage::glOwnsScreen() const
{
    return m_view == ViewVideo && m_gl && m_glShown;
}

/*
 * The volume bar, handed over as pixels because it cannot draw itself here -- see
 * the note on setRedirected() in volume.h.  The image is already positioned in
 * framebuffer coordinates by the shell, which is the one place that knows where
 * the bar sits, so there is nothing to map.
 *
 * present() and not refresh(): this is called from the shell's key handler, and a
 * volume press while a film is paused still has to put the bar on the glass.
 * present() re-blends the frame that is already there, which is exactly that.
 */
void MediaPage::setVolumeOverlay(const QImage &argb, const QRect &at)
{
    if (!m_gl)
        return;
    if (argb.isNull() || at.isEmpty())
        m_gl->clearOverlay(GlVideo::VolumeLayer);
    else
        m_gl->setOverlay(GlVideo::VolumeLayer, argb, at);
    present();
}

/*
 * The cursor, on the same terms as the bar above it and with one extra one: this
 * arrives on every motion event, so a still frame is re-composited at whatever
 * rate the mouse reports.  That is affordable -- the arrow is 20x28, the film is
 * already in the scanout, and the pass that re-blends it is the one that was
 * going to run anyway 25 times a second -- and it is the only thing that makes
 * the cursor move while a film is PAUSED, which is exactly when somebody reaches
 * for the mouse.
 */
void MediaPage::setPointerOverlay(const QImage &argb, const QRect &at)
{
    if (!m_gl)
        return;
    if (argb.isNull() || at.isEmpty())
        m_gl->clearOverlay(GlVideo::PointerLayer);
    else
        m_gl->setOverlay(GlVideo::PointerLayer, argb, at);
    present();
}

void MediaPage::paintEvent(QPaintEvent *)
{
    /*
     * ── WHILE THE GPU HAS THE FILM, THIS DRAWS NOTHING AT ALL ──
     *
     * Qt's linuxfb backend presents by memcpy'ing the dirty rectangle of its
     * backing store into /dev/fb0, and that backing store has never heard of the
     * frame lima put there.  So anything painted here would not go over the film,
     * it would go INSTEAD of it.
     *
     * The one thing worth doing is undoing: if this repaint was forced from
     * outside -- a toast, the console guard timer, a resize -- Qt is about to copy
     * stale pixels over the picture, so the picture is drawn again straight
     * afterwards.  Queued and not immediate, because that copy happens after this
     * returns.
     */
    if (glOwnsScreen()) {
        QTimer::singleShot(0, this, &MediaPage::present);
        return;
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (m_view == ViewImage || m_view == ViewVideo) {
        p.fillRect(rect(), QColor(8, 9, 14));

        if (!m_frame.isNull()) {
            QSize target = m_frame.size();
            target.scale(size(), Qt::KeepAspectRatio);
            const QRect at((width() - target.width()) / 2,
                           (height() - target.height()) / 2,
                           target.width(), target.height());
            /* No smooth transform: bilinear on a full 640x480 frame costs more than
             * the decode does on this CPU, and ffmpeg already scaled the video. */
            p.drawImage(at, m_frame);
        } else if (m_view == ViewVideo) {
            p.setPen(Theme::ink2());
            p.setFont(Theme::font(14));
            p.drawText(rect(), Qt::AlignCenter, tr("decoding..."));
        }

        paintChrome(p);
        return;
    }

    /* The browser and the player both live in the sheet. */
    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);

    QString heading = tr("Media");
    QString right;

    if (m_view == ViewPlayer) {
        heading = tr("Now playing");
        if (!m_order.isEmpty() && m_orderAt >= 0)
            right = QString("%1 / %2").arg(m_orderAt + 1).arg(m_order.size());
        if (m_shuffle)
            right += "   " + tr("shuffled");
    } else {
        right = m_dir;
        const QString home = QDir::homePath();
        if (right.startsWith(home))
            right = "~" + right.mid(home.size());
        if (right.size() > 34)
            right = "..." + right.right(31);
    }

    paintSheet(p, card, heading, right);

    /*
     * THE NOTE IS PAINTED IN EVERY VIEW NOW.  It used to be computed by the music
     * chain and painted only over a picture or a film, so an aplay that could not
     * open the card wrote its reason into a member that the browse view never
     * looked at.  That is the whole of "no audio and no message".
     */
    const QString note = noteText();
    if (!note.isEmpty()) {
        const QRectF strip(card.x() + 12, card.bottom() - 26, card.width() - 24, 20);
        const QFont small = Theme::font(11);
        p.setFont(small);
        p.setPen(Theme::orange());
        p.drawText(strip, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(small).elidedText(note, Qt::ElideRight, (int)strip.width()));
    }
}
