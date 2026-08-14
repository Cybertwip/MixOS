# <p align="center">MixOS</p>

### <p align="center">**M**icrosoft **I**ncorporates the **X** **OS**</p>

### <p align="center">A Microsoft line of operating systems, based on Linux and Debian, brought to you by Cybertwip for MediaTek and legacy Rockchip handhelds.</p>

[![Donate](https://img.shields.io/badge/Donate-PayPal-green.svg)](https://www.paypal.com/donate?hosted_button_id=RC72LJ4SSERSU)

## The name

Free software has a tradition of explaining itself through its acronym, and GNU set
the bar: **G**NU's **N**ot **U**nix, a name that answers the only question anybody
was going to ask, and answers it with itself, forever.

MixOS follows the tradition and misses the point entirely. It stands for

> **M**icrosoft **I**ncorporates the **X** **O**perating **S**ystem

which does not recurse, does not clarify, and name-drops two companies in five
words. GNU's acronym is a philosophical position. This one is a press release that
got away from somebody — the whole operating system, brought to you in partnership
with X (now part of the xAI company), incorporated, at last, by Microsoft.

What *is* true is the rest of this page.

## What it is

MixOS turns a cheap MediaTek handheld into a working Linux computer. Not a launcher
bolted onto a vendor Android image — a real Debian rootfs, a mainline kernel built
from source, device drivers written for the silicon that is actually in the case,
and a shell you can drive with a thumbstick.

The reference device is the **J36 Ultra**: MediaTek MT6592, eight Cortex-A7 cores at
ARMv7, a Mali-450 MP4, a 640x480 MIPI panel, a keypad matrix, two analog sticks and
a battery. It is a 2013 phone SoC in a 2024 games console shell, and nothing
upstream supported it. The port lives in
[`device/j36-ultra/`](device/j36-ultra/).

Legacy **Rockchip** devices — RK3326 and RK3566 — continue to be supported the way
they were, and share the same armhf Debian rootfs on the same card.

On the device you get:

* **mixdash**, the shell — Qt Widgets painted straight into `/dev/fb0`, no X, no
  Wayland, no compositor. Apps, Media, Settings and Power at the root; Files,
  Terminal, Wi-Fi, Sharing, Packages, Diagnostics, Mouse, Display, Language and
  System Info pushed on top. Everything is reachable from the D-pad, and the tabs
  are reachable by pushing left or right off the edge of a page, so a board being
  driven with one thumb is never stuck.
* **Six languages**, compiled in — English, French, Italian, German, Portuguese and
  Spanish — switchable from Settings without a restart.
* **The hardware, working.** Battery gauge and charger through the MT6592 PMIC.
  Backlight that inherits whatever the bootloader left and never blanks itself.
  Audio through the MT6592 AFE. Volume keys with an on-screen bar. Poweroff that
  actually cuts the rail instead of parking the CPU with the screen on.
* **USB host** — mice, keyboards, gamepads, mass storage that automounts, and a
  USB-HDMI dock: plug in a DisplayLink adapter and the panel is mirrored onto the
  TV, tile-diffed so a still screen costs no bus traffic. The handheld keeps its own
  screen the whole time.
* **Sharing** — the `DATA` partition (`/home/virtua`) offered over SMB to the
  network and over USB, so getting files onto the device does not mean taking the
  card out.
* **Debian underneath**, which is the point: `apt install` works, and there are over
  64,000 packages on the other end of it.

## What it aims at

1. **A full working mobile console PC station.** The handheld should be a computer
   you can dock, type on, share files with and install software on — not an
   appliance that plays one thing.
2. **MediaTek support, on real hardware, in the open.** Every register in this tree
   was found the hard way and is commented with why.
3. **Nothing that cannot be removed.** Every payload on the card is a directory and
   a word on the kernel command line. Delete the directory, or the word, and the
   boot carries on and says what it could not find. There is no configuration state
   that can only be repaired by reflashing.
4. **Updates without reflashing.** It is Debian; `apt` is the update mechanism.
   A new card is only needed for structural changes, like a partition layout.
5. **Performance on hardware nobody optimises for any more**, and enthusiasts first
   — if the feature you want is not here, fork it and add it.

## How it works

**The card is three partitions.** `BOOT` is FAT32 and holds only the four files the
device's own MVII little kernel can read — the kernel, the device tree, the
initramfs and `mvii/boot.conf`. `ROOTFS` is ext2 and is the shared armhf Debian
userspace, the same one a Rockchip board on the same card boots. `DATA` is ext2,
mounted at `/home/virtua`, and is where everything the user owns lives.

**The boot is a hand-off, not a bootloader replacement.** The stock MVII LK reads
`boot.conf`, loads the ARMv7 kernel and hands over with a framebuffer already
running. The initramfs `/init` takes it from there: it finds the rootfs, reads the
`j36.*` words off the kernel command line, and `insmod`s exactly the payloads those
words ask for — `j36.gl`, `j36.usb`, `j36.power`, `j36.audio`, `j36.wifi`,
`j36.dash` — each from its own directory under `/opt/mixos/j36/` with its own load
order, each failing on its own without taking the boot down. Then it writes the
dashboard's systemd unit into a tmpfs and switches root.

**The build is three stages.** `./build-j36-ultra.sh` runs on the host, brings up a
Multipass VM, and inside it `device/j36-ultra/build-in-vm.sh` does the real work:
kernel, device tree, modules, initramfs, the Qt dashboard cross-built in an emulated
armhf chroot, the Mesa payload, and the SD image. Everything is incremental and
cached; the script checks its own output at every stage and refuses to ship a card
that is half one build and half another.

Start with [`device/j36-ultra/README.md`](device/j36-ultra/README.md). It documents
the payload, every word of the kernel command line, and how to take any part of it
off a card without a reflash.

## Building

**Suggested environment** — Ubuntu 24.04 or newer, or a related variant. Windows
Subsystem for Linux is not supported and will not work, because this process uses
`chroot`.

Because `chroot` is used, heavy use is made of `sudo`. To reduce the chance of
privilege problems it is best to be able to run `sudo` without a password:

* **Method 1** — run `sudo visudo` and add `$USER ALL=(ALL) NOPASSWD: ALL` at the
  bottom, where `$USER` is your username. Save and close.
* **Method 2** — clone this repo and run `./FreeSudo.sh`. Check that
  `/etc/sudoers.d/$USER` exists and contains that line.

Then build for a supported device with `make <device_name>`, e.g. `make rg353m`.

### J36 Ultra (MediaTek MT6592)

Builds a 32-bit ARM Linux 6.12 LTS kernel, the MT6592 device tree, the
display/audio/input/USB/PMIC modules, an initramfs and the SD `BOOT` payload the
MVII little kernel hands control to. The ARMv7 kernel and the Rockchip arm64 kernel
coexist on one card and share one armhf Debian rootfs.

```bash
./build-j36-ultra.sh
# device tree only, in a second, no VM
./build-j36-ultra-dtb.sh
```

### R36 Ultra (Rockchip RK3326)

Builds the RG351MP/RK3326 base image used for bring-up; the R36-specific DTB layer
is still separate. The default profile is intentionally small — one native armhf
Debian userspace with no front end on it, and no arm64 userspace, bundled emulators
or standalone applications.

```bash
./build-r36-ultra.sh
# or
make r36-ultra
```

The default is four parallel jobs and an armhf-only userspace:

```bash
BUILD_JOBS=4 USERSPACE_ARCH=armhf ./build-r36-ultra.sh
```

* `BUILD_JOBS` controls internal Make/CMake/Meson parallelism.
* `USERSPACE_ARCH=arm64` builds a single-architecture arm64 userspace instead. The
  R36 helper never produces an arm64+armhf multiarch rootfs.

### Notes

* **Build time is about two hours** on a mid-range desktop, first run included, and
  much less than that on a rebuild — the Debian bootstrap, the package set, the
  emulated armhf chroot and the compiler output are all cached between runs, and a
  finished base build is not re-partitioned or re-bootstrapped.
* To build against a different Debian release, change the `DEBIAN_CODE_NAME` export
  in the Makefile or pass `DEBIAN_CODE_NAME=<release>` to `make`. Release names are
  at <https://www.debian.org/releases/>.
* `r36-ultra` and `j36-ultra` are the only build targets. `USERSPACE_ARCH` picks the
  architecture on both and defaults to armhf — which is simply the native
  architecture of the J36's Cortex-A7.
* Builds land in `MixOS-Artifacts/`, a sibling of the checkout. Set
  `MIXOS_ARTIFACT_DIR` to put the output somewhere else.
* Some environment variables, shell helpers and the build VM name still spell an
  older project name. Those are interfaces rather than branding, and renaming them
  would orphan existing build state and cached checkpoints, so they are deliberately
  left alone.
* The inherited `/opt/system` tool menu, the hostapd/dnsmasq access point and the
  OTA updater are gone. None of them could run on an image built from this tree.
  Their replacements belong in mixdash.

# Licence

MixOS is assembled from many open-source components, each under its own licence.
The full statement, file by file, is in [LICENSES.md](LICENSES.md). The short
version is below, and the parts that are not negotiable are marked as such.

### The MixOS work is dual-licensed: MPL-2.0 **or** GPL-2.0-or-later

```
SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
```

You may take any original MixOS file under **either** the
[Mozilla Public License 2.0](https://www.mozilla.org/MPL/2.0/) **or** the
[GNU General Public License, version 2 or later](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html),
at your option.

**Why this pair, and not something simpler.** The project needs three things at
once, and no single licence gives all three:

* **Commercial use has to stay open.** Both halves of this permit selling devices,
  selling images and shipping MixOS inside a product. Neither has a
  non-commercial clause, and the previous non-commercial framing of this repository
  is gone.
* **The source has to stay under control.** MPL-2.0 is *file-level* copyleft: a
  modified MixOS file must be published under MPL-2.0, so improvements to this code
  come back, while proprietary code in the same binary, the same product or the same
  card is explicitly allowed to stay proprietary (MPL-2.0 §3.3). That is what keeps
  a commercial device buildable on top of this without the vendor's own application
  being conscripted.
* **It has to be able to live next to Linux and Debian.** This is the part that
  forced the change. The previous licence, Ms-PL, is *not* GPL-compatible — its
  §3(D) adds a condition GPLv2 §6 forbids adding — so MixOS code could never be
  combined with, linked into or derived from the kernel it runs on. MPL-2.0 solves
  this deliberately and by design: §1.12 and §3.3 make it a *secondary-licence*
  arrangement, meaning MPL-2.0 code may be relicensed under the GPL when it is
  combined with GPL code. Offering GPL-2.0-or-later explicitly alongside it removes
  the last doubt, so a MixOS file can be moved into a kernel module or a GPL project
  without a lawyer being consulted first.

The result is the freedom the GPL enables, applied to a codebase that can still be
sold and still be built into a closed product — which is the position this project
needs to be in.

### The parts that are GPL because they cannot be anything else

MixOS **inherits the GPL** from the two things it is made of, and this is a
statement of fact rather than a choice:

* **The Linux kernel is GPL-2.0-only.** Everything under
  `device/j36-ultra/linux/` — the MT6592 input, audio, USB PHY, PMIC, backlight,
  Wi-Fi and panel drivers, and the `mtk-sd` and `drm/mediatek` patches — is a Linux
  kernel module or a Linux kernel patch. It derives from and links against
  GPL-2.0-only kernel internals, it carries `SPDX-License-Identifier: GPL-2.0-only`
  and `MODULE_LICENSE("GPL v2")`, and it is **not** dual-licensed and **not**
  relicensable. If you distribute a device running this kernel you are distributing
  GPL-2.0 software and you owe its recipients the corresponding source, which is
  this repository plus the kernel tree it names.
* **Debian is the operating system.** A MixOS card boots a Debian rootfs assembled
  with Debian's own tools, and all but a handful of the packages on it are the
  Debian project's work under their own licences — GPL, LGPL, MIT, BSD, Apache and
  more, recorded per package in `/usr/share/doc/*/copyright` on a running device.
  Debian's GPL packages carry Debian's GPL obligations, unchanged, onto every card.
  MixOS does not and cannot relicense any of it.

So a finished MixOS card is an aggregate: the MixOS scripts, drivers and dashboard
that assembled it, on top of a kernel that is GPL-2.0-only, on top of a
distribution that is thousands of licences deep. The dual licence above applies to
the MixOS work. Everything else keeps its own terms, and shipping a card means
honouring all of them.

### The build scripts

Everything outside `device/j36-ultra/`, `device/r36-ultra/` and `device/common/` —
the inherited image-assembly scripts, the `Makefile` targets and the Rockchip
tooling — came from **dArkOS** and keeps **dArkOS's MIT licence** and copyright,
including the files MixOS has modified: the modifications are MixOS's, the files
remain dArkOS's work under dArkOS's terms. dArkOS is in turn a Debian-based
continuation of [ArkOS](https://github.com/christianhaitian/arkos/wiki).

Nothing else in MixOS comes from either project any more. Please do not report
MixOS problems to dArkOS or ArkOS; neither endorses this work or is responsible for
it.

### Vendor sources

`device/j36-ultra/mvii-board/` holds verbatim MediaTek/MVII board headers and driver
sources, redistributed unmodified so the device tree generator can be seen to parse
real vendor source. They are inputs, not MixOS work, they carry whatever terms their
authors set, and `mvii-board/PROVENANCE.txt` records a SHA-256 for each.

# Credits and Thanks

**[Debian](https://www.debian.org/)** — the operating system all of this is built on.  The rootfs a MixOS device boots *is* Debian, assembled with Debian's own tools, and all but a handful of the packages on the card are the Debian project's work.  MixOS is a set of device ports and a build system on top of Debian; the operating system itself is theirs.  MixOS is not affiliated with or endorsed by the Debian project. \
**[The Linux kernel](https://www.kernel.org/)** — the other half of the foundation, and the reason a 2013 phone SoC can be brought up at all in 2026 \
**[ArkOS](https://github.com/christianhaitian/arkos/wiki)** and christianhaitian — the original handheld distribution \
**dArkOS** — the Debian rebuild of ArkOS, and the origin of the build scripts this tree still uses \
**MediaTek** — the MT6592 documentation and vendor driver sources the J36 Ultra port reads \
**[Mesa](https://www.mesa3d.org/)** — lima and kmsro, which are the entire reason a Mali-450 from 2013 can run a GLES 2.0 UI \
**[Qt](https://www.qt.io/)** — the toolkit mixdash is painted with, on a framebuffer, with no window system under it \
**The SDL, busybox, doomgeneric and Freedoom** projects \
[ChatGPT](https://chatgpt.com/) for guidance on how to build a Debian image \
Jetup13 for many themes \
dani7959 for the replica theme \
pix33l for the pixui theme \
TheGreatCrippler for testing and feedback \
kloptops for testing and feedback \
Fraxinus88 for testing and feedback \
ImCoKeMaN for testing and feedback \
[PortMaster](https://portmaster.games/) team for support in figuring out PM interface issues
