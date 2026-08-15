# J36 Ultra MT6592 bring-up DTB

MediaTek MT6592 (Cortex-A7, ARMv7, Mali-450 MP4) support for **MixOS**.  This
directory is dual-licensed, **MPL-2.0 or GPL-2.0-or-later** at your option — see
[LICENSE](LICENSE), which also records file by file the parts that are
`GPL-2.0-only` and why they are not relicensed.  Attribution and thanks are at the
bottom of this file and in [../../LICENSES.md](../../LICENSES.md).

Build from the repository root:

```sh
./build-j36-ultra-dtb.sh
```

Nothing outside this checkout is required. The five MVII board files the
generator parses live here, and they are the only ones it opens:

```text
device/j36-ultra/mvii-board/mt6592_board_j36.h
device/j36-ultra/mvii-board/mt6592_disp_hw.h
device/j36-ultra/mvii-board/panel_bringup.h
device/j36-ultra/mvii-board/dsi_drv.c
device/j36-ultra/mvii-board/mt6592_keys.c
device/j36-ultra/mvii-board/PROVENANCE.txt
```

That is 244 KB out of a 2.9 MB, 113-file driver tree. From them the generator
extracts the board constants, the exact compact 155-record JD9365 table, and the
keypad pad mux, and asserts on all three.

`PROVENANCE.txt` records the commit these were taken from and a SHA-256 per file,
as the redistribution record for vendor material. It is a record of an import,
not a link to a live tree: these five files are maintained here now, and there is
no refresh step. A script used to copy them in from a sibling PowerEngine
checkout and `build-j36-ultra.sh` used to warn when the two had drifted; both are
gone, because that made this build's output depend on what happened to be in a
neighbouring directory of a separate project.

The values could have been frozen into a JSON instead, which would be smaller
again, but that moves the numbers one copy further from the code that drives the
hardware. The generator's assertions are what turn an MVII pad-mux change into a
build failure here rather than seven dead keys on the device, and they only mean
something while they are reading real driver source.

Outputs — three files, and **not inside this directory**:

```text
$J36_DTB_OUT_DIR/mt6592-j36-ultra.dts
$J36_DTB_OUT_DIR/mt6592-j36-ultra.dtb
$J36_DTB_OUT_DIR/mt6592-j36-ultra.roundtrip.dts
```

`build-in-vm.sh` sets `J36_DTB_OUT_DIR` to `$J36_WORK_DIR/dtb` inside the VM, and
that is the copy the image is built from; it runs the generator first thing, before
the kernel is cloned, so a pad-mux regression costs a second. Run
`./build-j36-ultra-dtb.sh` by hand with nothing set and the three land in `build/`,
which is gitignored. They used to default into `device/j36-ultra/generated/`, and the
DTB that ended up on the card was never that one — it was the VM's.

This is a **bring-up hardware description**, not a complete Linux port. It
contains the panel timing/init records, power sequence, framebuffer handoff,
GPIO keys, keypad matrix, and AUXADC joystick wiring. Linux still needs the
small MT6592 adapter drivers named by the `j36,*` compatible strings before the
DTB can execute panel initialization or report input events.

## Boot-chain integration

The stock J36 Ultra Android `boot.img` format does not carry a separate DTB.
For the first Linux bring-up, keep the stock LK and build the ARM kernel with:

```text
CONFIG_OF=y
CONFIG_ARM_APPENDED_DTB=y
CONFIG_ARM_ATAG_DTB_COMPAT=y
CONFIG_ARM_ATAG_DTB_COMPAT_CMDLINE_FROM_BOOTLOADER=y
```

Then append this DTB to the ARM `zImage` before wrapping it as the MediaTek
`boot.img` kernel payload:

```sh
cat arch/arm/boot/zImage \
    "$J36_DTB_OUT_DIR/mt6592-j36-ultra.dtb" \
    > zImage-j36-ultra
```

That preserves the stock LK/ATAG handoff while the kernel discovers the new
board description. A later custom LK can instead pass the DTB address in ARM
register `r2`.

## Minimum-driver strategy

For the first visible Linux boot, preserve the stock LK display state and use
the `simple-framebuffer` node at `0x82700000`; do not reset DSI/MMSYS yet. The
full JD9365 program remains in the DTB for the later native panel adapter.

The first input layer is implemented by `linux/j36_mt6592_input.c` as one
minimal polled platform driver. It consumes the DTB's
`j36,j36-ultra-input` node and covers:

1. GPIO DIN for the D-pad, stick clicks, and special key, with a pull-up armed so
   "active low" has a defined idle to be low against.
2. KPD scan memory for face/shoulder/start/select/menu/volume keys.
3. AUXADC channels 15, 14, 12, and 13 for both analog sticks.

It also does the two things without which the matrix half of that list is dead:
it muxes the three keypad pads the boot chain leaves parked (KPROW3 pad 11 at
mode 3, KPCOL3 pad 12 at mode 3, KPCOL4 pad 2 at mode 6, from the DT keypad
node), and it ungates the keypad's 32 kHz clock in the MT6323 PMIC over PWRAP.
Both failures look identical from the SoC side -- scan memories reading their
idle all-ones pattern -- so both were chased in the keymap first. Pad mux writes
are confined to a pad whose mode is measurably wrong, and every one is logged
with its before and after. See `linux/README.md`.

It touches no MMSYS, DSI, MIPI-TX, panel, or backlight register.

## 32-bit, and where that is enforced

An MT6592 is ARMv7. An RK3326 is arm64. The two boot chains in this repository
now share an SD card, so neither architecture is left to memory:

| | R36 Ultra (RK3326) | J36 Ultra (MT6592) |
| --- | --- | --- |
| kernel | arm64 `Image` | ARMv7 `zImage` |
| toolchain | `aarch64-linux-gnu-` | `arm-linux-gnueabihf-` |
| userspace | armhf (32-bit) | armhf (32-bit), shared |

The rootfs is shared; the kernel never is. `build-r36-ultra.sh` asserts both
halves — `verify_native_userspace` for the armhf rootfs and
`verify_boot_kernel_arch` for the arm64 boot magic in its own kernel. `device/j36-ultra/build-in-vm.sh` runs the MVII LK's
own test on its own artifact: `readelf` for ELF32/ARM, then the arm64 magic at
offset 0x38 and the zImage magic at 0x24, on the exact bytes the LK will read.
That check is the LK's `sd_kernel_is_armv7()`, moved to the build machine so "no"
is a build failure instead of a board that quietly fell back to the eMMC.

## Two kernel payloads, and they are not interchangeable

The **eMMC BOOTIMG** path is entered the way the stock LK enters a kernel: an
ATAG list in `r2` and no device tree anywhere. That payload
(`zImage-j36-ultra`) therefore carries its tree **appended**, and
`CONFIG_ARM_ATAG_DTB_COMPAT` folds the ATAGs into it.

The **SD hand-off** path passes the tree itself in `r2` and patches `/chosen`
first: `bootargs`, `linux,initrd-start`, `linux,initrd-end`. An appended tree
would silently win that argument — `arch/arm/boot/compressed/head.S` takes the
appended DTB whenever its magic is present and consults `r2` only to fold ATAGs
in, so a DTB in `r2` is discarded along with the initramfs range, and the kernel
boots and then cannot find `/init`. The SD payload is therefore the **plain**
`zImage` with the tree beside it as a separate file.

## Incremental build

There are two things to build, and they are two commands.

```sh
./build-j36-ultra.sh --mix-only     # the board specifics.  This is the iteration loop.
./build-j36-ultra.sh                # the finished card: one flashable image.
```

This is an extension of `build-r36-ultra.sh` rather than a second build system.
It regenerates the DTB in the VM first — so a keymap or pad-mux regression fails
before the kernel is even cloned — then, unless `--mix-only` was given, resumes
the checkpointed R36 base build (already finished ones cost seconds;
`J36_RESUME_R36=0` skips it), then builds the J36 layer in the same `darkos-r36`
VM. The first J36 run creates a persistent ARMv7 Linux 6.12 LTS workspace; later
runs rebuild only changed kernel, DTB, input-module, initramfs and `boot.img`
files.

**The full build ships one file**, and it is not in this directory:

```text
../MixOS-Artifacts/MixOS_armhf_trixie_<commit>.img
```

Both payloads are already folded into it — the launcher into the vfat `BOOT`
partition and `/opt/mixos` into the ext2 OS partition — so flashing is one `dd`
and nothing else. That is not a convenience: the workstation here is macOS, which
mounts FAT and exFAT and neither ext2 nor btrfs, so "untar this onto the OS
partition" is a step that cannot be performed at all from the machine doing the
flashing.

**`--mix-only` ships two directories**, and nothing else — no `boot.img` copy, no
bare `zImage`, no `.cpio.xz`, no checksums. Those are intermediates that the image
already contains, and having them sit next to a flashable image is how a stale one
gets picked up:

```text
../MixOS-Artifacts/j36-ultra/boot/   -> the card's BOOT partition (vfat)
../MixOS-Artifacts/j36-ultra/root/   -> the card's OS partition (ext2)
```

`--mix-only` builds no base image and touches none, which is the whole point of it:
folding a 30 MB payload into an 8 GB image costs minutes of `losetup`, `e2fsck`,
`resize2fs` and `tar` for a change to one module. Copy `boot/` onto the card from
macOS and the next boot has the new payload — `sd-root.tar.gz` rides along in it and
`/init` unpacks it onto the OS partition, once per tarball.

## The SD BOOT payload

`boot/` above, and `sd-boot/` inside the build. Copy it into the root of the FAT
partition labelled `BOOT` and the MVII LK boots the card instead of the eMMC.
Nothing already there is disturbed: an R36S card keeps its `Image`, `uInitrd`,
rk3326 trees and `boot.ini`.

```text
zImage                 plain ARMv7 kernel, no appended tree
mt6592-j36-ultra.dtb   the tree the LK loads separately and patches
initrd.img             bring-up initramfs (busybox + the input module)
mvii/boot.conf         filenames and command line for the MVII LK
j36/mfgpower           powers the Mali-450 and reads its ID back (j36.lima=1)
j36/modules/           lima.ko and its dependencies, plus load.order
j36/mtkdrm/            the MT6592 display set, plus load.order (j36.mtkdrm=1)
j36/audio/             the ALSA core and the MT6592 AFE, plus load.order (j36.audio=1)
j36/usb/               the MUSB host stack, HID, udl and the disk set (j36.usb=1)
j36/power/             the MT6592 PMIC and the panel backlight (j36.power=1)
j36/wifi/              cfg80211, rfkill and the CONSYS driver, plus firmware/
                       holding the two ROM patches and the WLAN firmware the
                       driver downloads to bring up wlan0 (j36.wifi=1)
j36/gl/                Mesa's GL front end, plus links (vfat has no symlinks)
j36/eglprobe           what can create a GL context, and with -p whether a frame
                       reaches the glass: five held colours, CPU then lima
LICENSE.txt            which licence covers which file above, and where the
                       GPL-2.0-only source is
```

Everything under `j36/` is read by `/init` and by nothing else — no LK load
window, no size limit, no partition table entry. Deleting the directory, or the
matching word from `bootargs`, restores the previous boot exactly, from any
machine that can read an SD card and with no reflash. `/init` says on the panel
what it found and carries on either way.

`mvii/boot.conf` exists because an R36S card already carries a `boot.ini`, and
that `boot.ini` names the arm64 `Image` and an rk3326 tree. The LK parses
`boot.ini` first and `/mvii/boot.conf` second precisely so this file gets the
last word; without it the LK would load the arm64 kernel, refuse it at the magic
check, and fall back to the eMMC. Load addresses are deliberately absent from
it: those are the LK's business, and it knows this SoC's DRAM map and the
address of the framebuffer the DTB hands to `simple-framebuffer`.

## Storage: mounting the shared armhf rootfs

The microSD host is **MSDC1 at `0x11240000`** — the base the MVII LK's own
`mt6592_msdc_sd.c` programs to read the card this kernel is loaded from, with
MSDC0 at `0x11230000` being the eMMC. `mmc@11240000` claims it with
`mediatek,mt6592-mmc`, and no eMMC node is described, so the card is the only MMC
host and its partitions land on `mmcblk0`.

Three things had to be true before that worked, and none of them are guesses:

- **The interrupt is GIC_SPI 72, level-low.** A `struct resource` array in the
  stock kernel's `.data` (virt `0xc0b33050`–`0xc0b331c4`) gives MSDC0/1/3 →
  103/104/105 and UART0..3 → 115..118. Those are GIC **interrupt IDs**, not SPI
  indices: `mt_irq_mask` writes `GICD_ICENABLER + (irq>>5)*4` and
  `mt_irq_set_sens` writes `GICD_ICFGR + (irq>>4)*4`, both applying the number
  raw, and the GIC fixes those indices at INTID/32 and INTID/16. So SPI =
  INTID − 32, and 104 → 72.

  The proof is on this board, not in the derivation: the stock kernel's
  `irqaction` for `gpt_handler` (virt `0xc0b33380`) reads `irq=176`,
  `IRQF_TRIGGER_LOW`, `name="mt6592-gpt"`, and this tree already declares that
  timer as `GIC_SPI 144` — 176 − 32 — with no arch-timer node behind it, so
  nothing would ever schedule if that number were wrong.

  This said SPI 40 for one boot, from taking mainline `mt6592.dtsi`'s
  `GIC_SPI 51..54` for those UARTs at face value and inferring −64. Do not
  derive these from that file. A wrong UART SPI is invisible — 8250 console
  writes poll `THRE` and never use the interrupt — so the error only surfaced
  when the same arithmetic was applied to a device that needs its IRQ.

  The node's parent is `sysirq`, at `0x10200220` on this SoC and not the
  `MCUCFG+0x620` later MediaTek parts use: `mt_irq_set_polarity`
  (`0xc0325148`) computes `0xf0200220 + ((irq-32)>>5)*4`, bit `irq&31`. Its
  linear bit is INTID − 32, which is exactly the bit mainline's `mtk-sysirq`
  derives from `hwirq`, and its seven words cover SPI 0–223 for INTIDs 32–255
  with none left over — a third agreement with −32.
- **`mtk-sd` needed a compatible.** MT6592 is a 12-bit-divider part: the LK writes
  CKDIV into `MSDC_CFG[19:8]` and CKMOD into `[21:20]`, which is `clk_div_bits ==
  12`, not the 8-bit mt8135 layout. No in-tree entry pairs a 12-bit divider with
  this generation's `MSDC_PAD_TUNE` and no async FIFO, so `linux/0001-mtk-sd-mt6592.patch`
  adds `mt6592_compat` rather than borrowing mt2701's. The same patch makes
  pinctrl optional, because probe otherwise refuses a SoC that has no pinctrl
  driver at all — and the pads are already muxed by the bootloader.
- **`vmmc-supply` is mandatory, not decoration.** `mmc->ocr_avail` is assigned in
  exactly one place in the whole MMC core — `drivers/mmc/core/regulator.c`, from
  the vmmc regulator — and `mtk-sd` never sets it. Without a fixed regulator
  behind `vmmc-supply` the host advertises no voltage and card init fails with
  nothing in the log pointing at why.

`CONFIG_BTRFS_FS` is built in because that is what the shared rootfs is:
MixOS's own `scripts/setup_partition.sh` sets `ROOT_FILESYSTEM_FORMAT="btrfs"`.
`CONFIG_EXT4_FS` too, because a hand-made card usually is not.

## What PID 1 needs, and why the size prune took it away

With the card mounting and `switch_root` succeeding, systemd 257 aborted at 15 s:

```
systemd[1]: Failed to find module 'unix'
systemd[1]: Failed to open netlink, ignoring: Function not implemented
systemd[1]: Failed to allocate device monitor: Function not implemented
systemd[1]: Failed to allocate notification socket: Function not implemented
systemd[1]: Assertion '...' failed at src/core/device.c:64, function
            device_unset_sysfs(). Aborting.
systemd[1]: Freezing execution.
```

`ENOSYS` from `socket(2)`, three times. `CONFIG_NET` was in the size-prune
disable list while `CONFIG_UNIX` was in the enable list — and `UNIX` lives under
`if NET`, so `olddefconfig` dropped it, together with `INET`, `PACKET`,
`POSIX_MQUEUE` and `SECCOMP_FILTER` (which depends on `NET` as well as
`HAVE_ARCH_SECCOMP_FILTER`). With no `AF_UNIX` notification socket and no
`AF_NETLINK` uevent monitor, systemd's `.device` units never get a sysfs path and
the assertion that they have one kills PID 1. Nothing was wrong with the storage
or the hand-off; the kernel had no sockets.

`CONFIG_NAMESPACES` was `=n` for the same reason and was found by auditing the
shipped `kernel.config` instead of by another boot — it also removes
`NET_NS`/`PID_NS`/`IPC_NS`/`UTS_NS`, and Debian's units use `PrivateMounts`,
`PrivateTmp` and `ProtectSystem`.

The fix that matters is not `NET=y`, it is that **everything PID 1 cannot start
without is now asserted after `olddefconfig`**, including the symbols that arrive
by dependency. `UNIX` had been requested all along; what was missing was any
check that the request survived. `BT` and `MAC80211` are asserted *off* for the
mirror-image reason — this image has 2.5 MiB of slack in a fixed 9 MiB partition,
and neither is reachable from anything on the card. `MAC80211` in particular is a
refusal and not an omission; see the Wi-Fi section below.

`root=` on the command line is a **hint that `/init` verifies**, not an order to
the kernel: `rdinit=/init` keeps the kernel out of root mounting entirely, so a
`root=` that turns out to be wrong can no longer panic it. `/init` mounts each
candidate read-only, looks for `/sbin/init`, and only then remounts it writable
and `switch_root`s in. Failing that it scans the other `mmcblk` partitions, and
failing *that* it prints `/proc/partitions` and gives you a shell. Delete `root=`
from `mvii/boot.conf` and the card stops at the initramfs exactly as it used to.

## Where the shell appears, and why the boot went quiet

`/dev/console` is whichever `console=` came **last** on the command line. It used
to be `console=tty0 console=ttyS0,115200n8` — the UART — so a bare
`exec setsid cttyhack sh` put the prompt on a serial port that may have nothing
plugged into it while the panel showed only a blinking cursor. `/init` works
around that by echoing progress to both, starting a background shell on
`/dev/ttyS0`, and exec'ing the interactive one on `/dev/tty1` explicitly.

Userspace has no such workaround, and it showed. A boot that had in fact
succeeded looked like this on the panel:

```
[   26.120898] systemd-journald[146]: Received client request to flush runtime journal.
[   27.951283] random: crng init done
```

…and nothing more, for minutes. That is not a hang. systemd's log target is
*journal-or-kmsg*: until journald is up it writes to `/dev/kmsg`, which the
kernel echoes to every console including the framebuffer, and once journald takes
over it stops. Everything after 26 s went to the journal and to `/dev/console`,
which was the UART. The last kernel line on the panel is simply the last thing
the *kernel* said.

So `mvii/boot.conf` now puts `console=tty0` last and adds
`systemd.journald.forward_to_console=1`. Both consoles still receive every
`printk` — only `/dev/console` moved — and the service log follows it onto the
panel. Drop the forward once there is a shell or a network to read the journal
with; it costs a framebuffer redraw per line.

## Rebooting, which the kernel could not do

A clean shutdown that ends in a halt:

```
systemd-shutdown[1]: All filesystems unmounted.
systemd-shutdown[1]: Rebooting.
reboot: Restarting system
Reboot failed -- System halted
```

`machine_restart()` had no handler to call. On MediaTek the reset is the TOPRGU
watchdog, and `watchdog_core.c` registers a restart handler for any watchdog whose
ops provide `.restart` — which `mtk_wdt` does. `CONFIG_MEDIATEK_WATCHDOG` was
already `=y`, inherited from `multi_v7_defconfig` under `ARCH_MEDIATEK`: the driver
was built in the whole time and had nothing to probe, because the device tree
described no watchdog. The node is a three-liner:

```
watchdog: watchdog@10007000 {
        compatible = "mediatek,mt6589-wdt";
        reg = <0x10007000 0x100>;
};
```

`0x10007000` comes from three sources that agree — the mtkclient chip table
(`watchdog=0x10007000` for MT6592), its stage1 payload for this family, which
reboots by writing `RESTART 0x1971`, `MODE 0x22000014` and `SWRST 0x1209`, and
PowerEngine's own bare-metal `Standalone/src/hello.c`. `mtk_wdt_restart()` writes
that same `SWRST` key at offset `0x14`. `mediatek,mt6589-wdt` is used alone, with
no invented `mt6592` string in front of it: `mt6589-wdt` is the plain variant of
this block in the driver's OF table (no reset controller, no clock), and a leading
compatible that matches nothing only risks the node not binding. No interrupt is
needed, and probe calls `mtk_wdt_stop()`, so the hardware timer stays disarmed —
nothing in the MixOS rootfs opens `/dev/watchdog`, and the board already ran for
minutes with no node at all.

This is not only about a user pressing reboot. MixOS expands its partitions in
two stages *with a reboot between them*, so a reboot that halts turns first boot
into a sequence of power cycles by hand. `poweroff` still ends in a halt, because
nothing drives the PMIC yet.

## The R36S first boot, on a card that has no tars

`firstboot.service` is masked from the command line
(`systemd.mask=firstboot.service`). The script is `scripts/expandtoexfat.sh.rk3326`
and this configuration cannot finish it: it grows ROOTFS, converts EASYROMS to
exfat, and then untars `/roms.tar` and `/tempthemes`, which a GUI-mode build does
not ship. With the tars missing, `pv` fails, its two progress loops never see
`100`, and each spins 15 000 iterations — a `tail` subshell apiece — before giving
up. That is the several minutes of dead panel, twice, ending in the reboot that
halted.

Masking is a bring-up decision, not a fix: delete the `systemd.mask=` word to let
it run on a card that does carry the tars. What the kernel side needed either way
is `EXFAT_FS` and `VFAT_FS`, because the fstab the script installs as its last act
is

```
LABEL=BOOT /boot vfat defaults 0 2
LABEL=EASYROMS /roms exfat defaults,auto,umask=000,uid=1000,gid=1000,noatime 0 0
```

Neither entry carries `nofail`. A filesystem this kernel does not know is
therefore not a missing ROMs directory — `local-fs.target` fails, and systemd
takes a machine with no usable keyboard into emergency mode. Both are now built
in and asserted, along with `NLS_UTF8` and `NLS_CODEPAGE_437`, the charsets those
two mounts imply.

## The one RK3326 daemon that never gives up

With the log forwarded to the panel, the first thing it showed was this, on a
loop:

```
batt_life_warning.py[829]: FileNotFoundError: [Errno 2] No such file or
  directory: '/sys/class/power_supply/battery/capacity'
batt_led.service: Main process exited, code=exited, status=1/FAILURE
batt_led.service: Scheduled restart job, restart counter is at 21.
```

`batt_led.service` reads that capacity file and writes
`/sys/class/gpio/gpio77/value`, and this kernel had neither — no `power_supply`
driver for the MT6592 PMIC, and no sysfs GPIO export. What made it worth a
command-line word is the unit, not the script: `Restart=always`, `RestartSec=2`
and `StartLimitIntervalSec=0`, which is systemd's way of spelling *never give up*.
It was a traceback on the console every 2.3 seconds for as long as the board was
on — and on a board whose console *is* the panel, that is the splash screen being
overdrawn with a traceback forever. Fitting a cell does not help: the file was
missing because the driver was missing, not because the bay is empty.

Both ends are fixed now. `j36.power=1` loads `j36_mt6592_pmic.ko`, which
registers a `power_supply`, and `batt_life_warning.py` was rewritten to assume no
path exists and to treat nothing as fatal. It finds the gauge by walking
`/sys/class/power_supply` and taking the first supply whose `type` reads
`Battery` — by type rather than by the RK3326 name, and skipping a charger that
happens to publish a percentage. The LED is looked for at `gpio77` first and then
in `/sys/class/leds/*/brightness`, and **not finding one is a normal answer**: the
old loop read the LED on every branch including *battery is fine*, which is why a
board with a working gauge still crashed on the first pass. Missing gauge means
the driver has not come up yet, so it waits; missing LED means there is nothing to
blink, so it keeps the percentage in the journal. It never exits, so `Restart=always`
never fires. It is left unmasked deliberately — the percentage it prints is the
cheapest standing check that the `power_supply` registration survived a kernel bump.

That script is in the Debian rootfs, not the J36 payload: `finishing_touches.sh`
copies `device/rg351mp/*.py` into `/usr/local/bin` and enables the unit. A card
written before this change still carries the crashing copy, and `--mix-only` does
not replace it — a full `./build-j36-ultra.sh` does, the finalization stage running
on a resumed build too.

The other RK3326-only units are deliberately left running. `351mp.service` (power
LED, backlight, `amixer`), `audiopath`, `audiostate` and `wifi_importer` are all
`Type=oneshot`: they fail once and stay failed. Bounded noise is evidence; only
the unbounded one had to go.

## What used to cost ten seconds of every boot

The `backlight` node is `status = "disabled"`. It is a `pwm-backlight` consuming
`&disp_pwm` and `&gpio`, and neither has a driver in this profile. An *enabled* DT
consumer whose provider has no driver does not fail — it defers, and keeps
deferring until `driver_deferred_probe_timeout` expires. That was the whole gap
between 0.87 s and 11.38 s, plus an error on the panel.

## What a game proved, and where the rule it left behind came from

There used to be a framebuffer Doom here. It was staged on the BOOT partition and
`/init` ran it on `j36.doom=1`, and it existed because booting proves the machine
runs but does not prove a *program* can drive this display or read this gamepad —
32-bit pixels into `/dev/fb0` and events out of `/dev/input/event0` was the
cheapest honest answer to that, needing no DRM, no GL and no rootfs. Nothing
already on the card could ask the question: SDL2's video backends are KMSDRM,
X11, Wayland, offscreen and dummy, with **no fbdev backend**, so the `gzdoom` and
`lzdoom` in the shared rootfs both need DRM/KMS or a GL stack.

**It is gone.** MixOS ships a base operating system: nothing is cloned, compiled,
downloaded or staged for it, no IWAD reaches the card, and `j36.doom=1` is now an
unrecognised word. The dashboard's Packages page installs Debian's own —
`chocolate-doom`, `prboom-plus` and `freedoom` are in the Games collection there —
which is where a game on this device comes from now.

Two things it left behind are still load-bearing. `mixsplash` and `mixdash` hold
`/dev/tty0` in `KD_GRAPHICS` for exactly the reason it did: a full-screen
framebuffer program that lets go of the mode leaves the panel frozen on its last
frame, or lets kernel messages paint over it. And a 26 MiB IWAD was the first
thing that obviously did not belong on a 100 MB vfat launcher partition shared
with an R36S card's own boot files — the initramfs goes into `boot.img`, which is
asserted against the fixed 9 MiB BOOTIMG slot, so userland software goes to
`/opt/mixos` on the ext2 OS partition. Everything staged since obeys that rule.

## The Mali-450, and why a userspace helper runs before the driver

MT6592 carries a **Mali-450 MP4**, which DRM `lima` drives. The register map is
not inferred from a datasheet; it is read out of the stock kernel's own
`struct resource` array in `.data`, which names all eighteen blocks:

| block | base | INTID | `GIC_SPI` |
| --- | --- | --- | --- |
| GP | `0x13040000` | 234 | 202 |
| GP MMU | `0x13043000` | 235 | 203 |
| PP0 / PP0 MMU | `0x13048000` / `0x13044000` | 236 / 237 | 204 / 205 |
| PP1 / PP1 MMU | `0x1304a000` / `0x13045000` | 238 / 239 | 206 / 207 |
| PP2 / PP2 MMU | `0x1304c000` / `0x13046000` | 240 / 241 | 208 / 209 |
| PP3 / PP3 MMU | `0x1304e000` / `0x13047000` | 242 / 243 | 210 / 211 |
| L2 #1 / L2 #0 | `0x13041000` / `0x13050000` | — | — |
| DMA / Broadcast / DLBU | `0x13052000` / `0x13053000` / `0x13054000` | — | — |
| PP MMU bcast / PP bcast | `0x13055000` / `0x13056000` | — / 244 | — / 212 |

Every offset from `0x13040000` matches lima's own mali450 column in
`lima_device.c` block for block, and `0x13040000` is also the `MALI_BASE` the MVII
LK's bare-metal Utgard driver uses on this board. Three independent sources, one
map. The `GIC_SPI` column is `INTID − 32`, the same conversion MSDC1's 72 came
from, and the type cell is `IRQ_TYPE_LEVEL_LOW`.

The clocks are `fixed-clock`s at **0** Hz, and that is deliberate.
`lima_clk_init()` returns `devm_clk_get`'s error for a missing `"bus"` or
`"core"`, so the properties are mandatory; it then only `clk_prepare_enable`s them
— a no-op on a fixed-clock — and prints their rates. `lima_devfreq_init()` returns
0 immediately when `operating-points-v2` is absent. MT6592 has no clock driver and
nothing in this boot path programs a GPU rate, so a plausible-looking megahertz
number would be an invention. Zero is what lima prints and never uses.

### Why lima is `=m` and not built in

The MFG power domain is **gated when Linux starts**. The MVII LK has a proven
`mfg_power_on()` in `mt6592_gpu_offload.c`, but its only callers are in the MVII
kernel, not in the LK's hand-off to Linux. Reading an unpowered MTK subsystem does
not return garbage — it stalls the AXI bus, and the watchdog reboots the board with
nothing in any log. A built-in lima would do that during probe on *every* boot,
before the console, before the input driver, before anything this card is for.

So `CONFIG_DRM_LIMA=m`, and `tools/mfgpower.c` is the gate. It is a static ARMv7
`/dev/mem` helper that transcribes the LK's sequence register for register — set
`PWR_ON`, then `PWR_ON_S`, wait for both `SPM_PWR_STATUS` bits, clear
`PWR_CLK_DIS | PWR_ISO` and assert `PWR_RST_B`, clear `SRAM_PDN`, wait for
`MFG_SRAM_ACK` to fall, then open `DISP_CG_CLR0` bit 0 (SMI common) and
`MFG_CG_CLR` bit 0 — and then reads back `MALI_GP_VERSION` and the four
`MALI_PP_VERSION` registers, checking for products `0x0d07` and `0xcf07`. It
prints every value and exits non-zero unless a Mali-450 answered. `/init` runs it,
shows its log on the panel, and `insmod`s the modules in `j36/modules/load.order`
only on exit 0. That order is derived at build time from `modinfo -F depends`,
because the initramfs has `insmod` and not `modprobe` and resolves nothing itself.

`/dev/mem` is the right tool and not a shortcut: `phys_mem_access_prot` in
`drivers/char/mem.c` returns `pgprot_noncached` for an `O_SYNC` or non-RAM pfn, so
the window is an ordinary uncached device mapping, and `STRICT_DEVMEM` blocks RAM,
not MMIO. The MMU, reset and L2 half of the LK's driver is deliberately **not**
transcribed: lima resets those blocks itself during probe and wants the MMUs on
its own page tables, so anything done here would be work lima undoes.

The kernel configuration keeps `DRM=y` (lima's two tristate helpers, `DRM_SCHED`
and `DRM_GEM_SHMEM_HELPER`, follow lima to `=m`, so only the core has to be
resident) and prunes **every other** `DRM_*` symbol by enumeration. That prune
matters: `multi_v7_defconfig` turns on a dozen DRM drivers and one of them,
`DRM_SIMPLEDRM`, binds the same `simple-framebuffer` node `FB_SIMPLE` is driving —
and wins, by calling `drm_aperture_acquire_from_firmware()` and evicting the
working display. It is refused outright after `olddefconfig`, at either value.

### Why lima alone is not a GL stack

If lima loads you get `/dev/dri/renderD128` and **no** `card0`. That is not a
fault; lima is render-only, because a Mali-450 has no display controller. Four
links have to close before anything on this board can create a GL context, and
lima is one of them:

1. A KMS device. lima gives none — the panel is on `simplefb`, which is a
   framebuffer, not a DRM device — so `mediatek-drm` supplies `card0`.
2. SDL2's only viable backend here is KMSDRM, which needs that card node.
3. `simpledrm` could supply one, and is disabled for the reason above — and even
   enabled it would not help, because Mesa's kmsro/renderonly driver table has no
   `simpledrm` entry, so there would be no GBM and no EGL on top of it.
   `mediatek_dri.so` **is** in that table, which is why the display driver is
   `mediatek-drm` specifically.
4. A GL front end that is Mesa's. The shared rootfs points `libEGL.so` and
   `libgbm.so*` at the RK3326's Mali-G31 **Bifrost** blob, a different
   architecture from this Utgard part, so `j36.gl=1` stages Debian's armhf Mesa
   into `/run/j36/gl` ahead of it.

With all four closed, `j36/eglprobe` measures ES2 contexts current on an ARGB8888
window surface. The dashboard itself needs none of it: it is Qt on `linuxfb`.

## USB, and the register layout that decided it

There is one USB controller on this SoC and no companion: a Mentor MUSBMHDRC
dual-role core at `0x11200000` with its U2 PHY at `0x11210800`, and no EHCI, OHCI
or XHCI anywhere in the map. Everything a port can do here — a disk, a mouse, a
hub — goes through that one core in host mode. Both it and the PHY sit behind the
same PERI clock gate, and an APB access to a gated MediaTek peripheral does not
fault, it stalls the bus until the watchdog resets the board. That is why the PHY
is a separate driver (`j36_mt6592_usb_phy.c`) rather than three writes inside the
glue: it opens the gate in `.init`, which `musb_platform_init` calls before any
MUSB register is touched.

Everything above that was in place and the port still enumerated nothing —
no disk, no mouse, no hub, and no error either. Each of the obvious causes was
checked against the stock MT6592 Android kernel (3.10.72) and each came back
matching mainline:

| what was suspected | what the stock kernel says |
| --- | --- |
| wrong interrupt number | `mt_usb_init` writes 96 to `musb->nIrq`; INTID 96 is SPI 64, which is what the tree already said |
| no MediaTek L1 aggregator on this generation | it writes `0xf` to `L1INTM` at `mregs+0xa4` and its ISR ANDs `L1INTS` with it, identically to mainline |
| `MUSB_HSDMA_INTR` unmasked wrongly | `0xff0000ff` at `mregs+0x200`, the same value |
| FIFO RAM smaller than `MTK_MUSB_RAM_BITS 11` | 8192 bytes, exactly `1 << (11 + 2)` |
| fewer endpoints than `MTK_MUSB_MAX_EP_NUM 8` | endpoints 1..8, and a 16-entry FIFO table |

What is *not* the same is the register map. Mainline's
`drivers/usb/musb/mediatek.c` was written for MT2701/MT7623, which added a
TXTOG/RXTOG block at `0x80..0x87` and, to make room for it, relocated the
multipoint BUSCTL block to `0x480`. MT6592 is two generations older and has
neither: the stock kernel touches no address in `0x80..0x87` and no
`0x480 + 8 * epnum` address anywhere in its musb driver.

Getting that base wrong is silent and total. `musb_core` writes every device's
function address into `TXFUNCADDR`, so on the wrong base each `SET_ADDRESS` is
followed by traffic still aimed at address 0, every enumeration times out, and
nothing reports a fault because nothing faulted. That is the symptom, exactly.

`linux/0003-musb-mediatek-mt6592.patch` adds an `mt6592_musb_ops` variant
selected by a new `mediatek,mt6592-musb` compatible. It omits `.busctl_offset`,
`.get_toggle` and `.set_toggle`, which is not a loss of function: `musb_core`
substitutes `musb_default_busctl_offset()` — `0x80 + 8 * epnum + offset`, the
stock base — and the default software toggle handlers, which is how every MUSB
without MediaTek's toggle block has always worked. The patch fixes a second thing
while it is there: `mtk_musb_init()` wrote the toggle registers **before**
`phy_init()`, i.e. before the gate is open on this SoC, and that only survives
today because the stock LK happens to leave the USB gate on.

Both compatibles on the `usb@11200000` node are load-bearing. `mediatek,mtk-musb`
is what `mtk_musb_probe` matches; `mediatek,mt6592-musb` is what picks the
register layout.

The board confirms or refutes all of this in one line of boot log. The PHY driver
takes an optional `j36,musb-controller` phandle and, at `power_on` — after the
gate is open, before `musb_start()` — writes a different pattern into
`TXFUNCADDR` at each candidate base, reads both back and restores them. Whichever
base held its pattern is where the block decodes. It also dumps `DEVCTL`, `POWER`,
`FADDR`, `EPINFO`, `RAMINFO` and `L1INTM` then and again a few seconds later,
which separates the failure modes that otherwise look alike: `VBUS` below the
session-valid threshold means the 5 V never came up, `VBUS` at 3 with nothing else
set means the rail is fine and nothing is attached, and `HM` clear on a node that
asked for host means the role override did not take. `INTRUSB`, `INTRTX` and
`INTRRX` are deliberately not among them — they clear on read, and reading them
from here would swallow a connect interrupt before `musb_core` saw it.

Three things about the port itself, none of them software:

- **It is not the connector the board charges from.** A J36 Ultra has two: a DC
  inlet, which charges and carries no data lines at all, and this OTG port, which
  carries the data and holds its 5 V up the whole time the board is on. The
  PMIC's `CHRDET` hangs off the inlet and `DRVVBUS` hangs off the port; they are
  separate nets. Both drivers here were written believing there was one socket
  doing both jobs, which cost the console its charging — see `chrin_shared` in
  `j36_mt6592_pmic.c` and `vbus` in `j36_mt6592_usb_phy.c`, and pass
  `j36.usb=automeasure` plus `chrin_shared=1` on a board where the old belief is
  true.
- **The 5 V is a switch off VBAT, and VBAT on this PMIC is the system node.**
  `j36,drvvbus-pad = <15>` drives it, transcribed from the stock
  `mt_usb_set_vbus()`. A bus-powered hub on a board with no cell fitted is the
  same class of load that pulls VBAT under the undervoltage lockout. Use a
  self-powered hub, or pass `vbus=0` to the module, or fit a cell.
- **A USB-C→HDMI dongle will not work, and cannot.** The cheap ones are
  DisplayPort Alt Mode, which is a PHY-level capability MT6592 does not have at
  all. The only adapter that can ever put a picture on HDMI here is a genuine
  DisplayLink one, which is a USB device rather than a mode switch — `udl.ko` is
  already staged in `j36/usb/load.order` for it.

## Wi-Fi: four stages, and only the last one is 802.11

`j36.wifi=1` loads `j36_mt6592_wifi.ko` and ends in a `wlan0` that
NetworkManager and the dashboard's Wi-Fi page drive like any other interface.
Getting there is four stages, and each one is a different kind of hardware:

1. **Power.** MTCMOS for the CONSYS domain, then VCN18/VCN28/VCN33 on the MT6323
   over PWRAP, then the INFRA connectivity-MCU clock. Reading a CONSYS register
   before this is the same AXI stall the Mali section describes.
2. **The link.** BTIF is a UART-shaped mailbox to the connectivity MCU; STP is
   the framing on it and WMT the command set. Two ROM patches go down it, in the
   order their headers declare, and the MCU is muted and reset between them.
3. **The firmware.** `WIFI_RAM_CODE_SOC` is downloaded over the AHB HIF — not
   over BTIF — section by section, CRC checked, and the driver waits for
   `WLAN_READY`. The chip calibrates its RF during this and it is not quick.
4. **The radio.** Everything above is MediaTek's connectivity subsystem and
   would be identical for Bluetooth. Only here does 802.11 appear, and it appears
   as a *command and event protocol*, not as registers.

### It is fullmac, which decides the kernel configuration

The MAC lives in the firmware. The driver does not build frames, does not run a
retry counter, does not see an ACK; it sends `CMD_BSS_ACTIVATE`, `CMD_JOIN`,
`CMD_ADD_STA` and reads events back. So `j36_mt6592_wifi_net.c` registers a
`wiphy` and a netdev and never registers an `ieee80211_hw`.

That is why `CONFIG_CFG80211=m` is the whole wireless configuration and
`CONFIG_MAC80211` is asserted **off**. mac80211 is a softmac stack — a rate
control algorithm, a TX queueing discipline and a frame builder for parts whose
MAC is in the driver. On a fullmac part it is roughly 700 KB of code that
nothing calls, in a boot payload with 2.5 MiB of slack. `RFKILL=m` is asked for
alongside it — cfg80211 does not select rfkill, it `depends on RFKILL ||
!RFKILL`, and the half of that which keeps a real switch rather than the no-op
stubs is the half NetworkManager reads before it will bring a radio up.

The division of labour that follows from fullmac is worth stating, because it is
where the driver's boundaries are:

| | does |
| --- | --- |
| firmware | scanning, the probe request body, ACKs, retries, rate selection, CCMP encrypt/decrypt, power save |
| driver | the SME — channel lease, auth, assoc — plus key install, the netdev, and the event→cfg80211 translation |
| `wpa_supplicant` | the 4-way handshake, as EAPOL frames over `wlan0` and `NL80211_CMD_NEW_KEY` back down |

### Why it polls

The `wifi` node in the generated tree has **no `interrupts` property**, because
nothing in the sources this tree is derived from names one for the WLAN
function. So stage 4 runs an ordered workqueue that drains the HIF's RX FIFO,
services deadlines, and pushes queued TX. It sleeps 50 ms when idle, one jiffy
while a join or a scan is outstanding, and reschedules immediately after any
poll that moved data.

The TX queue is not an optimisation either. `ndo_start_xmit` is called with
softirqs disabled and may not sleep; the HIF's page accounting can wait up to
200 ms for the firmware to return credits. So `ndo_start_xmit` linearizes,
queues, and kicks the worker, which is the only context allowed to block.

### What it does and does not do

2.4 GHz only, channels 1–13, open and WPA2-PSK/CCMP, up to 54 Mb/s. There is no
802.11n here: this is a single-stream 802.11g radio and the rate table stops at
54. Not implemented, and each for a reason rather than an oversight — hidden
SSIDs (the scan command is a wildcard sweep with no directed probe),
WPA3/SAE and TKIP/WEP (only CCMP and PSK are advertised, so `wpa_supplicant`
never offers the rest), AP and monitor mode, and 5 GHz, which this part has not
got.

When it stops early it says where, by stage name, in one `dmesg` line —
`consys-mtcmos-timeout`, `rom-patch-missing`, `wmt-rf-calibration-failed`,
`firmware-ready-timeout`, `wlan-netdev-register` and about forty others. The
name is the first thing that failed, not the last thing that was tried, and
`j36-logdump` on the card collects them.

## Licence and attribution

The original MixOS work here — `build-in-vm.sh`, `generate_dts.py`,
`create_boot_image.py`, `tools/j36-eglprobe.c`, `tools/j36-mixmirror.c`, `tools/mfgpower.c`,
`tools/mixsplash.c`, `tools/mixdash/` and this documentation — is dual-licensed:
take it under the **Mozilla Public License 2.0** or under the **GNU General Public
License version 2 or later**, at your option. The reasoning, the exact per-file
scope and both full texts are in [LICENSE](LICENSE), [LICENSE.MPL-2.0](LICENSE.MPL-2.0)
and [LICENSE.GPL-2](LICENSE.GPL-2). In short: MPL-2.0 keeps this file-level
copyleft while letting a vendor ship proprietary code beside it, and the GPL half
is what lets any of it move into the kernel it exists to drive.

Two things in this directory are **not** part of that dual grant, and the
distinction is not cosmetic:

- Everything under `linux/` — the seven MixOS modules (`j36_mt6592_input.c`,
  `j36_mt6592_audio.c`, `j36_jd9365_panel.c`, `j36_mt6592_usb_phy.c`,
  `j36_mt6592_pmic.c`, `j36_mt6592_backlight.c` and the six-file
  `j36_mt6592_wifi` build) and the three `linux/*.patch` files — is
  **`GPL-2.0-only`**. They derive from and link against GPL-2.0-only kernel
  internals, which is narrower than either half of the dual grant, so relicensing
  them is not this project's to do and it is not attempted. Code may move *into*
  them from the dual-licensed list — that is what the GPL half is for — and it may
  not move back out.
- `mvii-board/` is five verbatim MediaTek/MVII board headers and driver sources,
  redistributed unmodified with a SHA-256 each in `mvii-board/PROVENANCE.txt`.
  They are inputs the DTS generator parses, not MixOS work.
- `firmware/mediatek/mt6592/` is two MediaTek connectivity ROM patches, taken off
  this device's own stock system image and redistributed unmodified with a
  SHA-256 each in `firmware/README.md`. MediaTek's terms, not this project's.

A finished card is an aggregate: the Linux kernel, Mesa, Qt, SDL, busybox and the
Debian rootfs each arrive under their own terms.
`build-in-vm.sh` writes the same statement onto the card as `sd-boot/LICENSE.txt`,
mapped payload file by payload file and with both licence texts appended, because
handing somebody a card is a distribution and both halves of the grant say the
notices and the source offer travel with it.

**MixOS supports the MediaTek line of processors**, and this directory is that
support. It adds a second SoC vendor and a 32-bit ARM kernel to a build pipeline
that assumed one vendor and arm64, and it changes shared files to do it. That
pipeline is the part that descends from dArkOS, itself a Debian-based continuation
of ArkOS by christianhaitian, and it keeps their MIT licence; nothing MixOS runs
comes from either any more. Neither dArkOS nor ArkOS endorses this port, is
affiliated with it, or should receive its bug reports.

Thanks, in the order the debt is owed: to the **Debian** project, whose operating
system this device actually runs — the rootfs is Debian, built with Debian's tools,
and everything on the card that is not the kernel or the eleven files above is
Debian's work; to **ArkOS** and **dArkOS** for the distribution this grew out of;
to **MediaTek**, whose register documentation and vendor driver sources the device
tree generator reads directly rather than guessing from; to **Mesa**, whose lima
and kmsro drivers are the only reason a Utgard part from 2013 can run a GLES 2.0
UI at all; and to the **Linux kernel**, **Qt**, **SDL** and **busybox**
projects. MixOS is not affiliated with or endorsed by any of them,
and neither half of the dual grant conveys a right in anybody's trademark.
