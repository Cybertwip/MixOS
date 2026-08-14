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
 * (needs a server).  This board has lima registered WITHOUT DRIVER_MODESET, so
 * there is no CRTC to give anyone, mtk_drm is a module that is not loaded, and
 * SDL2 as Debian builds it has no fbdev backend any more.  There is exactly one
 * thing on this device that can put a pixel on the panel, and it is the process
 * you are reading: the one holding /dev/fb0 through Qt's linuxfb plugin.
 *
 * SO THE PLAYER IS IN HERE.  ffmpeg decodes and scales; it writes raw BGRA frames
 * down a pipe; this page wraps each frame in a QImage and paints it.  ffmpeg is
 * doing the only part that is hard, this page is doing the only part that is
 * possible, and there is no video output driver involved anywhere.
 *
 *   ffmpeg -re -i FILE -map 0:v -f rawvideo -pix_fmt bgra -s WxH pipe:1 \
 *                      -map 0:a -f alsa default
 *
 * -re paces the decode at wall-clock speed, which is what makes this a player
 * rather than a transcoder racing to fill a pipe.  The audio leaves ffmpeg by the
 * ALSA muxer directly, so the two streams share one clock and one decode -- the
 * obvious alternative, two ffmpegs on the same file, drifts apart within a minute
 * and decodes everything twice on a CPU that cannot afford it once.  Where the
 * alsa muxer is missing from the build, audio falls back to a second chain into
 * aplay and the drift is accepted.
 *
 * MUSIC is the same trick with no video: ffmpeg to WAV on stdout, aplay reading
 * stdin.  PICTURES are QImage, which is the one media type Qt can do by itself.
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
    void readFrames();
    void onDecoderFinished();
    void tick();

private:
    enum Kind { KindDir = 0, KindAudio, KindVideo, KindImage, KindOther, KindUp };
    enum Mode { ModeBrowse = 0, ModeImage, ModeVideo, ModeAudio };

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
    void rebuild();
    void open(Entry entry);
    void openImage(const Entry &entry);
    /* startAt is why these take a time: a pipe cannot seek, so seeking is running
     * ffmpeg again from somewhere else, and that is the same code path as opening. */
    void openVideo(const Entry &entry, double startAt = 0.0);
    void openAudio(const Entry &entry, double startAt = 0.0);
    void stopPlayback();
    void stepImage(int delta);
    void togglePause();
    void seek(int seconds);
    /* One place that changes the mode, because two things have to happen with it:
     * the list has to be hidden behind a picture, and the shell has to be told the
     * title and the fullscreen state changed. */
    void setMode(int mode);

    /* Probed once: does this ffmpeg have the alsa output muxer compiled in.  If it
     * does, one process plays both streams; if not, audio needs its own chain. */
    bool ffmpegHasAlsa() const;
    bool hasSoundCard() const;
    QString ffmpegPath() const;
    double probeDuration(const QString &path) const;

    ListPane *m_list = nullptr;
    QString m_dir;
    QVector<Entry> m_entries;

    int m_mode = ModeBrowse;
    Entry m_playing;

    /* Video. */
    QProcess *m_decoder = nullptr;
    QProcess *m_audioSide = nullptr;   /* second ffmpeg, only without the alsa muxer */
    QProcess *m_aplay = nullptr;
    QByteArray m_buffer;
    QImage m_frame;
    int m_frameW = 0;
    int m_frameH = 0;
    int m_framesShown = 0;
    int m_framesDropped = 0;

    /* Audio. */
    QProcess *m_music = nullptr;

    QElapsedTimer m_clock;
    qint64 m_pausedAt = 0;
    bool m_paused = false;
    double m_duration = 0.0;
    QString m_note;

    QTimer *m_ui = nullptr;
};

#endif /* MIXDASH_MEDIA_H */
