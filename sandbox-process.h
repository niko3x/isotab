/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#ifndef ISOTAB_SANDBOX_PROCESS_H
#define ISOTAB_SANDBOX_PROCESS_H

/* Markers written in a sandbox contain its innermost PID, not a host PID. */
static pid_t namespace_pid_from_status(const char *status)
{
    const char *line = g_str_has_prefix(status, "NSpid:") ? status : strstr(status, "\nNSpid:");
    if (!line) return -1;
    if (*line == '\n') line++;
    line += 6;
    guint64 pid = 0;
    while (*line && *line != '\n') {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '\n' || !*line) break;
        if (!g_ascii_isdigit(*line)) return -1;
        char *end;
        errno = 0;
        pid = g_ascii_strtoull(line, &end, 10);
        if (errno || pid == 0 || pid > INT_MAX) return -1;
        line = end;
    }
    return pid ? (pid_t)pid : -1;
}

static gboolean process_uses_root(pid_t pid, int root, gboolean *uncertain)
{
    char *link = g_strdup_printf("/proc/self/fd/%d", root);
    char *directory = g_file_read_link(link, NULL);
    if (!directory) *uncertain = TRUE;
    g_free(link);
    char *proc = g_strdup_printf("/proc/%ld", (long)pid);
    char *path = g_build_filename(proc, "cmdline", NULL);
    char *args = NULL;
    gsize length = 0;
    gboolean readable = g_file_get_contents(path, &args, &length, NULL);
    gboolean match = FALSE;
    const char *next = NULL;
    for (gsize i = 0; readable && i < length;) {
        const char *arg = args + i;
        const char *end = memchr(arg, '\0', length - i);
        if (!end) break;
        const char *profile = next ? arg : NULL;
        next = NULL;
        if (!strcmp(arg, "--profile") || !strcmp(arg, "-profile")) next = arg;
        else if (g_str_has_prefix(arg, "--profile=")) profile = arg + 10;
        else if (g_str_has_prefix(arg, "--user-data-dir=")) profile = arg + 16;
        if (profile && g_path_is_absolute(profile)) {
            char *canonical = g_canonicalize_filename(profile, NULL);
            match = directory && !strcmp(canonical, directory);
            g_free(canonical);
            if (match) break;
        }
        i = end - args + 1;
    }
    g_free(args); g_free(path);
    /* Chromium headless processes may flatten their command line. */
    path = g_build_filename(proc, "fd", NULL);
    GDir *fds = !match ? g_dir_open(path, 0, NULL) : NULL;
    const char *entry;
    char *prefix = directory ? g_strconcat(directory, "/", NULL) : NULL;
    while (fds && !match && (entry = g_dir_read_name(fds))) {
        char *fd_path = g_build_filename(path, entry, NULL);
        char *target = g_file_read_link(fd_path, NULL);
        if (!target && errno == EACCES) *uncertain = TRUE;
        match = target && directory && (!strcmp(target, directory) || (prefix && g_str_has_prefix(target, prefix)));
        g_free(target); g_free(fd_path);
    }
    if (!match && !fds) *uncertain = TRUE;
    if (fds) g_dir_close(fds);
    g_free(prefix); g_free(path); g_free(proc); g_free(directory);
    return match;
}

static pid_t sandbox_profile_owner(int root, pid_t inner_pid, gboolean *uncertain)
{
    GDir *processes = g_dir_open("/proc", 0, NULL);
    if (!processes) { *uncertain = TRUE; return 0; }
    const char *entry;
    pid_t result = 0;
    while ((entry = g_dir_read_name(processes))) {
        char *end;
        guint64 candidate = g_ascii_strtoull(entry, &end, 10);
        if (!*entry || *end || candidate == 0 || candidate > INT_MAX) continue;
        char *proc = g_build_filename("/proc", entry, NULL);
        struct stat st;
        gboolean owned = stat(proc, &st) == 0 && st.st_uid == geteuid();
        char *status_path = g_build_filename(proc, "status", NULL);
        char *status = NULL;
        gboolean readable = owned && g_file_get_contents(status_path, &status, NULL, NULL);
        if (readable && namespace_pid_from_status(status) == inner_pid && process_uses_root(candidate, root, uncertain)) result = candidate;
        if (owned && !readable && kill(candidate, 0) == 0) *uncertain = TRUE;
        g_free(status); g_free(status_path); g_free(proc);
        if (result) break;
    }
    g_dir_close(processes);
    return result;
}
#endif
