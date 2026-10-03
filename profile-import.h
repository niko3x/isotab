/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#ifndef ISOTAB_PROFILE_IMPORT_H
#define ISOTAB_PROFILE_IMPORT_H

enum { PROFILE_UNKNOWN, PROFILE_FIREFOX, PROFILE_CHROMIUM, PROFILE_MIXED };

static gboolean profile_marker_at(int root, const char *name, mode_t type)
{
    struct stat st;
    return fstatat(root, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && (st.st_mode & S_IFMT) == type;
}

static int profile_family_at(int root)
{
    gboolean firefox = profile_marker_at(root, "prefs.js", S_IFREG) ||
        profile_marker_at(root, "cookies.sqlite", S_IFREG) ||
        profile_marker_at(root, "places.sqlite", S_IFREG) ||
        profile_marker_at(root, "compatibility.ini", S_IFREG) ||
        profile_marker_at(root, "key4.db", S_IFREG);
    gboolean chromium = profile_marker_at(root, "Local State", S_IFREG) ||
        profile_marker_at(root, "Default", S_IFDIR) ||
        profile_marker_at(root, "SingletonLock", S_IFLNK);
    return firefox && chromium ? PROFILE_MIXED : firefox ? PROFILE_FIREFOX :
        chromium ? PROFILE_CHROMIUM : PROFILE_UNKNOWN;
}

static gboolean validate_import_profile(const char *path, Browser *browser, GError **error)
{
    if (!browser) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Choose the original browser explicitly before importing.");
        return FALSE;
    }
    int root = open_profile_directory(path, error);
    if (root < 0) return FALSE;
    int family = profile_family_at(root);
    close(root);
    if (family == PROFILE_MIXED ||
        (family == PROFILE_FIREFOX && browser->family != FAMILY_FIREFOX) ||
        (family == PROFILE_CHROMIUM && browser->family != FAMILY_CHROMIUM)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
            family == PROFILE_MIXED ?
            "This folder contains both Firefox and Chromium data. Resolve the mixed profile before importing; its files were not changed." :
            "This folder contains %s data and cannot be imported as %s. Choose its original browser.",
            family == PROFILE_FIREFOX ? "Firefox" : "Chromium", browser->name);
        return FALSE;
    }
    return TRUE;
}
#endif
