/*
 This file is part of the Strada Language (https://github.com/mjflick/strada-lang).
 Copyright (c) 2026 Michael J. Flickinger

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, version 2.

 This program is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * gettext_wrapper.c - GNU gettext wrapper for Strada extern "C"
 *
 * This library provides thin wrappers around gettext functions.
 * Functions receive raw C types (char*, int) directly from Strada's
 * extern "C" mechanism.
 *
 * Compile as object file to link statically:
 *   gcc -c -o gettext_wrapper.o gettext_wrapper.c
 *
 * Or as shared library for dynamic linking:
 *   gcc -shared -fPIC -o libgettext_wrapper.so gettext_wrapper.c
 *
 * On systems with libintl as a separate library, add -lintl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

/* Check for gettext availability */
#if defined(__GLIBC__) || defined(HAVE_LIBINTL)
#include <libintl.h>
#define HAS_GETTEXT 1
#else
#ifdef __has_include
#if __has_include(<libintl.h>)
#include <libintl.h>
#define HAS_GETTEXT 1
#endif
#endif
#endif

#ifndef HAS_GETTEXT
#define HAS_GETTEXT 0
#endif

/* ===== Locale Functions ===== */

/* setlocale wrapper
 * Args: category (int), locale (char*)
 * Returns: char* (current locale or NULL on error)
 */
const char* gt_setlocale(int category, const char *locale) {
    const char *result = setlocale(category, locale);
    return result ? result : "";
}

/* ===== gettext Functions ===== */

#if HAS_GETTEXT

/* textdomain wrapper */
const char* gt_textdomain(const char *domain) {
    const char *result = textdomain(domain);
    return result ? result : "";
}

/* bindtextdomain wrapper */
const char* gt_bindtextdomain(const char *domain, const char *dirname) {
    const char *result = bindtextdomain(domain, dirname);
    return result ? result : "";
}

/* bind_textdomain_codeset wrapper */
const char* gt_bind_textdomain_codeset(const char *domain, const char *codeset) {
    const char *result = bind_textdomain_codeset(domain, codeset);
    return result ? result : "";
}

/* gettext wrapper */
const char* gt_gettext(const char *msgid) {
    if (!msgid) return "";
    return gettext(msgid);
}

/* dgettext wrapper */
const char* gt_dgettext(const char *domain, const char *msgid) {
    if (!msgid) return "";
    return dgettext(domain, msgid);
}

/* ngettext wrapper */
const char* gt_ngettext(const char *singular, const char *plural, unsigned long n) {
    if (!singular || !plural) return "";
    return ngettext(singular, plural, n);
}

/* dngettext wrapper */
const char* gt_dngettext(const char *domain, const char *singular, const char *plural, unsigned long n) {
    if (!singular || !plural) return "";
    return dngettext(domain, singular, plural, n);
}

/* dcgettext wrapper */
const char* gt_dcgettext(const char *domain, const char *msgid, int category) {
    if (!msgid) return "";
    return dcgettext(domain, msgid, category);
}

#else /* No gettext available - stub implementations */

const char* gt_textdomain(const char *domain) {
    (void)domain;
    return "";
}

const char* gt_bindtextdomain(const char *domain, const char *dirname) {
    (void)domain;
    (void)dirname;
    return "";
}

const char* gt_bind_textdomain_codeset(const char *domain, const char *codeset) {
    (void)domain;
    (void)codeset;
    return "";
}

const char* gt_gettext(const char *msgid) {
    return msgid ? msgid : "";
}

const char* gt_dgettext(const char *domain, const char *msgid) {
    (void)domain;
    return msgid ? msgid : "";
}

const char* gt_ngettext(const char *singular, const char *plural, unsigned long n) {
    if (n == 1) return singular ? singular : "";
    return plural ? plural : "";
}

const char* gt_dngettext(const char *domain, const char *singular, const char *plural, unsigned long n) {
    (void)domain;
    if (n == 1) return singular ? singular : "";
    return plural ? plural : "";
}

const char* gt_dcgettext(const char *domain, const char *msgid, int category) {
    (void)domain;
    (void)category;
    return msgid ? msgid : "";
}

#endif /* HAS_GETTEXT */

/* ===== Utility Functions ===== */

/* Check if gettext is available at runtime */
int gt_has_gettext(void) {
    return HAS_GETTEXT;
}

/* Get LC_* constants for use in Strada */
int gt_LC_ALL(void) { return LC_ALL; }
int gt_LC_COLLATE(void) { return LC_COLLATE; }
int gt_LC_CTYPE(void) { return LC_CTYPE; }
int gt_LC_MESSAGES(void) {
#ifdef LC_MESSAGES
    return LC_MESSAGES;
#else
    return 5; /* Common default */
#endif
}
int gt_LC_MONETARY(void) { return LC_MONETARY; }
int gt_LC_NUMERIC(void) { return LC_NUMERIC; }
int gt_LC_TIME(void) { return LC_TIME; }
