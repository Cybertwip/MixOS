## Licenses

MixOS is a Linux distribution assembled from many open-source components. Each
component is provided under its own licence, and a finished card is an aggregate of
all of them. This file says which is which.

The non-commercial components that were once part of this distribution have been
removed. Nothing MixOS builds or ships is restricted to non-commercial use, and the
MixOS work itself is explicitly licensed for commercial use — see below.

---

### The MixOS work — dual-licensed, MPL-2.0 or GPL-2.0-or-later

Copyright (c) 2025-present the MixOS project and contributors

```
SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
```

The original MixOS work — `device/j36-ultra/`, `device/r36-ultra/`,
`device/common/` and the `build-j36-ultra.sh`, `build-j36-ultra-dtb.sh` and
`build-r36-ultra.sh` entry points — may be used under **either** of:

* the [Mozilla Public License, version 2.0](https://www.mozilla.org/MPL/2.0/), or
* the [GNU General Public License, version 2 or later](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html),

at your option. The full text and a file-by-file statement of exactly what is
covered is in [`device/j36-ultra/LICENSE`](device/j36-ultra/LICENSE).

**Why a dual licence.** Three requirements had to hold at once:

1. **Commercial use stays permitted.** Neither licence has a non-commercial clause.
   MixOS may be sold, shipped inside a product, and built into a device offered for
   money.
2. **The source stays under control.** MPL-2.0 is file-level copyleft: modify a
   MixOS file and the modified file must be published under MPL-2.0, so improvements
   return. MPL-2.0 §3.3 explicitly permits combining that file with proprietary code
   in a Larger Work, so a vendor's own application is not conscripted by shipping it
   on the same card or in the same binary.
3. **It has to be combinable with Linux.** This is what forced the change away from
   the Microsoft Public License. Ms-PL is not GPL-compatible — its §3(D) imposes a
   condition GPLv2 §6 forbids adding — so MixOS code could never be moved into,
   linked with or derived from the kernel it runs on. MPL-2.0 is built for exactly
   this case: §1.12 and §3.3 define GPL-family licences as *Secondary Licenses*, so
   MPL-2.0 code may be relicensed under the GPL when combined with GPL code, and
   offering GPL-2.0-or-later alongside it removes any remaining ambiguity.

**Superseded licence.** This work was previously offered under the Microsoft Public
License. Copies obtained under Ms-PL remain under Ms-PL; from this revision forward
the terms are the dual MPL-2.0 / GPL-2.0-or-later grant above.

---

### The GPL MixOS inherits, and cannot license around

Two of the things MixOS is made of are GPL, and that reaches every card.

#### The Linux kernel — GPL-2.0-only, not relicensed

Everything in `device/j36-ultra/linux/` is a Linux kernel module or a Linux kernel
patch:

* `j36_mt6592_input.c` — keypad matrix, GPIO keys and the analog sticks
* `j36_mt6592_audio.c` — the MT6592 AFE ASoC card
* `j36_mt6592_usb_phy.c` — the USB PHY and its VBUS/charger interlock
* `j36_mt6592_pmic.c` — battery gauge, charger and poweroff
* `j36_mt6592_backlight.c` — the BLS block behind the panel
* `j36_mt6592_wifi.c` — the CONSYS connectivity MCU bring-up
* `j36_jd9365_panel.c` — the JD9365 panel that adopts the bootloader's state
* `0001-mtk-sd-mt6592.patch`, `0002-drm-mediatek-mt6592.patch`

These derive from and link against GPL-2.0-only kernel internals. They carry
`SPDX-License-Identifier: GPL-2.0-only` and `MODULE_LICENSE("GPL v2")`, they are
**not** part of the dual grant above, and they are **not** relicensable — not to
MPL, not to anything else.

Distributing a device that runs this kernel is distributing GPL-2.0 software, and
the recipients are owed the corresponding source: this repository, plus the upstream
Linux tree and version that `device/j36-ultra/build-in-vm.sh` names.

#### Debian — the operating system underneath

A MixOS card boots a Debian rootfs, assembled with Debian's own tools
(`debootstrap`, `apt`, `dpkg`). All but a handful of the packages on the card are
packaged and maintained by the Debian project under their own licences — GPL, LGPL,
MIT, BSD, Apache and others — recorded per package in `/usr/share/doc/*/copyright`
on a running device. Debian's GPL packages carry Debian's GPL obligations, unchanged,
onto every card MixOS produces. MixOS does not and cannot relicense any of it.

MixOS is a set of device ports and a build system on top of Debian; the operating
system itself is Debian's work. MixOS is not affiliated with, endorsed by, or
sponsored by the Debian project, and "Debian" is a registered trademark of Software
in the Public Interest, Inc. <https://www.debian.org/>

#### Everything else a build downloads or stages

The dual grant reaches none of it. Mesa (MIT), Qt and its runtime closure (LGPL-3
with Qt's exceptions), SDL, busybox, doomgeneric and the Freedoom IWAD (GPL, as
Debian and doomgeneric ship them) are each obtained under their own terms. The dual
licence applies to the MixOS scripts, drivers, probes, dashboard and documentation
that assembled the card — not to the card's contents as a whole.

---

### The build scripts — dArkOS, MIT

Copyright 2021-present dArkOS

Everything in this repository other than the MixOS work listed above came from
**dArkOS** and keeps dArkOS's copyright and
[MIT licence](https://choosealicense.com/licenses/mit/) — including the files MixOS
has modified: the modifications are MixOS's, the files remain dArkOS's work under
dArkOS's terms.

In practice that is now the build scripts and nothing else: the inherited
image-assembly pipeline, the `Makefile` targets and the Rockchip tooling. Every
runtime component that used to come from dArkOS or ArkOS has been replaced or
removed.

dArkOS is in turn a Debian-based continuation of
[ArkOS](https://github.com/christianhaitian/arkos/wiki) by christianhaitian.
Neither dArkOS nor ArkOS endorses MixOS, is affiliated with it, or is responsible
for it, and neither should receive MixOS bug reports.

---

### Vendor sources

`device/j36-ultra/mvii-board/` holds verbatim MediaTek/MVII board headers and driver
sources, redistributed unmodified so the device tree generator can be seen to parse
real vendor source. They are inputs, not MixOS work, and they carry whatever terms
their authors set; `mvii-board/PROVENANCE.txt` records a SHA-256 for each.

---

### Trademarks

"MixOS" is a backronym joke about how software gets named; see the README. MixOS is
not affiliated with, endorsed by or sponsored by Microsoft Corporation, X Corp.,
xAI, MediaTek Inc., Rockchip, the Debian project or the Qt Company, and every
trademark named anywhere in this repository belongs to its respective owner.
