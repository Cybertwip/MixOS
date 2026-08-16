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

class GlVideo;
class ListPane;
class QFileInfo;
class QMouseEvent;
class QPainter;
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
    void textEntered(const QString &text, bool accepted) override;
    bool wantsFullscreen() const override;

    /*
     * Open something chosen somewhere else -- a Files page selection, or a path on
     * the command line.  A directory is browsed to, a file is played.  Returns
     * false when the path is not something this page can open, so the caller can
     * say so rather than switching to a page that then does nothing.
     */
    bool openPath(const QString &path);

    /*
     * Play a sound file once, in a process nobody waits for and nobody reaps.
     * Static, because the startup chime happens before there is a Media page to
     * ask and would be absurd as a reason to build one; here rather than in
     * main.cpp because which ffmpeg and which ALSA device are questions this file
     * already answers -- and answers differently from `default', which is a trap
     * on this image.  Silent about every failure, deliberately: see media.cpp.
     */
    static void playOnce(const QString &path);

    /*
     * ── THE TWO THINGS THE SHELL HAS TO KNOW WHILE A FILM IS UP ──
     *
     * True while the GPU owns this page: a film is up, it came up on GL, and at
     * least one frame has landed.  In that state Qt must not paint -- its backing
     * store has never seen the picture, so anything it presents is a hole, and
     * anything it presents OVER the picture is a hole that lasts 40 ms until the
     * next frame paints the film back over it.
     *
     * That is why the volume bar is handed here as pixels instead of drawing
     * itself: `argb' is blended into the same GPU pass that puts the film up, at
     * `at' in framebuffer coordinates, and it stays there until it is replaced or
     * until a null image clears it.  A null image or an empty rectangle is the
     * clear.  Nothing happens if the GPU path is not up, which is the case where
     * the bar can simply paint itself and does.
     *
     * The mouse cursor arrives the same way and for a sharper version of the same
     * reason: it is the one widget that MOVES over the picture, so as a Qt child
     * it dirtied a rectangle of a backing store that holds no picture at all, and
     * linuxfb copied that grey rectangle onto the film once per motion event.
     * See the redirected-mode note in pointer.h.
     */
    bool glOwnsScreen() const;
    void setVolumeOverlay(const QImage &argb, const QRect &at);
    void setPointerOverlay(const QImage &argb, const QRect &at);
    /* And the loading ring, which is the shell's for the same reason the other two
     * are -- it goes over a film this page is drawing with the GPU, so it cannot be
     * a widget.  See busy.h. */
    void setBusyOverlay(const QImage &argb, const QRect &at);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    /*
     * The transport is painted, not built out of widgets, so the buttons have to
     * be hit-tested by hand -- see the note above transportRect() in media.cpp for
     * why there is no other way to have a control over a film.
     */
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private slots:
    void onActivated(int index);
    void onValueChanged(int index, int value);
    /* A volume was mounted or pulled.  Repopulates whatever is on screen, because
     * three separate things about it can have gone stale -- see media.cpp. */
    void onDisksChanged();
    void readFrames();
    /*
     * ── WHERE THE PICTURE IS KEPT LEVEL WITH THE SOUND ──
     *
     * The two chains are separate processes and always will be -- one muxer
     * blocked on a 64 KiB frame pipe is one muxer not feeding the DAC, which is
     * the whole reason they were split -- so there is no shared clock to hand.
     * What there is, is a clock they both already follow: ALSA plays 48000
     * samples every second of wall time, so the wall clock IS the audio clock to
     * within one period, and the picture only has to be held against it.
     *
     * That is what this does.  The decoder is asked for constant-rate frames, so
     * the Nth frame out of the pipe is due at startAt + N/fps and no timestamp has
     * to be carried down a raw pipe.  A frame that is not due yet waits; a frame
     * that is late is skipped so the next one can be on time.  ffmpeg's own `-re'
     * used to do the pacing, and that is exactly what could not recover: -re never
     * emits faster than real time, so a second lost to anything else was a second
     * behind the sound for the rest of the film.
     */
    void pump();
    /*
     * Put the newest frame back on the panel through the GPU.  Called from
     * readFrames() for every frame, and queued from paintEvent() whenever Qt has
     * repainted over the picture -- a toast, the console guard timer, anything
     * that asked the whole page to redraw.  Queued rather than immediate in that
     * case, because linuxfb copies its backing store into /dev/fb0 AFTER
     * paintEvent returns, so drawing during the paint would be overwritten by it.
     */
    void present();
    /*
     * The GPU path failed under a film that is already playing.  Both halves of
     * the chain have to come down and go up again, because ffmpeg is emitting
     * yuv420p that the software painter cannot use -- so this is a seek to where
     * the film had got to, with the GPU permanently off.
     */
    void restartWithoutGl();
    void onDecoderFinished();
    /* ffprobe answered, or died trying.  Either way whatever was waiting on it is
     * started here -- a probe that failed is not a reason not to play, it is a
     * reason to play without a length on the bar. */
    void onProbeFinished();
    void onMusicFinished(int code);
    /* Whatever the playing chain wrote to stderr, straight onto the glass.  It is
     * ffmpeg's when ffmpeg opens the card itself and aplay's when it does not, and
     * both want the same treatment -- hence one slot rather than two. */
    void onChildStderr();
    void tick();
    /* The two and a half seconds since the last thing anybody did are up: take the
     * transport off the film.  Never fires while a film is paused or stopped -- see
     * nudgeTransport(). */
    void hideTransport();
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
    /*
     * KindPlace is a ROOT -- the device itself or a mounted volume.  It is what the
     * places level is built out of (see populatePlaces() in media.cpp), and it is
     * ALSO what a mounted volume is listed as at the top of a root's own directory
     * listing, which is how a stick plugged in while this page is open shows up
     * without anybody going looking for it.  See populate().
     */
    enum Kind {
        KindDir = 0, KindAudio, KindVideo, KindImage, KindOther, KindUp, KindPlace
    };
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
        /*
         * The search box, and it is a ROW because everything on this page is.
         * Files has a real field because it has a pane system to put one in --
         * address, search, places, list, all reachable by walking off the edge of
         * the one you are on.  This page is a single ListPane, and bolting a
         * second focusable widget onto it would mean inventing that whole
         * mechanism again for one field.  A row is discoverable without it: it is
         * on the screen, it says what it is filtering by, and A opens the
         * keyboard on it like every other Action row in this shell.
         */
        RowSearch     = -11,
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
    /* The list of roots -- "Device" and every mounted volume -- which is what `..'
     * shows when you are standing on one.  Leaves m_dir alone: see media.cpp. */
    void populatePlaces();
    void open(Entry entry);
    /* Cursor onto the row showing this path; false if this listing has none. */
    bool selectPath(const QString &path);
    /*
     * Cursor onto the top of the LISTING, which is not row 0.  Row 0 is furniture
     * -- the now-playing row, the search box -- and a new listing has to start on
     * the first thing in it.  See media.cpp for why a filtered listing skips one
     * more row than an unfiltered one.
     */
    void selectTopEntry();
    void openImage(const Entry &entry);
    /*
     * startAt is why this takes a time: a pipe cannot seek, so seeking is running
     * ffmpeg again from somewhere else, and that is the same code path as opening.
     *
     * IT IS IN TWO HALVES BECAUSE THE FIRST ONE WAITS.  A film that has not been
     * probed yet cannot be opened -- the rate the picture has to be paced at and
     * whether there is any sound to pace it against both come out of ffprobe -- so
     * openVideo() puts the spinner up, asks, and returns.  openVideoNow() is what
     * onProbeFinished() calls, and it is also what a seek calls directly, because
     * a seek is the same container and the answer is already in hand.
     */
    void openVideo(const Entry &entry, double startAt = 0.0);
    void openVideoNow(const Entry &entry, double startAt);
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

    /*
     * One place builds the rows for whichever view is up.
     *
     * `keepSelection' IS ABOUT WHETHER IT IS THE SAME LIST.  Almost every caller is
     * refreshing rows that stand for the same things -- a track ticked on, a volume
     * slider moved, a disk arriving -- and there the cursor has to stay where the
     * operator put it.  populate() and populatePlaces() are not that: they have just
     * replaced the entries with a different directory's, and carrying row seven over
     * into it lands the cursor seven rows down a list it has never seen.  That is
     * what "Media does not start with the selection at the top" was.
     */
    void rebuild(bool keepSelection = true);
    void buildBrowseRows(QVector<ListRow> &rows) const;
    void buildPlayerRows(QVector<ListRow> &rows) const;

    /*
     * Whether this entry survives the filter.  Case-insensitive substring, which is
     * the same rule the Files page's glob comes to and the only one worth having on
     * a device where typing the term took a minute on an on-screen keyboard.
     *
     * WHAT IS NEVER FILTERED: `..' and the volume rows.  A filter that can hide the
     * way out of a directory is a filter that strands you in it, and the volumes are
     * the answer to "where else could this be" -- which is the question somebody who
     * has just searched and found nothing is asking.
     */
    bool matchesSearch(const Entry &e) const;

    /* ── the music queue ──────────────────────────────────────────────────── */

    /* Every playable audio file in `dir', in the order the list shows them, with
     * `startWith' selected.  Called once when a track is started by hand. */
    void queueDirectory(const QString &dir, const QString &startWith);
    void reshuffle();
    /* Index into m_queue of the position `m_orderAt' names, or -1. */
    int queuedIndex() const;
    const Entry *queuedTrack() const;
    /* Start what m_orderAt points at.  The only place that spawns the music
     * chain, so the only place that has to get the pipeline right.  Split for the
     * same reason openVideo() is: the tags and the length come from a probe that
     * is no longer allowed to stop the program while it runs. */
    void playQueued(double startAt = 0.0);
    void playQueuedNow(double startAt);
    /* +1 and -1, honouring repeat and shuffle.  `automatic' is true when a track
     * ended by itself, which is the only case RepeatOne acts on -- pressing Next
     * on a repeat-one track has to move, or the button is a lie. */
    void advance(int delta, bool automatic);
    void stopMusic();
    /* Video and pictures only.  Music has its own, because the two have opposite
     * lifetimes: see the header comment. */
    void stopVideo();
    /*
     * Let go of the picture -- the QImage, the planes the GPU was drawing from,
     * and the strip texture.  NOT part of stopVideo(), and that is the point: a
     * seek is stopVideo() followed by openVideo(), and the frame that is already
     * on the glass should stay there while the new ffmpeg loads libavcodec.  This
     * is for leaving the film behind, not for restarting it.
     */
    void dropFrame();
    /*
     * The strip along the foot: the name on the left, the clock and the dropped
     * count on the right, and the note above it when there is one.  Painted in
     * widget coordinates so the software path can call it straight from
     * paintEvent, and rendered into an image for the GPU path.
     */
    void paintChrome(QPainter &p) const;
    QString chromeRight() const;
    QRect chromeRect() const;

    /* ── the transport, for a film ────────────────────────────────────────── */
    /*
     * A film gets the panel below instead of the one-line strip above.  The ids
     * are what the hit test returns and what pressControl() acts on, and they are
     * NOT indices into anything -- the layout is rebuilt from the widget's width
     * every time it is asked for, so nothing may cache a position.
     */
    enum Control {
        CtlNone = 0,
        CtlBar,        /* the timebar, which is a control and not a readout */
        CtlBack,       /* -10 s */
        CtlPlay,
        CtlForward,    /* +10 s */
        CtlLoop,
        CtlZoom,       /* letterboxed <-> filling the panel */
        CtlClose
    };
    struct Button {
        int id = CtlNone;
        QRect where;
    };

    /* The whole panel, the groove the knob runs in, and the row of buttons.  All
     * three in widget coordinates, all three derived from nothing but the widget's
     * size, so the software path and the GPU path cannot disagree about where a
     * button is. */
    QRect transportRect() const;
    QRect barRect() const;
    QVector<Button> buttons() const;
    void paintTransport(QPainter &p) const;
    /* One button's glyph, centred in `box'.  Static because it holds nothing and
     * a member only because the ids it switches on are private to this class. */
    static void drawGlyph(QPainter &p, int id, const QRect &box, bool paused, bool on);
    /* Which control is under a widget-coordinate point, CtlNone for none. */
    int controlAt(const QPoint &at) const;
    void pressControl(int id);
    /* Put the transport up and start the two and a half seconds again.  Called
     * from every press, every mouse move and every change of state, which is the
     * whole of the show-and-hide policy. */
    void nudgeTransport();
    /* True while the transport should be on the glass: it was nudged recently, or
     * there is a reason it must not go away at all. */
    bool transportUp() const;
    /* Re-render the strip and hand it to the GPU, but only when its text has
     * changed since last time.  `into' is the page's rectangle on the
     * framebuffer, because the strip's position is given in those coordinates. */
    void refreshChrome(const QRect &into);
    /*
     * update() for a page that may not be Qt's to update.  Every "something
     * changed, show it" in this file goes through here, because the same line has
     * to mean "repaint the widget" on the software path and "re-present the frame
     * with a fresh strip" on the GPU one.  Calling update() in the second case
     * memcpy's a stale backing store over a film.
     */
    void refresh();
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

    /* One frame out of the pipe.  yuv420p is twelve bits a pixel and bgra is
     * thirty-two: that ratio is most of what the GPU path buys before a single
     * instruction runs on the GPU, and it is the unit everything about the pipe is
     * measured in -- the read buffer, the frame number, the drop. */
    int frameBytes() const
    {
        return m_gl ? (m_frameW * m_frameH * 3) / 2 : m_frameW * m_frameH * 4;
    }
    /* Everything the decoder has, into m_buffer, with a backstop that throws away
     * whatever is more than a second stale.  See the note in media.cpp: the pipe
     * must never be what stops ffmpeg. */
    void fill();

    /* ── ffmpeg, aplay and the card ───────────────────────────────────────── */

    /* `plughw:C,D' for the lowest-numbered playback PCM, or empty when there is
     * no card.  See the header comment for why this is not `default'. */
    static QString alsaDevice();
    /* Probed once: does this ffmpeg have the alsa output muxer compiled in.  If it
     * does, one process plays both streams; if not, audio needs its own chain. */
    bool ffmpegHasAlsa() const;
    static QString ffmpegPath();
    static QString aplayPath();
    QString ffprobePath() const;

    /*
     * Ask ffprobe about `path' and come back through onProbeFinished() with
     * `purpose' -- WaitVideo or WaitMusic -- to be resumed.  Returns immediately.
     * A path that has already been probed does not go out again; the caller is
     * resumed on the next trip round the event loop instead, so that "cached" and
     * "not cached" are the same code path at the call site and neither of them
     * re-enters the caller underneath itself.
     */
    void probeThen(const QString &path, int purpose, const Entry &entry, double startAt);
    /* Whatever this page is willing to show as the name of what is playing: the
     * container's title tag, else the artist and title, else the file name without
     * its extension.  Never empty for a file that exists, which is the point --
     * "decoding..." and a blank label were the two things it used to be. */
    QString displayTitle(const Entry &entry, const QString &tagged) const;
    /* The mixer, asked once per track: a muted card is the other way to have no
     * sound, and it is not the player's to fix silently. */
    QString mixerComplaint() const;
    /* Re-derive m_mixerNote and re-stamp m_volGeneration.  True when the text
     * changed and something on screen has to be redrawn. */
    bool refreshMixerNote();
    /* What the note strip actually shows: see m_mixerNote. */
    QString noteText() const { return m_note.isEmpty() ? m_mixerNote : m_note; }
    /*
     * ffmpeg's decode arguments for one audio stream, shared by the music chain and
     * by the video page's fallback chain.
     *
     * alsaSink names the card to write into -- `plughw:C,D' -- and makes this one
     * process that plays the track by itself.  Empty asks for raw s16le on stdout
     * instead, which is the aplay chain, and that form only survives for an ffmpeg
     * built without the alsa outdev.
     */
    QStringList audioDecodeArgs(const QString &path, double startAt,
                                const QString &alsaSink) const;

    ListPane *m_list = nullptr;
    QString m_dir;
    /* The places level is up, and m_entries is roots rather than files.  m_dir
     * still names the last real directory while this is true -- it is where `..'
     * came from and where the page goes back to -- so nothing that reads m_dir has
     * to know this level exists. */
    bool m_places = false;
    QVector<Entry> m_entries;

    /*
     * The filter over the listing, and it filters ROWS rather than m_entries.  Every
     * browse row carries its index into m_entries as its id, which is what makes
     * onActivated() a lookup instead of a search, so the entries have to keep their
     * positions -- a filtered m_entries would renumber them all and the first press
     * after a search would open the wrong file.
     *
     * Cleared whenever the listing changes directory, because a filter is a property
     * of the folder you typed it in.  It survives a refresh of the same folder --
     * a track ticking on, a disk arriving -- for the same reason the cursor does.
     */
    QString m_search;
    bool m_awaitingSearch = false;

    int m_view = ViewBrowse;
    /* Where the browser's selection was when the player view took the pane. */
    int m_browseAt = 0;

    /* ── video and pictures ───────────────────────────────────────────────── */
    Entry m_showing;                   /* the picture or the film */
    QProcess *m_decoder = nullptr;
    /* The film's sound, always in its own process: a decoder blocked on the frame
     * pipe must not be what stops the card being fed.  See openVideo(). */
    QProcess *m_videoAudio = nullptr;
    QProcess *m_videoAplay = nullptr;  /* only on an ffmpeg without the alsa outdev */
    QByteArray m_buffer;
    QImage m_frame;
    int m_frameW = 0;
    int m_frameH = 0;
    int m_framesShown = 0;
    int m_framesDropped = 0;

    /* ── the film on the GPU ──────────────────────────────────────────────── */
    /*
     * Non-null only while a film is being presented through GlVideo, which is a
     * decision taken once per film in openVideo() and never changed under it:
     * the two paths want different pixel formats out of ffmpeg, so switching
     * halfway means restarting the decoder.  Null is the software path, and
     * everything below is inert.
     */
    GlVideo *m_gl = nullptr;
    /* The newest whole frame, still yuv420p, kept rather than dropped after it
     * is drawn.  A paused film and a repaint forced from outside both need the
     * picture put back, and there is nothing else to put back from -- the GPU
     * drew into scanout memory that Qt is about to overwrite. */
    QByteArray m_planes;
    /* A frame has actually landed on the panel through the GPU.  Until it has,
     * paintEvent still paints the background and "decoding...", because nothing
     * else would. */
    bool m_glShown = false;
    /* The GPU path was tried and gave up mid-film.  Sticky for the life of the
     * page, so a driver that has stopped answering is not asked again twice a
     * second for the rest of the session. */
    bool m_glOff = false;
    /* The strip's text as it was last rendered.  QPainter runs over the strip
     * only when this changes, which is once a second for the clock rather than
     * twenty-five times a second for the frames. */
    QString m_chromeKey;
    /* Where restartWithoutGl() should pick the film up. */
    double m_glRestartAt = 0.0;
    double m_videoDuration = 0.0;
    /* Probed with the duration and kept for the same lifetime, so a seek does not
     * pay for it again.  Nothing false about a stale value: a new film opens at 0
     * and re-probes both. */
    bool m_videoHasAudio = false;
    /*
     * The rate the decoder is being asked to emit at, which is what turns "the Nth
     * frame out of the pipe" into "the moment that frame is due".  Zero until the
     * probe answers; see pump().
     */
    double m_videoFps = 0.0;
    /* Frames taken out of the pipe since this decoder started, shown and dropped
     * alike -- the frame NUMBER, which is the only timestamp a raw pipe carries. */
    int m_framesDecoded = 0;
    /* The `-ss' this decoder run was started with, so frame numbers can be turned
     * into positions in the film rather than in the run. */
    double m_videoStart = 0.0;
    /* Fires when the frame already sitting in the buffer becomes due.  Single
     * shot and re-armed by pump(): a frame arriving early has to be held, and
     * readyRead will not fire again to remind us. */
    QTimer *m_pace = nullptr;

    /* ── one ffprobe, asynchronous, cached per path ───────────────────────── */
    /*
     * THIS USED TO BE THREE BLOCKING FORKS AND IT WAS THE FREEZE.
     *
     * probeDuration(), probeHasAudio() and probeTitle() each started an ffprobe and
     * sat in Shell::waitForFinished until it answered -- up to four seconds apiece,
     * with the event loop stopped, before a single frame was asked for.  On a cold
     * page cache that is most of a minute for a folder of music, and there is no
     * spinner that can turn while nothing in the program is running.
     *
     * So it is one ffprobe now: streams and format tags in a single invocation,
     * started and forgotten about, and whatever wanted to play carries on from
     * onProbeFinished().  The answer is kept against the path it describes, which
     * is what stops a ten-second nudge of the D-pad -- openVideo() again, with a
     * start time -- from asking about the same container over and over.
     */
    QProcess *m_probe = nullptr;
    QString m_probedPath;
    double m_probeDuration = 0.0;
    double m_probeFps = 0.0;
    bool m_probeAudio = false;
    QString m_probeTitle;

    /* What the answer is for.  A probe with nothing waiting on it is a probe whose
     * film was abandoned while it ran, and its result is filed and dropped. */
    enum { WaitNothing = 0, WaitVideo, WaitMusic };
    int m_waiting = WaitNothing;
    Entry m_waitEntry;
    double m_waitStart = 0.0;

    /* Something is being opened and there is nothing to show yet.  What the spinner
     * is drawn from, and what keeps "end of file" from being painted over a film
     * that has not started. */
    bool m_loading = false;
    /* busyRequested(), but only when the answer has actually changed.  m_loading is
     * written from half a dozen places and the shell's spinner has a timer behind
     * it; saying "still loading" thirty times would restart nothing but would emit
     * thirty signals for no reason. */
    void setLoading(bool on, const QString &what = QString());
    QString m_loadingWhat;

    /* ── transport state ──────────────────────────────────────────────────── */
    /* Started by nudgeTransport(), and its expiry is the ONLY thing that takes the
     * panel off the film -- there is no other timer and no other rule. */
    QTimer *m_transportTimer = nullptr;
    bool m_transportShown = true;
    /* What the pointer is over, so a button can light under it.  CtlNone the whole
     * time there is no mouse on the board, which is the usual case. */
    int m_hover = CtlNone;
    /* The knob is being dragged.  While this is true the bar shows m_seekTarget
     * and the film is left alone; the seek commits on release, through the same
     * debounce the D-pad uses. */
    bool m_scrubbing = false;
    /* Play it again at the end.  Video only: music has m_repeat, which is a
     * three-way over a queue and means something different. */
    bool m_loopVideo = false;
    /* Fill the panel, cropping what does not fit, instead of letterboxing.  It is
     * ffmpeg that scales, so this is a decoder argument and changing it restarts
     * the film where it stood -- exactly what a seek already does. */
    bool m_fill = false;

    /* ── music ────────────────────────────────────────────────────────────── */
    QVector<Entry> m_queue;
    /* A permutation of [0, m_queue.size()).  The identity while shuffle is off,
     * so one walk serves both and there is no second code path to get wrong. */
    QVector<int> m_order;
    int m_orderAt = -1;
    QProcess *m_music = nullptr;
    /* Null whenever ffmpeg writes the card itself, which is the normal case.  Every
     * place that touches it has to cope with that -- including onMusicFinished(),
     * where a null m_aplay is what makes ffmpeg's own exit the end of the track. */
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

    /*
     * The mute/zero-volume complaint, KEPT APART FROM m_note and for one reason:
     * m_note is where every transient thing the player has to say ends up -- "end
     * of file", "ffmpeg exited 1", whatever a child wrote to stderr -- and the
     * mixer complaint has a completely different lifetime.  It is true for
     * exactly as long as the mixer says so, across starts, stops and end of file,
     * and it stops being true the instant somebody presses VOL+.
     *
     * Sharing one member meant whichever was written last won, in both
     * directions: a re-derived mixer note erasing the reason a child died, and a
     * child's message leaving a stale "press VOL+" that nothing would ever clear.
     * noteText() puts them back in order -- m_note first, because a chain that is
     * broken outranks a mixer that is merely down.
     */
    QString m_mixerNote;
    /* Volume::generation() as of the last time m_mixerNote was derived. */
    unsigned m_volGeneration = 0;

    /*
     * The last thing a child of the music chain wrote to stderr, kept apart from
     * m_note.
     *
     * readyReadStandardError DRAINS the buffer, so by the time finished() arrives
     * the reason the child died is already gone -- and the failure handler would
     * then paint a bare "aplay exited 1" over the sentence that said why.  This is
     * where onChildStderr puts a copy so the handler has something to fall back on.
     * Cleared at the top of every playQueued(), because a complaint from the
     * previous track is not evidence about this one.
     */
    QString m_childSaid;

    QTimer *m_ui = nullptr;

    /* Where the slider says we are, waiting for commitSeek() to make it true.
     * Negative when there is no seek outstanding. */
    QTimer *m_seekTimer = nullptr;
    double m_seekTarget = -1.0;
};

#endif /* MIXDASH_MEDIA_H */
