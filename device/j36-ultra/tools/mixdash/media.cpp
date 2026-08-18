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
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QTimer>

#include <cmath>
#include <signal.h>

#include "disks.h"
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

/* One argument for /bin/sh, safe whatever is in it.  Single quotes protect
 * everything except a single quote, and the standard dance closes the string,
 * escapes one, and opens it again.  Used by exactly one caller -- playOnce() --
 * and written properly anyway, because the one thing a quoting helper must never
 * be is nearly right. */
QString shellQuote(const QString &s)
{
    QString out = s;
    out.replace(QLatin1String("'"), QLatin1String("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
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
 * opened on the payload directory, next to mixdash itself, and every row in it
 * is a binary that rebuild() marks unselectable.  Worse, an empty m_dir has no
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
 * So there is a ceiling.  It is not a security boundary -- the Files page still
 * browses the whole disk, and that page is the one whose job that is -- it is a
 * browser that stays inside what it browses.
 *
 * ── AND WHY THERE IS NOW MORE THAN ONE ───────────────────────────────────────
 *
 * The ceiling was a single directory, and a single directory is wrong the moment
 * a USB stick is plugged in: the stick mounts under /media, /media is not beneath
 * /home/virtua, and clampToCeiling() therefore threw away every path that named
 * it.  Music on a stick was unreachable on the music page -- not hidden behind an
 * awkward walk, unreachable, because the one row that could have walked there was
 * the `..' row and `..' stopped at /home/virtua by construction.
 *
 * So the ceiling is a LIST.  The device's own media directory is always the first
 * root, and every mounted volume Disks knows about is a root alongside it.  Being
 * "inside the tree" means being under ANY of them, which is the only change the
 * clamp needed; the rest of the page is unchanged, because a root behaves exactly
 * the way the old ceiling did once you are inside one.
 *
 * Above the roots is the PLACES level -- the list of roots themselves, which is
 * what `..' shows when you are standing on one.  See populatePlaces().
 */
struct MediaRoot {
    QString path;
    QString name;      /* empty for the device, which the page names itself */
    bool removable = false;
};

QString mediaCeiling()
{
    return QDir::cleanPath(defaultMediaRoot());
}

/*
 * The device first, then whatever is mounted, in the order Disks lists it.
 *
 * Built fresh on every call rather than cached, and that is deliberate: a stick
 * can be pulled between two frames, and a cached root would leave the clamp
 * admitting paths on a filesystem that is no longer there.  The list is one entry
 * plus however many volumes are mounted, which on this board is never more than a
 * couple, so there is nothing here worth keeping.
 */
QVector<MediaRoot> mediaRoots()
{
    QVector<MediaRoot> roots;

    MediaRoot self;
    self.path = mediaCeiling();
    roots.append(self);

    for (const Disk &d : Disks::instance().list()) {
        if (d.mountPoint.isEmpty())
            continue;
        const QString p = QDir::cleanPath(d.mountPoint);
        if (!QFileInfo(p).isDir())
            continue;

        bool known = false;
        for (const MediaRoot &r : roots) {
            if (r.path == p) {
                known = true;
                break;
            }
        }
        if (known)
            continue;

        MediaRoot r;
        r.path = p;
        r.name = d.name();
        r.removable = true;
        roots.append(r);
    }
    return roots;
}

/*
 * The root `dir' is inside, or empty when it is inside none of them.
 *
 * Compared with a trailing separator so that /home/virtua-old is not mistaken for
 * a child of /home/virtua, which a bare startsWith() would do.  The LONGEST match
 * wins, for the case where one root is mounted underneath another -- /media is a
 * directory on the rootfs, so a volume mounted at /media/STICK while the device
 * root happened to be /media would otherwise resolve to the wrong one and `..'
 * would climb out of the volume without saying so.
 */
QString rootOf(const QString &dir)
{
    const QString d = QDir::cleanPath(dir);
    QString best;

    for (const MediaRoot &r : mediaRoots()) {
        const QString withSep = r.path.endsWith(QLatin1Char('/')) ? r.path
                                                                  : r.path + QLatin1Char('/');
        if (d != r.path && !d.startsWith(withSep))
            continue;
        if (r.path.size() > best.size())
            best = r.path;
    }
    return best;
}

/* True if `dir' is a root or anything beneath one. */
bool underCeiling(const QString &dir)
{
    return !rootOf(dir).isEmpty();
}

/* Whatever was asked for if it is inside the tree, the device root if it is not.
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

    /* Single shot, armed by pump() for whatever a decoded-but-not-yet-due frame
     * has left to wait.  Not a periodic 40 ms tick: the interval is the film's,
     * not this program's, and a fixed one would beat against it. */
    m_pace = new QTimer(this);
    m_pace->setSingleShot(true);
    m_pace->setTimerType(Qt::PreciseTimer);
    connect(m_pace, &QTimer::timeout, this, &MediaPage::pump);

    /* The only thing that takes the transport off a playing film.  Two and a half
     * seconds: long enough to read the title and reach a button, short enough that
     * a film watched to the end is not watched through a scrim. */
    m_transportTimer = new QTimer(this);
    m_transportTimer->setSingleShot(true);
    m_transportTimer->setInterval(2600);
    connect(m_transportTimer, &QTimer::timeout, this, &MediaPage::hideTransport);

    /* Without this Qt delivers a move only while a button is down, and the
     * transport's whole show-on-motion behaviour would need a click first. */
    setMouseTracking(true);

    /* Both remembered, because a handheld that forgets it was shuffling is a
     * handheld you set up again every boot. */
    m_repeat = Settings::instance().mediaRepeat();
    m_shuffle = Settings::instance().mediaShuffle();

    /* A stick plugged in while this page is up changes what the tree contains, so
     * the listing has to be told.  Disks is already running by the time any page
     * is built -- main.cpp starts it before the dashboard -- so this is only ever
     * about later changes. */
    connect(&Disks::instance(), &Disks::changed, this, &MediaPage::onDisksChanged);

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

    /* Left at the places level, come back to it -- unless the volume that made it
     * worth having has been pulled in the meantime, in which case there is one
     * root left and the list of it is not a level worth standing on. */
    if (m_places && mediaRoots().size() > 1)
        populatePlaces();
    else
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
    } else if (view == ViewVideo) {
        /* A film opens with its controls showing and they go two and a half
         * seconds later, which is how anybody finds out there are any. */
        m_hover = CtlNone;
        m_scrubbing = false;
        nudgeTransport();
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

    /*
     * A FILTER IS A PROPERTY OF THE FOLDER IT WAS TYPED IN, so it goes when the
     * folder does -- and stays when it does not.  The previous path is read before
     * m_dir is overwritten because both cases come through here: opening a
     * directory, which must clear the term, and onDisksChanged() repopulating the
     * SAME directory a second after a stick was plugged in, which must not.  A
     * refresh that silently dropped the search would look like the filter forgetting
     * itself at random, since what triggers it happens off-screen.
     */
    const QString previous = m_dir;
    const bool wasPlaces = m_places;

    m_dir = d.absolutePath();
    m_entries.clear();
    m_places = false;

    if (wasPlaces || previous != m_dir)
        m_search.clear();

    if (!d.exists()) {
        /* A new listing, so the cursor starts at the top of it -- see rebuild(). */
        rebuild(false);
        emit titleChanged();
        return;
    }

    /*
     * `..' only while there is somewhere above to go.  The test used to be
     * !d.isRoot(), which meant the row was on every directory but / and was the
     * way out of the media tree entirely.  Dropping the row rather than leaving
     * it and refusing the press is deliberate: a row that is on the list and does
     * nothing reads as a bug, and the absence of the row is itself the message
     * that this is the top.
     *
     * There are two kinds of "above" now.  Inside a root, it is the parent
     * directory.  ON a root it is the places list -- and that row is only offered
     * when there is more than one root, because a places list with a single entry
     * on it is a level whose only row takes you straight back where you came
     * from.  Plug a stick in and the row appears, on this listing, without the
     * page being left: see onDisksChanged().
     */
    const QString root = rootOf(m_dir);
    if (!root.isEmpty() && m_dir != root) {
        Entry up;
        up.kind = KindUp;
        up.name = QStringLiteral("..");
        up.path = QFileInfo(m_dir).absolutePath();
        m_entries.append(up);
    } else if (mediaRoots().size() > 1) {
        /* An empty path is what open() reads as "go to the places list".  It
         * cannot be confused with a directory: every other path in m_entries came
         * out of QFileInfo::absoluteFilePath(). */
        Entry up;
        up.kind = KindUp;
        up.name = QStringLiteral("..");
        m_entries.append(up);
    }

    /*
     * ── AND THE OTHER ROOTS, ON THE LISTING RATHER THAN ABOVE IT ────────────────
     *
     * A stick used to arrive on this page as a `..' row silently appearing at the
     * top of whatever was already listed.  That is not a stick being listed: the
     * row says "up one level" until you press it, the level above is a places list
     * nobody has been told exists, and the volume's own name appears nowhere on the
     * screen the user is actually looking at.  What that produced was the bug as it
     * was reported -- media that "does not list its directories" until it is reached
     * the long way round, through the Files page, which has had its volumes
     * permanently on the glass since the day it was written and for exactly this
     * reason (see the Places note in files.h).
     *
     * So when this listing IS a root, every OTHER root goes on it, by name, at the
     * top.  One press opens the volume.  The places level is untouched and `..'
     * still goes there -- this is the same list one level down, where somebody who
     * has just plugged a disk in is standing.
     *
     * Only on a root, deliberately.  Repeating the volumes inside every directory
     * would put a drive row above the files in every folder on the card, and the
     * question a listing answers is "what is in here".
     */
    if (root.isEmpty() || m_dir == root) {
        for (const MediaRoot &r : mediaRoots()) {
            if (QDir::cleanPath(r.path) == m_dir)
                continue;
            Entry e;
            e.kind = KindPlace;
            e.path = r.path;
            e.name = r.removable ? r.name : tr("Device");
            if (e.name.isEmpty())
                e.name = QFileInfo(r.path).fileName();
            m_entries.append(e);
        }
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

    /* These are a different directory's entries, so the cursor goes to the first of
     * them and not to whichever row number the last directory left behind. */
    rebuild(false);
    emit titleChanged();
}

/*
 * ── THE LEVEL ABOVE THE ROOTS ────────────────────────────────────────────────
 *
 * One row per root: the device itself, then every mounted volume.  It is the only
 * thing on this page that is not a directory listing, and it exists because a USB
 * stick has no parent inside the tree -- /media/STICK's parent is /media, which is
 * a directory on the rootfs with nothing in it but mount points, and browsing
 * there would show the volumes as ordinary folders with no way to tell a mounted
 * disk from a leftover empty directory.
 *
 * m_dir IS NOT TOUCHED HERE.  It still names the last real directory, so leaving
 * the page and coming back to it does not have to decide what "the places level"
 * means to Settings::mediaRoot -- and nothing that reads m_dir (the queue, the
 * heading, `..') has to learn about a path that is not a path.  m_places is the
 * whole of the state, and populate() clears it.
 */
void MediaPage::populatePlaces()
{
    m_places = true;
    m_entries.clear();
    /* Nothing to filter up here, and leaving a term set would put it back in force
     * the moment `..' went back down into the folder it came from. */
    m_search.clear();

    for (const MediaRoot &r : mediaRoots()) {
        Entry e;
        e.kind = KindPlace;
        e.path = r.path;
        e.name = r.removable ? r.name : tr("Device");
        if (e.name.isEmpty())
            e.name = QFileInfo(r.path).fileName();
        m_entries.append(e);
    }

    /* One row per root, which is not the list that was up a moment ago either. */
    rebuild(false);
    emit titleChanged();
}

/*
 * A volume was mounted or pulled while this page existed.
 *
 * Three things can be stale afterwards and all three are fixed by repopulating:
 * the places list itself, the `..' row on a root -- which is offered only while
 * there is more than one root, so plugging a stick in has to make it appear --
 * and the directory being browsed, if it was ON the stick that just went away.
 *
 * That last case is why the clamp is asked rather than trusted: m_dir was legal
 * when it was set and is not any more, and populate() would list a directory that
 * no longer exists.  clampToCeiling() sends it back to the device root, which is
 * the one root that cannot be pulled.
 *
 * NOTHING IS STOPPED.  A track playing off a stick that has been yanked will die
 * on its own and say so through the note strip, and that is a better answer than
 * this page killing it on the strength of a mount table -- the volume may have
 * been remounted somewhere else in the same event.
 */
void MediaPage::onDisksChanged()
{
    /*
     * NOT `if (m_view != ViewBrowse) return', which is what this used to be and
     * which lost the event outright.
     *
     * Music survives leaving this page -- that is the whole point of the queue --
     * so the player view is an ordinary place to be standing, and so is a picture
     * or a film.  A stick plugged in while any of them is up used to be dropped on
     * the floor: m_entries kept the listing built when there was one root, so it
     * had no volume rows and no `..', and going back to the browser showed a
     * directory with no sign of the disk in it.  The only thing that put it right
     * was leaving the page altogether and coming back, because onEnter() calls
     * populate() unconditionally.
     *
     * So the listing is rebuilt whichever view is up -- populate() and
     * populatePlaces() both end in rebuild(), which builds rows for the view that
     * is actually showing -- and only the CURSOR work below is browse-only, because
     * only the browser has a cursor over m_entries to keep.
     */
    const bool browsing = m_view == ViewBrowse;

    if (m_places) {
        populatePlaces();
        /* Down to the one root that is left: a places list of one is a level whose
         * only row goes where you already were. */
        if (m_entries.size() < 2)
            populate(mediaCeiling());
        if (browsing)
            selectTopEntry();
        return;
    }

    /* The cursor is kept by the path it was on and not by its index: the volume
     * rows and the `..' row appearing or disappearing shift every index below them,
     * which is exactly what a stick being plugged in does.  Read before the
     * repopulate and only when the browser owns the pane -- in the player view the
     * rows are the transport, and rows[at].key there is not a path. */
    QString cursor;
    if (browsing) {
        const QVector<ListRow> &rows = m_list->rows();
        const int at = m_list->current();
        if (at >= 0 && at < rows.size())
            cursor = rows[at].key;
    }

    const QString was = m_dir;
    populate(QFileInfo(was).isDir() ? clampToCeiling(was) : mediaCeiling());
    if (browsing && (cursor.isEmpty() || !selectPath(cursor)))
        selectTopEntry();
}

bool MediaPage::openPath(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return false;

    if (info.isDir()) {
        setView(ViewBrowse);
        /* populate() ends in rebuild(false), which already puts the cursor on the
         * first entry -- see selectTopEntry().  A setCurrent(0) here would undo it
         * and land on the search box. */
        populate(info.absoluteFilePath());
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

void MediaPage::rebuild(bool keepSelection)
{
    QVector<ListRow> rows;
    if (m_view == ViewPlayer)
        buildPlayerRows(rows);
    else
        buildBrowseRows(rows);

    const int keep = keepSelection ? m_list->current() : -1;
    m_list->setRows(rows, keepSelection);
    if (keep >= 0 && keep < rows.size())
        m_list->setCurrent(keep);
    else if (m_view != ViewPlayer)
        selectTopEntry();
    layOutList();
    refresh();
}

/*
 * ── THE TOP OF THE LISTING IS NOT ROW 0 ──────────────────────────────────────
 *
 * setRows(rows, false) lands on the first SELECTABLE row, and the first selectable
 * row is furniture: the now-playing row while music is on, and the search box under
 * it.  That is "at the top" in the literal sense and wrong in every other one -- it
 * costs a press to reach the files in every directory opened, and after a search it
 * would leave the cursor sitting on the box that was just typed into.
 *
 * WHEN A FILTER IS IN FORCE ONE MORE ROW IS SKIPPED, and that is not cosmetic.
 * `..' survives the filter on purpose (see matchesSearch), so it is the first row
 * of every filtered listing -- and landing on it means the first press of A after a
 * search walks OUT of the folder that was just searched.
 *
 * Falls through to whatever setRows chose when there is nothing to land on, which
 * is the empty directory and the search that matched nothing.  In the second case
 * that is the box itself, which is where somebody about to clear it wants to be.
 */
void MediaPage::selectTopEntry()
{
    const QVector<ListRow> &rows = m_list->rows();
    const bool filtered = !m_search.trimmed().isEmpty();

    for (int i = 0; i < rows.size(); ++i) {
        const int id = rows[i].id;
        if (id < 0 || id >= m_entries.size() || !rows[i].enabled)
            continue;
        if (filtered && (m_entries[id].kind == KindUp || m_entries[id].kind == KindPlace))
            continue;
        m_list->setCurrent(i);
        return;
    }
}

bool MediaPage::matchesSearch(const Entry &e) const
{
    if (m_search.trimmed().isEmpty())
        return true;
    if (e.kind == KindUp || e.kind == KindPlace)
        return true;
    return e.name.contains(m_search.trimmed(), Qt::CaseInsensitive);
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

    /*
     * The search box, under the now-playing row and above the listing -- which is
     * where it has to be: it is the one row whose position must not move when the
     * directory changes, because it is how you change what the directory shows.
     * Not offered on the places level, where there is nothing to filter but the
     * volumes themselves and those are never filtered.
     */
    if (!m_places) {
        ListRow s;
        s.kind = ListRow::Action;
        s.id = RowSearch;
        s.glyph = GlyphFiles;
        if (m_search.trimmed().isEmpty()) {
            s.text = tr("Search this folder");
            s.detail = tr("By name.  A opens the keyboard.");
            s.accent = Theme::ink3();
        } else {
            s.text = m_search;
            s.detail = tr("A edits it, B is not it -- press A and clear the box");
            s.accent = Theme::blue();
            s.badge = tr("filtered");
            s.badgeColour = Theme::blue();
        }
        rows.append(s);
    }

    int matched = 0;
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &e = m_entries[i];
        if (!matchesSearch(e))
            continue;
        if (e.kind != KindUp && e.kind != KindPlace)
            matched++;
        ListRow r;
        r.kind = ListRow::Item;
        r.text = e.name;
        /* THE INDEX INTO m_entries AND NOT THE ROW NUMBER -- see the note on
         * m_search in media.h.  A filtered listing skips rows; it renumbers
         * nothing. */
        r.id = i;
        r.key = e.path;

        switch (e.kind) {
        case KindUp:
            r.glyph = GlyphBack;
            r.accent = Theme::ink3();
            /* Two destinations, one row, and the detail is the only thing that
             * says which -- see populate() for why the empty path means the
             * places list. */
            r.detail = e.path.isEmpty() ? tr("all devices") : tr("up one level");
            break;
        case KindPlace:
            r.glyph = GlyphDrive;
            /* The device is not a disk you can pull, and it is the one row on this
             * list that is always there; the colour says so before the label is
             * read. */
            r.accent = e.path == mediaCeiling() ? Theme::teal() : Theme::yellow();
            r.detail = e.path;
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

    /*
     * A filter that matched nothing SAYS SO, on the list, rather than leaving a
     * folder that plainly had things in it looking empty.  The pane's own
     * placeholder cannot do this job: it is the empty-directory message, it is
     * only drawn when there are no rows at all, and there is at least one here --
     * the search box itself, and usually `..' with it.
     */
    if (!m_places && !m_search.trimmed().isEmpty() && matched == 0) {
        ListRow r;
        r.kind = ListRow::Item;
        r.id = RowInert;
        r.enabled = false;
        r.glyph = GlyphInfo;
        r.accent = Theme::ink3();
        r.text = tr("Nothing here matches %1").arg(m_search.trimmed());
        r.detail = tr("A on the box above clears it");
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
    case RowSearch:
        /*
         * The keyboard opens ON the term that is already in force, so clearing a
         * filter is select-all-and-delete rather than a second row nobody would
         * find.  m_awaitingSearch is what makes textEntered() ours: the shell hands
         * that callback to whichever page is up, and this page will one day want the
         * keyboard for something else.
         */
        m_awaitingSearch = true;
        emit textRequested(tr("Search this folder"), m_search, false);
        return;
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

/*
 * The keyboard closed.  Only the search box ever opens it from this page, and
 * m_awaitingSearch is checked rather than assumed -- a callback that arrives when
 * nothing asked for one belongs to somebody else, and taking it would set the
 * filter to whatever the last page typed.
 */
void MediaPage::textEntered(const QString &text, bool accepted)
{
    if (!m_awaitingSearch)
        return;
    m_awaitingSearch = false;
    if (!accepted)
        return;

    m_search = text;
    /* rebuild(false), because the filter has changed what row 0 IS.  Keeping the
     * cursor would leave it wherever the old row number now lands -- in the middle
     * of a three-row list, or past the end of it.  Same reason populate() does. */
    rebuild(false);
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
         *
         * The same applies one level higher: `..' from a root shows the places
         * list with the root you just left under the cursor, which is what makes
         * a wrong turn into one press back rather than a hunt.
         */
        const QString child = m_dir;
        if (entry.path.isEmpty())
            populatePlaces();
        else
            populate(entry.path);
        if (!selectPath(child))
            selectTopEntry();
        return;
    }
    case KindPlace:
        /* No setCurrent(0) after either of these: populate() ends in rebuild(false)
         * and that already lands on the first entry rather than on the furniture
         * above it.  See selectTopEntry(). */
        populate(entry.path);
        return;
    case KindDir:
        populate(entry.path);
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

QString MediaPage::ffmpegPath()
{
    static const QString path = firstExisting(QStringList()
                                              << "/usr/bin/ffmpeg" << "/bin/ffmpeg"
                                              << "/usr/local/bin/ffmpeg");
    return path;
}

QString MediaPage::aplayPath()
{
    static const QString path = firstExisting(QStringList()
                                              << "/usr/bin/aplay" << "/bin/aplay");
    return path;
}

/*
 * ── ONE SOUND, ONCE, WITH NOBODY WAITING FOR IT ─────────────────────────────
 *
 * The startup chime, and anything else this shell ever wants to make a noise
 * about.  It is here rather than in main.cpp because everything it has to get
 * right is here already: which ffmpeg, which card, and the fact that `default' is
 * a trap on this image -- see the header.
 *
 * DETACHED, and that is the whole design.  A chime that the dashboard waited for
 * would be a dashboard that does not appear until the sound has finished, which
 * is the exact failure this batch of work exists to remove.  Nothing reads its
 * output, nothing reaps it, and if it fails there is no message: a chime is not
 * evidence about anything, and a page that reported one would be reporting on a
 * sound card the user can hear for themselves.
 *
 * THE `||' IS FOR AN ffmpeg WITHOUT THE ALSA OUTDEV.  ffmpegHasAlsa() answers
 * that question and takes up to fifteen seconds to do it, which cannot happen at
 * startup -- so the fallback is expressed as a shell pipeline instead and the
 * shell decides at run time.  `A || B | C' is `A || (B | C)': a pipeline binds
 * tighter than the or, which is what is wanted here.
 */
void MediaPage::playOnce(const QString &path)
{
    if (path.isEmpty() || !QFileInfo(path).isReadable())
        return;

    const QString ff = ffmpegPath();
    const QString dev = alsaDevice();
    if (dev.isEmpty())
        return;

    /* The startup asset has a build-decoded WAV beside its source MP3.  Feeding
     * that straight to aplay avoids cold-loading ffmpeg and libavcodec from the
     * SD card at the exact moment X and the dashboard are starting.  The old path
     * took nearly three minutes in the device log once Firefox joined that I/O
     * queue; aplay begins the same six-second chime immediately.
     *
     * The `|| ffmpeg' is for the card whose aplay cannot even start: the shared
     * rootfs still has a libasound that predates snd_pcm_subformat_value, and
     * j36-asound covering it can lose a race with this first-paint call.  A
     * silent chime is worse than a late one. */
    /* MixOS overlays an old libasound that aplay cannot even start under.
     * The payload stages Debian's copy in /opt/mixos/lib; putting that
     * directory first is how the chime plays before j36-asound has bound
     * anything, and how it still plays if that unit is cancelled. */
    const QString mixlib = QStringLiteral("LD_LIBRARY_PATH=/opt/mixos/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} ");

    const QString ap = aplayPath();
    if (path.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive)
        && !ap.isEmpty()) {
        QString cmd = mixlib + shellQuote(ap) + " -q -D " + shellQuote(dev) + " " +
                      shellQuote(path);
        if (!ff.isEmpty())
            cmd += " || " + mixlib + shellQuote(ff) +
                   " -nostdin -hide_banner -loglevel quiet -i " +
                   shellQuote(path) + " -vn -ar " + QString::number(kRate) +
                   " -ac 2 -f alsa " + shellQuote(dev);
        QProcess::startDetached(QStringLiteral("/bin/sh"),
                                QStringList() << QStringLiteral("-c") << cmd);
        return;
    }
    if (ff.isEmpty())
        return;

    const QString decode = mixlib + shellQuote(ff) +
                           " -nostdin -hide_banner -loglevel quiet -i " + shellQuote(path) +
                           " -vn -ar " + QString::number(kRate) + " -ac 2 ";

    QString cmd = decode + "-f alsa " + shellQuote(dev);
    if (!ap.isEmpty())
        cmd += " || " + decode + "-f wav - | " + mixlib + shellQuote(ap) +
               " -q -D " + shellQuote(dev) + " -";

    QProcess::startDetached(QStringLiteral("/bin/sh"), QStringList() << "-c" << cmd);
}

QString MediaPage::alsaDevice()
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

QString MediaPage::ffprobePath() const
{
    return firstExisting(QStringList() << "/usr/bin/ffprobe" << "/bin/ffprobe");
}

/*
 * ── ONE PROBE, ASYNCHRONOUS, AND WHOEVER ASKED IS RESUMED FROM ITS EXIT ──────
 *
 * Everything this page needs to know about a file before it can play it comes out
 * of one ffprobe invocation: how long it is, whether it carries sound, what rate
 * the picture runs at, and what the container calls itself.  It used to be three
 * invocations and every one of them was waited for with the event loop stopped,
 * which is the freeze -- and the reason there was no point drawing a spinner,
 * because nothing in the program was running to turn it.
 *
 * WITH THE KEYS PRINTED, not with nokey=1.  ffprobe emits only the entries that
 * exist, in the container's own order, so bare values cannot be told apart -- a
 * file with an artist and no title would put the artist where the title goes, and
 * a stream section would be indistinguishable from the format section.
 *
 * A PROBE THAT FAILS IS NOT A REASON NOT TO PLAY.  No ffprobe on the card, a
 * container it cannot parse, a timeout: all of them end in onProbeFinished() with
 * whatever was understood, and the caller is started anyway.  What is lost is a
 * length on the bar and a title from the tags, and the file name covers the
 * second of those.
 */
void MediaPage::probeThen(const QString &path, int purpose, const Entry &entry,
                          double startAt)
{
    m_waiting = purpose;
    m_waitEntry = entry;
    m_waitStart = startAt;

    /*
     * Already known.  Queued rather than called, so that this function returns to
     * its caller before the caller is re-entered -- openVideo() is in the middle
     * of tearing a film down when it asks, and openVideoNow() running inside that
     * would be openVideo() running inside itself.
     */
    if (m_probedPath == path && !path.isEmpty()) {
        QTimer::singleShot(0, this, &MediaPage::onProbeFinished);
        return;
    }

    endProcess(m_probe);
    m_probedPath = path;
    m_probeDuration = 0.0;
    m_probeFps = 0.0;
    m_probeAudio = true;      /* see below: the useful way to be wrong */
    m_probeTitle.clear();

    const QString ffprobe = ffprobePath();
    if (ffprobe.isEmpty() || path.isEmpty()) {
        QTimer::singleShot(0, this, &MediaPage::onProbeFinished);
        return;
    }

    m_probe = new QProcess(this);
    connect(m_probe, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, &MediaPage::onProbeFinished);
    /* errorOccurred and not only finished: an ffprobe that cannot be executed
     * emits no exit code at all, and a caller left waiting for one would be a
     * spinner that never stops. */
    connect(m_probe, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            onProbeFinished();
    });
    m_probe->start(ffprobe, QStringList()
                                << "-v" << "error"
                                << "-show_entries"
                                << "stream=codec_type,r_frame_rate,avg_frame_rate:"
                                   "format=duration:format_tags=title,artist"
                                << "-of" << "default=noprint_wrappers=1"
                                << path);
}

void MediaPage::onProbeFinished()
{
    if (m_probe && m_probe->state() == QProcess::Running)
        return;                      /* an errorOccurred that was not fatal */

    if (m_probe) {
        /*
         * Parsed in one walk.  Both rates arrive as rationals -- "30000/1001" for
         * anything that came off American television -- and only the VIDEO stream's
         * are taken: ffprobe prints one per stream, the audio stream's are
         * meaningless, and the audio stream comes first in some containers, which
         * is what the in-stream tracking is for.  hasAudio is set from codec_type
         * rather than from a second -select_streams run.
         *
         * avg_frame_rate is preferred over r_frame_rate because they answer
         * different questions: avg is the stream's actual rate, while r is the
         * LOWEST rate every timestamp can be represented at -- a timebase in
         * disguise for VFR files and for some MPEG-TS, where it can be five
         * digits.  A rate that size becomes an ffmpeg `-r' that duplicates every
         * frame thousands of times and a pacing clock that calls every one of
         * them due, and the result is the film fast-forwarding at CPU speed.
         */
        QString title, artist;
        bool sawStreams = false, audio = false;
        bool inVideoStream = false;
        double avgRate = 0.0, baseRate = 0.0;
        const QStringList lines =
            QString::fromUtf8(m_probe->readAllStandardOutput()).split('\n');
        for (const QString &line : lines) {
            const QString t = line.trimmed();
            if (t.startsWith(QStringLiteral("codec_type="))) {
                sawStreams = true;
                const QString ctype = t.mid(11);
                audio = audio || ctype == QStringLiteral("audio");
                inVideoStream = ctype == QStringLiteral("video");
            } else if (inVideoStream && (t.startsWith(QStringLiteral("avg_frame_rate=")) ||
                                         t.startsWith(QStringLiteral("r_frame_rate=")))) {
                const bool avg = t.startsWith(QStringLiteral("avg_frame_rate="));
                const QStringList r = t.mid(t.indexOf('=') + 1).split('/');
                double fps = 0.0;
                if (r.size() == 2 && r.at(1).toDouble() > 0.0)
                    fps = r.at(0).toDouble() / r.at(1).toDouble();
                else if (r.size() == 1)
                    fps = r.value(0).toDouble();
                if (fps > 0.0 && !std::isnan(fps) && !std::isinf(fps)) {
                    if (avg && avgRate <= 0.0)
                        avgRate = fps;
                    else if (!avg && baseRate <= 0.0)
                        baseRate = fps;
                }
            } else if (t.startsWith(QStringLiteral("duration="))) {
                const double d = t.mid(9).toDouble();
                if (d > 0.0 && !std::isnan(d) && !std::isinf(d))
                    m_probeDuration = d;
            } else if (t.startsWith(QStringLiteral("TAG:title="))) {
                title = t.mid(10).trimmed();
            } else if (t.startsWith(QStringLiteral("TAG:artist="))) {
                artist = t.mid(11).trimmed();
            }
        }

        /* Only when the probe actually described some streams.  A run that printed
         * nothing at all understood nothing, and "there is no audio" is a much
         * worse guess than "there probably is" -- see the note below. */
        if (sawStreams)
            m_probeAudio = audio;

        if (!title.isEmpty())
            m_probeTitle = artist.isEmpty() ? title
                                            : artist + QStringLiteral(" - ") + title;

        m_probeFps = avgRate > 0.0 ? avgRate : baseRate;

        m_probe->deleteLater();
        m_probe = nullptr;
    }

    /*
     * TRUE WHEN NOTHING COULD BE ASKED, which is the useful way to be wrong: nearly
     * every film has sound, so assuming it plays the ones that do and costs a note
     * on the ones that do not.  Assuming the other way would silence everything on
     * a card that happens to ship ffmpeg without ffprobe.
     */

    const int purpose = m_waiting;
    m_waiting = WaitNothing;

    if (purpose == WaitVideo)
        openVideoNow(m_waitEntry, m_waitStart);
    else if (purpose == WaitMusic)
        playQueuedNow(m_waitStart);
}

/*
 * THE TAGS WIN, THE FILE NAME IS THE FLOOR, AND THERE IS NO THIRD ANSWER.
 *
 * A film had no title at all before this -- probeTitle() was only ever called from
 * the music path -- so the strip over a picture showed the file name and the label
 * over a track showed the tags, which is two rules for one question.  One rule
 * now, used by both: what the container says it is called, and failing that what
 * it is called on disk, without the extension, because ".mkv" is not part of a
 * film's name and never was.
 */
QString MediaPage::displayTitle(const Entry &entry, const QString &tagged) const
{
    if (!tagged.isEmpty())
        return tagged;
    const QString base = QFileInfo(entry.path).completeBaseName();
    return base.isEmpty() ? entry.name : base;
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

/*
 * "I am waiting", said once per change of answer.
 *
 * The shell is what draws the ring, because over a film this page cannot draw a
 * widget over itself and because there should be one spinner on the glass rather
 * than one per page.  All this end has to be careful about is not shouting: every
 * frame that arrives clears the flag and every seek sets it again, so an unguarded
 * emit here would be twenty-five signals a second, each of them restarting an
 * animation timer in another object.
 */
void MediaPage::setLoading(bool on, const QString &what)
{
    if (m_loading == on && (!on || m_loadingWhat == what))
        return;
    m_loading = on;
    m_loadingWhat = on ? what : QString();
    emit busyRequested(m_loading, m_loadingWhat);
}

/*
 * THE HALF THAT WAITS.  A film that has not been probed cannot be started -- the
 * rate the picture is paced at and whether there is any sound to pace it against
 * both come from ffprobe -- so this puts the spinner up, asks, and gets out of the
 * way.  A seek is the same container with the answer already in hand and goes
 * straight through.
 */
void MediaPage::openVideo(const Entry &entry, double startAt)
{
    /* By value before anything is torn down: commitSeek() passes m_showing, and
     * the assignment below would otherwise be reading a member it is writing. */
    const Entry item = entry;

    if (ffmpegPath().isEmpty()) {
        emit toastRequested(tr("ffmpeg is not installed.\nInstall it from Packages."), 4000);
        return;
    }

    if (!item.path.isEmpty() && m_probedPath == item.path) {
        openVideoNow(item, startAt);
        return;
    }

    /*
     * A different film.  Everything of the last one comes down here rather than in
     * openVideoNow(), because ffprobe is about to take a moment and a page still
     * showing the previous picture while it does is a page that looks like it
     * ignored the press.
     */
    stopMusic();
    stopVideo();
    dropFrame();

    m_showing = item;
    m_videoDuration = 0.0;
    m_videoHasAudio = false;
    m_videoFps = 0.0;
    m_trackTitle.clear();
    m_paused = false;
    m_note.clear();
    m_clock.restart();
    m_pausedAt = (qint64)(startAt * 1000.0);
    setLoading(true, item.name);
    setView(ViewVideo);
    refresh();

    probeThen(item.path, WaitVideo, item, startAt);
}

void MediaPage::openVideoNow(const Entry &entry, double startAt)
{
    const Entry item = entry;

    if (ffmpegPath().isEmpty())
        return;

    /* A film takes the sound card, so the record has to come off first.  This is
     * the one direction the asymmetry in onLeave() runs the other way. */
    stopMusic();
    stopVideo();

    m_showing = item;
    m_buffer.clear();
    m_framesShown = 0;
    m_framesDropped = 0;
    m_framesDecoded = 0;
    m_videoStart = startAt;
    m_paused = false;
    m_note.clear();
    setLoading(true, item.name);

    m_videoDuration = m_probeDuration;
    m_videoHasAudio = m_probeAudio;
    /*
     * Clamped, and BOTH ends of the clamp are load-bearing.
     *
     * The ceiling is not squeamishness about 60 fps material -- it is that the
     * whole cost of a frame on this board is paid whether or not the eye can use
     * it, and `-r' below makes ffmpeg do the dropping in the one place that can
     * do it without decoding twice.
     *
     * The floor is the other half: the probed rate is a container's own claim and
     * is not always a claim about the picture -- r_frame_rate is a timebase for
     * some MPEG-TS and avg_frame_rate can be a frame an hour on a slideshow, and
     * both flow straight into `-r' and into pump()'s due-time arithmetic.  A rate
     * of 90000 makes ffmpeg duplicate every frame thousands of times and pump()
     * call all of them due, which plays the film at CPU speed; a rate of 0.001
     * parks the first frame in the pace timer for a month.  Neither is a film.
     *
     * 25 when there was no answer at all.
     */
    const bool sane = m_probeFps > 0.0 && !std::isnan(m_probeFps)
                      && !std::isinf(m_probeFps);
    m_videoFps = sane ? qBound(5.0, m_probeFps, 30.0) : 25.0;
    /* The tags win and the file name is the floor -- for a film exactly as for a
     * track.  See displayTitle(). */
    m_trackTitle = displayTitle(item, m_probeTitle);

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

    /*
     * ── FIT OR FILL, AND WHY IT IS AN ffmpeg ARGUMENT ────────────────────────
     *
     * The panel is 4:3 and most film is not, so letterboxed is the honest default
     * and there are two black bands.  The transport's fullscreen button trades
     * them for a crop -- scale UP until the short side fits and cut the overhang
     * off -- which is what everything from a television to a video site calls
     * zoom, and on a 640x480 screen it is the difference between a face you can
     * read and a letterbox.
     *
     * It has to be done here because ffmpeg is what scales: this end receives
     * frames that are already exactly panel-sized, which is the whole reason a
     * Cortex-A7 can play anything at all.  So pressing the button restarts the
     * decoder where the film stood, and that is not a special case -- it is the
     * seek path, which already exists because a pipe cannot seek.
     */
    const QString fit = m_fill
        ? QString("scale=w=%1:h=%2:force_original_aspect_ratio=increase,crop=%1:%2")
              .arg(m_frameW).arg(m_frameH)
        : QString("scale=w=%1:h=%2:force_original_aspect_ratio=decrease,"
                  "pad=%1:%2:(ow-iw)/2:(oh-ih)/2")
              .arg(m_frameW).arg(m_frameH);
    const QString filter = fit + ",format=" + (m_gl ? "yuv420p" : "bgra");

    /*
     * ── `-re' STAYS, AND `-r' IS WHAT WAS MISSING ────────────────────────────
     *
     * `-re' is the ceiling: it makes ffmpeg emit at roughly real time, which is
     * what keeps a decoder that is faster than the film from putting the whole
     * container in this program's heap.  QProcess has no read-buffer limit to
     * throttle it with -- that is QAbstractSocket -- so the producer is the only
     * place a bound can come from.
     *
     * What `-re' is NOT is a synchroniser, and treating it as one is what was
     * wrong.  It delays a frame that is early and does nothing at all to a frame
     * that is late, so the moment this end stalled -- a blocking ffprobe in front
     * of a play, a launch that stopped the event loop, one full-screen repaint
     * that ran long -- ffmpeg blocked in write(2), fell behind its own schedule,
     * and every frame after that was late by the length of the stall while the
     * ALSA stream kept perfect time.  The old drop-to-the-newest could not fix it:
     * it dropped whatever happened to be queued, against no clock at all, so it
     * threw frames away without ever knowing whether that helped.
     *
     * The recovery `-re' does give is the burst: having been held up it emits as
     * fast as it can until it is back on schedule.  Those are the spare frames
     * pump() needs, and `-r' with `-vsync cfr' is what lets pump() know which ones
     * to keep -- constant rate means the Nth frame out of the pipe belongs at
     * startAt + N/fps, so a raw pipe that carries no timestamps does not have to.
     * It is also where a 60 fps source is halved, once, by the process that has
     * already decoded it.
     */
    QStringList args;
    args << "-nostdin" << "-hide_banner" << "-loglevel" << "error" << "-re";
    if (startAt > 0.0)
        args << "-ss" << QString::number(startAt, 'f', 2);
    args << "-i" << item.path
         << "-map" << "0:v:0"
         << "-vf" << filter
         << "-vsync" << "cfr"
         << "-r" << QString::number(m_videoFps, 'f', 3)
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

    /* The probe that gates this function can outlast a panel hand-off: a film
     * asked for at the glass may only start once the glass belongs to somebody
     * else.  It comes up stopped in that case, exactly as panelLost() would
     * have stopped it, and comes back with the panel. */
    if (m_panelHidden) {
        signalVideoChain(SIGSTOP);
        m_pausedAt += m_clock.elapsed();
    }
}

/*
 * DRAINED COMPLETELY, EVERY TIME, and the bound is `-re' at the other end.
 *
 * The pipe must never be what stops ffmpeg, because an ffmpeg blocked in write(2)
 * is an ffmpeg that has stopped decoding, and the frames it is not decoding are
 * the ones pump() needs to catch the sound up with.  So everything Qt has comes
 * out here and the queue is bounded from the producer instead.
 *
 * The cap is the backstop for the case `-re' does not cover: a stall long enough
 * that the burst afterwards is measured in seconds of film.  Six frames is a few
 * hundred kilobytes in the planar format and far more than pump() can ever make
 * use of -- to catch the sound up it only needs frames that are still DUE, and
 * anything older than that is going to be dropped unwatched anyway.  So it is
 * dropped here instead of after it has been carried around -- a buffer that used
 * to hold twenty-four frames was eleven megabytes on a board whose whole memory
 * budget is a thousand.
 */
void MediaPage::fill()
{
    if (!m_decoder)
        return;
    m_buffer.append(m_decoder->readAllStandardOutput());

    const int bytes = frameBytes();
    if (bytes <= 0)
        return;
    const int cap = bytes * 6;
    if (m_buffer.size() <= cap)
        return;

    const int over = (m_buffer.size() - cap) / bytes;
    if (over <= 0)
        return;
    m_buffer.remove(0, over * bytes);
    m_framesDecoded += over;
    m_framesDropped += over;
}

void MediaPage::readFrames()
{
    fill();
    pump();
}

/*
 * ── THE PICTURE, HELD AGAINST THE CLOCK THE SOUND IS ALREADY KEEPING ─────────
 *
 * The frame number IS the timestamp.  ffmpeg was asked for constant-rate output,
 * so the Nth frame to come out of the pipe belongs at startAt + N/fps, and a raw
 * pipe that carries no timestamps does not have to.
 *
 * Three cases, and only the first one is new:
 *
 *   EARLY.  The frame is decoded and its moment has not come.  It stays in the
 *   buffer and m_pace is armed for the difference.  This is what did not exist
 *   before: `-re' meant a frame was never early, which meant the buffer only ever
 *   held frames that were LATE, which meant the drop below ran constantly and the
 *   film sat permanently behind the sound with the count of what was skipped
 *   climbing on the strip.
 *
 *   DUE.  Draw it.  One frame per call, so a long stall does not turn into a
 *   burst of a dozen presents in one trip round the event loop -- which would be
 *   the freeze this whole change is about, arriving from the other direction.
 *
 *   LATE, WITH ANOTHER ONE BEHIND IT.  Skip it without drawing.  Nothing is drawn
 *   that is not going to be seen, and now that ffmpeg is running ahead rather than
 *   in lock step there are genuinely spare frames to skip, so the film reaches the
 *   sound again instead of trailing it for ever.
 */
void MediaPage::pump()
{
    if (m_pace)
        m_pace->stop();

    const int bytes = frameBytes();
    if (bytes <= 0 || m_view != ViewVideo || m_paused || m_panelHidden)
        return;

    const double rate = m_videoFps > 0.0 ? m_videoFps : 25.0;

    for (;;) {
        if (m_buffer.size() < bytes)
            return;

        /* Milliseconds into the film that this frame belongs at, against the same
         * position() the strip and the seek bar are drawn from. */
        const double due = (m_videoStart + m_framesDecoded / rate) * 1000.0;
        const double now = position() * 1000.0;
        const double ahead = due - now;

        /* If the frame is ahead of time, wait for its due time instead of showing immediately */
        if (ahead > 2.0) {
            if (m_pace)
                m_pace->start(qMax(1, (int)qRound(ahead)));
            return;
        }

        /* Late, and there is a fresher frame already decoded: this one is not worth
         * the memcpy and the GPU pass, because the next one is closer to the truth
         * and is going to overwrite it within milliseconds. */
        const bool late = ahead < -1000.0 / rate;
        if (late && m_buffer.size() >= 2 * bytes) {
            m_buffer.remove(0, bytes);
            ++m_framesDecoded;
            ++m_framesDropped;
            continue;
        }

        if (m_gl) {
            /* Kept rather than consumed: a paused film and a repaint forced from
             * outside both have to put this same frame back, and there is nowhere
             * else to get it from once the GPU has drawn it into scanout memory. */
            m_planes = m_buffer.left(bytes);
            m_buffer.remove(0, bytes);
            ++m_framesDecoded;
            ++m_framesShown;
            setLoading(false);
            present();
            return;
        }

        m_frame = QImage((const uchar *)m_buffer.constData(), m_frameW, m_frameH,
                         m_frameW * 4, QImage::Format_RGB32).copy();
        m_buffer.remove(0, bytes);
        ++m_framesDecoded;
        ++m_framesShown;
        setLoading(false);

        if (isVisible())
            update();
        return;
    }
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
    if (!m_gl || m_view != ViewVideo || !isVisible() || m_planes.isEmpty()
        || m_panelHidden)
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

    /* Whatever this is, it is not loading any more -- the spinner has to come down
     * even when the answer is "that file would not open". */
    setLoading(false);

    const QString err = QString::fromLocal8Bit(m_decoder->readAllStandardError()).trimmed();
    const int code = m_decoder->exitCode();

    if (code != 0 && !err.isEmpty())
        m_note = tidyChildError(err);
    else if (code != 0)
        m_note = tr("ffmpeg exited %1").arg(code);
    else
        m_note = tr("end of file");

    /*
     * The loop button.  QUEUED, and that is not tidiness: this is a slot on the
     * very process openVideo() would tear down, and deleting a QProcess from
     * inside its own finished() is a use-after-free in Qt's own dispatch on the
     * way back out.  Zero exit only -- a film that would not decode would
     * otherwise restart itself for ever, several times a second.
     */
    if (code == 0 && m_loopVideo && m_view == ViewVideo && !m_showing.path.isEmpty()) {
        const Entry again = m_showing;
        QTimer::singleShot(0, this, [this, again]() {
            if (m_loopVideo && m_view == ViewVideo && !videoLive())
                openVideo(again, 0.0);
        });
        refresh();
        return;
    }

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

/*
 * THE HALF THAT WAITS -- the same split openVideo() has, for the same reason.
 * The tags and the length used to be two blocking ffprobes in front of every
 * track, so pressing A on a song stopped the whole program for as much as eight
 * seconds before a note was heard.
 */
void MediaPage::playQueued(double startAt)
{
    const Entry *at = queuedTrack();
    if (!at)
        return;
    const Entry track = *at;

    if (!track.path.isEmpty() && m_probedPath == track.path) {
        playQueuedNow(startAt);
        return;
    }

    setLoading(true, track.name);
    m_duration = 0.0;
    m_trackTitle.clear();
    rebuild();
    probeThen(track.path, WaitMusic, track, startAt);
}

void MediaPage::playQueuedNow(double startAt)
{
    const Entry *at = queuedTrack();
    if (!at)
        return;
    /* By value: everything below can touch m_queue, and a pointer into a QVector
     * that reallocates is the same use-after-free the header warns about. */
    const Entry track = *at;

    setLoading(false);

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
    m_duration = m_probeDuration;
    /* The tags win; the file name is the floor.  A track called "03.mp3" with no
     * title tag is still a track with a name, and "03" is a better label than the
     * blank one this used to leave. */
    m_trackTitle = displayTitle(track, m_probeTitle);

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
    /* As in stopVideo(): a track abandoned while its ffprobe was still running
     * must not leave the shell's ring turning over whatever comes next. */
    setLoading(false);

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
    /* Whatever this film was waiting for, it is not waiting for it any more.  The
     * spinner belongs to the shell and nothing else here would take it down -- a
     * film abandoned between the probe and the first frame would have left a ring
     * turning on the card grid. */
    setLoading(false);

    endProcess(m_decoder);
    endProcess(m_videoAudio);
    endProcess(m_videoAplay);
    m_buffer.clear();
    m_framesShown = 0;
    m_framesDropped = 0;
    m_framesDecoded = 0;
    if (m_pace)
        m_pace->stop();
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

/* ── panel ownership ─────────────────────────────────────────────────────── */

/*
 * SIGSTOP or SIGCONT, to the film's processes and nothing else: the decoder
 * and whichever pair is feeding the card.  The music chain is deliberately not
 * in the list -- it does not draw, and a dashboard that is not in front is
 * still the one place its sound is meant to be heard from.
 */
void MediaPage::signalVideoChain(int sig)
{
    QProcess *const chain[] = { m_decoder, m_videoAudio, m_videoAplay };
    for (size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); ++i) {
        QProcess *p = chain[i];
        if (!p || p->state() != QProcess::Running || p->processId() <= 0)
            continue;
        ::kill((pid_t)p->processId(), sig);
    }
}

/*
 * THE PANEL HAS CHANGED HANDS, SO THE FILM HAS TO STOP WRITING IT.
 *
 * The GPU path draws into the memory the panel is scanning, straight past Qt --
 * which is what lets it put a picture up at all, and what would let it keep
 * that picture going over a task, a window service or the switcher the moment
 * one of them is in front.  Two writers on one framebuffer is the flicker the
 * board was reported for, and both of them paying for it -- the film decoding
 * and drawing at full rate while something else is on the glass -- is what
 * turned the flicker into a freeze.
 *
 * So the film goes exactly where the pause button puts it: its processes
 * SIGSTOP'd, and the clock banked, because the sound is stopped with the
 * picture and the due-time arithmetic in pump() has to stop with both.  It is
 * a nesting state, not the pause state: m_paused is what the user set, and a
 * film that was playing when the panel went away is playing -- from the same
 * position, sound and picture together -- the moment it comes back.
 */
void MediaPage::panelLost()
{
    if (m_panelHidden)
        return;
    m_panelHidden = true;
    if (m_pace)
        m_pace->stop();

    /*
     * A seek the slider was holding is COMMITTED, not dropped: seekTo() has
     * already moved the clock to the target, so abandoning the restart would
     * leave the picture minutes behind its own due-time arithmetic and pump()
     * dropping everything until ffmpeg caught up the hard way.  commitSeek()
     * starts the fresh chain, and openVideoNow() stops it again at its tail --
     * which is where a film that only came up while the panel was gone is
     * stopped too.
     */
    if (m_seekTimer && m_seekTimer->isActive()) {
        commitSeek();
        return;
    }

    if (!videoLive() || m_paused)
        return;
    signalVideoChain(SIGSTOP);
    m_pausedAt += m_clock.elapsed();
}

/*
 * The panel is the dashboard's again.  What was paused by the user stays
 * paused -- its frame goes back through the queued present() a repaint of this
 * page issues -- and what was running is let run, from the banked clock.
 */
void MediaPage::panelRegained()
{
    if (!m_panelHidden)
        return;
    m_panelHidden = false;
    if (!videoLive() || m_paused)
        return;
    signalVideoChain(SIGCONT);
    m_clock.restart();
    /* The frame that was in the pipe when the panel went away is due the
     * moment this returns; readyRead will not fire to say so until the decoder
     * -- which has only just been continued -- writes the next one. */
    pump();
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

    /*
     * The clock is what pump() paces against, so resuming has to give it a shove:
     * the frame that is now due is already in the buffer, and readyRead will not
     * fire again to say so until the decoder -- which has only just been sent
     * SIGCONT -- gets round to writing the next one.
     */
    if (!m_paused)
        pump();
    else if (m_pace)
        m_pace->stop();

    rebuild();
    refresh();
}

double MediaPage::position() const
{
    /* The shell's pause stops the clock the same way the user's does: the
     * picture and the sound are both frozen while the panel is gone. */
    const bool frozen = m_paused || m_panelHidden;
    return (m_pausedAt + (frozen ? 0 : m_clock.elapsed())) / 1000.0;
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
        /*
         * THE SAFETY NET UNDER pump()'s OWN RE-ARM.  Everything that normally
         * moves the picture along is edge-triggered -- a readyRead from the pipe,
         * a one-shot pace timer -- and a missed edge is missed for ever.  A
         * decoder that filled the pipe while a frame was being held emits no
         * further readyRead until something reads; a pace timer stopped by a pause
         * has nothing but this to start it again.  Twice a second is far too slow
         * to pace a film with and exactly right for noticing that the pacing has
         * stopped.
         */
        if (videoLive() && !m_paused)
            pump();
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
        /* Any press at all is somebody who is there, which is the whole of the
         * show-the-controls rule.  Before the switch, so that the transport is
         * already up when a press changes something it displays. */
        nudgeTransport();

        switch (action) {
        case Joypad::NavOk:
        case Joypad::NavMenu:
            togglePause();
            return true;
        case Joypad::NavLeft:  seekBy(-10); return true;
        case Joypad::NavRight: seekBy(10); return true;
        case Joypad::NavUp:    seekBy(60); return true;
        case Joypad::NavDown:  seekBy(-60); return true;
        /*
         * THE SHOULDERS REACH THE TWO TOGGLES.  They are free here -- the dock
         * they used to switch tabs on is gone -- and without them loop and
         * fullscreen would be buttons only a mouse could press, on a device that
         * usually has no mouse plugged into it.  Which is which is not something
         * anybody has to remember: pressing one lights its icon in the strip that
         * the press has just brought up.
         */
        case Joypad::NavPrevPage: pressControl(CtlLoop); return true;
        case Joypad::NavNextPage: pressControl(CtlZoom); return true;
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
    /* A film has the transport instead of the strip, and it comes and goes. */
    if (m_view == ViewVideo)
        return transportUp() ? transportRect() : QRect();

    const int barH = 34;
    const int noteH = noteText().isEmpty() ? 0 : 22;
    return QRect(0, height() - barH - noteH, width(), barH + noteH);
}

void MediaPage::paintChrome(QPainter &p) const
{
    if (m_view == ViewVideo) {
        paintTransport(p);
        return;
    }

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

/* ── the transport ───────────────────────────────────────────────────────── */

/*
 * ── WHY THE CONTROLS ARE PAINTED AND NOT BUILT ──────────────────────────────
 *
 * Every other page in this dashboard is made of widgets, and this one cannot be.
 * While a film is up the pixels on the glass came from lima writing into the
 * scanout, not from Qt's backing store, so a QPushButton over the picture is not
 * a button over a picture: it is a memcpy of a rectangle Qt believes is empty,
 * landing on top of the film, and being painted over again 25 times a second.  It
 * also DIRTIES that rectangle, which is the bug that used to drag a grey square
 * about behind the mouse.  See the layer note in glvideo.h.
 *
 * So the transport is a picture -- rendered once whenever its content changes,
 * uploaded as the chrome texture, and blended by the pass that owns the pixels.
 * The cost of that is the hit test below, which is thirty lines and cannot get out
 * of step with the drawing because both are built from the same buttons() list.
 *
 * WHAT IT REPLACED was the word "decoding..." in the middle of a black screen and
 * a strip along the foot with a clock in it.  There was no way to pause with a
 * mouse, no way to see how far through the film you were without reading a
 * timestamp, and nothing at all said whether the thing was going to loop.
 */
namespace {

/* All of the geometry, in one place, because the drawing and the hit test both
 * read it and a number that appeared twice would eventually appear differently. */
const int TrPad = 16;      /* left and right margin inside the panel */
const int TrNoteH = 20;    /* the complaint line, when there is one */
const int TrTitleY = 6;    /* everything below is from the panel top, past the note */
const int TrTitleH = 20;
const int TrBarY = 32;
const int TrBarH = 5;
const int TrKnob = 7;
const int TrTimesY = 40;
const int TrTimesH = 15;
const int TrBtnY = 60;
const int TrBtnW = 42;
const int TrBtnH = 34;
const int TrBtnGap = 6;
const int TrPanelH = TrBtnY + TrBtnH + 8;

}

/* The glyphs, drawn rather than shipped: a 20 px icon font would be another file
 * to stage on the card, and there are six of these.  A member, and not the file
 * static it started as, only because the ids it switches on are this class's. */
void MediaPage::drawGlyph(QPainter &p, int id, const QRect &box, bool paused, bool on)
{
    const QRect g(box.center().x() - 9, box.center().y() - 9, 19, 19);
    const QColor ink = on ? Theme::blue() : Theme::ink();

    p.setPen(Qt::NoPen);
    p.setBrush(ink);

    switch (id) {
    case CtlPlay:
        if (paused) {
            QPainterPath tri;
            tri.moveTo(g.left() + 3, g.top());
            tri.lineTo(g.left() + 3, g.bottom() + 1);
            tri.lineTo(g.right() + 1, g.center().y() + 0.5);
            tri.closeSubpath();
            p.drawPath(tri);
        } else {
            p.drawRect(QRect(g.left() + 3, g.top(), 5, g.height() + 1));
            p.drawRect(QRect(g.right() - 7, g.top(), 5, g.height() + 1));
        }
        break;

    case CtlBack:
    case CtlForward: {
        /* Two chevrons, and the direction is the sign of the step.  The 10 under
         * them is what makes it a jump rather than a scan -- there is no scan on
         * this player, because a pipe cannot be run backwards. */
        const bool back = (id == CtlBack);
        for (int i = 0; i < 2; ++i) {
            const int x = g.left() + i * 9;
            QPainterPath tri;
            if (back) {
                tri.moveTo(x + 8, g.top() + 1);
                tri.lineTo(x + 8, g.bottom() - 4);
                tri.lineTo(x, (g.top() + g.bottom() - 3) / 2.0);
            } else {
                tri.moveTo(x, g.top() + 1);
                tri.lineTo(x, g.bottom() - 4);
                tri.lineTo(x + 8, (g.top() + g.bottom() - 3) / 2.0);
            }
            tri.closeSubpath();
            p.drawPath(tri);
        }
        p.setPen(ink);
        p.setFont(Theme::font(9));
        p.drawText(QRect(g.left(), g.bottom() - 5, g.width(), 7),
                   Qt::AlignHCenter | Qt::AlignTop, "10");
        break;
    }

    case CtlLoop: {
        /* A track running round with an arrowhead on it.  Lit when it is on, which
         * is the only way a toggle painted into a texture can say so. */
        QPen pen(ink, 2);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const QRectF ring(g.left() + 1, g.top() + 3, g.width() - 2, g.height() - 7);
        QPainterPath path;
        path.addRoundedRect(ring, 5, 5);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        QPainterPath head;
        head.moveTo(ring.right() - 4, ring.top() - 4);
        head.lineTo(ring.right() + 1, ring.top());
        head.lineTo(ring.right() - 4, ring.top() + 4);
        head.closeSubpath();
        p.drawPath(head);
        break;
    }

    case CtlZoom: {
        /* Four corners, pointing out to fill and in to come back. */
        QPen pen(ink, 2);
        pen.setCapStyle(Qt::FlatCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const int a = on ? 7 : 0;         /* inset of the corner itself */
        const int len = 6;
        const int L = g.left() + a, R = g.right() - a;
        const int T = g.top() + a, B = g.bottom() - a;
        const int s = on ? -1 : 1;
        p.drawLine(L, T, L + len * s, T);
        p.drawLine(L, T, L, T + len * s);
        p.drawLine(R, T, R - len * s, T);
        p.drawLine(R, T, R, T + len * s);
        p.drawLine(L, B, L + len * s, B);
        p.drawLine(L, B, L, B - len * s);
        p.drawLine(R, B, R - len * s, B);
        p.drawLine(R, B, R, B - len * s);
        break;
    }

    case CtlClose: {
        QPen pen(ink, 2);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(g.topLeft() + QPoint(2, 2), g.bottomRight() - QPoint(2, 2));
        p.drawLine(g.topRight() + QPoint(-2, 2), g.bottomLeft() + QPoint(2, -2));
        break;
    }

    default:
        break;
    }
}

QRect MediaPage::transportRect() const
{
    const int noteH = noteText().isEmpty() ? 0 : TrNoteH;
    const int h = TrPanelH + noteH;
    return QRect(0, qMax(0, height() - h), width(), qMin(h, height()));
}

QRect MediaPage::barRect() const
{
    const QRect panel = transportRect();
    const int noteH = noteText().isEmpty() ? 0 : TrNoteH;
    return QRect(panel.left() + TrPad, panel.top() + noteH + TrBarY,
                 qMax(1, panel.width() - 2 * TrPad), TrBarH);
}

QVector<MediaPage::Button> MediaPage::buttons() const
{
    QVector<Button> out;
    const QRect panel = transportRect();
    const int noteH = noteText().isEmpty() ? 0 : TrNoteH;
    const int y = panel.top() + noteH + TrBtnY;

    /* Transport on the left, the two toggles and the way out on the right.  Both
     * groups are anchored to their own edge so a wider panel spreads them apart
     * rather than moving them together. */
    const int leftIds[] = { CtlBack, CtlPlay, CtlForward };
    int x = panel.left() + TrPad;
    for (int i = 0; i < 3; ++i) {
        Button b;
        b.id = leftIds[i];
        b.where = QRect(x, y, TrBtnW, TrBtnH);
        out.append(b);
        x += TrBtnW + TrBtnGap;
    }

    const int rightIds[] = { CtlLoop, CtlZoom, CtlClose };
    x = panel.right() + 1 - TrPad - (3 * TrBtnW + 2 * TrBtnGap);
    for (int i = 0; i < 3; ++i) {
        Button b;
        b.id = rightIds[i];
        b.where = QRect(x, y, TrBtnW, TrBtnH);
        out.append(b);
        x += TrBtnW + TrBtnGap;
    }
    return out;
}

bool MediaPage::transportUp() const
{
    /*
     * The two and a half seconds are for a film that is PLAYING and being watched.
     * Everything else here is a state in which the controls are the only thing on
     * the screen worth looking at -- nothing has decoded yet, it is paused, the
     * decoder has gone, or a thumb is on the bar -- and hiding them there would be
     * hiding the only way out of a black screen.
     */
    if (m_paused || m_scrubbing || m_loading || !videoLive())
        return true;
    return m_transportShown;
}

void MediaPage::nudgeTransport()
{
    const bool was = transportUp();
    m_transportShown = true;
    if (m_transportTimer)
        m_transportTimer->start();
    if (!was)
        refresh();
}

void MediaPage::hideTransport()
{
    if (!m_transportShown)
        return;
    m_transportShown = false;
    m_hover = CtlNone;
    if (m_view == ViewVideo)
        refresh();
}

int MediaPage::controlAt(const QPoint &at) const
{
    if (m_view != ViewVideo || !transportUp())
        return CtlNone;

    /* Generous: these are 42x34 targets being hit with a stick-driven arrow on a
     * 640x480 panel, and a miss costs a press of something else. */
    const QVector<Button> bs = buttons();
    for (int i = 0; i < bs.size(); ++i) {
        if (bs[i].where.adjusted(-3, -6, 3, 6).contains(at))
            return bs[i].id;
    }
    const QRect bar = barRect();
    if (bar.adjusted(-4, -11, 4, 11).contains(at))
        return CtlBar;
    return CtlNone;
}

void MediaPage::pressControl(int id)
{
    switch (id) {
    case CtlPlay:
        togglePause();
        break;
    case CtlBack:
        seekBy(-10);
        break;
    case CtlForward:
        seekBy(10);
        break;
    case CtlLoop:
        m_loopVideo = !m_loopVideo;
        emit toastRequested(m_loopVideo ? tr("repeat on") : tr("repeat off"), 1400);
        break;
    case CtlZoom:
        /* The scale is ffmpeg's, so this is the seek path with the same start
         * time -- see the filter note in openVideoNow(). */
        m_fill = !m_fill;
        if (!m_showing.path.isEmpty())
            openVideo(m_showing, qMax(0.0, position()));
        break;
    case CtlClose:
        stopVideo();
        dropFrame();
        m_note.clear();
        setView(ViewBrowse);
        return;
    default:
        return;
    }

    nudgeTransport();
    refresh();
}

void MediaPage::paintTransport(QPainter &p) const
{
    if (!transportUp())
        return;

    const QRect panel = transportRect();
    const int noteH = noteText().isEmpty() ? 0 : TrNoteH;

    /*
     * A scrim rather than a slab: it fades in from nothing at the top edge, so the
     * bottom of the picture is dimmed into the controls instead of being cut off
     * by a line across it.  Nearly opaque at the foot, because white text on a
     * bright scene is text nobody can read.
     */
    QLinearGradient scrim(panel.topLeft(), panel.bottomLeft());
    scrim.setColorAt(0.0, QColor(6, 7, 11, 0));
    scrim.setColorAt(0.35, QColor(6, 7, 11, 170));
    scrim.setColorAt(1.0, QColor(6, 7, 11, 232));
    p.setPen(Qt::NoPen);
    p.setBrush(scrim);
    p.drawRect(panel);

    const QString note = noteText();
    if (!note.isEmpty()) {
        p.setFont(Theme::font(11));
        p.setPen(Theme::orange());
        p.drawText(QRect(panel.left() + TrPad, panel.top() + 3,
                         panel.width() - 2 * TrPad, TrNoteH),
                   Qt::AlignLeft | Qt::AlignVCenter, note);
    }

    /* The name of the thing, which is the tags' title if the container had one and
     * the file name without its extension otherwise -- never "decoding...". */
    const QString title = m_trackTitle.isEmpty() ? m_showing.name : m_trackTitle;
    const QRect titleRect(panel.left() + TrPad, panel.top() + noteH + TrTitleY,
                          panel.width() - 2 * TrPad, TrTitleH);
    p.setFont(Theme::font(13));
    p.setPen(Theme::ink());
    QString shown = title;
    {
        const QFontMetrics fm(p.font());
        const int room = titleRect.width() - 90;
        if (room > 40)
            shown = fm.elidedText(title, Qt::ElideRight, room);
    }
    p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, shown);

    if (m_framesDropped > 0) {
        p.setFont(Theme::font(10));
        p.setPen(Theme::ink3());
        p.drawText(titleRect, Qt::AlignRight | Qt::AlignVCenter,
                   tr("%1 dropped").arg(m_framesDropped));
    }

    /* ── the timebar ─────────────────────────────────────────────────────── */
    const QRect bar = barRect();
    const double total = m_videoDuration;
    const double at = qBound(0.0, position(), total > 0.0 ? total : position());
    const double frac = total > 0.0 ? at / total : 0.0;

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 58));
    p.drawRoundedRect(bar, TrBarH / 2.0, TrBarH / 2.0);

    if (total > 0.0) {
        QRect done = bar;
        done.setWidth(qMax(1, (int)(bar.width() * frac)));
        p.setBrush(Theme::blue());
        p.drawRoundedRect(done, TrBarH / 2.0, TrBarH / 2.0);

        /* The knob is bigger while it is being dragged, which is the only feedback
         * a scrub gets on a panel with no cursor of its own. */
        const int r = m_scrubbing ? TrKnob + 2 : TrKnob;
        p.setBrush(Theme::ink());
        p.drawEllipse(QPointF(bar.left() + bar.width() * frac, bar.center().y() + 0.5),
                      r, r);
    }

    p.setFont(Theme::font(11));
    p.setPen(Theme::ink2());
    const QRect times(panel.left() + TrPad, panel.top() + noteH + TrTimesY,
                      panel.width() - 2 * TrPad, TrTimesH);
    p.drawText(times, Qt::AlignLeft | Qt::AlignVCenter, humanTime((int)at));
    p.drawText(times, Qt::AlignRight | Qt::AlignVCenter,
               total > 0.0 ? humanTime((int)total) : QString("--:--"));

    /* ── the buttons ─────────────────────────────────────────────────────── */
    const QVector<Button> bs = buttons();
    for (int i = 0; i < bs.size(); ++i) {
        const Button &b = bs[i];
        if (m_hover == b.id) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 34));
            p.drawRoundedRect(b.where, 8, 8);
        }
        const bool lit = (b.id == CtlLoop && m_loopVideo) ||
                         (b.id == CtlZoom && m_fill);
        drawGlyph(p, b.id, b.where, m_paused, lit);
    }
}

void MediaPage::mousePressEvent(QMouseEvent *event)
{
    if (m_view != ViewVideo) {
        PageWidget::mousePressEvent(event);
        return;
    }

    /* THE FIRST CLICK ONLY WAKES IT.  Pressing a button that was not on the screen
     * when the press started is how a hidden transport turns into a film that
     * pauses itself for no reason anybody can see. */
    const bool was = transportUp();
    nudgeTransport();
    if (!was) {
        refresh();
        return;
    }

    const int id = controlAt(event->pos());
    if (id == CtlBar) {
        if (m_videoDuration > 0.0) {
            m_scrubbing = true;
            const QRect bar = barRect();
            const double frac = qBound(0.0, (event->pos().x() - (double)bar.left()) /
                                                qMax(1, bar.width()), 1.0);
            seekTo(frac * m_videoDuration);
        }
        return;
    }
    if (id != CtlNone)
        pressControl(id);
}

void MediaPage::mouseMoveEvent(QMouseEvent *event)
{
    if (m_view != ViewVideo) {
        PageWidget::mouseMoveEvent(event);
        return;
    }

    /* Moving the mouse is asking for the controls, on every player ever written. */
    nudgeTransport();

    if (m_scrubbing && m_videoDuration > 0.0) {
        const QRect bar = barRect();
        const double frac = qBound(0.0, (event->pos().x() - (double)bar.left()) /
                                            qMax(1, bar.width()), 1.0);
        /* seekTo() moves the clock now and restarts ffmpeg a third of a second
         * after the last one of these, so a drag costs one decoder and not one per
         * pixel.  That debounce was written for the D-pad's autorepeat and it is
         * exactly what a drag needs. */
        seekTo(frac * m_videoDuration);
        return;
    }

    const int id = controlAt(event->pos());
    if (id == m_hover)
        return;
    m_hover = id;
    refresh();
}

void MediaPage::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_view != ViewVideo) {
        PageWidget::mouseReleaseEvent(event);
        return;
    }
    if (!m_scrubbing)
        return;
    m_scrubbing = false;
    nudgeTransport();
    refresh();
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
    QString key = m_showing.name + QChar(0x1f) + chromeRight() + QChar(0x1f) +
                  noteText() + QChar(0x1f) + QString::number(cr.width()) + "x" +
                  QString::number(cr.height());
    /*
     * EVERYTHING THE TRANSPORT DRAWS HAS TO BE IN HERE, or the strip is not
     * re-rendered when it changes and the panel keeps a texture of the old one.
     * chromeRight() already carries the clock, the pause and the dropped count;
     * these are the rest of it -- what is under the mouse, which way the two
     * toggles are set, and the title, which arrives late because it comes out of
     * ffprobe.
     */
    if (m_view == ViewVideo)
        key += QChar(0x1f) + m_trackTitle + QChar(0x1f) +
               QString::number(m_hover) + (m_loopVideo ? "L" : "-") +
               (m_fill ? "F" : "-") + (m_scrubbing ? "S" : "-");
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
/*
 * The spinner, on the same terms as the two above it.  It arrives about twelve
 * times a second while it is turning and not at all when it is not, which is why
 * the ring stops its own timer the moment it is taken down -- an animation nobody
 * can see would otherwise re-composite a film for the rest of its length.
 */
void MediaPage::setBusyOverlay(const QImage &argb, const QRect &at)
{
    if (!m_gl)
        return;
    if (argb.isNull() || at.isEmpty())
        m_gl->clearOverlay(GlVideo::BusyLayer);
    else
        m_gl->setOverlay(GlVideo::BusyLayer, argb, at);
    present();
}

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
        }
        /*
         * NOTHING WHERE "decoding..." USED TO BE.  A film that has not produced a
         * frame yet is a film something is still being done about, and what says so
         * is the shell's spinner -- one ring, over whatever page is up, turning,
         * which is the one thing a line of static text could never do.  See
         * setLoading() and busyRequested().
         */

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
    } else if (m_places) {
        right = m_entries.size() == 1 ? tr("1 device")
                                      : tr("%1 devices").arg(m_entries.size());
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
