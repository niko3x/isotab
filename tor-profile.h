/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#ifndef ISOTAB_TOR_PROFILE_H
#define ISOTAB_TOR_PROFILE_H
#define TOR_PREF_BEGIN "// IsoTab Tor paths v1\n"
#define TOR_PREF_END "// End IsoTab Tor paths\n"

static gboolean profile_tag_at(int root, const char *tag)
{
    struct stat st;
    return fstatat(root, tag, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
        st.st_uid == geteuid() && st.st_nlink == 1 && !(st.st_mode & 0022);
}

/* Tor may retain its data lock briefly after the browser's profile lock exits. */
static int tor_data_guard_at(int root, GError **error)
{
    if (!profile_tag_at(root, ".isotab-tor")) return -2;
    int data = openat(root, "tor-data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    /* A partial purge may have already removed this whole data subtree. */
    if (data < 0 && errno == ENOENT) return -2;
    if (data < 0) { file_error(error, "Cannot access this session's Tor data"); return -1; }
    struct stat st;
    int fd = -1;
    if (fstat(data, &st) == 0 && st.st_uid == geteuid() && !(st.st_mode & 0022)) fd = open_lock_at(data, "lock");
    else errno = EACCES;
    close(data);
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0 && fcntl(fd, F_SETLK, &lock) == 0) return fd;
    int saved = errno;
    if (fd >= 0) close(fd);
    errno = saved;
    file_error(error, "Tor is still using this session's data; wait for it to close");
    return -1;
}

static int ensure_browser_profile_root_for_home(Browser *browser, const char *data, const char *home, GError **error)
{
    if (browser->package != PACKAGE_SNAP) {
        char *profiles = browser_profile_root(browser, data);
        int fd = ensure_profile_directory(profiles, error);
        g_free(profiles); return fd;
    }
    char *path = g_strdup(home);
    const char *parts[] = {"snap", browser->application, "common", "isotab", "profiles"};
    int fd = -1;
    for (guint i = 0; i < G_N_ELEMENTS(parts); i++) {
        char *next = g_build_filename(path, parts[i], NULL);
        g_free(path); path = next;
        fd = ensure_profile_directory(path, error);
        if (fd < 0) break;
        if (i + 1 < G_N_ELEMENTS(parts)) { close(fd); fd = -1; }
    }
    g_free(path); return fd;
}

static int ensure_browser_profile_root(Browser *browser, const char *data, GError **error)
{
    return ensure_browser_profile_root_for_home(browser, data, g_get_home_dir(), error);
}

static char *js_path_string(const char *path)
{
    GString *out = g_string_new("\"");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p == '"' || *p == '\\') g_string_append_c(out, '\\');
        if (*p < 32) g_string_append_printf(out, "\\u%04x", *p);
        else g_string_append_c(out, *p);
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}

static gboolean prepare_tor_profile(Browser *browser, const char *profile, int root, GError **error)
{
    char *old = NULL;
    int fd = openat(root, "user.js", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() ||
            (st.st_mode & 0022) || st.st_size > 65536) { close(fd); errno = EACCES; file_error(error, "Unsafe Tor user.js file"); return FALSE; }
        GString *contents = g_string_new(NULL);
        char buffer[4096];
        ssize_t n;
        while ((n = read(fd, buffer, sizeof(buffer))) != 0) {
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 || contents->len + n > 65536) {
                close(fd); g_string_free(contents, TRUE); errno = EIO; file_error(error, "Cannot read Tor user.js"); return FALSE;
            }
            g_string_append_len(contents, buffer, n);
        }
        gsize old_size = contents->len;
        close(fd); old = g_string_free(contents, FALSE);
        if (strlen(old) != old_size || !g_utf8_validate(old, old_size, NULL) ||
            !g_str_has_prefix(old, TOR_PREF_BEGIN) || !strstr(old, TOR_PREF_END)) {
            g_free(old);
            g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                "This Tor profile contains an unmanaged user.js. Its contents were preserved. Use a fresh Tor Browser session.");
            return FALSE;
        }
    } else if (errno != ENOENT) { file_error(error, "Cannot read Tor user.js"); return FALSE; }
    char *tor_data = g_build_filename(profile, "tor-data", NULL);
    fd = ensure_profile_directory(tor_data, error);
    if (fd < 0) { g_free(old); g_free(tor_data); return FALSE; }
    close(fd);
    char *bundle = g_path_get_dirname(browser_tor_executable(browser));
    char *defaults = g_build_filename(bundle, "TorBrowser", "Data", "Tor", "torrc-defaults", NULL);
    char *torrc = g_build_filename(tor_data, "torrc", NULL);
    char *auth = g_build_filename(tor_data, "onion-auth", NULL);
    const char *keys[] = {"tordatadir", "torrc", "toronionauthdir", "torrc-defaults"};
    const char *values[] = {tor_data, torrc, auth, defaults};
    GString *prefs = g_string_new(TOR_PREF_BEGIN);
    for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
        char *quoted = js_path_string(values[i]);
        g_string_append_printf(prefs, "user_pref(\"extensions.torlauncher.%s_path\", %s);\n", keys[i], quoted);
        g_free(quoted);
    }
    /* Tor's per-process IPC directory avoids a shared TCP control port. */
    g_string_append(prefs, "user_pref(\"extensions.torlauncher.control_port_use_ipc\", true);\n");
    g_string_append(prefs, TOR_PREF_END);
    if (old) g_string_append(prefs, strstr(old, TOR_PREF_END) + strlen(TOR_PREF_END));
    char *path = g_strdup_printf("/proc/self/fd/%d/user.js", root);
    gboolean ok = g_file_set_contents_full(path, prefs->str, prefs->len,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
    if (ok) {
        int tag = open_lock_at(root, ".isotab-tor");
        if (tag < 0) { file_error(error, "Cannot mark the Tor profile"); ok = FALSE; }
        else close(tag);
    }
    g_free(path); g_string_free(prefs, TRUE); g_free(old); g_free(bundle);
    g_free(defaults); g_free(tor_data); g_free(torrc); g_free(auth);
    return ok;
}

static gboolean prepare_browser_profile(Browser *browser, const char *profile, GError **error)
{
    int root = open_profile_directory(profile, error);
    if (root < 0) return FALSE;
    gboolean ok = TRUE;
    if (browser_is_tor(browser)) ok = prepare_tor_profile(browser, profile, root, error);
    if (ok && (browser_is_flatpak(browser) || browser->package == PACKAGE_SNAP)) {
        int tag = open_lock_at(root, ".isotab-sandbox");
        if (tag < 0) { file_error(error, "Cannot mark the packaged profile"); ok = FALSE; }
        else close(tag);
    }
    close(root); return ok;
}

static gboolean mark_imported_package_profile(int root, Browser *browser, GError **error)
{
    if (!browser_is_flatpak(browser) && browser->package != PACKAGE_SNAP) return TRUE;
    int tag = open_lock_at(root, ".isotab-sandbox");
    if (tag < 0) { file_error(error, "Cannot mark the imported package profile safely"); return FALSE; }
    close(tag); return TRUE;
}

static char **browser_environment(Browser *browser)
{
    if (!browser_is_tor(browser)) return NULL;
    char **env = g_get_environ();
    /* External Tor overrides must not silently bypass this session's daemon. */
    const char *unset[] = {"TOR_TRANSPROXY", "TOR_SKIP_LAUNCH", "TOR_PROVIDER", "TOR_SOCKS_HOST", "TOR_SOCKS_IPC_PATH",
        "TOR_CONTROL_HOST", "TOR_CONTROL_PORT", "TOR_CONTROL_IPC_PATH", "TOR_CONTROL_PASSWD", "TOR_CONTROL_COOKIE_AUTH_FILE",
        "TOR_CONFIGURE_ONLY", "MOZ_BROWSER_TOOLBOX_PORT", NULL};
    for (guint i = 0; unset[i]; i++) env = g_environ_unsetenv(env, unset[i]);
    env = g_environ_setenv(env, "TOR_SOCKS_PORT", "0", TRUE);
    env = g_environ_setenv(env, "GSETTINGS_BACKEND", "memory", TRUE);
    env = g_environ_setenv(env, "__GL_SHADER_DISK_CACHE", "0", TRUE);
    return env;
}

static gboolean tor_daemon_matches(Browser *browser, const char *profile, pid_t pid)
{
    const char *exe = browser_tor_executable(browser);
    if (!exe) return FALSE;
    char *bundle = g_path_get_dirname(exe);
    char *expected = g_build_filename(bundle, "TorBrowser", "Tor", "tor", NULL);
    char *proc = g_strdup_printf("/proc/%ld", (long)pid);
    char *actual = g_build_filename(proc, "exe", NULL);
    struct stat owner, want, running;
    gboolean match = stat(proc, &owner) == 0 && owner.st_uid == geteuid() &&
        stat(expected, &want) == 0 && stat(actual, &running) == 0 &&
        want.st_dev == running.st_dev && want.st_ino == running.st_ino;
    g_free(actual);
    char *path = g_build_filename(proc, "cmdline", NULL);
    char *args = NULL; gsize size = 0;
    gboolean data_match = FALSE;
    if (match && g_file_get_contents(path, &args, &size, NULL)) {
        char *data_dir = g_build_filename(profile, "tor-data", NULL);
        gboolean after_data = FALSE;
        for (gsize i = 0; i < size;) {
            const char *arg = args + i;
            const char *end = memchr(arg, '\0', size - i);
            if (!end) break;
            if (after_data && !strcmp(arg, data_dir)) { data_match = TRUE; break; }
            after_data = !strcmp(arg, "DataDirectory");
            i = end - args + 1;
        }
        g_free(data_dir);
    }
    g_free(args); g_free(path); g_free(proc); g_free(expected); g_free(bundle);
    return match && data_match;
}

/* SIGTERM of Firefox does not always complete Tor's asynchronous JS shutdown.
 * Signal only a daemon from this bundle with this exact data directory, pinned
 * by a process descriptor and verified again before signaling. */
static gboolean stop_tor_daemons(Browser *browser, const char *profile, GError **error)
{
    GDir *processes = g_dir_open("/proc", 0, error);
    if (!processes) return FALSE;
    const char *entry;
    gboolean ok = TRUE;
    while ((entry = g_dir_read_name(processes))) {
        if (!g_ascii_isdigit(*entry)) continue;
        char *end;
        guint64 candidate = g_ascii_strtoull(entry, &end, 10);
        if (*end || !candidate || candidate > INT_MAX || !tor_daemon_matches(browser, profile, candidate)) continue;
        int fd = syscall(SYS_pidfd_open, candidate, 0);
        if (fd < 0) { if (errno == ESRCH) continue; ok = FALSE; break; }
        if (tor_daemon_matches(browser, profile, candidate) &&
            syscall(SYS_pidfd_send_signal, fd, SIGTERM, NULL, 0) != 0 && errno != ESRCH) ok = FALSE;
        close(fd);
        if (!ok) break;
    }
    g_dir_close(processes);
    if (!ok) file_error(error, "Could not safely stop this session's Tor daemon");
    return ok;
}
#endif
