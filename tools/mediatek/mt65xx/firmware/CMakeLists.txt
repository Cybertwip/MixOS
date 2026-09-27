cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv7)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MINOS_ROOT "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(MVII_REPO_ROOT "${MINOS_ROOT}/../../.." ABSOLUTE)
option(J36_WITHOUT_BATTERY "Preserve preloader cable power; skip battery policy" OFF)
add_compile_definitions(MVII_WITHOUT_BATTERY=$<BOOL:${J36_WITHOUT_BATTERY}>)

if(NOT DEFINED MVII_ARMV7_TARGET_TRIPLE OR MVII_ARMV7_TARGET_TRIPLE STREQUAL "")
    set(MVII_ARMV7_TARGET_TRIPLE "armv7a-none-eabi")
endif()
if(NOT DEFINED MVII_ARMV7_CPU OR MVII_ARMV7_CPU STREQUAL "")
    set(MVII_ARMV7_CPU "cortex-a7")
endif()
if(NOT DEFINED MVII_ARMV7_LK_FB_ADDR OR MVII_ARMV7_LK_FB_ADDR STREQUAL "")
    set(MVII_ARMV7_LK_FB_ADDR "0x82700000")
endif()
# The LK's off-screen compose canvas, same geometry as the framebuffer above.
#
# The OVL scans 0x82700000 out continuously, so anything drawn there is on the
# panel while it is still half-drawn -- which is what made the charge screen
# flicker on every percentage step. The charge screen composes here instead and
# copies only finished rectangles across.
#
# THE WINDOW ABOVE THE FRAMEBUFFER IS NOT FREE. This was 0x82900000 for one
# build, on the reasoning that it is past the framebuffer's extent (0x82700000 +
# 480*2560 ends at 0x8282c000) and short of the SD boot path's DTB at 0x83000000
# -- and 0x82900000 is MVII_ASSET_STAGE_ADDR, where the LOGO partition's asset
# container is staged. The charge screen's first full clear painted 1.2 MB of
# black over the container it was about to blit FROM, so nothing was drawn and the
# splash stayed up: the visible symptom of that collision is "no battery screen".
#
# 0x82b00000 is the top of the asset stage's 2 MB cap (MVII_ASSET_STAGE_MAX, and
# a container claiming more is refused rather than staged), so this starts exactly
# where the assets can no longer reach. It ends at 0x82c2c000, short of the Mali
# job buffers mvii_lk_linker.ld puts at 0x82d00000 and of the debug console's
# firmware scratch at 0x83000000 above those, and it is above stage1's
# RAMDISK_SCAN_END (0x82600000) so the ramdisk magic scan cannot find it.
#
# mvii_lk_main.c has compile-time checks that this overlaps neither the asset
# stage nor the GPU window. Keep the map in mvii_assets.h in step with any
# change here.
if(NOT DEFINED MVII_ARMV7_LK_SHADOW_ADDR OR MVII_ARMV7_LK_SHADOW_ADDR STREQUAL "")
    set(MVII_ARMV7_LK_SHADOW_ADDR "0x82b00000")
endif()
if(NOT DEFINED MVII_ARMV7_LK_FB_WIDTH OR MVII_ARMV7_LK_FB_WIDTH STREQUAL "")
    set(MVII_ARMV7_LK_FB_WIDTH "640")
endif()
if(NOT DEFINED MVII_ARMV7_LK_FB_HEIGHT OR MVII_ARMV7_LK_FB_HEIGHT STREQUAL "")
    set(MVII_ARMV7_LK_FB_HEIGHT "480")
endif()
if(NOT DEFINED MVII_ARMV7_LK_FB_PITCH OR MVII_ARMV7_LK_FB_PITCH STREQUAL "")
    set(MVII_ARMV7_LK_FB_PITCH "2560")
endif()
if(NOT DEFINED MVII_ARMV7_LK_FB_BPP OR MVII_ARMV7_LK_FB_BPP STREQUAL "")
    set(MVII_ARMV7_LK_FB_BPP "32")
endif()
if(NOT DEFINED MVII_ARMV7_EMMC_USER_SECTORS OR MVII_ARMV7_EMMC_USER_SECTORS STREQUAL "")
    set(MVII_ARMV7_EMMC_USER_SECTORS "0x00738000")
endif()
# UBOOT/LK slot geometry from the stock J36 Ultra scatter: 2 MiB at 0x1d40000.
# The minimal LK image is padded to exactly this size so the boot-status sector
# it carries lands on the address ./flash -mtk-read-boot-status reads back.
if(NOT DEFINED MVII_ARMV7_UBOOT_SLOT_SIZE OR MVII_ARMV7_UBOOT_SLOT_SIZE STREQUAL "")
    set(MVII_ARMV7_UBOOT_SLOT_SIZE "0x200000")
endif()
# The rest of the eMMC USER geometry this package writes to, from the same stock
# scatter (Reference/J36-ULTRA/MT6592_Android_scatter.txt). ./flash has these
# baked in as j36UltraScatter*Offset; they are declared here because the
# generated mvii.txt / mvii.hypervisor.txt scatter files below have to name them,
# and a scatter that disagreed with the tool reading it would write a bootloader
# into the wrong slot.
if(NOT DEFINED MVII_ARMV7_UBOOT_SLOT_OFFSET OR MVII_ARMV7_UBOOT_SLOT_OFFSET STREQUAL "")
    set(MVII_ARMV7_UBOOT_SLOT_OFFSET "0x1d40000")
endif()
if(NOT DEFINED MVII_ARMV7_LOGO_SLOT_OFFSET OR MVII_ARMV7_LOGO_SLOT_OFFSET STREQUAL "")
    set(MVII_ARMV7_LOGO_SLOT_OFFSET "0x38c0000")
endif()
if(NOT DEFINED MVII_ARMV7_LOGO_SLOT_SIZE OR MVII_ARMV7_LOGO_SLOT_SIZE STREQUAL "")
    set(MVII_ARMV7_LOGO_SLOT_SIZE "0x800000")
endif()
# BOOTIMG is the slot the resident LK reads when SD/memory boot declines. Same
# stock scatter as UBOOT above (ota_scatter.txt: BOOTIMG 0x1f40000). This package
# never writes that slot; these numbers only tell the bootloader where the stock
# firmware left boot.img.
if(NOT DEFINED MVII_ARMV7_BOOTIMG_SLOT_OFFSET OR MVII_ARMV7_BOOTIMG_SLOT_OFFSET STREQUAL "")
    set(MVII_ARMV7_BOOTIMG_SLOT_OFFSET "0x1f40000")
endif()
if(NOT DEFINED MVII_ARMV7_BOOTIMG_SLOT_SIZE OR MVII_ARMV7_BOOTIMG_SLOT_SIZE STREQUAL "")
    set(MVII_ARMV7_BOOTIMG_SLOT_SIZE "0x900000")
endif()
# Wi-Fi discovery transport (mt6592_wifi_sdio.c + mt6592_wifi_wmt.c +
# mt6592_wifi_hif.c): power MT6323 VCN rails and CONSYS_6592, apply the stock
# WMT ROM patches/calibration over BTIF, bind its AHB HIF, download the stock
# divided/encrypted WIFI_RAM_CODE_SOC image, then poll
# Beacon/Probe Response frames for Settings. BOOT-SAFE: hardware writes remain
# deferred until the first user-triggered scan, so a bad board register costs
# one scan attempt with a precise breadcrumb rather than a restart loop.
if(NOT DEFINED MVII_MT6592_WIFI_ENABLE_TRANSPORT OR MVII_MT6592_WIFI_ENABLE_TRANSPORT STREQUAL "")
    set(MVII_MT6592_WIFI_ENABLE_TRANSPORT "1")
endif()


set(_mvii_arm_clang "")
set(_mvii_arm_clangxx "")
foreach(_candidate IN ITEMS compiler clang)
    if(EXISTS "${MVII_LLVM_ROOT}/bin/${_candidate}")
        set(_mvii_arm_clang "${MVII_LLVM_ROOT}/bin/${_candidate}")
        break()
    endif()
endforeach()
foreach(_candidate IN ITEMS compiler++ clang++)
    if(EXISTS "${MVII_LLVM_ROOT}/bin/${_candidate}")
        set(_mvii_arm_clangxx "${MVII_LLVM_ROOT}/bin/${_candidate}")
        break()
    endif()
endforeach()
if(NOT _mvii_arm_clang)
    message(FATAL_ERROR "MVII ARM needs clang/compiler under MVII_LLVM_ROOT=${MVII_LLVM_ROOT}")
endif()

set(CMAKE_C_COMPILER "${_mvii_arm_clang}")
set(CMAKE_ASM_COMPILER "${_mvii_arm_clang}")
set(CMAKE_C_COMPILER_TARGET "${MVII_ARMV7_TARGET_TRIPLE}")
set(CMAKE_ASM_COMPILER_TARGET "${MVII_ARMV7_TARGET_TRIPLE}")

project(MinosViiArmJ36Ultra C ASM)
include_directories("${MVII_REPO_ROOT}/device/j36-ultra/linux")

find_program(MVII_ARMV7_OBJCOPY NAMES llvm-objcopy
    HINTS "${MVII_LLVM_ROOT}/bin" "/usr/local/opt/llvm/bin" "/opt/homebrew/opt/llvm/bin"
    "/usr/local/opt/lld/bin" "/opt/homebrew/opt/lld/bin")
if(NOT MVII_ARMV7_OBJCOPY)
    message(FATAL_ERROR "MVII ARM needs llvm-objcopy")
endif()

find_program(MVII_ARMV7_LD_LLD NAMES ld.lld
    HINTS "${MVII_LLVM_ROOT}/bin" "/usr/local/opt/llvm/bin" "/opt/homebrew/opt/llvm/bin"
    "/usr/local/opt/lld/bin" "/opt/homebrew/opt/lld/bin")
if(NOT MVII_ARMV7_LD_LLD)
    message(FATAL_ERROR "MVII ARM needs ld.lld")
endif()

if(NOT MVII_PACKAGE_ROOT)
    set(MVII_PACKAGE_ROOT "${CMAKE_BINARY_DIR}")
endif()

set(MVII_ARM_DRIVER_DIR "${MINOS_ROOT}/Drivers")
find_program(_mvii_python NAMES python3 python)

# The picture the loader draws for its own splash, and the source image the asset
# slot below bakes into its logo record. Defaults to
# Hardware/Virtua/Standalone/tools/Microsoft.jpg; override with
# -DMVII_ARM_LOGO_IMAGE=/path/to/image (jpg/png, or a prebaked 640x480 BGRA .bin).
if(NOT DEFINED MVII_ARM_LOGO_IMAGE OR MVII_ARM_LOGO_IMAGE STREQUAL "")
    set(MVII_ARM_LOGO_IMAGE "${MVII_REPO_ROOT}/device/j36-ultra/resources/MixOS.jpg")
endif()

# The asset slot: every picture the loader draws, in one container flashed to
# the stock LOGO partition on its own with
#
#     ./flash -target=arm -dbg -device /dev/cu.usbmodem... -upload assets -yes
#
# This is NOT linked into anything. That is the entire point -- the boot pictures
# used to be C (rectangles, and a 3x5 bitmap font scaled ten times) plus an
# .incbin'd logo, so new art meant a new bootloader and a full flash. The loader
# reads the container off eMMC at boot and falls back to its built-in drawing if
# the partition has never been written, so an image built here still boots on a
# board with no assets on it. Format and rationale: Drivers/mvii_assets.h.
set(_mvii_arm_asset_slot "${CMAKE_BINARY_DIR}/mvii_assets.bin")
set(_mvii_arm_asset_script "${MINOS_ROOT}/scripts/create-mvii-asset-slot.py")
add_custom_command(
    OUTPUT "${_mvii_arm_asset_slot}"
    COMMAND "${_mvii_python}" "${_mvii_arm_asset_script}"
        --output "${_mvii_arm_asset_slot}"
        --logo "${MVII_ARM_LOGO_IMAGE}"
        --width "${MVII_ARMV7_LK_FB_WIDTH}"
        --height "${MVII_ARMV7_LK_FB_HEIGHT}"
    DEPENDS "${_mvii_arm_asset_script}" "${MVII_ARM_LOGO_IMAGE}"
    VERBATIM
    COMMENT "Generating MVII LK asset slot (battery, plug, bolt, numerals, logo)")
add_custom_target(mvii-armv7-asset-slot DEPENDS "${_mvii_arm_asset_slot}")

set(_mvii_arm_compile_options
    "--target=${MVII_ARMV7_TARGET_TRIPLE}"
    "-mcpu=${MVII_ARMV7_CPU}"
    -marm
    -mfloat-abi=soft
    # This second stage runs with the MMU off, so every access is
    # Strongly-Ordered and any unaligned load/store faults (data abort). ARMv7-A
    # otherwise defaults to -munaligned-access, which lets clang merge byte-wise
    # accessors (e.g. FAT get16le() on the odd BPB offset 11) into a single
    # unaligned ldrh — that is exactly what aborted boot_main's storage mount.
    # Force strict alignment so the compiler never emits unaligned wide accesses.
    -mno-unaligned-access
    -ffreestanding
    -fno-builtin
    -fno-pic
    -fno-pie
    -fno-stack-protector
    -ffunction-sections
    -fdata-sections
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -Wall
    -Wextra
    -Os)

set(_mvii_arm_link_options
    "--target=${MVII_ARMV7_TARGET_TRIPLE}"
    "-mcpu=${MVII_ARMV7_CPU}"
    -marm
    -mfloat-abi=soft
    "-fuse-ld=${MVII_ARMV7_LD_LLD}"
    -nostdlib
    -nostartfiles
    -static
    -Wl,--build-id=none
    -Wl,--gc-sections)

# The MVII minimal LK: a full replacement for the stock MediaTek LK in the UBOOT
# slot. It arms the PMIC input path, lights the JD9365 panel, and hands off to a
# kernel. This is the only thing this package builds for the board.
#
# Two images are built from this one source list. They are the same bootloader;
# they differ in one compile definition and therefore in where they look for a
# kernel:
#
#   lk.bin          boot.img out of the eMMC BOOTIMG slot, ATAG hand-off, and
#                   the live debug console compiled in.
#   lk-release.bin  zImage + DTB off the microSD, Linux DTB hand-off, the eMMC
#                   path above as its fallback, and no debug console at all.
#
# There is one UBOOT partition and one resident bootloader, so which of these is
# installed IS the board's boot policy -- `./flash -upload release' puts the
# second one there and `-upload lk' puts the first one back. Making it a build
# variant rather than a runtime switch is deliberate: a single image that had to
# decide per boot whether the card in the slot was meant to be booted would get
# that wrong the first time somebody inserted a card just to copy a file off it.
set(_mvii_arm_lk_sources
    "${MVII_ARM_DRIVER_DIR}/mvii_lk.S"
    "${MVII_ARM_DRIVER_DIR}/mvii_lk_main.c"
    # The boot pictures, read off the LOGO partition at runtime rather than
    # linked in. See the asset-slot custom command above.
    "${MVII_ARM_DRIVER_DIR}/mvii_assets.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_uart.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_timer.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_pwrap.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_pmic.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_led.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_msdc.c"
    # Removable storage. MSDC1 is a different controller, clock gate and pinmux
    # from the eMMC above, and until now nothing in LK had a reason to touch it.
    # The reason is that the connectivity chip's firmware is three files on the
    # card, so mvii_fat.c -- a read-only FAT12/16/32 reader written for this,
    # because the tree's only other filesystem is a six-thousand-line FatFs that
    # does not fit the 512 KiB slot -- sits on top of it.
    "${MVII_ARM_DRIVER_DIR}/mt6592_msdc_sd.c"
    "${MVII_ARM_DRIVER_DIR}/mvii_fat.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_bootstatus.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_bootinfo.c"
    "${MVII_ARM_DRIVER_DIR}/backlight.c"
    "${MVII_ARM_DRIVER_DIR}/dsi_drv.c"
    "${MVII_ARM_DRIVER_DIR}/lcd_drv.c"
    "${MVII_ARM_DRIVER_DIR}/panel_bringup.c"
    # The live debug console and the two pieces it stands on: a one-shot eMMC
    # flag the BROM payload arms, and a MUSB peripheral-mode transport of our
    # own. This is the only USB device on this board that survives a reboot and
    # a clock reprogram, which is what makes debugging a *booted* machine
    # possible at all. See mvii_debug_console.h.
    "${MVII_ARM_DRIVER_DIR}/mt6592_dbgflag.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_usb_gadget.c"
    "${MVII_ARM_DRIVER_DIR}/mvii_debug_console.c"
    # The whole Wi-Fi stack, for the console's `wifi` command. Wi-Fi itself
    # belongs to the OS, but the OS is a forty-second boot away and reports a
    # bring-up failure as one frozen string on a dashboard; the rails, the SPM
    # domain, the BTIF, the WLAN HIF and the scan path are all reachable from
    # here the moment the console is up.
    #
    # The HIF and crypto halves used to be left out on the grounds that the link
    # below them was unproven. It still is -- but leaving them out meant the only
    # way to prove it was to boot the OS, which is the loop this console exists
    # to break. They need nothing MVIILK.elf does not already link: uart, timer,
    # pwrap, pmic, bootstatus, delay.
    "${MVII_ARM_DRIVER_DIR}/mt6592_wifi_sdio.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_wifi_wmt.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_wifi_hif.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_wifi_crypto.c"
    # The Mali-450 MP4 bring-up, for the console's `gpu` command, and here for
    # the same reason the Wi-Fi stack is: the MFG power domain, the clock gates,
    # the per-core MMU disable and the GP/PP self-tests are all reachable from
    # the bootloader, and proving them here does not cost a forty-second boot
    # into something that reports failure as one frozen string.
    #
    # It brings two things nothing else in this image does: ~1.9 MiB of job
    # buffers, which mvii_lk_linker.ld puts in .gpubss out at 0x82d00000 rather
    # than in the 512 KiB slot, and floating point -- see the per-source options
    # below.
    "${MVII_ARM_DRIVER_DIR}/mt6592_gpu_offload.c"
    # No compiler-rt builtins are linked here, and mt6592_timer's microsecond
    # conversion divides 64-bit counts: without this the link fails on
    # __aeabi_uldivmod. The HIF assigns whole scan-result structs, which the
    # compiler lowers to __aeabi_memcpy for the same reason.
    "${MVII_ARM_DRIVER_DIR}/mvii_arm_aeabi_div.c"
    "${MVII_ARM_DRIVER_DIR}/mvii_arm_aeabi_mem.c")

# Everything both LK images share. Kept as a function rather than copied so the
# two cannot drift: a release bootloader that differs from the normal one in
# anything but its boot policy is a second bootloader to debug.
function(mvii_arm_configure_lk_target target)
    target_include_directories(${target} PRIVATE "${MVII_ARM_DRIVER_DIR}")
    target_compile_options(${target} PRIVATE ${_mvii_arm_compile_options})
    target_compile_definitions(${target} PRIVATE
        MT6592_DEBUG_UART_BASE=MT6592_UART0_BASE
        MVII_MT6592_LK_FB_ADDR=${MVII_ARMV7_LK_FB_ADDR}
        MVII_MT6592_LK_SHADOW_ADDR=${MVII_ARMV7_LK_SHADOW_ADDR}
        MVII_MT6592_LK_FB_WIDTH=${MVII_ARMV7_LK_FB_WIDTH}
        MVII_MT6592_LK_FB_HEIGHT=${MVII_ARMV7_LK_FB_HEIGHT}
        MVII_MT6592_LK_FB_PITCH=${MVII_ARMV7_LK_FB_PITCH}
        MVII_MT6592_LK_FB_BPP=${MVII_ARMV7_LK_FB_BPP}
        MVII_MT6592_LK_BOOTIMG_OFFSET=${MVII_ARMV7_BOOTIMG_SLOT_OFFSET}
        MVII_MT6592_LK_BOOTIMG_SIZE=${MVII_ARMV7_BOOTIMG_SLOT_SIZE}
        MVII_MT6592_USER_CAPACITY_SECTORS=${MVII_ARMV7_EMMC_USER_SECTORS}
        # Without this mt6592_wifi_sdio_bind() compiles to the stub that reports
        # "transport-disabled", and the console's `wifi` command would probe nothing.
        MVII_MT6592_WIFI_ENABLE_TRANSPORT=${MVII_MT6592_WIFI_ENABLE_TRANSPORT}
        MVII_MT6592_ASSUME_DRAM_READY=1)
    # Deliberately NOT MVII_MT6592_WARM_LK. The stage images set it because the
    # stock LK ran ahead of them and left the MIPITX reference alive; this image IS
    # the LK. Nothing but the preloader has run, the preloader has no display path,
    # and the warm branch of mt6592_lcd_clocks_on() only re-muxes MIPI_26M — it
    # never brings UNIVPLL/AP_PLL up, so the D-PHY has no byte clock to lock to and
    # the panel lights with no valid video on it. DRAM is a separate question and
    # the preloader really has trained it, hence MVII_MT6592_ASSUME_DRAM_READY.
    target_link_options(${target} PRIVATE
        ${_mvii_arm_link_options}
        "-T${MVII_ARM_DRIVER_DIR}/mvii_lk_linker.ld"
        "-Wl,-Map=${CMAKE_BINARY_DIR}/${target}.map")
    set_target_properties(${target} PROPERTIES
        LINK_DEPENDS "${MVII_ARM_DRIVER_DIR}/mvii_lk_linker.ld")
endfunction()

# The one translation unit in this image that is allowed a floating-point unit.
#
# Everything else is -mfloat-abi=soft, which is fine because no other driver has
# a float in it -- and it has to stay that way, because no compiler-rt is linked
# here and so a single __aeabi_fsub is an undefined symbol at link time, not a
# slow multiply. The Mali's vertex and texcoord arrays are IEEE-754 words and
# gpu_triangle_vertex is a float ABI, so mt6592_gpu_offload.c cannot avoid them.
#
# softfp, not hard: the calling convention is identical to soft -- arguments in
# core registers -- so these objects link against the rest of the soft-float
# image unchanged, and only the arithmetic inside the file uses the VFP. These
# options are appended after the target-wide ones, and clang takes the last
# -mfloat-abi/-mfpu it is given.
#
# The unit itself is off at reset and the preloader does not turn it on;
# mt6592_gpu_offload.c's probe does that for itself before it touches a float.
set_source_files_properties("${MVII_ARM_DRIVER_DIR}/mt6592_gpu_offload.c"
    PROPERTIES COMPILE_OPTIONS "-mfloat-abi=softfp;-mfpu=neon-vfpv4")

add_executable(MVIILK.elf ${_mvii_arm_lk_sources})
mvii_arm_configure_lk_target(MVIILK.elf)

# The release bootloader. One extra translation unit — the flattened device tree
# writer, which the eMMC path has no use for because ATAGs carry the same facts —
# and two definitions: one turns on the SD hand-off in mvii_lk_main.c, the other
# takes the live debug console out. The eMMC boot is still compiled in and is
# still what runs when the card has no kernel on it, so this image is not a
# one-way door.
#
# MVII_MT6592_LK_RELEASE removes the only call to mvii_debug_console_run(), and
# because every object here is built -ffunction-sections -fdata-sections and
# linked --gc-sections, removing the call removes the code: the console's command
# table, the MUSB peripheral gadget under it and the whole Wi-Fi stack it is the
# sole caller of all become unreachable and are dropped. So this is not a
# bootloader that merely declines to open a debug path — there is no debug path
# in the image to open. The one function in mvii_debug_console.c that is *not*
# debug, mvii_battery_boot_policy(), is still called from the boot path and is
# still linked in, alone.
#
# The sources stay shared, deliberately. A release bootloader assembled from a
# different file list is a second bootloader to debug, and the whole point of
# this pair is that they differ in boot policy and nothing else.
add_executable(MVIILKRelease.elf
    ${_mvii_arm_lk_sources}
    "${MVII_ARM_DRIVER_DIR}/mt6592_fdt.c")
mvii_arm_configure_lk_target(MVIILKRelease.elf)
target_compile_definitions(MVIILKRelease.elf PRIVATE
    MVII_MT6592_LK_SD_HANDOFF=1
    MVII_MT6592_LK_RELEASE=1)

# The live BROM payload. `./flash' feeds this through SEND_DA/JUMP_DA and then
# drives it: it owns MSDC0, writes the eMMC areas directly without the legacy DA,
# and carries the handful of live commands (panel, led, wifi bind, debug mode)
# that let a board be poked at before anything resident is on it. It is a flash
# tool that happens to run on the target, which is why it survives the trim.
add_executable(MVIIFlash.elf
    "${MVII_ARM_DRIVER_DIR}/flash_stage.S"
    "${MVII_ARM_DRIVER_DIR}/flash_stage.c"
    "${MVII_ARM_DRIVER_DIR}/stage1.c"
    "${MVII_ARM_DRIVER_DIR}/mvii_assets.c"
    "${MVII_ARM_DRIVER_DIR}/microsoft_logo_stub.S"
    "${MVII_ARM_DRIVER_DIR}/mt6592_bootinfo.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_pwrap.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_dram.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_bootinfo.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_bootstatus.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_msdc.c"
    "${MVII_ARM_DRIVER_DIR}/mt6592_uart.c"
    "${MVII_ARM_DRIVER_DIR}/backlight.c"
    "${MVII_ARM_DRIVER_DIR}/dsi_drv.c"
    "${MVII_ARM_DRIVER_DIR}/lcd_drv.c"
    # Shared with MVIILK.elf so the resident payload's live "panel" command runs
    # the exact bring-up the LK slot runs at boot.
    "${MVII_ARM_DRIVER_DIR}/panel_bringup.c"
    # Centre LED, for the live "led" command that names which ISINK channel is
    # which colour. Reaches the MT6323 over pwrap only -- no PMIC service, so
    # mt6592_pmic.c stays out of this payload (see the weak reference in
    # mt6592_led.c).
    "${MVII_ARM_DRIVER_DIR}/mt6592_led.c"
    # CONSYS power-up for the live "wifi bind" command: MT6323 VCN rails, CONSYS
    # MTCMOS, INFRA clock and the chip-ID probe. Registers only -- the firmware
    # download stages need DRAM and a filesystem and stay in the OS.
    "${MVII_ARM_DRIVER_DIR}/mt6592_wifi_sdio.c"
    # The whole point of this payload now: `debug mode` arms this flag and
    # resets, and the LK picks it up. See mt6592_dbgflag.h for why the channel is
    # an eMMC sector and not WDT_NONRST_REG.
    "${MVII_ARM_DRIVER_DIR}/mt6592_dbgflag.c")
target_include_directories(MVIIFlash.elf PRIVATE "${MVII_ARM_DRIVER_DIR}")
target_compile_options(MVIIFlash.elf PRIVATE ${_mvii_arm_compile_options})
target_compile_definitions(MVIIFlash.elf PRIVATE
    MT6592_DEBUG_UART_BASE=MT6592_UART0_BASE
    MVII_MT6592_LK_FB_ADDR=${MVII_ARMV7_LK_FB_ADDR}
    MVII_MT6592_LK_SHADOW_ADDR=${MVII_ARMV7_LK_SHADOW_ADDR}
    MVII_MT6592_LK_FB_WIDTH=${MVII_ARMV7_LK_FB_WIDTH}
    MVII_MT6592_LK_FB_HEIGHT=${MVII_ARMV7_LK_FB_HEIGHT}
    MVII_MT6592_LK_FB_PITCH=${MVII_ARMV7_LK_FB_PITCH}
    MVII_MT6592_LK_FB_BPP=${MVII_ARMV7_LK_FB_BPP}
    # The live stage1 payload must paint the SAME reserved DRAM canvas the LK
    # uses (0x82700000). Low FB would corrupt BROM working buffers and drop USB.
    # PRESERVE_USB=1 further ensures we never replay cold UNIVPLL (which also
    # drops configured VCOM on the host side).
    # MVII_MT6592_PRESERVE_USB=1 skips the cold UNIVPLL/APMIXED replay in
    # lcd_drv so the active BROM VCOM USB session is not disturbed by PLL mux
    # changes.
    MVII_MT6592_STAGE1_FB_ADDR=${MVII_ARMV7_LK_FB_ADDR}
    MVII_MT6592_DRAM_PROBE_ADDR=${MVII_ARMV7_LK_FB_ADDR}
    MVII_MT6592_ENABLE_INTERNAL_DRAM_INIT=1
    MVII_MT6592_ASSUME_DRAM_READY=0
    # The live "wifi bind" command exists to run the real CONSYS power-up; with
    # the transport compiled out mt6592_wifi_sdio_bind() is a stub that reports
    # "transport-disabled" and touches no hardware, which is the one answer the
    # command cannot usefully give.
    MVII_MT6592_WIFI_ENABLE_TRANSPORT=${MVII_MT6592_WIFI_ENABLE_TRANSPORT}
    MVII_MT6592_PRESERVE_USB=1)
target_link_options(MVIIFlash.elf PRIVATE
    ${_mvii_arm_link_options}
    "-T${MVII_ARM_DRIVER_DIR}/flash_stage_linker.ld"
    "-Wl,-Map=${CMAKE_BINARY_DIR}/MVIIFlash.map")
set_target_properties(MVIIFlash.elf PROPERTIES
    LINK_DEPENDS "${MVII_ARM_DRIVER_DIR}/flash_stage_linker.ld")

set(_mvii_arm_flash_stage_bin "${CMAKE_BINARY_DIR}/MVIIFlash.bin")
set(_mvii_arm_minimal_lk_bin "${CMAKE_BINARY_DIR}/MVIILK.bin")
set(_mvii_arm_release_lk_bin "${CMAKE_BINARY_DIR}/MVIILKRelease.bin")

add_custom_command(
    OUTPUT "${_mvii_arm_flash_stage_bin}"
    COMMAND "${MVII_ARMV7_OBJCOPY}" -O binary
            "$<TARGET_FILE:MVIIFlash.elf>"
            "${_mvii_arm_flash_stage_bin}"
    DEPENDS MVIIFlash.elf
    VERBATIM
    COMMENT "Generating MVII ARM native MTK flash-stage payload")

add_custom_command(
    OUTPUT "${_mvii_arm_minimal_lk_bin}"
    COMMAND "${MVII_ARMV7_OBJCOPY}" -O binary
            "$<TARGET_FILE:MVIILK.elf>"
            "${_mvii_arm_minimal_lk_bin}"
    DEPENDS MVIILK.elf
    VERBATIM
    COMMENT "Generating MVII ARM minimal LK binary")

add_custom_command(
    OUTPUT "${_mvii_arm_release_lk_bin}"
    COMMAND "${MVII_ARMV7_OBJCOPY}" -O binary
            "$<TARGET_FILE:MVIILKRelease.elf>"
            "${_mvii_arm_release_lk_bin}"
    DEPENDS MVIILKRelease.elf
    VERBATIM
    COMMENT "Generating MVII ARM release LK binary (SD hand-off)")


set(J36_BOOT_DIR "${MVII_PACKAGE_ROOT}/boot")
set(J36_WRAP "${MINOS_ROOT}/scripts/create-mtk-minimal-lk-image.py")
foreach(pair IN ITEMS "MVIILK;lk" "MVIILKRelease;lk-release")
    list(GET pair 0 target)
    list(GET pair 1 image)
    add_custom_command(OUTPUT "${J36_BOOT_DIR}/${image}.bin"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${J36_BOOT_DIR}"
        COMMAND "${_mvii_python}" "${J36_WRAP}"
            --loader "${CMAKE_BINARY_DIR}/${target}.bin"
            --output "${J36_BOOT_DIR}/${image}.bin"
            --slot-size "${MVII_ARMV7_UBOOT_SLOT_SIZE}"
        DEPENDS "${CMAKE_BINARY_DIR}/${target}.bin" "${J36_WRAP}"
        VERBATIM)
endforeach()
add_custom_command(OUTPUT "${J36_BOOT_DIR}/MVIIFlash.bin" "${J36_BOOT_DIR}/assets.bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${J36_BOOT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_mvii_arm_flash_stage_bin}" "${J36_BOOT_DIR}/MVIIFlash.bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_mvii_arm_asset_slot}" "${J36_BOOT_DIR}/assets.bin"
    DEPENDS "${_mvii_arm_flash_stage_bin}" "${_mvii_arm_asset_slot}"
    VERBATIM)
add_custom_target(firmware ALL DEPENDS
    "${J36_BOOT_DIR}/lk.bin" "${J36_BOOT_DIR}/lk-release.bin"
    "${J36_BOOT_DIR}/MVIIFlash.bin" "${J36_BOOT_DIR}/assets.bin")
file(GENERATE OUTPUT "${J36_BOOT_DIR}/power-mode.txt"
    CONTENT "without_battery=$<BOOL:${J36_WITHOUT_BATTERY}>\n")
