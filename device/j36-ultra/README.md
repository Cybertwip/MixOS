# J36 Ultra MT6592 bring-up DTB

Build from the repository root:

```sh
./build-j36-ultra-dtb.sh
```

No PowerEngine checkout is required. The five MVII board files the generator
parses are vendored here, and they are the only ones it opens:

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

`PROVENANCE.txt` records the upstream commit and a SHA-256 per file.
`./device/j36-ultra/sync-mvii-board.sh` refreshes the copies from a PowerEngine
checkout and rewrites that record; it is run **by hand**, never by the build. If
a PowerEngine tree does happen to sit beside dArkOS, `build-j36-ultra.sh`
re-checks those hashes and warns when they have drifted — a warning and not a
failure, because a missing or older sibling repository must not be able to stop
this build.

The values could have been frozen into a JSON instead, which would be smaller
again, but that moves the numbers one copy further from the code that drives the
hardware. The generator's assertions are what turn an MVII pad-mux change into a
build failure here rather than seven dead keys on the device, and they only mean
something while they are reading real driver source.

Outputs:

```text
device/j36-ultra/generated/mt6592-j36-ultra.dts
device/j36-ultra/generated/mt6592-j36-ultra.dtb
device/j36-ultra/generated/mt6592-j36-ultra.roundtrip.dts
```

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
    device/j36-ultra/generated/mt6592-j36-ultra.dtb \
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
halves — `verify_native_userspace` for the armhf rootfs, `verify_gui_architecture`
for an ELF32/ARM EmulationStation, and `verify_boot_kernel_arch` for the arm64
boot magic in its own kernel. `device/j36-ultra/build-in-vm.sh` runs the MVII LK's
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

```sh
./build-j36-ultra.sh
```

This is an extension of `build-r36-ultra.sh` rather than a second build system.
It regenerates the DTB on the host first — so a keymap or pad-mux regression
fails in a second — then resumes the checkpointed R36 base build (already
finished ones cost seconds; `J36_RESUME_R36=0` skips it), then builds the J36
layer in the same `darkos-r36` VM. The first J36 run creates a persistent ARMv7
Linux 6.12 LTS workspace; later runs rebuild only changed kernel, DTB,
input-module, initramfs and `boot.img` files.

Artifacts are copied to:

```text
../dArkOS-artifacts/j36-ultra/boot.img            eMMC BOOTIMG payload (9 MiB slot)
../dArkOS-artifacts/j36-ultra/zImage              plain 32-bit ARM kernel
../dArkOS-artifacts/j36-ultra/mt6592-j36-ultra.dtb
../dArkOS-artifacts/j36-ultra/j36_mt6592_input.ko
../dArkOS-artifacts/j36-ultra/sd-boot/            copy onto the BOOT partition
../dArkOS-artifacts/j36-ultra/manifest.txt
```

## The SD BOOT payload

Copy `sd-boot/` into the root of the FAT partition labelled `BOOT` and the MVII
LK boots the card instead of the eMMC. Nothing already there is disturbed: an
R36S card keeps its `Image`, `uInitrd`, rk3326 trees and `boot.ini`.

```text
zImage                 plain ARMv7 kernel, no appended tree
mt6592-j36-ultra.dtb   the tree the LK loads separately and patches
initrd.img             bring-up initramfs (busybox + the input module)
mvii/boot.conf         filenames and command line for the MVII LK
```

`mvii/boot.conf` exists because a dArkOS card already carries a `boot.ini`, and
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
dArkOS's own `scripts/setup_partition.sh` sets `ROOT_FILESYSTEM_FORMAT="btrfs"`.
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
check that the request survived. `WIRELESS` and `BT` are asserted *off* for the
mirror-image reason: `WIRELESS` defaults to `y` under `NET`, and this image has
2.5 MiB of slack in a fixed 9 MiB partition.

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
nothing in the dArkOS rootfs opens `/dev/watchdog`, and the board already ran for
minutes with no node at all.

This is not only about a user pressing reboot. dArkOS expands its partitions in
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
`/sys/class/gpio/gpio77/value`, and this kernel has neither — no `power_supply`
driver for the MT6592 PMIC, and no sysfs GPIO export. What makes it worth a
command-line word is the unit, not the script: `Restart=always`, `RestartSec=2`
and `StartLimitIntervalSec=0`, which is systemd's way of spelling *never give up*.
It is a traceback on the console every 2.3 seconds for as long as the board is on.
Fitting a cell does not help: the file is missing because the driver is missing,
not because the bay is empty. A `power_supply` for the MT6592 PMIC is the real
fix, and it would make this daemon work unmodified.

The other RK3326-only units are deliberately left running. `351mp.service` (power
LED, backlight, `amixer`), `audiopath`, `audiostate` and `wifi_importer` are all
`Type=oneshot`: they fail once and stay failed. `emulationstation.service` is
`Restart=on-failure` under the default five-starts-in-ten-seconds limit, so
systemd stops it by itself — and its failure is the thing we still need to read.
Bounded noise is evidence; only the unbounded one had to go.

## What used to cost ten seconds of every boot

The `backlight` node is `status = "disabled"`. It is a `pwm-backlight` consuming
`&disp_pwm` and `&gpio`, and neither has a driver in this profile. An *enabled* DT
consumer whose provider has no driver does not fail — it defers, and keeps
deferring until `driver_deferred_probe_timeout` expires. That was the whole gap
between 0.87 s and 11.38 s, plus an error on the panel.
