/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * media.h -- music, video and pictures, on a board with no video output layer.
 *
 * WHAT WENT WRONG WITH THE OLD CARD.  The "Video" card launched mpv, then cvlc,
 * then vlc, then ffplay, and every one of them exited 1 -- which is what the
 * screenshot showed.  None of them failed for a fixable reason: mpv's remaining
 * video outputs are gpu (needs EGL), drm (needs a KMS card node), and x11/wayland
 * (needs a server).  SDL2 as Debian builds it has no fbdev backend any more.
 * There is exactly one thing on this device that can put a pixel on the panel,
 * and it is the process you are reading: the one holding /dev/fb0 through Qt's
 * linuxfb plugin.
 *
 * SO THE PLAYER IS IN HERE.  ffmpeg decodes and scales; it writes raw BGRA frames
 * down a pipe; this page wraps each frame in a QImage and paints it.  ffmpeg is
 * doing the only part that is hard, this page is doing the only part that is
 * possible, and there is no video output driver involved anywhere.
 *
 *   ffmpeg -re -i FILE -map 0:v -vf scale,pad,format=bgra -f rawvideo pipe:1 \
 *                      -map 0:a:0? -f alsa plughw:0,0
 *
 * -re paces the decode at wall-clock speed, which is what makes this a player
 * rather than a transcoder racing to fill a pipe.  MUSIC is the same trick with
 * no video: ffmpeg to raw s16le on stdout, aplay reading stdin.  PICTURES are
 * QImage, which is the one media type Qt can do by itself.
 *
 * ── WHY THERE WAS NO SOUND, AND WHY THE DEVICE IS NAMED ─────────────────────
 *
 * Both chains used to name `default', and on this image `default' is a trap.
 * finishing_touches.sh links /etc/asound.conf to /home/virtua/.asoundrc, and that
 * file is the RG351MP's:
 *
 *   pcm.!default { type plug  slave.pcm "dmixer" }
 *   pcm.dmixer   { type dmix  ipc_key 1024
 *                  slave { pcm "hw:0,0" period_size 1024 buffer_size 4096
 *                          rate 44100 } }
 *
 * So every stream on this board went through a shared-memory software mixer with
 * an RK3326-era buffer geometry hard-coded into it, on top of an AFE that has no
 * playback interrupt at all -- j36_mt6592_audio polls the DL1 cursor from a work
 * item and calls snd_pcm_period_elapsed() from there.  dmix exists to let several
 * processes share one card; this handheld has exactly one audio consumer, which
 * is this page, so the layer bought nothing and stood between the player and the
 * only DAC on the machine.
 *
 * alsaDevice() therefore names `plughw:C,D' -- the LOWEST-numbered playback PCM
 * in /dev/snd, found by reading the directory rather than by assuming card 0.
 * plughw is the plug converter over the raw hw device: it does rate, format and
 * channel conversion exactly as `default' would, and it is resolved entirely
 * inside alsa-lib's built-in definitions, so nothing in /etc/asound.conf can
 * redirect it.  Naming the device also means the note under the track can say
 * WHICH device, which is the difference between "no sound" and a fault report.
 *
 * The stream is 48 kHz and not 44.1: 48 kHz is the family the MT6592's audio PLL
 * runs natively, ffmpeg has to resample most of the world either way, and the
 * fewer dividers involved the fewer things can be subtly wrong.
 *
 * AND EVERY CHILD'S STDERR IS READ.  aplay's was not, which is why a card that
 * would not open produced silence and no message anywhere: aplay died, ffmpeg
 * took EPIPE, the finished handler wrote the reason into a member -- and nothing
 * painted that member in the browse view.  Now m_note is painted in all four
 * views and it carries whatever the children said.
 *
 * ── WHAT THE PAGE IS, AS OPPOSED TO WHAT IT WAS ─────────────────────────────
 *
 * The old page had four modes and playback WAS one of them: starting a track put
 * the page in ModeAudio and left it there for ever, because nothing cleared it.
 * The consequences were all one bug: the track kept its "playing" badge after it
 * had ended, the clock kept counting, Left/Right and Menu stayed stolen for a
 * transport that no longer had a process behind it, and there was no stop -- the
 * only ways out were to start something else or to leave the page.
 *
 * So playback is no longer a mode.  It is a QUEUE plus an index, and it runs
 * underneath whichever view is on screen:
 *
 *   ViewBrowse  the directory, with a Now-playing row pinned at the top while
 *               something is playing.  Navigation always works here.
 *   ViewPlayer  the transport, as list rows -- seek bar, pause, next, previous,
 *               stop, repeat, shuffle, show-in-folder.  Rows and not chords,
 *               because a handheld with eleven buttons cannot afford a player
 *               whose controls have to be memorised, and every other page in
 *               this dashboard is already a ListPane.
 *   ViewImage   one picture, full screen, Left/Right walks the directory.
 *   ViewVideo   one film, full screen, with its own transport on the pad.
 *
 * THE QUEUE is the playable audio in the directory the track was started from,
 * in list order, with a permutation over it for shuffle.  A track that ends
 * advances it; repeat says what happens at the end -- stop, wrap, or the same
 * track again.  Both settings persist, because a handheld that forgets it was
 * shuffling is a handheld you set up again every boot.
 *
 * MUSIC SURVIVES LEAVING THE PAGE and pictures do not.  That asymmetry is
 * deliberate and it is the whole reason the player does not take the screen: a
 * device that stops the album the moment you go and look at something else is a
 * device nobody listens to music on.  Video stops, because a film you cannot see
 * is just a fan spinning.
 *
 * WHAT THIS IS NOT.  It will not play 1080p h264 -- a Cortex-A7 at this clock
 * decodes maybe 480p, and the scale to 640x480 is on the CPU too.  The frame drop
 * is honest rather than hidden: whole frames are skipped to stay on the audio
 * clock, and the foot of the screen says how many.
 */
#ifndef MIXDASH_MEDIA_H
#define MIXDASH_MEDIA_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include "widgets.h"

class ListPane;
class QFileInfo;
class QProcess;
class QTimer;

class MediaPage : public PageWidget
{
    Q_OBJECT

public:
    explicit MediaPage(QWidget *parent = nullptr);
    ~MediaPage() override;

    QString title() const override;
    bool handleNav(int action) override;
    void onEnter() override;
    void onLeave() override;
    bool wantsFullscreen() const override;

    /*
     * Open something chosen somewhere else -- a Files page selection, or a path on
     * the command line.  A directory is browsed to, a file is played.  Returns
     * false when the path is not something this page can open, so the caller can
     * say so rather than switching to a page that then does nothing.
     */
    bool openPath(const QString &path);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onActivated(int index);
    void onValueChanged(int index, int value);
    void readFrames();
    void onDecoderFinished();
    void onMusicFinished(int code);
    void onAplayStderr();
    void tick();
    /*
     * The seek the slider has been showing for the last third of a second.
     *
     * Seeking on a pipe means killing ffmpeg and starting it again with -ss, and
     * the D-pad repeats.  Committing on every repeat would spawn one ffmpeg per
     * press on a CPU that takes a visible fraction of a second to load libavcodec,
     * and every one of them but the last is thrown away before it decodes a frame.
     * So the slider and the clock move immediately and the process restarts once,
     * when the pressing stops.
     */
    void commitSeek();

private:
    enum Kind { KindDir = 0, KindAudio, KindVideo, KindImage, KindOther, KindUp };
    enum View { ViewBrowse = 0, ViewImage, ViewVideo, ViewPlayer };
    enum RepeatMode { RepeatOff = 0, RepeatAll, RepeatOne };

    /*
     * ListRow::id, shared by both views because both views drive one ListPane.
     * Non-negative is an index into m_entries, so the browser needs no mapping
     * table; everything the player owns is negative and named.
     */
    enum RowId {
        RowNowPlaying = -1,
        RowSeek       = -2,
        RowPause      = -3,
        RowNext       = -4,
        RowPrev       = -5,
        RowStop       = -6,
        RowRepeat     = -7,
        RowShuffle    = -8,
        RowReveal     = -9,
        /* Headers and the read-only output line.  They are unselectable, so they
         * can never reach onActivated() -- but their id defaults to 0, which reads
         * as "entry 0" there, and one future edit that made such a row selectable
         * would silently open the first file in the directory.  Named, so it
         * cannot. */
        RowInert      = -10
    };

    struct Entry {
        QString path;
        QString name;
        int kind = KindOther;
        qint64 size = 0;
    };

    static int kindFor(const QFileInfo &info);
    static QString humanSize(qint64 bytes);
    static QString humanTime(int seconds);

    /*
     * BY VALUE, both of them, and that is not an oversight.  What the callers pass
     * is an element of m_entries -- open(m_entries[i]) is how every activation
     * works -- and populate() CLEARS m_entries in its second line, while still
     * having its own parameter to read in its fourth.  Through a const reference
     * that parameter is a QString inside a destroyed Entry inside a released
     * QVector buffer, which is the same use-after-free that the QStringList
     * temporary in dashboard.cpp turned into a bad_alloc from inside libQt5Core.
     * The copy is two implicitly-shared QStrings and two ints, once per button
     * press.
     */
    void populate(QString dir);
    void open(Entry entry);
    void openImage(const Entry &entry);
    /* startAt is why this takes a time: a pipe cannot seek, so seeking is running
     * ffmpeg again from somewhere else, and that is the same code path as opening. */
    void openVideo(const Entry &entry, double startAt = 0.0);
    void stepImage(int delta);
    void setView(int view);
    /*
     * The list pane's rectangle, which is not a constant: the note strip at the
     * foot of the sheet is only there when there is a note, and a page that
     * reserved room for it permanently would give up half a row to a message that
     * is almost never showing.  Called from resizeEvent and from anywhere m_note
     * changes.
     */
    void layOutList();

    /* One place builds the rows for whichever view is up. */
    void rebuild();
    void buildBrowseRows(QVector<ListRow> &rows) const;
    void buildPlayerRows(QVector<ListRow> &rows) const;

    /* ── the music queue ──────────────────────────────────────────────────── */

    /* Every playable audio file in `dir', in the order the list shows them, with
     * `startWith' selected.  Called once when a track is started by hand. */
    void queueDirectory(const QString &dir, const QString &startWith);
    void reshuffle();
    /* Index into m_queue of the position `m_orderAt' names, or -1. */
    int queuedIndex() const;
    const Entry *queuedTrack() const;
    /* Start what m_orderAt points at.  The only place that spawns the music
     * chain, so the only place that has to get the pipeline right. */
    void playQueued(double startAt = 0.0);
    /* +1 and -1, honouring repeat and shuffle.  `automatic' is true when a track
     * ended by itself, which is the only case RepeatOne acts on -- pressing Next
     * on a repeat-one track has to move, or the button is a lie. */
    void advance(int delta, bool automatic);
    void stopMusic();
    /* Video and pictures only.  Music has its own, because the two have opposite
     * lifetimes: see the header comment. */
    void stopVideo();
    void togglePause();
    void seekTo(double seconds);
    void seekBy(int seconds);
    double position() const;
    /*
     * One predicate each for "there is a chain running", asked from a dozen
     * places between the row builders, the painter and handleNav.  Both process
     * pointers of a pair are created and cleared together, so one of them answers
     * for both -- and a predicate rather than a bool member means there is no
     * second piece of state that can disagree with the processes themselves.
     */
    bool musicLive() const { return m_music != nullptr; }
    bool videoLive() const { return m_decoder != nullptr; }

    /* ── ffmpeg, aplay and the card ───────────────────────────────────────── */

    /* `plughw:C,D' for the lowest-numbered playback PCM, or empty when there is
     * no card.  See the header comment for why this is not `default'. */
    QString alsaDevice() const;
    /* Probed once: does this ffmpeg have the alsa output muxer compiled in.  If it
     * does, one process plays both streams; if not, audio needs its own chain. */
    bool ffmpegHasAlsa() const;
    QString ffmpegPath() const;
    QString aplayPath() const;
    double probeDuration(const QString &path) const;
    /* Title and artist out of the container's tags, for the one track that is
     * playing.  Empty when there are no tags or no ffprobe. */
    QString probeTitle(const QString &path) const;
    /* The mixer, asked once per track: a muted card is the other way to have no
     * sound, and it is not the player's to fix silently. */
    QString mixerComplaint() const;
    /* ffmpeg's decode arguments for one audio stream, shared by the music chain
     * and by the video page's fallback chain. */
    QStringList audioDecodeArgs(const QString &path, double startAt) const;

    ListPane *m_list = nullptr;
    QString m_dir;
    QVector<Entry> m_entries;

    int m_view = ViewBrowse;
    /* Where the browser's selection was when the player view took the pane. */
    int m_browseAt = 0;

    /* ── video and pictures ───────────────────────────────────────────────── */
    Entry m_showing;                   /* the picture or the film */
    QProcess *m_decoder = nullptr;
    QProcess *m_videoAudio = nullptr;  /* second ffmpeg, only without the alsa muxer */
    QProcess *m_videoAplay = nullptr;
    QByteArray m_buffer;
    QImage m_frame;
    int m_frameW = 0;
    int m_frameH = 0;
    int m_framesShown = 0;
    int m_framesDropped = 0;
    double m_videoDuration = 0.0;

    /* ── music ────────────────────────────────────────────────────────────── */
    QVector<Entry> m_queue;
    /* A permutation of [0, m_queue.size()).  The identity while shuffle is off,
     * so one walk serves both and there is no second code path to get wrong. */
    QVector<int> m_order;
    int m_orderAt = -1;
    QProcess *m_music = nullptr;
    QProcess *m_aplay = nullptr;
    QString m_trackTitle;              /* from the tags, or empty */
    QString m_device;                  /* what alsaDevice() said when it started */
    double m_duration = 0.0;
    int m_repeat = RepeatOff;
    bool m_shuffle = false;
    /* Set across a deliberate kill so the finished handler does not read it as a
     * track that ended and advance the queue into the next one. */
    bool m_stopping = false;

    /* Shared by both transports: elapsed is the wall clock since the last start
     * or resume, plus whatever was banked before the last pause or seek. */
    QElapsedTimer m_clock;
    qint64 m_pausedAt = 0;
    bool m_paused = false;

    QString m_note;
    QTimer *m_ui = nullptr;

    /* Where the slider says we are, waiting for commitSeek() to make it true.
     * Negative when there is no seek outstanding. */
    QTimer *m_seekTimer = nullptr;
    double m_seekTarget = -1.0;
};

#endif /* MIXDASH_MEDIA_H */
