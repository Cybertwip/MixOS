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

#include <QImage>
#include <QString>
#include <QWidget>

class QPainter;
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
 * A counter that moves whenever this process learns a new level or mute -- set
 * or read, from the keys or from the Settings slider.  It exists so a page can
 * notice the mixer moved WITHOUT forking amixer to find out: the Media player's
 * "the output is muted" note has to disappear the moment VOL+ unmutes, and its
 * half-second tick asking amixer twice a second for the whole length of a film
 * is not a price worth paying for a line of text that is usually absent.
 *
 * Compare it against the last value seen; any difference means ask properly.  It
 * is a counter and not a bool so two changes between two looks are still one
 * difference, and it never resets, so a missed look is caught by the next one.
 */
unsigned generation();

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
 * THESE TWO ARE STILL SETTINGS AND NOT A STATUS, even now that something else
 * moves them.  What stood here said the board brings no jack-detect line out to
 * anything the kernel can read, and that was true of every kernel this file had
 * seen: nothing noticed a plug, and the person holding the device was the detect
 * line.  j36_mt6592_input can now sample one -- an ADC channel or a GPIO pad,
 * whichever jack_adc=/jack_gpio= names -- and reports SW_HEADPHONE_INSERT, which
 * Joypad hears and Dashboard::onHeadphoneJack turns into a call to setOn() here.
 *
 * That changes who calls these and it does not change what they are.  There is
 * no detect line on a kernel that was not given one, both outputs remain
 * independently switchable while there is, and the user can still turn the
 * speaker on with headphones in -- Settings::speakerWanted() is where that
 * intention is kept, precisely because the mixer cannot hold it.  A page that
 * drew these as "Headphones (connected)" would still be inventing something
 * unless it had asked Joypad::jackKnown() first.
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

/*
 * WHAT THE JACK IS DOING, KEPT WHERE A PAGE CAN ASK WITHOUT HOLDING A JOYPAD.
 *
 * The detect line arrives as an evdev switch, so the thing that knows about it
 * is Joypad, and Joypad is the shell's -- a Settings page reaching into the
 * input stack to word one line of text would be a page that has to be handed a
 * Joypad pointer for the rest of its life.  The shell files the answer here
 * instead, beside the two switches it is about, and anything that draws those
 * switches reads it from the same place it reads everything else about them.
 *
 * THREE VALUES AND NOT A BOOL.  Unknown is the ordinary state on a kernel that
 * was never told where the line is, and it is not Empty: with no detect line the
 * two switches are the user's to set, and a page that said "nothing in the jack"
 * on a board that cannot tell would be stating something it does not know.
 *
 * This is a note and not a probe: it changes nothing on the card and forks
 * nothing.  Only Dashboard writes it.
 */
enum JackState { JackUnknown, JackEmpty, JackPlugged };
void noteJack(JackState state);
JackState jack();

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
 *
 * ── AND THERE IS ONE PAGE raise() CANNOT PUT IT OVER ──
 *
 * While a film is up, the picture in the scanout is not Qt's: GlVideo has the
 * framebuffer memory imported as a GL colour buffer and rewrites the player's
 * whole rectangle twenty-five times a second.  Qt's backing store has never heard
 * of it.  So this widget painted over a film is a memcpy that the next GPU frame
 * erases 40 ms later, and Qt repaints it, and the frame after that erases it
 * again -- a bar that flickers at frame rate.  raise() cannot fix that, because
 * there is no stacking order between the two: they are two writers to the same
 * pixels, and the fast one wins.
 *
 * The fix is to stop painting and start being painted.  setRedirected(true) makes
 * this a model with no surface of its own: it keeps the level, the mute and the
 * three-second timer, it emits changed() whenever any of those move, and whoever
 * owns the screen calls snapshot() and blends the result in its own pass.  That
 * is one texture upload per press instead of a repaint per frame, and the bar is
 * genuinely on top because it is drawn last by the thing that is drawing.
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

    /*
     * Hand the pixels to somebody else instead of painting them.  While
     * redirected this never shows itself, and every change to what it would have
     * drawn -- including the three-second expiry -- comes out as changed().
     *
     * The three seconds belong to the press and not to whoever is drawing, so
     * switching mode does not restart or cancel them: a film that ends one second
     * into a flash hands a bar with two seconds left back to Qt, which shows it.
     */
    void setRedirected(bool on);
    bool isRedirected() const { return m_redirected; }

    /* True while the three seconds are still running, whether or not this widget
     * is the thing showing it. */
    bool isUp() const { return m_up; }

    /* What this would paint, as a premultiplied ARGB image at its own size.
     * Null when nothing is up. */
    QImage snapshot() const;

signals:
    /* The bar appeared, changed, or timed out.  Only connected to while
     * redirected, but emitted either way -- a signal whose meaning depends on a
     * mode is a signal somebody eventually connects in the other one. */
    void changed();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void expire();
    void paintBody(QPainter &p) const;

    int m_value = -1;
    bool m_muted = false;
    bool m_up = false;
    bool m_redirected = false;
    QTimer *m_timer = nullptr;
};

#endif /* MIXDASH_VOLUME_H */
