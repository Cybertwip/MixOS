# <p align="center">Welcome to MixOS</p>

### <p align="center">A Debian based operating system for portable gaming handhelds, with first-class support for the MediaTek line of processors alongside Rockchip RK3326 and RK3566.</p>

[![Donate](https://img.shields.io/badge/Donate-PayPal-green.svg)](https://www.paypal.com/donate?hosted_button_id=RC72LJ4SSERSU)

**MixOS supports the MediaTek line of processors.** That is the reason this fork
exists and it is what makes it different from what it came from. The MediaTek
MT6592 (Cortex-A7, ARMv7, Mali-450 MP4) is brought up natively here — a 32-bit ARM
kernel, a mainline `mtk_drm` display path, `mtk-sd` storage, the MT6592 AFE for
audio, a keypad adapter, and Mesa's lima/kmsro pair driving EmulationStation — on
the J36 Ultra. The MediaTek work lives in [`device/j36-ultra/`](device/j36-ultra/)
and is licensed under the
[Microsoft Public License](device/j36-ultra/LICENSE). Rockchip RK3326 and RK3566
devices continue to be supported as they were.

The overarching goals of MixOS are as follows:
1. MediaTek support, on real hardware, in the open
1. Highly customizable 
1. Performance
1. Online Updates (Won't require SD card reflashing unless there are major structural changes like file system changes.)
1. Enthusiats focused

## This fork diverges

MixOS is a **divergent fork of dArkOS**, which is itself a Debian-based
continuation of [ArkOS](https://github.com/christianhaitian/arkos/wiki).
Divergent is meant literally: MixOS does not track dArkOS, and its changes are not
a patch set that will be sent back. It adds a second SoC vendor and a 32-bit ARM
kernel to a build system that assumed one vendor and arm64, and it edits shared
files to do that. A MixOS card, artifact or bug is not dArkOS's concern and not
ArkOS's — please do not report MixOS problems to either project.

Everything outside `device/j36-ultra/` and `device/r36-ultra/` came from dArkOS and
keeps its own copyright and licence — see [LICENSES.md](LICENSES.md). Neither
dArkOS nor ArkOS endorses this fork or is responsible for it.

This is intended to continue the work from ArkOS and dArkOS in a way that allows others to easily fork and modify the OS to their own taste.  If there's a feature not currently available that you want, you can fork this and add it yourself.
Don't feel like building the OS from scratch or don't have the resources to do so?  Ok, just download one of the available prebuilt images and make changes right in the OS.  Since this OS is based on the latest stable version of Debian, you have
access to over 64,000 packages you can install via the Debian Advanced Package Tool (APT).  Want to build the latest testing or bleeding edge release of Debian? See the notes below on how to accomplish this.

**Building instructions:**
   - Suggested Environment - Ubuntu or related variants, version 24.04 or newer.  Windows Subsystem for Linux (WSL) is not supported and will not work due to no support for chroot. \
     Because chroot is used in this process, heavy use of sudo is made.  To reduce the possibility of priviledge issues, \
     it's best to be able to execute sudo without needing a password.  This can be done using one of the 2 methods below.
      - Method 1: - Open a Terminal window and type `sudo visudo` \
                    In the bottom of the file, add the following line: `$USER ALL=(ALL) NOPASSWD: ALL` \
                    Where $USER is your username on your system. Save and close the sudoers file (if you haven't changed your \
                    default terminal editor (you'll know if you have), press Ctl + x to exit nano and it'll prompt you to save).
      - Method 2: - Clone this git repo then run `./FreeSudo.sh`.  If there were no errors, it should've completed this change for you. \
                    You can verify this by checking if a `/etc/sudoers.d/$USER` file exists and contains `$USER ALL=(ALL) NOPASSWD: ALL` in it.
     
Now you should be able to just run make <device_name> to build for a supported device.  Example: `make rg353m`

### J36 Ultra (MediaTek MT6592) bring-up build

The MediaTek target.  It builds a 32-bit ARM Linux 6.12 LTS kernel, the MT6592
device tree, the display/audio/input modules, an initramfs and an SD `BOOT`
payload that the device's own MVII little kernel hands control to — the ARMv7
kernel and the RK3326 arm64 kernel coexist on one card and share one armhf Debian
rootfs.

```bash
./build-j36-ultra.sh
# device tree only, in a second, no VM
./build-j36-ultra-dtb.sh
```

Read [`device/j36-ultra/README.md`](device/j36-ultra/README.md) first — it
documents the payload, every word of the kernel command line, and how to remove
any part of it from a card without a reflash.

### R36 Ultra bring-up build

The R36 Ultra helper currently builds the RG351MP/RK3326 base image used for
bring-up (the R36-specific DTB layer is still separate).  Its default profile
is intentionally small: one native armhf (32-bit) Debian userspace and the
EmulationStation GUI are built, while the arm64 userspace, bundled emulators,
and standalone applications are skipped.  The existing RK3326 kernel/U-Boot
chain is still arm64; a 32-bit kernel would be a separate board port.

```bash
./build-r36-ultra.sh
# or
make r36-ultra
```

The default is four parallel build jobs and an armhf-only userspace:

```bash
BUILD_JOBS=4 USERSPACE_ARCH=armhf BUILD_BUNDLED_APPS=n ./build-r36-ultra.sh
```

- Change `BUILD_JOBS` to control internal Make/CMake/Meson parallelism.
- Set `USERSPACE_ARCH=arm64` to build a single-architecture arm64 userspace
  instead.  The R36 helper never produces an arm64+armhf multiarch rootfs.
- The complete emulator/application bundle is retained only for the arm64
  profile (`USERSPACE_ARCH=arm64 BUILD_BUNDLED_APPS=y`).

**Notes**
- To build on a different release of Debian, change the DEBIAN_CODE_NAME export in the Makefile or add DEBIAN_CODE_NAME=<release> as a variable to `make`.  Other debian code names can be found at https://www.debian.org/releases/
- Standard device targets (other than `r36-ultra` and `j36-ultra`) build with both a 64bit and 32bit userspace by default.  This is primarily to support some 32bit ports available through PortMaster.  There are also some 32bit retroarch emulators available but the performance seems to be similar to the 64bit retroarch emulators at this point.
 - To build without 32bit support, change the BUILD_ARMHF export in the Makefile to n or add BUILD_ARMHF=n as a variable to `make`.
- For RK3566, you can add Kodi to your build.  Just change the BUILD_KODI export in the Makefile to y or add BUILD_KODI=y as a variavble to `make`.  Kodi is also available as a prepackaged build in the extra_packages/rk3566 subfolder.  Just copy it to your tools folder and launch from Options/Tools in the start menu.
 - Be aware that building Kodi will add a significant amount of time to your build.  Could be double or triple the build time.
- Initial build time on an Intel I7-8700 65w unit with a 512GB NVME SSD and 32GB of DDR4 memory is a little over 19 hours.  Subsequent builds are about 3 hours thanks to ccache.
- Some directories, environment variables and shell functions still spell the old
  name: the `dArkOS-artifacts/` output directory, the `DARKOS_*` build variables,
  the `darkos_*` shell helpers, the `darkos-r36` build VM and its
  `~/darkos-r36-state` checkpoints, and `dArkOS_Tools/` in the rootfs.  Those are
  paths and interfaces rather than branding, and renaming them would orphan
  existing build state, cached checkpoints and cards already in the field, so they
  are deliberately left as they are.

# Licence

MixOS is made up of many open-source components, each under its own licence; see
[LICENSES.md](LICENSES.md).

- The MediaTek J36 Ultra device work is under the **Microsoft Public License
  (Ms-PL)** — [`device/j36-ultra/LICENSE`](device/j36-ultra/LICENSE).
- The Linux kernel modules and kernel patches in `device/j36-ultra/linux/` stay
  **GPL-2.0-only**.  Ms-PL is not GPL-compatible and they are not relicensed.
- Everything inherited from dArkOS keeps dArkOS's MIT licence and copyright.

# Credits and Thanks
**[Debian](https://www.debian.org/)** — the operating system all of this is built on.  The rootfs a MixOS device boots *is* Debian, assembled with Debian's own tools, and all but a handful of the packages on the card are the Debian project's work.  MixOS is a set of device ports and a build system on top of Debian; the operating system itself is theirs.  MixOS is not affiliated with or endorsed by the Debian project. \
**[ArkOS](https://github.com/christianhaitian/arkos/wiki)** and christianhaitian — the original handheld distribution, and the layout, tools menu and EmulationStation integration this still uses \
**dArkOS** — the Debian rebuild of ArkOS that this fork diverged from, and the origin of everything outside the `device/` ports \
**MediaTek** — the MT6592 documentation and vendor driver sources the J36 Ultra port reads \
**[Mesa](https://www.mesa3d.org/)** — lima and kmsro, which are the entire reason a Mali-450 from 2013 can run a GLES 2.0 UI \
**EmulationStation** (fcamod) — the front end, and a renderer architecture clean enough to add a third backend to \
**The Linux kernel, SDL, busybox, doomgeneric and Freedoom** projects \
[ChatGPT](https://chatgpt.com/) for guidance on how to build a Debian image \
Jetup13 for many themes \
dani7959 for the replica theme \
pix33l for the pixui theme \
TheGreatCrippler for testing and feedback \
kloptops for testing and feedback \
Fraxinus88 for testing and feedback \
ImCoKeMaN for testing and feedback \
[PortMaster](https://portmaster.games/) team for support in figuring out PM interface issues
