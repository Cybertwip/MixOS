## Licenses
MixOS is a Linux distribution that is made up of many open-source components.  Components are provided under their respective licenses.  This distribution includes components licensed for non-commercial use only.

#### You are free to
* Share — copy and redistribute the material in any medium or format
* Adapt — remix, transform, and build upon the material

#### Under the following terms
* Attribution — You must give appropriate credit, provide a link to the license, and indicate if changes were made. You may do so in any reasonable manner, but not in any way that suggests the licensor endorses you or your use.
* ShareAlike — If you remix, transform, or build upon the material, you must distribute your contributions under the same license as the original.

### MixOS device support (MediaTek and R36 Ultra)
Copyright (c) 2025-present the MixOS project and contributors

The original MixOS work — `device/j36-ultra/`, `device/r36-ultra/`,
`device/common/` and the `build-j36-ultra.sh`, `build-j36-ultra-dtb.sh` and
`build-r36-ultra.sh` entry points — is licensed under the terms of the
[Microsoft Public License (Ms-PL)](https://opensource.org/license/ms-pl-html).
The full text, together with a file-by-file statement of exactly what it covers,
is in [`device/j36-ultra/LICENSE`](device/j36-ultra/LICENSE).

Two carve-outs matter, and they are not optional:

* The Linux kernel modules and kernel patches in `device/j36-ultra/linux/` —
  `j36_mt6592_input.c`, `j36_mt6592_audio.c`, `j36_jd9365_panel.c`,
  `0001-mtk-sd-mt6592.patch` and `0002-drm-mediatek-mt6592.patch` — are
  **GPL-2.0-only** and are not relicensed.  They derive from and link against
  GPL-2.0-only kernel internals, and Ms-PL is not GPL-compatible: its section
  3(D) adds a condition that GPLv2 section 6 forbids adding.
* `device/j36-ultra/es/Renderer_GLES20.cpp` is written to drop into
  EmulationStation's own source tree and follows **EmulationStation's** license.

Ms-PL also does not reach anything a build downloads or stages.  The Linux kernel,
Mesa, SDL, EmulationStation, busybox, doomgeneric, the Freedoom IWAD and the
Debian rootfs are each obtained under their own terms.  A finished card is an
aggregate of many licenses; Ms-PL applies to the MixOS scripts, probes and
documentation that assembled it.

`device/j36-ultra/mvii-board/` holds five verbatim MediaTek/MVII board headers and
driver sources, redistributed unmodified so the device tree generator can be seen
to parse real vendor source.  They are inputs, not MixOS work, and they carry
whatever terms their authors set; `mvii-board/PROVENANCE.txt` records a SHA-256
for each.

### dArkOS Software
Copyright 2021-present dArkOS

Original software and scripts developed by the dArkOS team are licensed under the terms of the [MIT License](https://choosealicense.com/licenses/mit/).

MixOS is a **divergent fork** of dArkOS.  Everything in this repository other than
the MixOS work listed above came from dArkOS and keeps dArkOS's
copyright and MIT license — including the files MixOS has modified: the
modifications are MixOS's, the files remain dArkOS's work under dArkOS's terms.
dArkOS is in turn a Debian-based continuation of
[ArkOS](https://github.com/christianhaitian/arkos/wiki) by christianhaitian, whose
layout, tools menu and EmulationStation integration this distribution still uses.

Neither dArkOS nor ArkOS endorses MixOS, is affiliated with it, or is responsible
for it, and neither should receive MixOS bug reports.

### Debian
The operating system underneath all of it.  A MixOS card boots a Debian rootfs,
assembled with Debian's own tools (`debootstrap`, `apt`, `dpkg`), and all but a
handful of the packages on the card are packaged and maintained by the Debian
project under their own licenses — see `/usr/share/doc/*/copyright` on a running
device for the per-package terms.

MixOS is a set of device ports and a build system on top of Debian; the operating
system itself is Debian's work.  MixOS is not affiliated with, endorsed by, or
sponsored by the Debian project, and "Debian" is a registered trademark of
Software in the Public Interest, Inc.  https://www.debian.org/
