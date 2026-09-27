cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv7)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MINOS_ROOT "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(MVII_REPO_ROOT "${MINOS_ROOT}/../../../.." ABSOLUTE)

# ── bring-up knobs ──────────────────────────────────────────────────────
# Every default here is a prior from mt67xx_facts.h; overriding one is how a
# LK-BRINGUP step lands. build-info.txt records the values in force, and the
# boot banner prints them, so a serial log always names its own assumptions.
if(NOT DEFINED MT67XX_SOC OR MT67XX_SOC STREQUAL "")
    set(MT67XX_SOC "6739")
endif()
if(NOT DEFINED MT67XX_MEMBASE OR MT67XX_MEMBASE STREQUAL "")
    set(MT67XX_MEMBASE "0x41E00000")
endif()
if(NOT DEFINED MT67XX_DEBUG_UART OR MT67XX_DEBUG_UART STREQUAL "")
    set(MT67XX_DEBUG_UART "0")
endif()
if(NOT DEFINED MT67XX_DEVICE OR MT67XX_DEVICE STREQUAL "")
    message(FATAL_ERROR "MT67XX_DEVICE is required (a devices.sh codename, e.g. lm-x120)")
endif()
if(NOT DEFINED MVII_BUILD_COMMIT OR MVII_BUILD_COMMIT STREQUAL "")
    set(MVII_BUILD_COMMIT "nogit")
endif()
# UBOOT/LK slot geometry. UNKNOWN for these phones until LK-BRINGUP step 1
# reads the stock scatter -- the 2 MiB default is the j36 slot size, and a
# wrong size only mis-pads this file (SP Flash Tool flashes per ITS scatter
# and refuses a file bigger than the partition, so the failure is loud, not
# a brick). Override with -DMT67XX_LK_SLOT_SIZE=0x... the day it is known.
if(NOT DEFINED MT67XX_LK_SLOT_SIZE OR MT67XX_LK_SLOT_SIZE STREQUAL "")
    set(MT67XX_LK_SLOT_SIZE "0x200000")
    message(WARNING "MT67XX_LK_SLOT_SIZE unset: padding to the j36 2 MiB prior (LK-BRINGUP step 1)")
endif()

# armv7a + cortex-a7 on purpose, even though the silicon is Cortex-A53: the
# v7 subset runs unchanged on an A53 in AArch32, and MediaTek's own LK on
# this line (reference/lk mt6735 target: ARCH := arm, cortex-a7) is a
# 32-bit image too. The AArch64 switch happens once, at the kernel handoff
# (LK-BRINGUP step 5), not by compiling the bootloader for it.
if(NOT DEFINED MVII_ARMV7_TARGET_TRIPLE OR MVII_ARMV7_TARGET_TRIPLE STREQUAL "")
    set(MVII_ARMV7_TARGET_TRIPLE "armv7a-none-eabi")
endif()
if(NOT DEFINED MVII_ARMV7_CPU OR MVII_ARMV7_CPU STREQUAL "")
    set(MVII_ARMV7_CPU "cortex-a7")
endif()

set(_mt67xx_arm_clang "")
foreach(_candidate IN ITEMS compiler clang)
    if(EXISTS "${MVII_LLVM_ROOT}/bin/${_candidate}")
        set(_mt67xx_arm_clang "${MVII_LLVM_ROOT}/bin/${_candidate}")
        break()
    endif()
endforeach()
if(NOT _mt67xx_arm_clang)
    message(FATAL_ERROR "mt67xx LK needs clang/compiler under MVII_LLVM_ROOT=${MVII_LLVM_ROOT}")
endif()

set(CMAKE_C_COMPILER "${_mt67xx_arm_clang}")
set(CMAKE_ASM_COMPILER "${_mt67xx_arm_clang}")
set(CMAKE_C_COMPILER_TARGET "${MVII_ARMV7_TARGET_TRIPLE}")
set(CMAKE_ASM_COMPILER_TARGET "${MVII_ARMV7_TARGET_TRIPLE}")

project(MinosViiArmMt67xx C ASM)

find_program(MT67XX_OBJCOPY NAMES llvm-objcopy
    HINTS "${MVII_LLVM_ROOT}/bin" "/usr/local/opt/llvm/bin" "/opt/homebrew/opt/llvm/bin"
    "/usr/local/opt/lld/bin" "/opt/homebrew/opt/lld/bin")
if(NOT MT67XX_OBJCOPY)
    message(FATAL_ERROR "mt67xx LK needs llvm-objcopy")
endif()

find_program(MT67XX_LD_LLD NAMES ld.lld
    HINTS "${MVII_LLVM_ROOT}/bin" "/usr/local/opt/llvm/bin" "/opt/homebrew/opt/llvm/bin"
    "/usr/local/opt/lld/bin" "/opt/homebrew/opt/lld/bin")
if(NOT MT67XX_LD_LLD)
    message(FATAL_ERROR "mt67xx LK needs ld.lld")
endif()

if(NOT MVII_PACKAGE_ROOT)
    set(MVII_PACKAGE_ROOT "${CMAKE_BINARY_DIR}")
endif()

set(MT67XX_DRIVER_DIR "${MINOS_ROOT}/Drivers")
find_program(_mt67xx_python NAMES python3 python)

set(_mt67xx_compile_options
    "--target=${MVII_ARMV7_TARGET_TRIPLE}"
    "-mcpu=${MVII_ARMV7_CPU}"
    -marm
    -mfloat-abi=soft
    # Same rule as the j36: this image runs with the MMU off, so every
    # access is Strongly-Ordered and any unaligned load/store faults. Force
    # strict alignment so clang never emits unaligned wide accesses.
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

set(_mt67xx_link_options
    "--target=${MVII_ARMV7_TARGET_TRIPLE}"
    "-mcpu=${MVII_ARMV7_CPU}"
    -marm
    -mfloat-abi=soft
    "-fuse-ld=${MT67XX_LD_LLD}"
    -nostdlib
    -nostartfiles
    -static
    -Wl,--build-id=none
    -Wl,--gc-sections)

# The mt67xx minimal LK, v1: a bring-up instrument, not yet a bootloader.
# Serial hello + watchdog off + proven-ticking clock + heartbeat park (see
# Drivers/mt67xx_lk_main.c). One image, one policy -- the j36's lk/release
# pair exists to serve two boot policies, and this tree has one until the
# handoff step lands. A second variant before then would be packaging, not
# engineering.
configure_file(
    "${MT67XX_DRIVER_DIR}/mt67xx_lk_linker.ld.in"
    "${CMAKE_BINARY_DIR}/mt67xx_lk_linker.ld"
    @ONLY)

add_executable(MT67LK.elf
    "${MT67XX_DRIVER_DIR}/mt67xx_lk.S"
    "${MT67XX_DRIVER_DIR}/mt67xx_lk_main.c"
    "${MT67XX_DRIVER_DIR}/mt67xx_uart.c"
    "${MT67XX_DRIVER_DIR}/mt67xx_timer.c"
    "${MT67XX_DRIVER_DIR}/mt67xx_wdt.c")
target_include_directories(MT67LK.elf PRIVATE "${MT67XX_DRIVER_DIR}")
target_compile_options(MT67LK.elf PRIVATE ${_mt67xx_compile_options})
target_compile_definitions(MT67LK.elf PRIVATE
    MT67XX_SOC=${MT67XX_SOC}
    MT67XX_MEMBASE=${MT67XX_MEMBASE}
    MT67XX_DEBUG_UART=${MT67XX_DEBUG_UART}
    "MT67XX_DEVICE=\"${MT67XX_DEVICE}\""
    "MVII_BUILD_COMMIT=\"${MVII_BUILD_COMMIT}\""
    MT67XX_LK_SLOT_SIZE=${MT67XX_LK_SLOT_SIZE})
target_link_options(MT67LK.elf PRIVATE
    ${_mt67xx_link_options}
    "-T${CMAKE_BINARY_DIR}/mt67xx_lk_linker.ld"
    "-Wl,-Map=${CMAKE_BINARY_DIR}/MT67LK.map")
set_target_properties(MT67LK.elf PROPERTIES
    LINK_DEPENDS "${CMAKE_BINARY_DIR}/mt67xx_lk_linker.ld")

set(_mt67xx_lk_bin "${CMAKE_BINARY_DIR}/MT67LK.bin")
add_custom_command(
    OUTPUT "${_mt67xx_lk_bin}"
    COMMAND "${MT67XX_OBJCOPY}" -O binary
            "$<TARGET_FILE:MT67LK.elf>"
            "${_mt67xx_lk_bin}"
    DEPENDS MT67LK.elf
    VERBATIM
    COMMENT "Generating mt67xx minimal LK binary")

# The UBOOT-slot image: MTK header + payload + pad-to-slot + boot-status
# sector, via the j36 wrap script (copied verbatim to scripts/). The status
# sector's reader lands with the bootstatus step; until then it is format,
# not telemetry -- identical bytes, documented in FACTS.md.
set(MT67XX_BOOT_DIR "${MVII_PACKAGE_ROOT}/boot")
set(MT67XX_WRAP "${MINOS_ROOT}/scripts/create-mtk-lk-image.py")
add_custom_command(OUTPUT "${MT67XX_BOOT_DIR}/lk.bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MT67XX_BOOT_DIR}"
    COMMAND "${_mt67xx_python}" "${MT67XX_WRAP}"
        --loader "${_mt67xx_lk_bin}"
        --output "${MT67XX_BOOT_DIR}/lk.bin"
        --slot-size "${MT67XX_LK_SLOT_SIZE}"
    DEPENDS "${_mt67xx_lk_bin}" "${MT67XX_WRAP}"
    VERBATIM)
# The ELF travels with the image: the user debugging over serial wants
# symbols for the PC the exception report names.
add_custom_command(OUTPUT "${MT67XX_BOOT_DIR}/lk.elf"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MT67XX_BOOT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:MT67LK.elf>" "${MT67XX_BOOT_DIR}/lk.elf"
    DEPENDS MT67LK.elf
    VERBATIM)
# The boot dir carries its own provenance: FACTS.md (what each address is
# waiting on) plus the values this exact image was built with.
add_custom_command(OUTPUT "${MT67XX_BOOT_DIR}/FACTS.md"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MT67XX_BOOT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MINOS_ROOT}/../FACTS.md" "${MT67XX_BOOT_DIR}/FACTS.md"
    DEPENDS "${MINOS_ROOT}/../FACTS.md"
    VERBATIM)
file(GENERATE OUTPUT "${MT67XX_BOOT_DIR}/build-info.txt"
    CONTENT "device=${MT67XX_DEVICE}\nsoc=mt${MT67XX_SOC}\ncommit=${MVII_BUILD_COMMIT}\ndebug_uart=${MT67XX_DEBUG_UART}\nmembase=${MT67XX_MEMBASE}\nslot_size=${MT67XX_LK_SLOT_SIZE}\n")
add_custom_target(firmware ALL DEPENDS
    "${MT67XX_BOOT_DIR}/lk.bin" "${MT67XX_BOOT_DIR}/lk.elf"
    "${MT67XX_BOOT_DIR}/FACTS.md")

message(STATUS "mt67xx LK for ${MT67XX_DEVICE}: uart${MT67XX_DEBUG_UART} "
    "membase=${MT67XX_MEMBASE} slot=${MT67XX_LK_SLOT_SIZE} commit=${MVII_BUILD_COMMIT}")
message(STATUS "mt67xx LK priors in force: UART/GPT/WDT bases [mt6735], "
    "MEMBASE [mt6735], slot size UNVERIFIED -- see FACTS.md before flashing")
