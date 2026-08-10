# SPDX-License-Identifier: MS-PL
# Copyright (c) 2025-2026 the MixOS project and contributors
# See device/j36-ultra/LICENSE for the licence text and what it covers.
#
# Built by qmake inside the armhf chroot build-in-vm.sh already keeps for
# EmulationStation, against Debian's own qtbase5-dev.  Nothing here is
# cross-compiled and nothing is built from source but these five files.
#
# QT does NOT include opengl, and that is the point of the whole exercise: the
# platform plugin this runs on is linuxfb, which writes into the framebuffer the
# LK is already scanning out.

QT += core gui widgets
CONFIG += c++11 release
CONFIG -= app_bundle debug_and_release

TEMPLATE = app
TARGET = mixdash

QMAKE_CXXFLAGS += -Wall -Wextra

SOURCES += \
    main.cpp \
    dashboard.cpp \
    widgets.cpp \
    joypad.cpp

HEADERS += \
    dashboard.h \
    widgets.h \
    joypad.h \
    theme.h
