/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#ifndef ISOTAB_PROFILE_RESET_H
#define ISOTAB_PROFILE_RESET_H
#include <linux/fs.h>

/* A stale Chromium marker is exchanged with our reservation atomically. If
 * another browser replaced it first, inspect the displaced marker and undo
 * the exchange rather than unlinking that browser's lock. */
static gboolean reserve_chromium_at(int root, const char *token, GError **error)
{
    if (symlinkat(token, root, "SingletonLock") == 0) return TRUE;
    if (errno != EEXIST || !chromium_available_at(root, error)) {
        if (error && !*error) file_error(error, "Cannot reserve profile");
        return FALSE;
    }
    char *old = read_link_at(root, "SingletonLock");
    char *uuid = g_uuid_string_random();
    char *temporary = g_strconcat(".isotab-reserve-", uuid, NULL);
    g_free(uuid);
    gboolean ok = chromium_marker_stale(old) && symlinkat(token, root, temporary) == 0;
    if (ok && syscall(SYS_renameat2, root, temporary, root, "SingletonLock", RENAME_EXCHANGE) == 0) {
        char *displaced = read_link_at(root, temporary);
        char *current = read_link_at(root, "SingletonLock");
        ok = !g_strcmp0(displaced, old) && !g_strcmp0(current, token);
        /* Validate the complete displaced snapshot again after exchange. */
        ok = ok && chromium_marker_stale(displaced);

        if (!ok && !g_strcmp0(current, token))
            (void)syscall(SYS_renameat2, root, temporary, root, "SingletonLock", RENAME_EXCHANGE);
        if (ok || !g_strcmp0(displaced, token)) unlinkat(root, temporary, 0);
        else {
            char *after = read_link_at(root, temporary);
            if (!g_strcmp0(after, token)) unlinkat(root, temporary, 0);
            g_free(after);
        }
        g_free(current); g_free(displaced);
    } else {
        ok = FALSE;
        unlinkat(root, temporary, 0);
    }
    if (!ok && error && !*error)
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_AGAIN, "The browser lock changed. No profile data was removed.");
    g_free(temporary); g_free(old);
    return ok;
}

static void release_chromium_at(int root, const char *token)
{
    char *current = read_link_at(root, "SingletonLock");
    if (!g_strcmp0(current, token)) unlinkat(root, "SingletonLock", 0);
    g_free(current);
}

/* Recover a reservation left by an interrupted reset before native launch.
 * The atomic exchange path also safely handles ordinary stale local markers. */
static gboolean prepare_chromium_launch(const char *path, GError **error)
{
    int root = open_profile_directory(path, error);
    if (root < 0) return FALSE;
    int guard = open_lock_at(root, ".isotab.lock");
    gboolean ok = guard >= 0 && flock(guard, LOCK_EX | LOCK_NB) == 0;
    char *token = g_strdup_printf("%s.isotab-clear-%ld", g_get_host_name(), (long)getpid());
    if (ok) ok = reserve_chromium_at(root, token, error);
    if (ok) release_chromium_at(root, token);
    if (!ok && error && !*error) file_error(error, "Could not safely prepare browser launch");
    if (guard >= 0) close(guard);
    close(root); g_free(token);
    return ok;
}

typedef struct {
    int parent_old, parent_new, old_root, new_root, old_lock, new_lock;
    char *old_name, *new_name, *token;
    BrowserFamily family;
    gboolean exchanged;
} ProfileReset;

static void reset_release(ProfileReset *reset)
{
    if (!reset) return;
    if (reset->family == FAMILY_CHROMIUM && reset->token) {
        if (reset->old_root >= 0) release_chromium_at(reset->old_root, reset->token);
        if (reset->new_root >= 0) release_chromium_at(reset->new_root, reset->token);
    }
    int fds[] = {reset->old_lock, reset->new_lock, reset->old_root, reset->new_root, reset->parent_old, reset->parent_new};
    for (guint i = 0; i < G_N_ELEMENTS(fds); i++) if (fds[i] >= 0) close(fds[i]);
    g_free(reset->old_name); g_free(reset->new_name); g_free(reset->token); g_free(reset);
}

static gboolean reset_rollback(ProfileReset *reset, GError **error)
{
    if (!reset->exchanged) return TRUE;
    if (syscall(SYS_renameat2, reset->parent_old, reset->old_name,
        reset->parent_new, reset->new_name, RENAME_EXCHANGE) != 0) {
        file_error(error, "Rollback failed; original data is intact in Recovery");
        return FALSE;
    }
    reset->exchanged = FALSE;
    return TRUE;
}

/* No original file is deleted. Both native profile locks stay held until
 * metadata is committed or the directory exchange has been rolled back. */
static ProfileReset *reset_prepare(const char *old_path, const char *new_path,
                                   BrowserFamily family, GError **error)
{
    ProfileReset *r = g_new0(ProfileReset, 1);
    r->parent_old = r->parent_new = r->old_root = r->new_root = r->old_lock = r->new_lock = -1;
    r->family = family;
    char *parent = g_path_get_dirname(old_path);
    r->parent_old = open_directory(parent); g_free(parent);
    parent = g_path_get_dirname(new_path);
    r->parent_new = open_profile_directory(parent, NULL);
    if (r->parent_new < 0) r->parent_new = ensure_profile_directory(parent, error);
    g_free(parent);
    r->old_name = g_path_get_basename(old_path);
    r->new_name = g_path_get_basename(new_path);
    if (r->parent_old < 0 || r->parent_new < 0) goto failed;
    r->old_root = open_profile_directory(old_path, error);
    if (r->old_root < 0) goto failed;
    if (mkdirat(r->parent_new, r->new_name, 0700) != 0) goto failed;
    r->new_root = open_profile_directory(new_path, error);
    if (r->new_root < 0) goto failed;
    struct stat old_stat, new_stat, named_stat;
    if (fstat(r->old_root, &old_stat) != 0 || fstat(r->new_root, &new_stat) != 0) goto failed;
    if (family == FAMILY_FIREFOX) {
        r->old_lock = lock_profile_at(r->old_root, error);
        r->new_lock = r->old_lock < 0 ? -1 : lock_profile_at(r->new_root, error);
        if (r->old_lock < 0 || r->new_lock < 0) goto failed;
    } else {
        r->old_lock = open_lock_at(r->old_root, ".isotab.lock");
        r->new_lock = open_lock_at(r->new_root, ".isotab.lock");
        if (r->old_lock < 0 || r->new_lock < 0 || flock(r->old_lock, LOCK_EX | LOCK_NB) != 0 ||
            flock(r->new_lock, LOCK_EX | LOCK_NB) != 0) goto failed;
        r->token = g_strdup_printf("%s.isotab-clear-%ld", g_get_host_name(), (long)getpid());
        if (!reserve_chromium_at(r->old_root, r->token, error) ||
            !reserve_chromium_at(r->new_root, r->token, error)) goto failed;
    }
    if (fstatat(r->parent_old, r->old_name, &named_stat, AT_SYMLINK_NOFOLLOW) != 0 ||
        old_stat.st_ino != named_stat.st_ino || old_stat.st_dev != named_stat.st_dev) { errno = EAGAIN; goto failed; }
    if (fstatat(r->parent_new, r->new_name, &named_stat, AT_SYMLINK_NOFOLLOW) != 0 ||
        new_stat.st_ino != named_stat.st_ino || new_stat.st_dev != named_stat.st_dev) { errno = EAGAIN; goto failed; }
    if (syscall(SYS_renameat2, r->parent_old, r->old_name, r->parent_new, r->new_name, RENAME_EXCHANGE) != 0) goto failed;
    r->exchanged = TRUE;
    if (fsync(r->parent_old) != 0 || fsync(r->parent_new) != 0) {
        file_error(error, "Profile reset could not be made durable; original data is retained");
        GError *rollback_error = NULL;
        if (!reset_rollback(r, &rollback_error)) {
            g_clear_error(error);
            g_propagate_error(error, rollback_error);
        }
        goto failed;
    }
    return r;
failed:
    if (error && !*error) file_error(error, "Could not reset profile; original data is unchanged");
    reset_release(r);
    return NULL;
}
#endif
