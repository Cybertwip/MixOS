/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * volume.h -- the ALSA playback control, and the bar that appears when it moves.
 *
 * WHY THIS IS ITS OWN FILE.  The mixer plumbing lived in settingspage.cpp, as
 * four private methods and five members of SettingsPage, and that was right for
 * as long as the Settings slider was the only thing that ever changed the volume.
 * The two hardware volume keys are not on a page: they work from the Media
 * player, from a full-screen Doom, from the Apps grid.  Two copies of "which
 * control is the playback control" would be two probes of amixer, two caches to
 * disagree with each other, and a Settings slider that shows the level from
 * before the last VOL+.  So there is one, here, and SettingsPage uses it too.
 *
 * THE PROBE IS PROCESS-WIDE AND IT IS CACHED, because it costs a fork of amixer
 * and the answer cannot change while the dashboard runs -- the card is whatever
 * the codec driver registered at boot.  Volume::invalidate() exists for the one
 * case that is not true, which is a card appearing after mixdash started.
 */
#ifndef MIXDASH_VOLUME_H
#define MIXDASH_VOLUME_H

#include <QString>
#include <QWidget>

class QTimer;

namespace Volume {

/*
 * The simple control this board's playback goes through, cached after the first
 * call.  Empty means either no amixer or no card with a playback control on it,
 * and haveAmixer() is what tells those two apart -- Settings says which.
 */
QString control();
bool haveAmixer();
/* Forget the probe.  For a card that arrived after we looked. */
void invalidate();

/*
 * Level and mute in ONE fork of amixer, which is the whole reason this is not two
 * calls.  Per cent is ALSA's MAPPED scale, or -1 when there is no control: mapped
 * and not raw, because a raw ALSA percentage is a dB register value wearing a
 * percent sign and at "50%" a raw control is usually already almost silent.
 *
 * Returns false when there is nothing to read, and leaves both outputs at -1 and
 * false.  Either pointer may be null.
 *
 * This ALWAYS forks -- it is the "what is actually true" call, and something
 * outside this program can change the mixer: a script, an ssh session, the
 * Speaker Amp switch.  See nudge() for the one path that is allowed to trust a
 * recent answer instead.
 */
bool read(int *percent, bool *muted);

/* Both clamp, both are no-ops when there is no control, and setPercent returns
 * what the level ended up as so the caller does not have to read it back. */
int setPercent(int value);
void setMuted(bool value);

/*
 * VOL+ and VOL- in one call: step by delta, unmute if the level is being raised,
 * and hand back the level to show.  Returns -1 when there is nothing to drive,
 * which is what the overlay paints as "no sound card".  mutedOut may be null.
 *
 * THE STEP IS 5 AND NOT 1.  The keys autorepeat at Joypad's rate -- 380 ms to the
 * first repeat and 90 ms between the rest -- and a 1 per cent step would mean a
 * fork of amixer every 90 ms to move the level by an amount nobody can hear.
 * Five gets from silent to full in twenty presses, which is about how far a
 * volume rocker travels on any other handheld.
 *
 * AND IT REMEMBERS THE LEVEL FOR A SECOND AND A HALF, which is the other half of
 * making a held key affordable.  Reading before every write would be TWO forks
 * per 90 ms tick on a Cortex-A7 -- more wall clock than the interval itself, so
 * the event loop falls behind and the bar stutters while the key is down.  Inside
 * that window the level this function last set is taken as still true, because
 * for that window it is: the only thing that has touched the mixer is us, 90 ms
 * ago.  Outside it, it forks and asks.
 */
enum { Step = 5 };
int nudge(int delta, bool *mutedOut = nullptr);

/*
 * WHERE THE SOUND COMES OUT.
 *
 * The J36's card carries two analog outputs and a switch for each: "Speaker Amp"
 * for the class-D behind the built-in speaker, "Headphone" for the buffers
 * behind the jack.  They are not exclusive -- both on is a real setting and it
 * plays out of both.
 *
 * THIS IS A SETTING AND NOT A STATUS, and that is the part worth stating out
 * loud.  The board brings no jack-detect line out to anything the kernel can
 * read, so nothing anywhere in this system notices a plug going in.  A page that
 * drew these as "Headphones (connected)" would be inventing the connected part.
 * They are two switches, they say what they are, and the person holding the
 * device is the detect line.
 *
 * present() is false for a card that has neither, which is every card but this
 * one -- a USB headset or an HDMI adapter has no such controls and the rows are
 * simply not drawn.  The level is unaffected either way: "Master" moves both
 * outputs together, in the driver, so there is no per-output volume to show.
 */
enum Output { Speaker, Headphones };
bool present(Output which);
bool isOn(Output which);
void setOn(Output which, bool on);

} /* namespace Volume */

/*
 * The bar itself: a vertical track at the right-hand edge, on screen for three
 * seconds after the last press and then gone.
 *
 * A CHILD OF THE DASHBOARD AND NOT A WINDOW.  There is one surface on this board
 * -- the framebuffer -- and a second top-level would be a second full-screen
 * QWidget for linuxfb to composite.  As a child it costs one raise() and repaints
 * only its own 40x220 rectangle.
 *
 * IT IS DELIBERATELY NOT A PageWidget.  It has no navigation, it cannot be
 * entered or left, and it must be able to appear over a page that has taken the
 * whole panel -- the Media player at full screen, or a Terminal.  Overlays are
 * the shell's, like the toast and the keyboard, and they live outside the page
 * stack for the same reason those do.
 */
class VolumeOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit VolumeOverlay(QWidget *parent = nullptr);

    /* Show `value' per cent for three seconds, restarting the three seconds if it
     * is already up.  A negative value paints the no-card message instead. */
    void flash(int value, bool isMuted);

    /* Where it sits, given the panel.  The shell calls this from its own
     * resizeEvent -- the overlay does not lay itself out, because on this board
     * "the panel" and "my parent" are the same rectangle and only the shell knows
     * whether a dock is in the way. */
    void placeIn(const QRect &panel);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int m_value = -1;
    bool m_muted = false;
    QTimer *m_timer = nullptr;
};

#endif /* MIXDASH_VOLUME_H */
