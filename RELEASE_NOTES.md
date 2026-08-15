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
every card between the two slots on every frame.

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

**Diagnostics.** What the "3D cube" card grew into: EGL/GLES2, the display pipe, audio, USB,
the PMIC and the connectivity subsystem, each probed and reported.

**Settings.** A hub with volume and mute, over three pages: Mouse, Display (backlight), and
**Region & Language** — one world map that sets the time zone and the interface language
together, because a device out of a box does not know where it is and everything that
follows from where it is is the same question.

**Terminal**, **Info**, **Power off**, and a **Browser** session — Firefox on an X server on
the framebuffer, with the pad usable inside it.

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
| `j36-padx` | The pad as a mouse and a keyboard inside an X server. The kernel presents everything — both sticks, the D-pad, four face buttons, four shoulders, Start/Select/Menu — as one evdev device, and X wants a pointer |
| `j36-eglprobe` | Brings up an ES 2.0 context on lima's render node and draws a cube; the ground truth for "is the GPU working" |
| `j36-mixmirror` | The USB-HDMI mirror, over `DRM_UDL` |
| `mfgpower` | Powers up the MT6592 MFG (GPU) domain through the SPM's MTCMOS from userspace and proves a Mali-450 MP4 is answering, before anything hands the block to lima. Nothing on the Linux boot path un-gates it |
| `fbdoom` | The first moving picture this panel ever showed |

---

## Fixed in 1.1.3

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
- There is no task switcher; the FN button does not yet raise a multitasking overlay across
  cards and running binaries.
