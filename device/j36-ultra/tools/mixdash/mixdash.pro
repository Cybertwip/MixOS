# SPDX-License-Identifier: MS-PL
# Copyright (c) 2025-2026 the MixOS project and contributors
# See device/j36-ultra/LICENSE for the licence text and what it covers.
#
# Built by qmake inside the armhf chroot build-in-vm.sh already keeps for
# EmulationStation, against Debian's own qtbase5-dev.  Nothing here is
# cross-compiled and nothing is built from source but the files listed below.
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

# -rdynamic puts this binary's own symbols in its .dynsym, which is what makes
# alloc.cpp's backtrace name mixdash's frames rather than print bare addresses.
# It matters HERE and not on a development machine because build-in-vm.sh strips
# the shipped binary: strip removes .symtab, so .dynsym is the only symbol table
# left on the card, and without this flag it holds nothing but imports.  Qt's own
# frames resolve either way -- a shared library exports its symbols already.
QMAKE_LFLAGS += -rdynamic

SOURCES += \
    main.cpp \
    trace.cpp \
    alloc.cpp \
    dashboard.cpp \
    widgets.cpp \
    joypad.cpp

HEADERS += \
    dashboard.h \
    trace.h \
    widgets.h \
    joypad.h \
    theme.h
