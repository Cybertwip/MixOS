/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "media.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QProcess>
#include <QResizeEvent>
#include <QTimer>

#include <signal.h>

#include "joypad.h"
#include "settings.h"
#include "theme.h"

namespace {

const char *kAudioExt = "mp3 flac ogg oga opus wav m4a aac wma aiff mid";
const char *kVideoExt = "mp4 mkv avi webm mov m4v mpg mpeg wmv flv ts 3gp ogv";
const char *kImageExt = "jpg jpeg png bmp gif webp pbm pgm ppm xbm xpm tif tiff";

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

/* The remembered root if it is still a directory, the default if not.  Checked
 * again here and not just at load time because the card it named can be pulled. */
QString mediaStartDir()
{
    const QString remembered = Settings::instance().mediaRoot();
    return QFileInfo(remembered).isDir() ? remembered : defaultMediaRoot();
}

} /* namespace */

MediaPage::MediaPage(QWidget *parent)
    : PageWidget(parent)
{
    m_list = new ListPane(this);
    m_list->setRowHeight(30);
    m_list->setPlaceholder(tr("Nothing playable here.\nB goes up a directory."));
    connect(m_list, &ListPane::activated, this, &MediaPage::onActivated);

    m_ui = new QTimer(this);
    m_ui->setInterval(500);
    connect(m_ui, &QTimer::timeout, this, &MediaPage::tick);

    m_dir = mediaStartDir();
}

MediaPage::~MediaPage()
{
    stopPlayback();
}

QString MediaPage::title() const
{
    if (m_mode != ModeBrowse && !m_playing.name.isEmpty())
        return m_playing.name;
    return tr("Media");
}

bool MediaPage::wantsFullscreen() const
{
    /* Only while a picture or a video is on the glass.  The browser and the music
     * player both want the status bar -- one to say where you are, the other to
     * keep the clock and the battery in sight while a record plays. */
    return m_mode == ModeImage || m_mode == ModeVideo;
}

void MediaPage::resizeEvent(QResizeEvent *event)
{
    const QRect card(Theme::Margin, Theme::Margin,
                     width() - 2 * Theme::Margin, height() - 2 * Theme::Margin);
    m_list->setGeometry(card.x() + 6, card.y() + 36 + 4, card.width() - 12,
                        card.height() - 36 - 10);
    QWidget::resizeEvent(event);
}

void MediaPage::onEnter()
{
    if (m_dir.isEmpty() || !QFileInfo(m_dir).isDir())
        m_dir = mediaStartDir();
    populate(m_dir);
    m_ui->start();
}

void MediaPage::onLeave()
{
    m_ui->stop();
    /*
     * Video and pictures stop; MUSIC KEEPS PLAYING.  That asymmetry is the whole
     * reason the music player does not take the screen: a handheld that stops the
     * album the moment you go and look at something else is a handheld nobody
     * listens to music on.
     */
    if (m_mode == ModeVideo || m_mode == ModeImage) {
        stopPlayback();
        setMode(ModeBrowse);
    }
}

void MediaPage::setMode(int mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    /* The list is a real child widget; behind a full-screen picture it would paint
     * over the picture rather than under it. */
    m_list->setVisible(mode == ModeBrowse);
    emit titleChanged();
    update();
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
    QDir d(dir.isEmpty() ? defaultMediaRoot() : dir);
    m_dir = d.absolutePath();
    m_entries.clear();

    if (!d.exists()) {
        rebuild();
        emit titleChanged();
        return;
    }

    if (!d.isRoot()) {
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

    rebuild();
    emit titleChanged();
}

bool MediaPage::openPath(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return false;

    if (info.isDir()) {
        populate(info.absoluteFilePath());
        m_list->setCurrent(0);
        return true;
    }

    const int kind = kindFor(info);
    if (kind != KindAudio && kind != KindVideo && kind != KindImage)
        return false;

    /* Browse to the containing directory first: opening a file with no list
     * behind it means Back from the picture lands on an empty page, and the
     * next/previous image walk has nothing to walk. */
    populate(info.absolutePath());
    const QString want = info.absoluteFilePath();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].path != want)
            continue;
        m_list->setCurrent(i);
        open(m_entries[i]);
        return true;
    }
    return false;
}

void MediaPage::rebuild()
{
    QVector<ListRow> rows;

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

        if (m_mode == ModeAudio && e.path == m_playing.path) {
            r.badge = m_paused ? tr("paused") : tr("playing");
            r.badgeColour = m_paused ? Theme::orange() : Theme::green();
        }
        rows.append(r);
    }

    const int keep = m_list->current();
    m_list->setRows(rows);
    if (keep >= 0 && keep < rows.size())
        m_list->setCurrent(keep);
    update();
}

void MediaPage::onActivated(int index)
{
    const QVector<ListRow> &rows = m_list->rows();
    if (index < 0 || index >= rows.size())
        return;
    const int entry = rows[index].id;
    if (entry < 0 || entry >= m_entries.size())
        return;
    open(m_entries[entry]);
}

void MediaPage::open(Entry entry)
{
    switch (entry.kind) {
    case KindUp:
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
        openAudio(entry);
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

    stopPlayback();
    m_frame = image;
    m_playing = entry;
    m_note.clear();
    setMode(ModeImage);
    update();
}

void MediaPage::stepImage(int delta)
{
    /* Next and previous picture in the same directory, skipping everything that
     * is not one. */
    int at = -1;
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].path == m_playing.path)
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

/* ── ffmpeg ──────────────────────────────────────────────────────────────── */

QString MediaPage::ffmpegPath() const
{
    static const QString path = firstExisting(QStringList()
                                              << "/usr/bin/ffmpeg" << "/bin/ffmpeg"
                                              << "/usr/local/bin/ffmpeg");
    return path;
}

bool MediaPage::hasSoundCard() const
{
    /*
     * A PLAYBACK PCM, not merely /dev/snd.  This is what decides whether the
     * single-process path below is allowed to name an alsa output at all: with
     * no card, `-f alsa default' does not degrade to silence, it makes ffmpeg
     * exit -- and that one process is also carrying the video, so an absent card
     * would take the picture down with it.  The fallback has to be picked before
     * the pipeline is built, which means asking here rather than finding out.
     *
     * Not cached: the card is a module the boot word may or may not have loaded,
     * and it can also go away.  Two readdirs on a tmpfs are not worth a stale
     * answer.
     */
    const QDir snd(QStringLiteral("/dev/snd"));
    if (!snd.exists())
        return false;
    /* pcmC0D0p -- the trailing p is playback; capture-only devices end in c. */
    return !snd.entryList(QStringList() << QStringLiteral("pcmC*D*p"),
                          QDir::System | QDir::NoDotAndDotDot).isEmpty();
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
     * that has the muxer.  That is the message the board was showing.
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
    if (!p.waitForStarted(8000))
        return false;
    if (!p.waitForFinished(15000)) {
        p.kill();
        p.waitForFinished(500);
        return false;
    }

    cached = 0;

    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput())
                        + QString::fromLocal8Bit(p.readAllStandardError());
    for (const QString &line : out.split('\n')) {
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
        if (p.waitForStarted(1500) && p.waitForFinished(4000)) {
            bool ok = false;
            const double d = QString::fromLatin1(p.readAllStandardOutput()).trimmed().toDouble(&ok);
            if (ok && d > 0.0)
                return d;
        } else {
            p.kill();
            p.waitForFinished(500);
        }
    }

    /* No ffprobe: ask ffmpeg to open the file and say what it found.  It exits 1
     * for having no output, which is expected and not an error here. */
    if (ffmpegPath().isEmpty())
        return 0.0;
    QProcess p;
    p.start(ffmpegPath(), QStringList() << "-hide_banner" << "-i" << path);
    if (!p.waitForStarted(1500) || !p.waitForFinished(4000)) {
        p.kill();
        p.waitForFinished(500);
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

/* ── video ───────────────────────────────────────────────────────────────── */

void MediaPage::openVideo(const Entry &entry, double startAt)
{
    if (ffmpegPath().isEmpty()) {
        emit toastRequested(tr("ffmpeg is not installed.\nInstall it from Packages."), 4000);
        return;
    }

    stopPlayback();

    m_playing = entry;
    m_buffer.clear();
    m_framesShown = 0;
    m_framesDropped = 0;
    m_paused = false;
    m_note.clear();
    if (m_duration <= 0.0 || startAt <= 0.0)
        m_duration = probeDuration(entry.path);

    /* Even dimensions: several of the scalers and every yuv420 path want them, and
     * an odd width is a whole class of "ffmpeg exited 1" that is not worth having. */
    m_frameW = (width() / 2) * 2;
    m_frameH = (height() / 2) * 2;
    if (m_frameW < 16 || m_frameH < 16) {
        m_frameW = 640;
        m_frameH = 480;
    }

    const QString filter = QString("scale=w=%1:h=%2:force_original_aspect_ratio=decrease,"
                                   "pad=%1:%2:(ow-iw)/2:(oh-ih)/2,format=bgra")
                               .arg(m_frameW).arg(m_frameH);

    QStringList args;
    args << "-nostdin" << "-hide_banner" << "-loglevel" << "error" << "-re";
    /*
     * -ss BEFORE -i is the fast form: it jumps by the container index rather than
     * decoding and throwing away everything up to the point, which on this CPU is
     * the difference between instant and half a minute.
     */
    if (startAt > 0.0)
        args << "-ss" << QString::number(startAt, 'f', 2);
    args << "-i" << entry.path
         << "-map" << "0:v:0"
         << "-vf" << filter
         << "-f" << "rawvideo" << "-pix_fmt" << "bgra";

    /*
     * The card is asked about FIRST and separately, because the two failures want
     * different words.  No card is a boot-word or a driver matter and no ffmpeg
     * will fix it; no muxer is an ffmpeg matter and the card is fine.  Reporting
     * either as the other is what sends someone rebuilding the wrong half.
     */
    const bool card = hasSoundCard();
    const bool alsa = card && ffmpegHasAlsa();
    if (alsa) {
        /* pipe:1 has to be named before the second output, and the audio map is
         * optional -- the trailing ? is what keeps a silent clip from failing. */
        args << "pipe:1"
             << "-map" << "0:a:0?" << "-f" << "alsa" << "default";
    } else {
        args << "-an" << "pipe:1";
    }

    m_decoder = new QProcess(this);
    m_decoder->setReadChannel(QProcess::StandardOutput);
    connect(m_decoder, &QProcess::readyReadStandardOutput, this, &MediaPage::readFrames);
    connect(m_decoder, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, &MediaPage::onDecoderFinished);
    m_decoder->start(ffmpegPath(), args);

    if (!card) {
        /*
         * Nothing to play into, so neither chain is started -- a second ffmpeg
         * feeding an aplay that cannot open a device is two processes failing in
         * the background while the screen says nothing.  The note is the whole of
         * the response.
         */
        m_note = tr("no sound card on this device");
    } else if (!alsa) {
        /*
         * Audio in its own chain, which drifts from the video because the two
         * decodes have no shared clock.  aplay's blocking writes pace it, so the
         * drift is bounded by the sound card's buffer rather than unbounded, and
         * this only happens on an ffmpeg without the alsa muxer.
         */
        const QString aplay = firstExisting(QStringList()
                                            << "/usr/bin/aplay" << "/bin/aplay");
        if (!aplay.isEmpty()) {
            QStringList sideArgs;
            sideArgs << "-nostdin" << "-hide_banner" << "-loglevel" << "error";
            if (startAt > 0.0)
                sideArgs << "-ss" << QString::number(startAt, 'f', 2);
            sideArgs << "-i" << entry.path
                     << "-vn" << "-f" << "s16le"
                     << "-ar" << "44100" << "-ac" << "2" << "-";

            m_audioSide = new QProcess(this);
            m_aplay = new QProcess(this);
            m_audioSide->setStandardOutputProcess(m_aplay);
            m_audioSide->start(ffmpegPath(), sideArgs);
            m_aplay->start(aplay, QStringList() << "-q" << "-f" << "cd" << "-");
            m_note = tr("audio on a separate clock -- no alsa muxer in ffmpeg");
        }
    }

    m_clock.restart();
    m_pausedAt = (qint64)(startAt * 1000.0);
    setMode(ModeVideo);
    update();
}

void MediaPage::readFrames()
{
    if (!m_decoder)
        return;

    const int frameBytes = m_frameW * m_frameH * 4;
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
    m_frame = QImage((const uchar *)m_buffer.constData() + offset,
                     m_frameW, m_frameH, m_frameW * 4, QImage::Format_RGB32).copy();
    m_buffer.remove(0, whole * frameBytes);
    ++m_framesShown;

    if (isVisible())
        update();
}

void MediaPage::onDecoderFinished()
{
    if (!m_decoder)
        return;

    const QString err = QString::fromLocal8Bit(m_decoder->readAllStandardError()).trimmed();
    const int code = m_decoder->exitCode();

    if (code != 0 && !err.isEmpty())
        m_note = err.section('\n', 0, 1);
    else if (code != 0)
        m_note = tr("ffmpeg exited %1").arg(code);
    else
        m_note = tr("end of file");

    /* The picture stays on the glass with the note under it, rather than dropping
     * back to the list -- which is what the old card did, and why the only thing
     * anybody ever saw of a failure was a toast that said "exited 1". */
    update();
}

/* ── music ───────────────────────────────────────────────────────────────── */

void MediaPage::openAudio(const Entry &entry, double startAt)
{
    const QString ff = ffmpegPath();
    const QString aplay = firstExisting(QStringList() << "/usr/bin/aplay" << "/bin/aplay");
    if (ff.isEmpty() || aplay.isEmpty()) {
        emit toastRequested(ff.isEmpty() ? tr("ffmpeg is not installed")
                                         : tr("aplay is not installed (alsa-utils)"), 4000);
        return;
    }

    stopPlayback();

    m_playing = entry;
    m_paused = false;
    m_note.clear();
    if (m_duration <= 0.0 || startAt <= 0.0)
        m_duration = probeDuration(entry.path);

    /*
     * Raw s16le at 44.1 kHz into `aplay -f cd' rather than a WAV stream: a WAV
     * header written to a pipe has to lie about its length, and while aplay copes
     * with that, the raw form has no header to be wrong.
     */
    m_music = new QProcess(this);
    m_aplay = new QProcess(this);
    m_music->setStandardOutputProcess(m_aplay);
    connect(m_music, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
                if (code != 0) {
                    const QString err = m_music
                        ? QString::fromLocal8Bit(m_music->readAllStandardError()).trimmed()
                        : QString();
                    m_note = err.isEmpty() ? tr("ffmpeg exited %1").arg(code)
                                           : err.section('\n', 0, 0);
                } else {
                    m_note = tr("end of track");
                }
                rebuild();
            });

    QStringList args;
    args << "-nostdin" << "-hide_banner" << "-loglevel" << "error";
    if (startAt > 0.0)
        args << "-ss" << QString::number(startAt, 'f', 2);
    args << "-i" << entry.path
         << "-vn" << "-f" << "s16le"
         << "-ar" << "44100" << "-ac" << "2" << "-";

    m_music->start(ff, args);
    m_aplay->start(aplay, QStringList() << "-q" << "-f" << "cd" << "-");

    m_clock.restart();
    m_pausedAt = (qint64)(startAt * 1000.0);
    setMode(ModeAudio);
    rebuild();
}

/* ── transport ───────────────────────────────────────────────────────────── */

void MediaPage::stopPlayback()
{
    /* SIGCONT first: a stopped process ignores SIGTERM until it runs again, and a
     * paused player that is then closed would otherwise stay in the process table
     * holding the sound card. */
    QProcess *const all[] = { m_decoder, m_audioSide, m_aplay, m_music };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        QProcess *p = all[i];
        if (!p || p->state() == QProcess::NotRunning)
            continue;
        if (p->processId() > 0)
            ::kill((pid_t)p->processId(), SIGCONT);
        p->terminate();
    }
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        QProcess *p = all[i];
        if (!p)
            continue;
        if (p->state() != QProcess::NotRunning && !p->waitForFinished(400))
            p->kill();
        p->deleteLater();
    }

    m_decoder = nullptr;
    m_audioSide = nullptr;
    m_aplay = nullptr;
    m_music = nullptr;
    m_buffer.clear();
    m_frame = QImage();
    m_paused = false;
}

void MediaPage::togglePause()
{
    /*
     * SIGSTOP and SIGCONT rather than a pause protocol, because neither ffmpeg on a
     * pipe nor aplay has one.  Stopping the writer stops the pipe filling; stopping
     * aplay as well stops the sound card being fed.  It is exactly what ^Z does in
     * a shell, and it resumes with the buffers intact.
     */
    const int sig = m_paused ? SIGCONT : SIGSTOP;
    QProcess *const all[] = { m_decoder, m_audioSide, m_aplay, m_music };
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

    if (m_paused) {
        m_clock.restart();
    } else {
        m_pausedAt += m_clock.elapsed();
    }
    m_paused = !m_paused;
    rebuild();
    update();
}

void MediaPage::seek(int seconds)
{
    if (m_mode != ModeVideo && m_mode != ModeAudio)
        return;

    /* A pipe cannot seek, so seeking is opening the file again somewhere else --
     * which is exactly what the openers already do, given a start time. */
    const double now = (m_pausedAt + (m_paused ? 0 : m_clock.elapsed())) / 1000.0;
    double target = now + seconds;
    if (m_duration > 0.0)
        target = qBound(0.0, target, qMax(0.0, m_duration - 1.0));
    else
        target = qMax(0.0, target);

    const Entry entry = m_playing;
    const int mode = m_mode;

    if (mode == ModeVideo)
        openVideo(entry, target);
    else
        openAudio(entry, target);

    m_note = tr("seek to %1").arg(humanTime((int)target));
    update();
}

void MediaPage::tick()
{
    if (m_mode == ModeAudio || m_mode == ModeVideo)
        update();
}

/* ── input ───────────────────────────────────────────────────────────────── */

bool MediaPage::handleNav(int action)
{
    if (m_mode == ModeImage) {
        switch (action) {
        case Joypad::NavLeft:  stepImage(-1); return true;
        case Joypad::NavRight: stepImage(1); return true;
        case Joypad::NavBack:
        case Joypad::NavOk:
            m_frame = QImage();
            setMode(ModeBrowse);
            return true;
        default:
            return true;   /* Nothing else does anything while a picture is up. */
        }
    }

    if (m_mode == ModeVideo) {
        switch (action) {
        case Joypad::NavOk:    togglePause(); return true;
        case Joypad::NavLeft:  seek(-10); return true;
        case Joypad::NavRight: seek(10); return true;
        case Joypad::NavBack:
            stopPlayback();
            setMode(ModeBrowse);
            return true;
        default:
            return true;
        }
    }

    /* Browsing -- and possibly with music playing, which keeps its own keys on the
     * shoulders so up and down still walk the list. */
    switch (action) {
    case Joypad::NavUp:    m_list->step(-1); return true;
    case Joypad::NavDown:  m_list->step(1); return true;
    case Joypad::NavOk:    m_list->press(); return true;
    case Joypad::NavLeft:
        if (m_mode == ModeAudio) {
            seek(-10);
            return true;
        }
        return false;
    case Joypad::NavRight:
        if (m_mode == ModeAudio) {
            seek(10);
            return true;
        }
        return false;
    case Joypad::NavMenu:
        if (m_mode == ModeAudio) {
            togglePause();
            return true;
        }
        return false;
    case Joypad::NavBack: {
        /* Up a directory first; only leave the page from the top of the tree. */
        const QString parent = QFileInfo(m_dir).absolutePath();
        if (!m_dir.isEmpty() && parent != m_dir && QFileInfo(parent).isDir()
            && m_dir != QDir::rootPath()) {
            populate(parent);
            m_list->setCurrent(0);
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

/* ── painting ────────────────────────────────────────────────────────────── */

void MediaPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (m_mode == ModeImage || m_mode == ModeVideo) {
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
        } else if (m_mode == ModeVideo) {
            p.setPen(Theme::ink2());
            p.setFont(Theme::font(14));
            p.drawText(rect(), Qt::AlignCenter, tr("decoding..."));
        }

        /* A strip along the foot with the name, the clock and any complaint. */
        const int barH = 34;
        const QRect bar(0, height() - barH, width(), barH);
        QColor wash(8, 9, 14, 205);
        p.fillRect(bar, wash);

        p.setFont(Theme::font(12));
        p.setPen(Theme::ink());
        p.drawText(bar.adjusted(10, 0, -10, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   m_playing.name);

        QString right;
        if (m_mode == ModeVideo) {
            const int pos = (int)((m_pausedAt + (m_paused ? 0 : m_clock.elapsed())) / 1000);
            right = humanTime(pos);
            if (m_duration > 0.0)
                right += " / " + humanTime((int)m_duration);
            if (m_paused)
                right = tr("paused") + "  " + right;
            if (m_framesDropped > 0)
                right += "  " + tr("%1 dropped").arg(m_framesDropped);
        } else {
            int at = 0;
            int of = 0;
            for (int i = 0; i < m_entries.size(); ++i) {
                if (m_entries[i].kind != KindImage)
                    continue;
                ++of;
                if (m_entries[i].path == m_playing.path)
                    at = of;
            }
            right = QString("%1 / %2   %3x%4").arg(at).arg(of)
                        .arg(m_frame.width()).arg(m_frame.height());
        }
        p.setPen(Theme::ink3());
        p.drawText(bar.adjusted(10, 0, -10, 0), Qt::AlignRight | Qt::AlignVCenter, right);

        if (!m_note.isEmpty()) {
            const QRect noteRect(0, bar.top() - 22, width(), 22);
            p.fillRect(noteRect, QColor(8, 9, 14, 180));
            p.setFont(Theme::font(11));
            p.setPen(Theme::orange());
            p.drawText(noteRect.adjusted(10, 0, -10, 0),
                       Qt::AlignLeft | Qt::AlignVCenter, m_note);
        }
        return;
    }

    /* Browsing. */
    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);

    QString right = m_dir;
    const QString home = QDir::homePath();
    if (right.startsWith(home))
        right = "~" + right.mid(home.size());
    if (right.size() > 34)
        right = "..." + right.right(31);

    paintSheet(p, card, tr("Media"), right);

    if (m_mode == ModeAudio) {
        /* The now-playing line, drawn over the foot of the sheet so a record that
         * is playing is visible while the list is being walked. */
        const int barH = 26;
        const QRectF bar(card.x() + 6, card.bottom() - barH - 6, card.width() - 12, barH);
        p.setBrush(QColor(28, 30, 42, 235));
        p.setPen(QPen(Theme::separator(), 1.0));
        p.drawRoundedRect(bar, 8, 8);

        const int pos = (int)((m_pausedAt + (m_paused ? 0 : m_clock.elapsed())) / 1000);
        p.setFont(Theme::font(11));
        p.setPen(m_paused ? Theme::orange() : Theme::green());
        p.drawText(bar.adjusted(10, 0, -10, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   (m_paused ? tr("paused") : tr("playing")) + "  " + m_playing.name);

        QString clock = humanTime(pos);
        if (m_duration > 0.0)
            clock += " / " + humanTime((int)m_duration);
        p.setPen(Theme::ink3());
        p.drawText(bar.adjusted(10, 0, -10, 0), Qt::AlignRight | Qt::AlignVCenter, clock);
    }
}
