/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#ifndef ISOTAB_PROFILE_PURGE_H
#define ISOTAB_PROFILE_PURGE_H

static G_GNUC_UNUSED char *recovery_trash_path(const char *id)
{
    return g_build_filename(data_dir, "trash", id, NULL);
}

static char *recovery_trash_path_for_profile(const char *profile, const char *id)
{
    char *parent = g_path_get_dirname(profile);
    char *name = g_path_get_basename(parent);
    char *store = !strcmp(name, "profiles") ? g_path_get_dirname(parent) : g_strdup(data_dir);
    char *path = g_build_filename(store, "trash", id, NULL);
    g_free(store); g_free(parent); g_free(name);
    return path;
}

static gboolean path_missing(const char *path)
{
    char *parent = g_path_get_dirname(path);
    char *name = g_path_get_basename(path);
    int fd = open_directory(parent);
    gboolean missing;
    if (fd < 0) missing = errno == ENOENT;
    else {
        struct stat st;
        missing = fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT;
        close(fd);
    }
    g_free(parent); g_free(name);
    return missing;
}

/* Only the explicit permanent-delete action uses this operation. Detach the
 * natively locked profile into private trash before recursive removal, so a
 * browser opening its former location cannot race with deletion of that data.
 * The persisted deletion_started flag prevents restoration after interruption. */
static gboolean purge_profile(const char *original, const char *id, BrowserFamily family, gboolean *started, GError **error)
{
    char *destination = recovery_trash_path_for_profile(original, id);
    char *trash = g_path_get_dirname(destination);
    char *parent = g_path_get_dirname(original);
    char *name = g_path_get_basename(original);
    int trash_parent = -1, original_parent = -1, root = -1, lock = -1;
    int tor_lock = -1;
    char *token = NULL;
    gboolean ok = FALSE;
    gboolean detached = !path_missing(destination);
    *started = detached;
    if (!detached && path_missing(original)) { ok = TRUE; goto done; }
    trash_parent = ensure_profile_directory(trash, error);
    if (trash_parent < 0) goto done;
    root = open_profile_directory(detached ? destination : original, error);
    if (root < 0) goto done;
    if (family == FAMILY_FIREFOX) {
        lock = lock_profile_at(root, error);
        if (lock < 0) goto done;
        tor_lock = tor_data_guard_at(root, error);
        if (tor_lock == -1) goto done;
    } else {
        lock = open_lock_at(root, ".isotab.lock");
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) goto failed;
        token = g_strdup_printf("%s.isotab-clear-%ld", g_get_host_name(), (long)getpid());
        if (!reserve_chromium_at(root, token, error)) goto done;
    }
    struct stat pinned, named;
    if (fstat(root, &pinned) != 0) goto failed;
    if (!detached) {
        original_parent = open_directory(parent);
        if (original_parent < 0 || fstatat(original_parent, name, &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            pinned.st_ino != named.st_ino || pinned.st_dev != named.st_dev) { errno = EAGAIN; goto failed; }
        if (syscall(SYS_renameat2, original_parent, name, trash_parent, id, RENAME_NOREPLACE) != 0) goto failed;
        *started = TRUE;
        if (fsync(original_parent) != 0 || fsync(trash_parent) != 0) goto failed;
    }
    if (fstatat(trash_parent, id, &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        pinned.st_ino != named.st_ino || pinned.st_dev != named.st_dev) { errno = EAGAIN; goto failed; }
    const char *keep[] = {family == FAMILY_FIREFOX ? ".parentlock" : ".isotab.lock",
        family == FAMILY_CHROMIUM ? "SingletonLock" : NULL, ".isotab-sandbox", ".isotab-tor"};
    if (!delete_contents_preserving_at(root, keep, G_N_ELEMENTS(keep), pinned.st_dev, 0, error)) goto done;
    if (family == FAMILY_CHROMIUM) {
        char *current = read_link_at(root, "SingletonLock");
        gboolean ours = !g_strcmp0(current, token);
        g_free(current);
        if (!ours) { errno = EAGAIN; goto failed; }
        if (unlinkat(root, "SingletonLock", 0) != 0) goto failed;
    }
    if (unlinkat(root, family == FAMILY_FIREFOX ? ".parentlock" : ".isotab.lock", 0) != 0) goto failed;
    const char *tags[] = {".isotab-sandbox", ".isotab-tor"};
    for (guint i = 0; i < G_N_ELEMENTS(tags); i++) {
        if (unlinkat(root, tags[i], 0) != 0 && errno != ENOENT) goto failed;
    }
    if (fstatat(trash_parent, id, &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        pinned.st_ino != named.st_ino || pinned.st_dev != named.st_dev) { errno = EAGAIN; goto failed; }
    if (unlinkat(trash_parent, id, AT_REMOVEDIR) != 0 || fsync(trash_parent) != 0) goto failed;
    ok = TRUE;
    goto done;
failed:
    if (error && !*error) file_error(error, "Permanent deletion could not finish");
done:
    if (root >= 0 && token) release_chromium_at(root, token);
    if (lock >= 0) close(lock);
    if (tor_lock >= 0) close(tor_lock);
    if (root >= 0) close(root);
    if (trash_parent >= 0) close(trash_parent);
    if (original_parent >= 0) close(original_parent);
    g_free(token); g_free(trash); g_free(destination); g_free(parent); g_free(name);
    return ok;
}
#endif
