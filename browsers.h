/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#ifndef ISOTAB_BROWSERS_H
#define ISOTAB_BROWSERS_H
#include <glib.h>
#include <gio/gio.h>

typedef enum { FAMILY_FIREFOX, FAMILY_CHROMIUM } BrowserFamily;
typedef enum { PACKAGE_NATIVE, PACKAGE_FLATPAK, PACKAGE_SNAP, PACKAGE_TOR, PACKAGE_TOR_FLATPAK } BrowserPackage;
typedef struct {
    const char *id;
    const char *name;
    BrowserFamily family;
    const char *commands[4];
    char *executable;
    BrowserPackage package;
    const char *application;
    char *bundle_executable;
} Browser;
#define NATIVE(id, name, family, ...) {id, name, family, {__VA_ARGS__, NULL}, NULL, PACKAGE_NATIVE, NULL, NULL}
#define PACKAGED(id, name, family, kind, app) {id, name, family, {NULL}, NULL, kind, app, NULL}
static Browser browsers[] = {
    NATIVE("firefox", "Firefox", FAMILY_FIREFOX, "firefox", "firefox-bin"),
    NATIVE("firefox-esr", "Firefox ESR", FAMILY_FIREFOX, "firefox-esr"),
    NATIVE("librewolf", "LibreWolf", FAMILY_FIREFOX, "librewolf"),
    NATIVE("chromium", "Chromium", FAMILY_CHROMIUM, "chromium", "chromium-browser"),
    NATIVE("chrome", "Google Chrome", FAMILY_CHROMIUM, "google-chrome-stable", "google-chrome"),
    NATIVE("brave", "Brave", FAMILY_CHROMIUM, "brave-browser", "brave", "brave-browser-stable"),
    NATIVE("edge", "Microsoft Edge", FAMILY_CHROMIUM, "microsoft-edge-stable", "microsoft-edge"),
    NATIVE("vivaldi", "Vivaldi", FAMILY_CHROMIUM, "vivaldi-stable", "vivaldi"),
    PACKAGED("tor-browser", "Tor Browser", FAMILY_FIREFOX, PACKAGE_TOR, NULL),
    PACKAGED("tor-browser-flatpak", "Tor Browser (Flatpak)", FAMILY_FIREFOX, PACKAGE_TOR_FLATPAK, "org.torproject.torbrowser-launcher"),
    PACKAGED("firefox-flatpak", "Firefox (Flatpak)", FAMILY_FIREFOX, PACKAGE_FLATPAK, "org.mozilla.firefox"),
    PACKAGED("librewolf-flatpak", "LibreWolf (Flatpak)", FAMILY_FIREFOX, PACKAGE_FLATPAK, "io.gitlab.librewolf-community"),
    PACKAGED("chromium-flatpak", "Chromium (Flatpak)", FAMILY_CHROMIUM, PACKAGE_FLATPAK, "org.chromium.Chromium"),
    PACKAGED("chrome-flatpak", "Google Chrome (Flatpak)", FAMILY_CHROMIUM, PACKAGE_FLATPAK, "com.google.Chrome"),
    PACKAGED("brave-flatpak", "Brave (Flatpak)", FAMILY_CHROMIUM, PACKAGE_FLATPAK, "com.brave.Browser"),
    PACKAGED("edge-flatpak", "Microsoft Edge (Flatpak)", FAMILY_CHROMIUM, PACKAGE_FLATPAK, "com.microsoft.Edge"),
    PACKAGED("vivaldi-flatpak", "Vivaldi (Flatpak)", FAMILY_CHROMIUM, PACKAGE_FLATPAK, "com.vivaldi.Vivaldi"),
    PACKAGED("firefox-snap", "Firefox (Snap)", FAMILY_FIREFOX, PACKAGE_SNAP, "firefox"),
    PACKAGED("chromium-snap", "Chromium (Snap)", FAMILY_CHROMIUM, PACKAGE_SNAP, "chromium"),
    PACKAGED("brave-snap", "Brave (Snap)", FAMILY_CHROMIUM, PACKAGE_SNAP, "brave")
};
#undef NATIVE
#undef PACKAGED
static char *tor_browser_directory;
static GHashTable *flatpak_apps;
static gboolean flatpak_scan_pending;
static gint64 flatpak_last_scan;

static gboolean browser_is_tor(const Browser *browser)
{
    return browser->package == PACKAGE_TOR || browser->package == PACKAGE_TOR_FLATPAK;
}

static gboolean browser_is_flatpak(const Browser *browser)
{
    return browser->package == PACKAGE_FLATPAK || browser->package == PACKAGE_TOR_FLATPAK;
}

static const char *browser_tor_executable(const Browser *browser)
{
    return browser->package == PACKAGE_TOR_FLATPAK ? browser->bundle_executable : browser->executable;
}

static Browser *find_browser(const char *id)
{
    for (guint i = 0; i < G_N_ELEMENTS(browsers); i++)
        if (g_strcmp0(id, browsers[i].id) == 0) return &browsers[i];
    return NULL;
}

static void flatpak_scan_done(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)unused;
    char *output = NULL;
    if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, NULL, NULL) &&
        g_subprocess_get_successful(G_SUBPROCESS(source))) {
        if (!flatpak_apps) flatpak_apps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_hash_table_remove_all(flatpak_apps);
        char **lines = g_strsplit(output, "\n", -1);
        for (guint i = 0; lines[i]; i++)
            if (*g_strstrip(lines[i])) g_hash_table_add(flatpak_apps, g_strdup(lines[i]));
        g_strfreev(lines);
    }
    g_free(output);
    flatpak_scan_pending = FALSE;
}

/* Discovery never waits for a package-manager subprocess on the GTK thread. */
static void refresh_flatpak_apps(const char *flatpak)
{
    gint64 now = g_get_monotonic_time();
    if (!flatpak || flatpak_scan_pending || (flatpak_last_scan && now - flatpak_last_scan < 30 * G_USEC_PER_SEC)) return;
    flatpak_last_scan = now;
    GSubprocess *scan = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        NULL, flatpak, "list", "--app", "--columns=application", NULL);
    if (!scan) return;
    flatpak_scan_pending = TRUE;
    g_subprocess_communicate_utf8_async(scan, NULL, NULL, flatpak_scan_done, NULL);
    g_object_unref(scan);
}

static char *read_bundle_metadata(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 65536) { close(fd); return NULL; }
    GString *contents = g_string_new(NULL);
    char buffer[4096];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) != 0) {
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || contents->len + n > 65536) { close(fd); g_string_free(contents, TRUE); return NULL; }
        g_string_append_len(contents, buffer, n);
    }
    close(fd); return g_string_free(contents, FALSE);
}

/* Only a Tor bundle, never torbrowser-launcher or an arbitrary Firefox, is used.
 * Version 15+ is the supported Tor process interface exercised by this adapter. */
static char *tor_bundle_executable(const char *directory)
{
    if (!directory || !g_path_is_absolute(directory)) return NULL;
    char *dir = g_canonicalize_filename(directory, NULL);
    char *nested = g_build_filename(dir, "Browser", "application.ini", NULL);
    if (g_file_test(nested, G_FILE_TEST_IS_REGULAR)) {
        char *browser_dir = g_build_filename(dir, "Browser", NULL);
        g_free(dir); dir = browser_dir;
    }
    g_free(nested);
    char *exe = g_build_filename(dir, "firefox", NULL);
    char *ini = g_build_filename(dir, "application.ini", NULL);
    GKeyFile *file = g_key_file_new();
    char *ini_contents = read_bundle_metadata(ini);
    GString *filtered = g_string_new(NULL);
    if (ini_contents) {
        char **lines = g_strsplit(ini_contents, "\n", -1);
        for (guint i = 0; lines[i]; i++) {
            if (*g_strstrip(lines[i]) != ';') { g_string_append(filtered, lines[i]); g_string_append_c(filtered, '\n'); }
        }
        g_strfreev(lines);
    }
    gboolean loaded = g_key_file_load_from_data(file, filtered->str, filtered->len, G_KEY_FILE_NONE, NULL);
    g_string_free(filtered, TRUE); g_free(ini_contents);
    char *vendor = loaded ? g_key_file_get_string(file, "App", "Vendor", NULL) : NULL;
    char *version_file = g_build_filename(dir, "tbb_version.json", NULL);
    char *version = read_bundle_metadata(version_file);
    const char *v = version ? strstr(version, "\"version\"") : NULL;
    v = v ? strchr(v + 9, ':') : NULL;
    if (v) { v++; while (g_ascii_isspace(*v) || *v == '"') v++; }
    char *version_end = NULL;
    guint64 major = v && g_ascii_isdigit(*v) ? g_ascii_strtoull(v, &version_end, 10) : 0;
    gboolean valid = !g_strcmp0(vendor, "Tor Project") && major >= 15 && version_end && *version_end == '.' &&
        g_file_test(exe, G_FILE_TEST_IS_EXECUTABLE);
    char *tor = g_build_filename(dir, "TorBrowser", "Tor", "tor", NULL);
    valid = valid && g_file_test(tor, G_FILE_TEST_IS_EXECUTABLE);
    g_free(tor); g_free(version); g_free(version_file); g_free(vendor); g_key_file_unref(file); g_free(ini); g_free(dir);
    if (!valid) g_clear_pointer(&exe, g_free);
    return exe;
}

static char *find_tor_bundle(void)
{
    if (tor_browser_directory && *tor_browser_directory) return tor_bundle_executable(tor_browser_directory);
    const char *arch = sizeof(void *) == 8 ? "x86_64" : "i686";
    char *launcher = g_build_filename(g_get_user_data_dir(), "torbrowser", "tbb", arch, "tor-browser", "Browser", NULL);
    char *exe = tor_bundle_executable(launcher);
    g_free(launcher);
    const char *locations[] = {"tor-browser/Browser", "Tor Browser/Browser", ".tor-browser/Browser", NULL};
    for (guint i = 0; !exe && locations[i]; i++) {
        char *dir = g_build_filename(g_get_home_dir(), locations[i], NULL);
        exe = tor_bundle_executable(dir); g_free(dir);
    }
    const char *system_locations[] = {"/opt/tor-browser/Browser", "/usr/lib/tor-browser/Browser", "/usr/lib/tor-browser", NULL};
    for (guint i = 0; !exe && system_locations[i]; i++) exe = tor_bundle_executable(system_locations[i]);
    return exe;
}

static gboolean packaged_wrapper(const char *path)
{
    if (g_str_has_prefix(path, "/snap/bin/")) return TRUE;
    char *resolved = realpath(path, NULL);
    gboolean snap = resolved && g_str_has_suffix(resolved, "/snap");
    free(resolved);
    if (!snap) {
        char buffer[4097];
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        ssize_t n = fd >= 0 ? read(fd, buffer, sizeof(buffer) - 1) : -1;
        if (fd >= 0) close(fd);
        if (n > 1 && buffer[0] == '#' && buffer[1] == '!') {
            buffer[n] = '\0';
            snap = strstr(buffer, "/snap/bin/") || strstr(buffer, "snap run ") || strstr(buffer, "flatpak run ");
        }
    }
    return snap;
}

static void detect_browsers(void)
{
    char *flatpak = g_find_program_in_path("flatpak");
    char *snap = g_find_program_in_path("snap");
    refresh_flatpak_apps(flatpak);
    for (guint i = 0; i < G_N_ELEMENTS(browsers); i++) {
        Browser *b = &browsers[i];
        g_clear_pointer(&b->executable, g_free);
        g_clear_pointer(&b->bundle_executable, g_free);
        if (b->package == PACKAGE_TOR) b->executable = find_tor_bundle();
        else if (browser_is_flatpak(b)) {
            if (flatpak && flatpak_apps && g_hash_table_contains(flatpak_apps, b->application)) {
                if (b->package == PACKAGE_TOR_FLATPAK) {
                    const char *arch = sizeof(void *) == 8 ? "x86_64" : "i686";
                    char *bundle = g_build_filename(g_get_home_dir(), ".var", "app", b->application, "data",
                        "torbrowser", "tbb", arch, "tor-browser", "Browser", NULL);
                    b->bundle_executable = tor_bundle_executable(bundle); g_free(bundle);
                    if (b->bundle_executable) b->executable = g_strdup(flatpak);
                } else b->executable = g_strdup(flatpak);
            }
        } else if (b->package == PACKAGE_SNAP) {
            char *installed = g_strdup_printf("/snap/%s/current/meta/snap.yaml", b->application);
            char *alternative = g_strdup_printf("/var/lib/snapd/snap/%s/current/meta/snap.yaml", b->application);
            if (snap && (g_file_test(installed, G_FILE_TEST_IS_REGULAR) ||
                g_file_test(alternative, G_FILE_TEST_IS_REGULAR))) b->executable = g_strdup(snap);
            g_free(alternative);
            g_free(installed);
        } else for (int j = 0; b->commands[j] && !b->executable; j++) {
            char *candidate = g_find_program_in_path(b->commands[j]);
            if (candidate && packaged_wrapper(candidate)) g_free(candidate);
            else b->executable = candidate;
        }
    }
    g_free(snap); g_free(flatpak);
}

static char *browser_profile_root_for_home(Browser *browser, const char *data, const char *home)
{
    return browser->package == PACKAGE_SNAP ?
        g_build_filename(home, "snap", browser->application, "common", "isotab", "profiles", NULL) :
        g_build_filename(data, "profiles", NULL);
}

static char *browser_profile_root(Browser *browser, const char *data)
{
    return browser_profile_root_for_home(browser, data, g_get_home_dir());
}

static char **browser_arguments(Browser *browser, const char *profile)
{
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, g_strdup(browser->executable));
    if (browser_is_flatpak(browser)) {
        g_ptr_array_add(args, g_strdup("run"));
        g_ptr_array_add(args, g_strdup_printf("--filesystem=%s:rw", profile));
        if (browser_is_tor(browser)) {
            g_ptr_array_add(args, g_strdup_printf("--command=%s", browser->bundle_executable));
            char *dir = g_path_get_dirname(browser->bundle_executable);
            g_ptr_array_add(args, g_strdup_printf("--cwd=%s", dir)); g_free(dir);
            g_ptr_array_add(args, g_strdup("--env=TOR_SOCKS_PORT=0"));
        }
        g_ptr_array_add(args, g_strdup(browser->application));
    } else if (browser->package == PACKAGE_SNAP) {
        g_ptr_array_add(args, g_strdup("run"));
        g_ptr_array_add(args, g_strdup(browser->application));
    }
    if (browser->family == FAMILY_FIREFOX) {
        g_ptr_array_add(args, g_strdup("--profile"));
        g_ptr_array_add(args, g_strdup(profile));
        g_ptr_array_add(args, g_strdup("--no-remote"));
    } else {
        g_ptr_array_add(args, g_strdup_printf("--user-data-dir=%s", profile));
        g_ptr_array_add(args, g_strdup("--no-first-run"));
        g_ptr_array_add(args, g_strdup("--no-default-browser-check"));
    }
    g_ptr_array_add(args, NULL);
    return (char **)g_ptr_array_free(args, FALSE);
}
#endif
