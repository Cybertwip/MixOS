/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * stringsdb.h -- the strings database, and the translator that reads it.
 *
 * THE NAME IS NOT `strings.h', AND MUST NOT GO BACK TO BEING `strings.h'.  There is a
 * <strings.h> in the C library -- the one with strcasecmp in it -- and glibc's own
 * <string.h> includes it, at /usr/include/string.h:462, from inside __BEGIN_DECLS.
 * qmake puts -I. on every compile line and -I paths are searched before the system
 * ones, so a file of this name in this directory answers that include.  What glibc
 * then got, in the middle of an extern "C" block, was the QObject include below, and
 * the compiler said `error: template with C linkage' about eight hundred times in
 * <type_traits> before giving up.  It survived a long time because nothing here
 * included a system header early enough to trigger it; trace.cpp's <execinfo.h> does,
 * and the whole dashboard stopped building the day it was added.
 *
 * SIX LANGUAGES: English, French, Italian, German, Portuguese, Spanish.  That is
 * the EU handheld market this board is sold into, and it is a closed set on
 * purpose -- adding a seventh is a column in one table in stringsdb.cpp and nothing
 * else in the tree changes.
 *
 * WHY A COMPILED-IN TABLE AND NOT .ts/.qm.  Qt has a perfectly good translation
 * system and this deliberately does not use it, for three reasons that are all
 * about where this program runs:
 *
 *   - lrelease lives in qttools5-dev-tools, which is not in the armhf chroot and
 *     would be a new build dependency on the slowest step of the whole image
 *     build.  A table costs nothing at build time.
 *   - .qm files are files, and files have to be staged into the payload, found at
 *     runtime, and kept in step with the binary.  On a device whose OS partition
 *     is mounted read-only on several recovery paths, a translation that lives
 *     inside .rodata cannot go missing and cannot be half-updated.
 *   - The user asked for a strings database.  This is literally one: a table of
 *     phrases with a column per language.
 *
 * The cost is real and worth naming.  There are no numerus forms, so `%n' is
 * BANNED in this codebase's tr() calls -- QCoreApplication::translate falls back
 * to the source text when a translator returns nothing, and the source text is
 * where the %n is, so it would reach the glass unsubstituted.  Write
 * `n == 1 ? tr("1 file") : tr("%1 files").arg(n)' instead.  Context and
 * disambiguation are ignored too: a phrase means one thing in this program.
 *
 * THE ENGLISH IS THE KEY.  Column 0 of the table is both the lookup key and the
 * English text, so source that has never been touched by a translator is already
 * correct English, and a phrase that is missing a French cell falls back to
 * English rather than to an identifier.  That is the failure mode you want on a
 * handheld: a half-translated screen is usable, a screen full of
 * MIXDASH_SETTINGS_TITLE is not.
 */
#ifndef MIXDASH_STRINGSDB_H
#define MIXDASH_STRINGSDB_H

#include <QObject>
#include <QString>

namespace Lang {

/*
 * Column order in the table, and the number stored in the settings file is NOT
 * this -- see Strings::code().  An enum in an INI file is a value that changes
 * meaning when somebody inserts a language in the middle; a two-letter code does
 * not, and it is readable by whoever opens the file on a PC.
 */
enum Id {
    English = 0,
    French,
    Italian,
    German,
    Portuguese,
    Spanish,
    Count
};

} /* namespace Lang */

class Strings : public QObject
{
    Q_OBJECT

public:
    static Strings &instance();

    /*
     * Build the translator, install it on qApp, and set the language to whatever
     * Settings remembers -- or, the first time the device is used, to whatever
     * the environment asks for.  Call it once, after QApplication exists and
     * before anything builds a string.
     */
    static void install();

    int language() const { return m_language; }
    /* Writes it through to Settings and emits languageChanged().  Every page
     * refills its rows in onEnter(), so most of the shell retranslates by being
     * walked back to; what does not is rebuilt by Dashboard::retranslate(). */
    void setLanguage(int id);

    /* "Français" -- the name of the language in itself, which is the only name a
     * user who cannot read the current one will recognise. */
    static QString nativeName(int id);
    /* "French" -- for the second column, and for someone reading the screen out
     * over a telephone. */
    static QString englishName(int id);
    /* The two-letter ISO 639-1 code, which is what goes in the settings file. */
    static QString code(int id);
    /* Lang::English for anything unrecognised, including an empty string. */
    static int fromCode(const QString &code);
    /*
     * What LC_ALL / LC_MESSAGES / LANG asks for, or -1 if they ask for nothing
     * this program has.  -1 rather than English so the caller can tell "the
     * environment wants English" from "the environment said nothing", which is
     * the difference between honouring a choice and guessing.
     */
    static int fromEnvironment();

signals:
    void languageChanged();

private:
    Strings() {}

    int m_language = Lang::English;
};

#endif /* MIXDASH_STRINGSDB_H */
