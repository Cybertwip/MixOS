# J36 firmware sources

Imported from PowerEngine/OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers on
2026-09-24. The build is derived from
OS/MVII/Architecture/armv7/mediatek-j36-ultra/CMakeLists.txt; the two image
packaging scripts come from OS/MVII/scripts. Existing source notices are
retained. This import does not assign a new license to the PowerEngine code.

Local changes provide a standalone build and an explicit batteryless mode.
All source dependencies are in this checkout. The build uses MixOS.jpg from
device/j36-ultra/resources for the boot logo. The original PowerEngine tree
remains independent.

This builds both LK variants and the native BROM flashing payload. It does
not build or replace the board's proprietary stock preloader, which still
initializes DRAM and supplies the initial PMIC state.
