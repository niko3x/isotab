/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#ifndef ISOTAB_SAFE_FILES_H
#define ISOTAB_SAFE_FILES_H
#include <dirent.h>

static void file_error(GError **error, const char *operation)
{
    int saved = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved),
        "%s: %s", operation, g_strerror(saved));
}

/* Walk from / without following any path component supplied by the filesystem. */
static int open_directory(const char *path)
{
    if (!g_path_is_absolute(path)) { errno = EINVAL; return -1; }
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    char **parts = g_strsplit(path, "/", -1);
    for (guint i = 0; parts[i]; i++) {
        if (!*parts[i] || !strcmp(parts[i], ".")) continue;
        if (!strcmp(parts[i], "..")) { close(fd); fd = -1; errno = EINVAL; break; }
        int next = openat(fd, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int saved = errno;
        close(fd);
        fd = next;
        if (fd < 0) { errno = saved; break; }
    }
    g_strfreev(parts);
    return fd;
}

static int open_profile_directory(const char *path, GError **error)
{
    int fd = open_directory(path);
    struct stat st;
    if (fd >= 0) {
        if (fstat(fd, &st) == 0 && st.st_uid == geteuid() && !(st.st_mode & 0022)) return fd;
        close(fd);
        errno = EACCES;
    }
    file_error(error, "Profile must be an owned directory without symlinks or shared write access");
    return -1;
}

/* Create only the final component through an already validated parent. */
static int ensure_profile_directory(const char *path, GError **error)
{
    char *parent = g_path_get_dirname(path);
    char *name = g_path_get_basename(path);
    int parent_fd = open_profile_directory(parent, error);
    int fd = -1;
    if (parent_fd >= 0) {
        if (!strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/')) {
            errno = EINVAL;
            file_error(error, "Invalid profile directory name");
        } else if (mkdirat(parent_fd, name, 0700) != 0 && errno != EEXIST) {
            file_error(error, "Cannot create profile directory");
        } else {
            fd = open_profile_directory(path, error);
        }
        close(parent_fd);
    }
    g_free(parent);
    g_free(name);
    return fd;
}

static int open_lock_at(int dirfd, const char *name)
{
    int fd = openat(dirfd, name, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
        st.st_uid == geteuid() && !(st.st_mode & 0022)) return fd;
    close(fd);
    errno = EACCES;
    return -1;
}

static char *read_link_at(int dirfd, const char *name)
{
    char buf[4096];
    ssize_t n = readlinkat(dirfd, name, buf, sizeof(buf) - 1);
    if (n < 0 || n == (ssize_t)sizeof(buf) - 1) return NULL;
    buf[n] = '\0';
    return g_strdup(buf);
}

static gboolean delete_entry_at(int parent, const char *name, dev_t device,
                                unsigned depth, GError **error);

static gboolean delete_contents_preserving_at(int fd, const char *const *keep, gsize keep_count,
                                             dev_t device, unsigned depth, GError **error)
{
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (copy < 0) { file_error(error, "Cannot open directory"); return FALSE; }
    DIR *entries = fdopendir(copy);
    if (!entries) { close(copy); file_error(error, "Cannot list directory"); return FALSE; }
    gboolean ok = TRUE;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(entries);
        if (!entry) {
            if (errno) { file_error(error, "Cannot read directory"); ok = FALSE; }
            break;
        }
        const char *name = entry->d_name;
        gboolean retained = !strcmp(name, ".") || !strcmp(name, "..");
        for (gsize i = 0; i < keep_count; i++) retained |= !g_strcmp0(name, keep[i]);
        if (retained) continue;
        if (!delete_entry_at(fd, name, device, depth + 1, error)) { ok = FALSE; break; }
    }
    closedir(entries);
    return ok;
}

static gboolean delete_contents_at(int fd, const char *keep1, const char *keep2,
                                   dev_t device, unsigned depth, GError **error)
{
    const char *keep[] = {keep1, keep2};
    return delete_contents_preserving_at(fd, keep, G_N_ELEMENTS(keep), device, depth, error);
}

static gboolean delete_entry_at(int parent, const char *name, dev_t device,
                                unsigned depth, GError **error)
{
    struct stat before, pinned, current;
    if (depth > 128) { errno = ELOOP; goto failed; }
    if (fstatat(parent, name, &before, AT_SYMLINK_NOFOLLOW) != 0) goto failed;
    if (!S_ISDIR(before.st_mode)) {
        if (unlinkat(parent, name, 0) == 0) return TRUE;
        goto failed;
    }
    if (before.st_dev != device) { errno = EXDEV; goto failed; }
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) goto failed;
    if (fstat(fd, &pinned) != 0 || before.st_ino != pinned.st_ino || before.st_dev != pinned.st_dev) {
        close(fd); errno = EAGAIN; goto failed;
    }
    gboolean ok = delete_contents_at(fd, NULL, NULL, device, depth, error);
    close(fd);
    if (!ok) return FALSE;
    if (fstatat(parent, name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        current.st_ino != pinned.st_ino || current.st_dev != pinned.st_dev) {
        errno = EAGAIN; goto failed;
    }
    if (unlinkat(parent, name, AT_REMOVEDIR) == 0) return TRUE;
failed:
    file_error(error, "Cannot safely delete profile entry");
    return FALSE;
}

/* Used for temporary test trees; production Clear retains its open root and locks. */
static G_GNUC_UNUSED gboolean delete_tree(GFile *file, GError **error)
{
    char *path = g_file_get_path(file);
    if (!path) { errno = EINVAL; file_error(error, "Not a local path"); return FALSE; }
    char *parent = g_path_get_dirname(path);
    char *name = g_path_get_basename(path);
    int fd = open_directory(parent);
    struct stat st;
    gboolean ok = FALSE;
    if (fd < 0 || fstat(fd, &st) != 0) file_error(error, "Cannot open parent directory");
    else ok = delete_entry_at(fd, name, st.st_dev, 0, error);
    if (fd >= 0) close(fd);
    g_free(name); g_free(parent); g_free(path);
    return ok;
}
#endif
