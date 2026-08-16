# MixOS 1.1.3 — release notes

This release is the J36 Ultra port: a Debian trixie armhf userspace on a kernel built
from source, with device drivers written for the silicon that is actually in the case.
Everything below is in [`device/j36-ultra/`](device/j36-ultra/) unless it says
otherwise, and everything below is code that runs on the board — this is a description
of the tree, not a roadmap.

Legacy Rockchip devices (RK3326, RK3566) continue to build from the same repository and
share the same armhf rootfs on the same card.

Dual-licensed **MPL-2.0 or GPL-2.0-or-later** at your option; see
[`device/j36-ultra/LICENSE`](device/j36-ultra/LICENSE), which records file by file the
parts that are `GPL-2.0-only` and why.

---

## The machine

| | |
|---|---|
| SoC | MediaTek MT6592 — eight Cortex-A7 cores, ARMv7 |
| GPU | Mali-450 MP4, driven by `lima` |
| Panel | 640×480 JD9365, MIPI DSI |
| Memory | 946 MB usable, plus optional zram |
| Input | keypad matrix, direct GPIO buttons, two analog sticks |
| Storage | one microSD card, one USB port |
| Power | MT6592 PMIC — linear charger, coulomb-counting gauge |

## The image

Two partitions and no more:

| Partition | Format | Size | Contents |
|---|---|---|---|
| p1 `BOOT` | FAT32 | 100 MB | boot payload, kernel modules, optional payloads |
| p2 `ROOTFS` | ext2 | 4096 MB as shipped | the Debian rootfs |

The shipped image is about 4.2 GB — the rootfs rounded up to the next whole GiB and
nothing else. **`ROOTFS` is the last partition on the disk, and the initramfs grows it
to the end of the card on the first boot**, with a percentage on the splash rather than
a seconds counter, because on a 64 GB card the `e2fsck` and `resize2fs` pass is minutes
and a blind wait reads as a hang. Nothing is written to the card to remember that it
happened; the check is the geometry itself.

There is no `DATA` partition. `/home/virtua` is an ordinary directory on the rootfs.

The kernel plus initramfs is held under the fixed 9 MiB boot-image slot the vendor LK
loads from; the build fails rather than overruns it, which is why several subsystems
below are deliberately modules on `BOOT` instead of built in.

## Kernel

### Out-of-tree drivers

| Module | What it is |
|---|---|
| `j36_mt6592_input` | The pad. Reads GPIO DIN with the pull-up armed, muxes the three keypad pads the boot chain leaves parked, ungates the keypad's 32 kHz clock in the MT6323 PMIC over PWRAP, reads the KPD scan memory, and runs the stop/settle/start AUXADC sequence for the sticks. Reports **one** Linux gamepad, `2454:6500`. |
| `j36_mt6592_pmic` | Battery gauge, linear charger, BC1.2 detection and power-off, through the `power_supply` class. The published percentage is a coulomb integrator seeded from the wakeup OCV latch, not a curve lookup — with no power-path FET, BATSNS reads the charger's setpoint whenever a cable is in. |
| `j36_mt6592_backlight` | The MT6592 BLS block in front of a TPS61161, as a real `/sys/class/backlight` device. Before it, the panel ran at whatever level the LK left it at for the whole boot. |
| `j36_mt6592_audio` | One playback PCM on the AFE's DL1 memif, so this kernel has an ALSA card at all. There is no MT6592 audio support upstream — `sound/soc/mediatek` starts at MT2701. |
| `j36_mt6592_usb_phy` | The U2 PHY as a generic-PHY provider for `drivers/usb/musb`. MT6592's PHY has no driver upstream, and the ordering matters: an APB access to a clock-gated MediaTek peripheral stalls the bus until the watchdog fires. |
| `j36_jd9365_panel` | A `mipi_dsi_driver` for the panel the LK has already brought up. It exists because `mtk_dsi` cannot finish binding without one — no panel driver, no DRM master, no `/dev/dri/card0`. It adopts the live panel and sends it nothing. |
| `j36_fbmem` | A dma-buf over the framebuffer the bootloader handed us, so a GL frame can be scanned out instead of `glReadPixels()`'d into a `write()`. Removes 2.4 MB of memory traffic per frame. |
| `j36_mt6592_wifi` | CONSYS Wi-Fi, in six translation units: the VCN rails, MTCMOS domain, CONNMCU clock and chip-ID probe; the BTIF link with MediaTek STP framing and both ROM patches; the WLAN AHB HIF at `0x180f0000` and the firmware load; the running firmware's command and event protocol; and a fullmac `cfg80211` driver that registers `wlan0`. |

### In-tree patches

| Patch | Subsystem |
|---|---|
| `0001-mtk-sd-mt6592` | MSDC — the microSD this kernel is loaded from |
| `0002-drm-mediatek-mt6592` | the display pipe |
| `0003-musb-mediatek-mt6592` | the USB controller glue |
| `0004-usb-phy-generic-no-node-no-vbus` | a generic PHY with no DT node and no VBUS regulator |
| `0005-arm-mediatek-mt6592-smp` | SMP bring-up for cores 4..7 |

### The per-CPU tick

The device tree now carries an `arm,armv7-timer` node with
`arm,cpu-registers-not-fw-configured`, and `ARM_ARCH_TIMER`, `HIGH_RES_TIMERS` and
`NO_HZ_IDLE` are selected. Before it, the only clockevent was the global
`mediatek,mt6577-timer` GPT, which `tick_check_new_device()` will not make anyone's
per-CPU tick — so every core fell back to `dummy_timer`, `hrtimer_switch_to_hres()`
gave up, and every timer in the system rounded to a jiffy. The eight copies each of

```
Clockevents: could not switch to one-shot mode: dummy_timer is not functional.
Could not switch to high resolution mode on CPU N
```

were never a report about cores failing to come up — they are printed once per **online**
CPU, so eight of each was the proof that all eight were up. They are gone, and with them
the timer granularity that showed up as frame pacing in the media player.

### Subsystems selected

DRM core built in with `lima` and `mtk_drm` as modules on `BOOT`; `DRM_UDL` as a third
card for the USB-HDMI mirror; ALSA core modular with the AFE driver; USB host mode only,
with mass storage and USB gamepads; SCSI as a module for USB disks; ext2, vfat and exfat;
`cfg80211` and nothing above it; the `power_supply` and `backlight` classes; zram, sized
from the kernel command line (`j36.zram=<MB>`, `j36.zram=0` to turn it off).

## The shell — `mixdash`

Qt5 Widgets on `linuxfb`, driven by a thumbstick. Software raster compositing throughout:
there is no reachable 2D engine on this board.

**Dashboard.** Cards for every destination, arranged by the operator and remembered. Card
art is cached per card and blitted at integer offsets, so a placement no longer repaints
every card between the two slots on every frame. A startup sound plays once, on the first
frame that is worth showing.

**Files.** Four panes — an editable address bar, a search box, a Places list, and the
listing — with a right-hand info panel (name, kind, size, modified, access; deliberately
no path). Places includes every volume mounted under `/media`.

**USB disks.** Thumb drives and disk drives appear as dashboard cards when they are
plugged in and disappear when they are pulled. A card opens a file browser scoped to that
filesystem and nothing outside it. Long-press opens a menu: Open, Move, Eject. Eject goes
through systemd, then the mount helper, then `sync` and `umount`, checking after each rung
and reporting honestly when the volume is busy.

The mounting itself is the initramfs's: a udev rule, a `BindsTo=` systemd template unit,
and `/run/j36/bin/mixos-automount`, which mounts under `/media/<label>` and writes a state
file per volume for the dashboard to read. The dashboard reads. It never mounts.

**Media.** Music, video and pictures. The GPU draws the film into the memory the panel is
already reading, instead of the five copies per frame the old path made. Audio-synced
decode, a timebar, transport controls that fade after a few seconds, full-screen and loop,
and titles taken from metadata with the filename as the fallback.

**Wi-Fi.** Scan, join and forget, including WPA passphrases typed on the virtual keyboard.

**Sharing.** The device's storage on the network over SMB — `/home/virtua` and the mounted
volumes — so getting a file onto the device no longer means powering it off and taking the
card out.

**Packages.** Debian's archive from the handheld, `apt` behind a search box. This is the
page that makes the device a computer rather than an appliance.

**Diagnostics.** What the "3D cube" card grew into: seven sections — Display, Graphics,
Input, Audio, USB, Power and System — each probed and reported with the reason, plus the
EGL and mirror tests as things you can run from the page.

**Settings.** A hub with volume and mute, over three pages: Mouse, Display (backlight), and
**Region & Language** — one world map that sets the time zone and the interface language
together, because a device out of a box does not know where it is and everything that
follows from where it is is the same question.

**Terminal**, **Info**, **Power off**, and a **Desktop** — an X server on the framebuffer
with a window manager on it, which the **Browser** card is now one window inside rather than
the whole of. Anything else graphical goes in beside it: `j36-xrun COMMAND` from the
Terminal card hands a command line to the running session over a control pipe, so a program
installed from the Packages card gets a screen without knowing anything about how this
device is put together. **Menu** tapped pages round the open windows, **Menu** held brings
the dashboard back with the session left running behind it, and four windows at a time is
the cap. The Terminal card exports `DISPLAY=:0` for the same reason. The pad drives the
pointer inside the session and the on-screen keyboard is laid out like the dashboard's.

**Task switcher.** Hold **FN** and a list of what is running comes up over the top of it —
the dashboard itself, every card that started a program, and every binary launched from the
file browser. A tapped FN still means what it always did; only a hold opens the switcher, so
Ctrl+C in the Terminal is untouched. FN steps down the list, A switches, Menu closes the
highlighted task, B puts back whatever was in front.

Several programs run at once and exactly one of them owns the panel: the rest are stopped by
the kernel, on their own process group, so a desktop session — X server, window manager, pad
bridge, on-screen keyboard and every window in it — goes down and comes back as one thing. A task's last frame is kept and put back before it is continued,
because an X server only repaints what it thinks is damaged and would otherwise come back
with the switcher still on its screen. Four at a time, which is what the list shows without
scrolling and about what 946 MB will hold.

**Virtual keyboard.** Three ways in at once, all live: the D-pad walks the grid and A
presses, the pointer clicks a key, and a real USB keyboard types straight through. Shift
latches for one character; a second press locks caps, the cap says which state it is in,
and the lit key is the accent colour.

**Languages.** English, French, Italian, German, Portuguese and Spanish, from a compiled-in
phrase table.

## Userspace tools

| Tool | What it does |
|---|---|
| `mixsplash` | The boot splash, including the first-boot expansion percentage |
| `j36-padx` | The pad as a mouse and a keyboard inside an X server. The kernel presents everything — both sticks, the D-pad, four face buttons, four shoulders, Start/Select/Menu — as one evdev device, and X wants a pointer. Menu is the session gesture: tapped it asks for the next window, held it hands the panel back to the dashboard |
| `j36-eglprobe` | Brings up an ES 2.0 context on lima's render node and draws a cube; the ground truth for "is the GPU working" |
| `j36-mixmirror` | The USB-HDMI mirror, over `DRM_UDL` |
| `mfgpower` | Powers up the MT6592 MFG (GPU) domain through the SPM's MTCMOS from userspace and proves a Mali-450 MP4 is answering, before anything hands the block to lima. Nothing on the Linux boot path un-gates it |

---

## Fixed in 1.1.3

**The board went dark partway through the first boot's partition grow.** The charge stopped
four seconds into *every* boot, and the resize was simply the first thing long enough to
notice. `CHR_CON13` holds a four-second charger watchdog: the preloader arms it, LK kicks it,
and nothing kicked it once Linux had the machine, because the driver that does was loaded
from `j36/power/` — after the card scan and after the grow. This PMIC family has no
power-path FET, so `VBAT` *is* `VSYS`, and past that fourth second the whole board was
running off the cell with `CHR_CON16`'s UVLO threshold still set to whatever the loader
chose. Below that threshold the PMIC latches off, which on this board is not a warning about
a brownout — it is the brownout, and it reads to whoever is holding the device as a
spontaneous restart. `expand_root` then arrives with minutes of the heaviest sustained card
current in the boot.

`j36_mt6592_pmic.ko` is now staged into the initramfs as well as into `j36/power/`, and
`/init` loads it before it looks for the card at all — so the driver's one-second poll is
feeding the four-second timer and UVLO is at its widest ride-through before any of the work
starts. `j36.power` still gates it and `j36.power=nocharge` is still honoured; `run_power`
skips the payload copy when the early one is already in `/sys/module`. Reflashing never
helped because the card was never at fault.

Alongside it, three things the grow now does for the case where power *is* lost anyway: the
filesystem check is skipped when the superblock already says `clean` (and forced back on with
`j36.expand=fsck`), roughly halving the window; the splash carries **Do not turn the device
off** for the duration; and a card left damaged by an interrupted grow says **Reflash MixOS
into the installation media** rather than presenting as an intermittent fault.

**The browser's on-screen keyboard could not type `.com`.** Not an exaggeration — the
session was using matchbox-keyboard's packaged layout, which is three rows: `q`–`p` and
backspace, `a`–`l` and return, shift and `z`–`m` and space. No digits, no full stop, no
slash, no colon, no at sign, no symbols layer of any kind, and its one shift key was a
one-shot that only ever reached `A`–`Z`. Handed that keyboard and an address bar, there is
no address you can type.

The browser now uses a layout built to match the dashboard's, staged to
`/opt/mixos/share/keyboard/` and selected with `MB_KBD_CONFIG` — Debian's own files are
left as installed. Digits are the top row; `-` `,` `.` `/` `:` `@` are on the base layer
where no modifier can move them, so `.com` is four taps with nothing held. A **`?123`** key
reaches the brackets, currency and the rest of the symbol set on the other faces of the
keys, and a **`caps`** key is a real lock — it holds until pressed again and reads `CAPS`
while it is on, so the state is visible. Caps and `?123` do not stack; with the lock on you
get capitals.

**The browser was not Firefox.** `needed_packages.txt` is read in exactly two places, and
both of them sit inside checkpointed build stages — so on any machine that had completed a
build once, editing that file did nothing at all. `firefox-esr` is how it was found: it was
added to the list, every build afterwards reported success, and the card kept booting with
`netsurf-gtk`, which has no JavaScript engine. No error, no warning, not even a line in the
log naming the package. The build now digests the package lists it actually reads into the
stage stamps — separately for the runtime list and the dev list, because honouring them
costs very different amounts — so adding or removing a package invalidates the install and
the finished image instead of being walked past. Alongside it, `j36-browser` now says out
loud, on the console and in the log, when it is falling back to something that is not the
browser this session was written for. **This one needs one more build**; the fix is in the
builder, and an image already flashed cannot grow a browser it was never given. It is the
cheap half of the build — the runtime list is reinstalled during finalization.

**The on-screen keyboard came up thumbnail-sized on the first launch after a boot**, and the
right size on every launch after that. Not a font cache: a race with the session's own
window manager. `mb_kbd_ui_resources_create()` works out how small the keyboard can be from
the font metrics, then stretches it to the width it finds in `_NET_WORKAREA` — a property
published by matchbox-window-manager, which on a cold launch has not got that far yet, every
binary involved still being faulted in off the card. `--daemon` realizes the window once at
startup, so the minimum stuck for the rest of the session. The session now measures the
panel itself, off `/sys/class/graphics/fb0/virtual_size`, and asks for `--width`/`--height`;
that same function skips the `_NET_WORKAREA` path entirely when a size is requested, so there
is no window left in which the answer can be wrong. It asks for two fifths of the panel
height because that is what `mb_kbd_ui_resize()` clamps to anyway — the tallest keyboard it
will agree to draw, and the font scales with the box.

**The pointer crossed the screen in a second.** 560 px/s is the right instinct for a mouse on
a desk and the wrong one for a thumb on a 12 mm stick with no wrist behind it: every target
was overshot and the correction overshot back. The default is now **200 px/s**. `j36-padx`,
which drives the pointer inside the browser's X session, had its own hard-coded "one screen
width per second" and so ignored the dashboard entirely; it now reads `[mouse] pointerSpeed`
out of `/var/lib/mixos/mixdash.conf` and uses that, with the same 200 px/s as a fallback —
expressed as a fraction of the panel width, so a different screen gets a proportionate speed
rather than this one's. A speed already saved from the Settings page is still honoured.

**mixsplash dropped to a text console partway through "Starting Audio".** The splash has a
90-second fuse for a boot that has gone quiet, and what it was actually measuring was "the
headline has not changed" — the only messages that reset it were `stage:`, `detail:` and
`progress:`. The audio stage is a run of `insmod`s under a single headline, and it got
slower when the input driver began sampling the headphone jack line every 5 ms against a
codec probe touching the same PMIC, which is why re-plugging a speaker was what made it
appear. Every `say()` in `/init` now also sends `ping` — a message with nothing to draw,
whose only job is to push both fuses out — so the fuse means what its own comment always
claimed; and the module loop names each module as it loads it, so that stage visibly moves.

**The grid did not close up when a USB stick was inserted.** The bottom row centred three
cards and left the fourth, the one the insertion had just added, sitting where the old
layout had put it. `CardGrid::setEntries()` recomputed the geometry and repainted, but never
started the spring: `resetMotion()` deliberately leaves already-placed cards at their current
coordinates and it is `step()` that walks them to their new slots, so with the animation
timer asleep the new layout was worked out and never travelled to. It now wakes the spring
on every change to the list.

**A volume plugged in while the Media page was open did not list.** Two faults with one
symptom, which is why it read as "the disk only shows up after you have been through the
Files page". `onDisksChanged()` began with `if (m_view != ViewBrowse) return`, and music
outliving the page it was started from is the whole point of the queue — so a stick plugged
in while the player, a picture or a film was up was dropped on the floor, and coming back to
the listing showed the one built when there was a single root. And when the event *was*
taken, all it added to a root's listing was a `..` row at the top: that row says "up one
level" until you press it, the level above is a places list nobody has been told exists, and
the volume's own name appears nowhere on the screen the user is actually looking at. Files
has had its volumes permanently on the glass since the day it was written, for exactly this
reason. The listing is now rebuilt whichever view is up — only the cursor bookkeeping is
browse-only — and a root's listing carries every *other* root on it by name, one press to
open, so a disk arrives where somebody who has just plugged one in is standing. Only on a
root: repeating the volumes inside every folder would answer a question nobody asked of a
directory listing.

**Compiled shaders were being thrown away.** `j36-glwarm.service` brings EGL up once after
the dashboard has painted, so the first graphics program does not pay for the whole stack
arriving off the card — but it named a shader-cache directory on itself and nothing else
did. What it compiled went somewhere no other process on the board could name, and every EGL
program rebuilt its shaders from source on every launch. The Diagnostics page's GPU render
row is the sharpest case: it runs the same `eglprobe`, with the same cube, whose two shaders
the warm-up had already compiled at boot. The initramfs now picks one directory —
`/var/cache/mixos/gl` when the rootfs can be written to, `/run/j36/glcache` when it cannot —
and puts it in the environment of both the warm-up and `mixdash`, which is where every card
and every program launched from the grid inherits it.

**The battery collapsed to 0% after a full charge.** Three defects in the gauge, all in
`j36_mt6592_pmic.c`:

- *The full latch was thrown away by a report, not a measurement.* `charge_full` was
  cleared whenever `online` went false — and on this board `online` is vetoed by a
  comparator sitting on the OTG port's own 5 V net, so it goes false for reasons that have
  nothing to do with the cell. Losing the latch un-pins the level, and a full pack is then
  handed to the two paths that can only walk downward: 100 seconds to zero on one, fifty
  minutes on the other. Full is now cleared by one thing, the pack relaxing under the
  recharge voltage for six samples together, whether or not a cable is claimed.
- *The empty ramp was measuring the rail.* The 1 %/s "0 % tracking" ramp compared the raw
  terminal voltage against 3450 mV. VBAT is the system node here and the OTG port sources
  5 V off it for the whole uptime, so a pack with real charge in it crosses that line at
  the connector while the cell behind it is nowhere near empty. It now compares the
  IR-compensated OCV, which under load is the higher of the two — so the ramp only ever
  declines to start, never starts one it would not have before.
- *The coulomb accumulator never saturated.* It went on counting charge into a pack that
  had stopped taking it. It now winds back to the base, and a run that has delivered a
  whole pack's worth past top-off is itself a termination condition — which matters here
  because the shunt reads the **net** current, so a charger still sourcing into a full pack
  never shows up as a small current and the old six-in-a-row could never complete.

**All eight cores had no per-CPU tick.** See *The per-CPU tick* above.

**Caps lock did not lock.** Two independent faults: the shift cap was labelled with the
*next* state rather than the current one, so a locked keyboard read "shift" and a one-shot
read "CAPS" — exactly inverted; and a USB keyboard's Caps Lock key did nothing at all,
because there is no caps modifier bit to carry. Caps is now a keyboard state, letters only,
with shift inverting it, and the engaged cap is drawn in the accent colour.

**Card placement was choppy.** Every card between the two slots was repainted every 33 ms —
six antialiased shadow strokes, three gradient fills, a clip path and shaped text, none of
it cached by Qt. Cards are now painted once into a pixmap and blitted; only the selected and
carried cards are drawn live.

**The media player skipped and froze.** Audio-synced decode, an async binary invocation path
so the dashboard does not block on a launch, a spinning overlay while loading, real transport
controls in place of the word "Decoding", metadata-or-filename titles, and the pointer no
longer leaves a square behind it when it moves.

**`-Wformat-truncation` in `j36-padx.c`.** The `EVIOCGNAME` fallback copied a 320-byte path
into a 128-byte name; the precision is now explicit.

**The image shipped 12 GB of emptiness.** The `DATA` partition is gone, the image is the
rootfs rounded up to the next GiB, and the headroom comes from the card the operator
actually bought.

## Known gaps

These are in the tree's plan and are not in this release:

- The media player starts decoding before it is fully ready; playback should not begin until
  the first frame and the audio clock are both in hand.
- In the media player the sticks and D-pad are bound to commands rather than hover-navigating
  the controls, and the right stick's click is bound the same way.
