/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
/* IsoTab: a small C/GTK3 manager for independent browser profiles. */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <limits.h>
#include <sys/syscall.h>
#include "browsers.h"
#include "safe-files.h"

/* ─── constants ──────────────────────────────────────── */
#define APP_TITLE     "IsoTab - Browser Sessions"

/* ─── per-session state ──────────────────────────────── */
typedef struct {
    char       *id;
    char       *name;
    Browser    *browser;
    GtkWidget  *frame;
    char       *profile_dir;
    gboolean    unavailable;
    gboolean    installed;
    GPid        pid;
    pid_t       external_pid;
    gboolean    running;
    gboolean    archived;
    gint64      archived_at;
    gboolean    deletion_started;
    GtkWidget  *recovery_size_lbl;
    GtkWidget  *recovery_row;
    GtkWidget  *recovery_name_lbl;
    GtkWidget  *recovery_info_lbl;
    GtkWidget  *recovery_restore_btn;
    GtkWidget  *recovery_cleanup_btn;
    gboolean    busy;
    gboolean    size_pending;
    gboolean    size_known;
    double      size_mb;
    guint       size_generation;
    GtkWidget  *rename_btn;
    GtkWidget  *name_lbl;
    GtkWidget  *status_lbl;
    GtkWidget  *launch_btn;
    GtkWidget  *clear_btn;
    GtkWidget  *remove_btn;
} Session;

static GPtrArray *sessions;
static GPtrArray *retained_sessions;
static GtkWidget *header_actions;
static guint pending_operations;
static char *data_dir;
static GtkWidget *session_grid;
static GtkWidget *empty_label;
static GtkWidget *main_win;
typedef struct {
    gboolean show_sizes;
    gboolean shortcuts;
    char *default_browser;
} Preferences;
static Preferences preferences = { TRUE, TRUE, NULL };


/* SVG artwork is embedded in the executable, with theme-aware foreground
 * colors and rendering at the display scale factor. */
static const char *ICON_SETTINGS = "<path d='M4 7h2m6 0h8M4 17h8m6 0h2'/><circle cx='9' cy='7' r='3' fill='none'/><circle cx='15' cy='17' r='3' fill='none'/>";
static const char *ICON_PLAY = "<path d='m8 5 11 7-11 7Z'/>";
static const char *ICON_MORE = "<circle cx='5' cy='12' r='1'/><circle cx='12' cy='12' r='1'/><circle cx='19' cy='12' r='1'/>";
static const char *ICON_STOP = "<rect x='6' y='6' width='12' height='12' rx='1'/>";

static void refresh_svg_icon(GtkWidget *image, gpointer unused)
{
    (void)unused;
    const char *art = g_object_get_data(G_OBJECT(image), "svg-art");
    int size = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(image), "svg-size"));
    int scale = gtk_widget_get_scale_factor(image);
    GdkRGBA color;
    gtk_style_context_get_color(gtk_widget_get_style_context(image),
        gtk_widget_get_state_flags(image), &color);
    char *svg = g_strdup_printf(
        "<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24' "
        "viewBox='0 0 24 24' fill='none' stroke='#%02x%02x%02x' "
        "stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round'>%s</svg>",
        (unsigned)(color.red * 255), (unsigned)(color.green * 255),
        (unsigned)(color.blue * 255), art ? art : "");
    GError *error = NULL;
    GdkPixbuf *pixbuf;
    if (art) {
        GInputStream *stream = g_memory_input_stream_new_from_data(svg, -1, g_free);
        pixbuf = gdk_pixbuf_new_from_stream_at_scale(stream,
            size * scale, size * scale, TRUE, NULL, &error);
        g_object_unref(stream);
    } else {
        g_free(svg);
        pixbuf = gdk_pixbuf_new_from_resource_at_scale("/org/isotab/isotab.svg",
            size * scale, size * scale, TRUE, &error);
    }
    if (!pixbuf) {
        g_warning("Cannot render SVG icon: %s", error->message);
        g_error_free(error);
        return;
    }
    cairo_surface_t *surface = gdk_cairo_surface_create_from_pixbuf(pixbuf, scale, NULL);
    gtk_image_set_from_surface(GTK_IMAGE(image), surface);
    cairo_surface_destroy(surface);
    g_object_unref(pixbuf);
}

static void svg_scale_changed(GObject *image, GParamSpec *spec, gpointer data)
{
    (void)spec;
    refresh_svg_icon(GTK_WIDGET(image), data);
}

static GtkWidget *svg_icon(const char *art, int size)
{
    GtkWidget *image = gtk_image_new();
    g_object_set_data(G_OBJECT(image), "svg-art", (gpointer)art);
    g_object_set_data(G_OBJECT(image), "svg-size", GINT_TO_POINTER(size));
    g_signal_connect(image, "style-updated", G_CALLBACK(refresh_svg_icon), NULL);
    g_signal_connect(image, "notify::scale-factor", G_CALLBACK(svg_scale_changed), NULL);
    refresh_svg_icon(image, NULL);
    return image;
}

static void set_action(GtkWidget *button, const char *label, const char *art)
{
    gtk_button_set_label(GTK_BUTTON(button), label);
    gtk_button_set_image(GTK_BUTTON(button), art ? svg_icon(art, 16) : NULL);
    gtk_button_set_always_show_image(GTK_BUTTON(button), TRUE);
}

/* ═══════════════════════════════════════════════════════
   profile helpers
═══════════════════════════════════════════════════════ */
static gboolean profile_exists(const char *dir)
{
    return g_file_test(dir, G_FILE_TEST_IS_DIR);
}

static void show_error(const char *message)
{
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(main_win),
        GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", message);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* Match Firefox's Unix .parentlock record lock. Keep the descriptor open
 * throughout deletion, and never remove the lock file itself. */
static int lock_profile_at(int dirfd, GError **error)
{
    struct stat st;
    int saved_errno;
    int fd = open_lock_at(dirfd, ".parentlock");
    if (fd < 0) goto failed;
    struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(fd, F_SETLK, &lock) == 0) {
        int result = fstatat(dirfd, "lock", &st, AT_SYMLINK_NOFOLLOW);
        saved_errno = errno;
        gboolean safe = result < 0 && saved_errno == ENOENT;
        if (result == 0 && S_ISLNK(st.st_mode)) {
            char *target = read_link_at(dirfd, "lock");
            char *colon = target ? strchr(target, ':') : NULL;
            /* Firefox's IP:+PID format means the owner also takes the
             * fcntl lock. Owning that lock proves this symlink is stale.
             * Do not unlink it here: Firefox replaces it on launch. */
            if (colon && colon[1] == '+' && g_ascii_isdigit(colon[2])) {
                struct in_addr address;
                *colon = '\0';
                const char *pid = colon + 2;
                const char *end = pid;
                while (g_ascii_isdigit(*end)) end++;
                safe = inet_pton(AF_INET, target, &address) == 1 &&
                    *end == '\0' && g_ascii_strtoull(pid, NULL, 10) > 0;
            }
            g_free(target);
        }
        if (safe) return fd;
        if (result == 0) saved_errno = EBUSY;
    } else {
        saved_errno = errno;
    }
    close(fd);
    errno = saved_errno;
failed:
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
        "Cannot access this profile safely: %s. Close the browser before trying again.",
        g_strerror(errno));
    return -1;
}

static int lock_profile(const char *dir, GError **error)
{
    int root = open_profile_directory(dir, error);
    if (root < 0) return -1;
    int fd = lock_profile_at(root, error);
    close(root);
    return fd;
}

static gboolean profile_unavailable(const char *dir)
{
    struct stat st;
    if (lstat(dir, &st) != 0 && errno == ENOENT) return FALSE;
    int fd = lock_profile(dir, NULL);
    if (fd < 0) return TRUE;
    close(fd);
    return FALSE;
}

static gboolean G_GNUC_UNUSED clear_profile(const char *dir, GError **error)
{
    int root = open_profile_directory(dir, error);
    if (root < 0) return FALSE;
    int fd = lock_profile_at(root, error);
    if (fd < 0) { close(root); return FALSE; }
    struct stat st;
    gboolean ok = fstat(root, &st) == 0;
    if (ok) ok = delete_contents_at(root, ".parentlock", NULL, st.st_dev, 0, error);
    else file_error(error, "Cannot inspect profile");
    close(fd);
    close(root);
    return ok;
}

static double profile_size_mb(const char *dir)
{
    char *argv[] = { "du", "-sm", "--", (char *)dir, NULL };
    char *output = NULL;
    int status;
    gboolean ok = g_spawn_sync(NULL, argv, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
        NULL, NULL, &output, NULL, &status, NULL);
    double mb = -1;
    if (ok && g_spawn_check_wait_status(status, NULL))
        mb = g_ascii_strtod(output, NULL);
    g_free(output);
    return mb;
}

/* Validate every marker snapshot, including the one displaced by exchange.
 * Only a fully parsed local native marker or our local reservation may be stale. */
static gboolean chromium_marker_stale(const char *marker)
{
    if (!marker) return FALSE;
    const char *dash = strrchr(marker, '-');
    if (!dash || !g_ascii_isdigit(dash[1])) return FALSE;
    char *host = g_strndup(marker, dash - marker);
    char *reservation = g_strdup_printf("%s.isotab-clear", g_get_host_name());
    gboolean local = !strcmp(host, g_get_host_name()) || !strcmp(host, reservation);
    g_free(host); g_free(reservation);
    const char *digit = dash + 1;
    while (g_ascii_isdigit(*digit)) digit++;
    if (!local || *digit) return FALSE;
    char *end;
    errno = 0;
    guint64 owner = g_ascii_strtoull(dash + 1, &end, 10);
    if (errno || *end || owner == 0 || owner > INT_MAX) return FALSE;
    return kill((pid_t)owner, 0) == -1 && errno == ESRCH;
}

static gboolean chromium_available_at(int dirfd, GError **error)
{
    struct stat st;
    int result = fstatat(dirfd, "SingletonLock", &st, AT_SYMLINK_NOFOLLOW);
    if (result < 0) {
        if (errno == ENOENT) return TRUE;
        goto failed;
    }
    char *target = read_link_at(dirfd, "SingletonLock");
    gboolean stale = chromium_marker_stale(target);
    g_free(target);
    if (stale) return TRUE;
    errno = EBUSY;
failed:
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
        "Profile is in use or inaccessible: %s", g_strerror(errno));
    return FALSE;
}

static gboolean chromium_available(const char *dir, GError **error)
{
    struct stat st;
    if (lstat(dir, &st) != 0 && errno == ENOENT) return TRUE;
    int root = open_profile_directory(dir, error);
    if (root < 0) return FALSE;
    gboolean ok = chromium_available_at(root, error);
    close(root);
    return ok;
}

static gboolean session_unavailable(Session *s)
{
    return s->browser->family == FAMILY_FIREFOX ?
        profile_unavailable(s->profile_dir) : !chromium_available(s->profile_dir, NULL);
}

/* Reserve SingletonLock with a distinct host name while deleting. Chromium
 * refuses this foreign-host lock instead of killing its owner as a hung browser.
 * The separate flock serializes IsoTab operations on the directory. */
static gboolean G_GNUC_UNUSED clear_chromium(const char *dir, GError **error)
{
    int root = open_profile_directory(dir, error);
    if (root < 0) return FALSE;
    int fd = open_lock_at(root, ".isotab.lock");
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0) close(fd);
        close(root);
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
            "Another operation is using this profile, or its lock is invalid.");
        return FALSE;
    }
    char *token = g_strdup_printf("%s.isotab-clear-%ld", g_get_host_name(), (long)getpid());
    /* Atomic creation only: never unlink a stale marker that a browser could
     * replace between our check and unlink. Let the browser recover it. */
    gboolean reserved = symlinkat(token, root, "SingletonLock") == 0;
    gboolean ok = reserved;
    if (!ok) g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_AGAIN,
        "Profile has a browser lock. If the browser is closed, launch this session once and close it normally before clearing.");
    struct stat st;
    if (ok) {
        ok = fstat(root, &st) == 0;
        if (ok) ok = delete_contents_at(root, "SingletonLock", ".isotab.lock", st.st_dev, 0, error);
        else file_error(error, "Cannot inspect profile");
    }
    if (reserved) {
        char *current = read_link_at(root, "SingletonLock");
        if (g_strcmp0(current, token) != 0 || unlinkat(root, "SingletonLock", 0) != 0) {
            if (ok) g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                "Profile lock changed during clearing.");
            ok = FALSE;
        }
        g_free(current);
    }
    g_free(token);
    close(fd);
    close(root);
    return ok;
}

#include "profile-reset.h"
#include "profile-import.h"
#include "profile-purge.h"

static void session_free(gpointer data)
{
    Session *s = data;
    g_free(s->id);
    g_free(s->name);
    g_free(s->profile_dir);
    g_free(s);
}

static Session *new_session(const char *id, const char *name, Browser *browser)
{
    Session *s = g_new0(Session, 1);
    s->id = g_strdup(id);
    s->name = g_strdup(name);
    s->browser = browser;
    if (g_str_has_prefix(id, "legacy_"))
        s->profile_dir = g_strdup_printf("%s/session_%s", data_dir, id + 7);
    else
        s->profile_dir = g_build_filename(data_dir, "profiles", id, NULL);
    return s;
}

static char *recovery_data_path(Session *s)
{
    char *trash = recovery_trash_path(s->id);
    if (s->deletion_started && !path_missing(trash)) return trash;
    g_free(trash);
    return g_strdup(s->profile_dir);
}

static gboolean save_sessions(GError **error)
{
    GKeyFile *file = g_key_file_new();
    g_key_file_set_integer(file, "IsoTab", "version", 1);
    g_key_file_set_boolean(file, "IsoTab", "show_sizes", preferences.show_sizes);
    g_key_file_set_boolean(file, "IsoTab", "shortcuts", preferences.shortcuts);
    g_key_file_set_string(file, "IsoTab", "default_browser",
        preferences.default_browser ? preferences.default_browser : "");
    for (guint i = 0; i < sessions->len + (retained_sessions ? retained_sessions->len : 0); i++) {
        Session *s = i < sessions->len ? g_ptr_array_index(sessions, i) : g_ptr_array_index(retained_sessions, i - sessions->len);
        g_key_file_set_boolean(file, s->id, "archived", s->archived);
        if (s->archived_at) g_key_file_set_int64(file, s->id, "archived_at", s->archived_at);
        g_key_file_set_boolean(file, s->id, "deletion_started", s->deletion_started);
        g_key_file_set_string(file, s->id, "name", s->name);
        g_key_file_set_string(file, s->id, "browser", s->browser->id);
    }
    gsize size;
    char *contents = g_key_file_to_data(file, &size, NULL);
    if (size > 1024 * 1024) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_NOSPC, "Session settings exceed the 1 MiB safety limit.");
        g_free(contents); g_key_file_unref(file); return FALSE;
    }
    int root = open_profile_directory(data_dir, error);
    if (root < 0) { g_free(contents); g_key_file_unref(file); return FALSE; }
    char *path = g_strdup_printf("/proc/self/fd/%d/sessions.ini", root);
    gboolean ok = g_file_set_contents_full(path, contents, size,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
    g_free(path);
    close(root);
    g_free(contents);
    g_key_file_unref(file);
    return ok;
}

static gboolean valid_session_id(const char *id)
{
    return g_uuid_string_is_valid(id) ||
        (g_str_has_prefix(id, "legacy_") && strlen(id) == 8 && g_ascii_isdigit(id[7]));
}

static gboolean load_sessions(GError **error)
{
    g_clear_pointer(&preferences.default_browser, g_free);
    preferences.show_sizes = TRUE;
    preferences.shortcuts = TRUE;
    sessions = g_ptr_array_new_with_free_func(session_free);
    retained_sessions = g_ptr_array_new_with_free_func(session_free);
    GKeyFile *file = g_key_file_new();
    GError *local = NULL;
    int root = open_profile_directory(data_dir, &local);
    int config = root < 0 ? -1 : openat(root, "sessions.ini", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (config < 0 && !local) file_error(&local, "Cannot open session settings");
    if (root >= 0) close(root);
    gboolean loaded = FALSE;
    if (config >= 0) {
        struct stat st;
        if (fstat(config, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            st.st_nlink != 1 || (st.st_mode & 0022) || st.st_size > 1024 * 1024) {
            g_set_error_literal(&local, G_FILE_ERROR, G_FILE_ERROR_ACCES,
                "Settings must be an owned, unshared regular file smaller than 1 MiB.");
        } else {
            GString *contents = g_string_new(NULL);
            char buffer[4096];
            for (;;) {
                ssize_t n = read(config, buffer, sizeof(buffer));
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) { file_error(&local, "Cannot read settings"); break; }
                if (n == 0) {
                    loaded = g_key_file_load_from_data(file, contents->str, contents->len, G_KEY_FILE_NONE, &local);
                    break;
                }
                if (contents->len + n > 1024 * 1024) {
                    g_set_error_literal(&local, G_FILE_ERROR, G_FILE_ERROR_NOSPC, "Settings exceed the 1 MiB safety limit.");
                    break;
                }
                g_string_append_len(contents, buffer, n);
            }
            g_string_free(contents, TRUE);
        }
        close(config);
    }
    if (!loaded) {
        g_key_file_unref(file);
        if (!g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_propagate_error(error, local);
            return FALSE;
        }
        g_clear_error(&local);
        /* Import only existing profiles; never move or rewrite browser data. */
        Browser *legacy_browser = find_browser("firefox");
        if (!legacy_browser->executable && find_browser("firefox-esr")->executable)
            legacy_browser = find_browser("firefox-esr");
        for (int i = 0; i < 10; i++) {
            char *id = g_strdup_printf("legacy_%d", i);
            char *name = g_strdup_printf("Session %d", i + 1);
            Session *s = new_session(id, name, legacy_browser);
            if (profile_exists(s->profile_dir)) g_ptr_array_add(sessions, s);
            else session_free(s);
            g_free(id);
            g_free(name);
        }
        return save_sessions(error);
    }
    if (g_key_file_get_integer(file, "IsoTab", "version", NULL) != 1) goto invalid;
    if (g_key_file_has_key(file, "IsoTab", "show_sizes", NULL))
        preferences.show_sizes = g_key_file_get_boolean(file, "IsoTab", "show_sizes", NULL);
    if (g_key_file_has_key(file, "IsoTab", "shortcuts", NULL))
        preferences.shortcuts = g_key_file_get_boolean(file, "IsoTab", "shortcuts", NULL);
    char *preferred = g_key_file_get_string(file, "IsoTab", "default_browser", NULL);
    if (find_browser(preferred)) preferences.default_browser = preferred;
    else g_free(preferred);
    gsize count;
    char **groups = g_key_file_get_groups(file, &count);
    gboolean valid = TRUE;
    for (gsize i = 0; i < count; i++) {
        if (!strcmp(groups[i], "IsoTab")) continue;
        char *name = g_key_file_get_string(file, groups[i], "name", NULL);
        char *browser_id = g_key_file_get_string(file, groups[i], "browser", NULL);
        Browser *browser = find_browser(browser_id);
        if (!valid_session_id(groups[i]) || !name || !*name ||
            !g_utf8_validate(name, -1, NULL) || !browser ||
            (g_str_has_prefix(groups[i], "legacy_") && browser->family != FAMILY_FIREFOX)) {
            valid = FALSE;
        } else {
            Session *s = new_session(groups[i], name, browser);
            GError *field_error = NULL;
            if (g_key_file_has_key(file, groups[i], "archived", NULL))
                s->archived = g_key_file_get_boolean(file, groups[i], "archived", &field_error);
            if (!field_error && g_key_file_has_key(file, groups[i], "archived_at", NULL))
                s->archived_at = g_key_file_get_int64(file, groups[i], "archived_at", &field_error);
            if (!field_error && g_key_file_has_key(file, groups[i], "deletion_started", NULL))
                s->deletion_started = g_key_file_get_boolean(file, groups[i], "deletion_started", &field_error);
            if (field_error || s->archived_at < 0 || (s->deletion_started && !s->archived)) {
                g_clear_error(&field_error); session_free(s); valid = FALSE;
                g_free(name); g_free(browser_id); continue;
            }
            g_ptr_array_add(s->archived ? retained_sessions : sessions, s);
        }
        g_free(name);
        g_free(browser_id);
    }
    g_strfreev(groups);
    if (!valid) goto invalid;
    g_key_file_unref(file);
    return TRUE;
invalid:
    g_key_file_unref(file);
    g_set_error_literal(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
        "Session settings are invalid or from a newer version. They have not been overwritten.");
    return FALSE;
}

/* Recognize an earlier launch without trusting a persisted PID. */
static pid_t profile_owner(Session *s)
{
    int root = open_profile_directory(s->profile_dir, NULL);
    if (root < 0) return 0;
    pid_t pid = 0;
    if (s->browser->family == FAMILY_FIREFOX) {
        int fd = open_lock_at(root, ".parentlock");
        if (fd >= 0) {
            struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
            if (fcntl(fd, F_GETLK, &lock) == 0 && lock.l_type != F_UNLCK && lock.l_pid > 0)
                pid = lock.l_pid;
            close(fd);
        }
    } else {
        char *target = read_link_at(root, "SingletonLock");
        char *dash = target ? strrchr(target, '-') : NULL;
        if (dash && g_ascii_isdigit(dash[1])) {
            *dash = '\0';
            char *end = NULL;
            guint64 candidate = g_ascii_strtoull(dash + 1, &end, 10);
            if (!strcmp(target, g_get_host_name()) && !*end && candidate > 0 && candidate <= INT_MAX)
                pid = (pid_t)candidate;
        }
        g_free(target);
    }
    close(root);
    return pid;
}

static gboolean process_matches_session(Session *s, pid_t pid)
{
    if (pid <= 0 || pid == getpid()) return FALSE;
    char *proc = g_strdup_printf("/proc/%ld", (long)pid);
    struct stat st;
    gboolean owned = stat(proc, &st) == 0 && st.st_uid == geteuid();
    char *path = g_build_filename(proc, "exe", NULL);
    char *exe = owned ? g_file_read_link(path, NULL) : NULL;
    g_free(path);
    char *base = exe ? g_path_get_basename(exe) : NULL;
    gboolean browser = s->browser->family == FAMILY_FIREFOX ?
        (!g_strcmp0(base, "firefox") || !g_strcmp0(base, "firefox-bin") || !g_strcmp0(base, "firefox-esr")) :
        (!g_strcmp0(base, "chrome") || !g_strcmp0(base, "chromium") ||
         !g_strcmp0(base, "brave") || !g_strcmp0(base, "brave-browser") ||
         !g_strcmp0(base, "msedge") || !g_strcmp0(base, "vivaldi") || !g_strcmp0(base, "vivaldi-bin"));
    g_free(base); g_free(exe);
    path = g_build_filename(proc, "cmdline", NULL);
    g_free(proc);
    char *args = NULL;
    gsize size = 0;
    gboolean match = FALSE;
    if (browser && g_file_get_contents(path, &args, &size, NULL) && size <= 1024 * 1024) {
        const char *next_profile = NULL;
        for (gsize i = 0; i < size;) {
            const char *arg = args + i;
            const char *end = memchr(arg, '\0', size - i);
            if (!end) break;
            const char *profile = next_profile ? arg : NULL;
            next_profile = NULL;
            if (s->browser->family == FAMILY_FIREFOX) {
                if (!strcmp(arg, "--profile") || !strcmp(arg, "-profile")) next_profile = arg;
                else if (g_str_has_prefix(arg, "--profile=")) profile = arg + 10;
            } else if (g_str_has_prefix(arg, "--user-data-dir=")) profile = arg + 16;
            if (profile && g_path_is_absolute(profile)) {
                char *canonical = g_canonicalize_filename(profile, NULL);
                match = !strcmp(canonical, s->profile_dir);
                g_free(canonical);
                if (match) break;
            }
            i = end - args + 1;
        }
    }
    /* Chromium's headless mode can flatten argv into one process-title string.
     * Do not guess its quoting: verify an open file inside the locked profile. */
    if (!match && browser && s->browser->family == FAMILY_CHROMIUM) {
        char *fd_dir = g_strdup_printf("/proc/%ld/fd", (long)pid);
        GDir *fds = g_dir_open(fd_dir, 0, NULL);
        char *prefix = g_strconcat(s->profile_dir, "/", NULL);
        const char *name;
        while (fds && !match && (name = g_dir_read_name(fds))) {
            char *fd_path = g_build_filename(fd_dir, name, NULL);
            char *target = g_file_read_link(fd_path, NULL);
            match = target && g_str_has_prefix(target, prefix);
            g_free(target); g_free(fd_path);
        }
        if (fds) g_dir_close(fds);
        g_free(prefix); g_free(fd_dir);
    }
    g_free(args); g_free(path);
    return match;
}

static pid_t recovered_session_pid(Session *s)
{
    pid_t pid = profile_owner(s);
    return process_matches_session(s, pid) ? pid : 0;
}

static gboolean stop_recovered_session(Session *s, GError **error)
{
    pid_t pid = s->external_pid;
    int fd = pid > 0 ? syscall(SYS_pidfd_open, pid, 0) : -1;
    if (fd < 0) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
            "Could not safely access this browser process. Close its window instead.");
        return FALSE;
    }
    /* The descriptor pins the process. Recheck both the live profile lock and
     * the process identity before signaling; a recycled PID cannot be targeted. */
    gboolean ok = profile_owner(s) == pid && process_matches_session(s, pid);
    if (ok) ok = syscall(SYS_pidfd_send_signal, fd, SIGTERM, NULL, 0) == 0;
    close(fd);
    if (!ok) g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
        "The browser process changed or exited. Refresh the session and try again.");
    return ok;
}

static void update_session_ui(Session *s);
static void update_recovery_row(Session *s);
static void size_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)cancel;
    double *size = g_new(double, 1);
    *size = profile_size_mb(data);
    g_task_return_pointer(task, size, g_free);
}

typedef struct { char *id; guint generation; } SizeRequest;
static Session *session_by_id(const char *id)
{
    for (guint i = 0; i < sessions->len; i++) {
        Session *s = g_ptr_array_index(sessions, i);
        if (!strcmp(s->id, id)) return s;
    }
    return NULL;
}
static void size_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    SizeRequest *request = data;
    double *size = g_task_propagate_pointer(G_TASK(result), NULL);
    Session *s = session_by_id(request->id);
    for (guint i = 0; !s && i < retained_sessions->len; i++) {
        Session *candidate = g_ptr_array_index(retained_sessions, i);
        if (!strcmp(candidate->id, request->id)) s = candidate;
    }
    if (s && s->size_generation == request->generation) {
        s->size_pending = FALSE;
        s->size_known = TRUE;
        s->size_mb = size ? *size : -1;
        if (s->frame) update_session_ui(s);
        if (s->recovery_row) update_recovery_row(s);
    }
    g_free(size); g_free(request->id); g_free(request);
}
static void request_profile_size(Session *s)
{
    if (s->size_pending || s->size_known || s->busy) return;
    s->size_pending = TRUE;
    SizeRequest *request = g_new0(SizeRequest, 1);
    request->id = g_strdup(s->id); request->generation = s->size_generation;
    GTask *task = g_task_new(NULL, NULL, size_done, request);
    g_task_set_task_data(task, recovery_data_path(s), g_free);
    g_task_run_in_thread(task, size_worker);
    g_object_unref(task);
}

/* ═══════════════════════════════════════════════════════
   update a session card's UI state
═══════════════════════════════════════════════════════ */
static void update_session_ui(Session *s)
{
    char status[100];
    if (s->busy) {
        gtk_label_set_text(GTK_LABEL(s->status_lbl), "Resetting profile...");
        gtk_widget_set_sensitive(s->launch_btn, FALSE);
        gtk_widget_set_sensitive(s->clear_btn, FALSE);
        gtk_widget_set_sensitive(s->remove_btn, FALSE);
        gtk_widget_set_sensitive(s->rename_btn, FALSE);
        return;
    }
    gtk_widget_set_tooltip_text(s->status_lbl, NULL);
    gtk_widget_set_sensitive(s->rename_btn, pending_operations == 0);
    s->external_pid = s->running ? 0 : recovered_session_pid(s);
    s->installed = s->browser->executable != NULL;
    s->unavailable = !s->running && session_unavailable(s);
    gtk_widget_set_sensitive(s->remove_btn, !pending_operations && !s->running && !s->unavailable);
    gtk_widget_set_sensitive(s->launch_btn, !pending_operations && (s->external_pid > 0 || (!s->unavailable && (s->running || s->browser->executable))));
    if (s->unavailable && !s->external_pid) {
        gtk_label_set_text(GTK_LABEL(s->status_lbl), "Unavailable");
        gtk_widget_set_tooltip_text(s->status_lbl, "The profile is in use or inaccessible. Close its browser before managing it.");
        set_action(s->launch_btn, "Unavailable", NULL);
        gtk_widget_set_sensitive(s->clear_btn, FALSE);
    } else if (s->running || s->external_pid) {
        gtk_label_set_text(GTK_LABEL(s->status_lbl), "Running");
        char *detail = g_strdup_printf("Browser PID %d%s", (int)(s->running ? s->pid : s->external_pid),
            s->external_pid ? " - reconnected" : "");
        gtk_widget_set_tooltip_text(s->status_lbl, detail);
        g_free(detail);
        set_action(s->launch_btn, "Stop", ICON_STOP);
        gtk_widget_set_sensitive(s->clear_btn, FALSE);
    } else if (!s->browser->executable) {
        gtk_label_set_text(GTK_LABEL(s->status_lbl), "Browser missing");
        set_action(s->launch_btn, "Not installed", NULL);
        gtk_widget_set_sensitive(s->clear_btn, !pending_operations && profile_exists(s->profile_dir));
    } else if (profile_exists(s->profile_dir)) {
        if (preferences.show_sizes) request_profile_size(s);
        double mb = s->size_known ? s->size_mb : 0;
        if (preferences.show_sizes && s->size_known && mb >= 0)
            snprintf(status, sizeof(status), "Stopped · %.0f MB", mb);
        else g_strlcpy(status, "Stopped", sizeof(status));
        gtk_label_set_text(GTK_LABEL(s->status_lbl), status);
        set_action(s->launch_btn, "Launch", ICON_PLAY);
        gtk_widget_set_sensitive(s->clear_btn, pending_operations == 0);
    } else {
        gtk_label_set_text(GTK_LABEL(s->status_lbl), "Fresh profile");
        set_action(s->launch_btn, "Launch", ICON_PLAY);
        gtk_widget_set_sensitive(s->clear_btn, FALSE);
    }
}

/* ═══════════════════════════════════════════════════════
   child process exited → update UI
═══════════════════════════════════════════════════════ */
static void cb_child_exit(GPid pid, gint status, gpointer data)
{
    Session *s = (Session *)data;
    g_spawn_close_pid(pid);
    s->pid     = 0;
    s->running = FALSE;
    s->size_known = FALSE; s->size_generation++; s->size_pending = FALSE;
    update_session_ui(s);
    GError *error = NULL;
    if (!g_spawn_check_wait_status(status, &error)) {
        if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM))
            show_error(error->message);
        g_error_free(error);
    }
}

/* ═══════════════════════════════════════════════════════
   launch / stop the browser for a session
═══════════════════════════════════════════════════════ */
static void cb_launch(GtkButton *btn, gpointer data)
{
    (void)btn;
    Session *s = (Session *)data;
    if (s->busy || pending_operations) return;

    /* stop if already running */
    if (s->running) {
        if (s->pid > 0 && kill(s->pid, SIGTERM) != 0)
            show_error(g_strerror(errno));
        return;
    }

    if (s->external_pid) {
        GError *error = NULL;
        if (!stop_recovered_session(s, &error)) {
            show_error(error->message);
            g_error_free(error);
        }
        update_session_ui(s);
        return;
    }

    detect_browsers();
    if (!s->browser->executable) {
        show_error("This browser is not installed. Install it and try again.");
        update_session_ui(s);
        return;
    }

    /* ensure profile directory exists */
    if (g_mkdir_with_parents(s->profile_dir, 0700) != 0) {
        show_error(g_strerror(errno));
        return;
    }
    GError *lock_error = NULL;
    gboolean available;
    if (s->browser->family == FAMILY_FIREFOX) {
        int fd = lock_profile(s->profile_dir, &lock_error);
        available = fd >= 0;
        if (fd >= 0) close(fd);
    } else {
        available = prepare_chromium_launch(s->profile_dir, &lock_error);
    }
    if (!available) {
        show_error(lock_error->message);
        g_error_free(lock_error);
        update_session_ui(s);
        return;
    }
    char **argv = browser_arguments(s->browser, s->profile_dir);

    GError *err = NULL;
    gboolean ok = g_spawn_async(
        NULL, argv, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL, &s->pid, &err);

    g_strfreev(argv);
    if (ok) {
        s->running = TRUE;
        g_child_watch_add(s->pid, cb_child_exit, s);
        update_session_ui(s);
    } else {
        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(main_win), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Failed to launch browser:\n%s", err->message);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_error_free(err);
    }
}

/* ═══════════════════════════════════════════════════════
   clear / delete a session's profile data
═══════════════════════════════════════════════════════ */
static void operation_ui(void)
{
    if (header_actions) gtk_widget_set_sensitive(header_actions, pending_operations == 0);
    for (guint i = 0; i < sessions->len; i++) {
        Session *s = g_ptr_array_index(sessions, i);
        if (s->frame) update_session_ui(s);
    }
}

typedef struct { Session *active, *snapshot; } ResetRequest;

static void reset_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)cancel;
    ResetRequest *request = data;
    GError *error = NULL;
    ProfileReset *reset = reset_prepare(request->active->profile_dir,
        request->snapshot->profile_dir, request->active->browser->family, &error);
    if (!reset) g_task_return_error(task, error);
    else {
        reset_release(reset);
        g_task_return_boolean(task, TRUE);
    }
}

static void reset_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    ResetRequest *request = data;
    GError *error = NULL;
    gboolean ok = g_task_propagate_boolean(G_TASK(result), &error);
    request->active->busy = FALSE;
    request->active->size_known = FALSE;
    request->active->size_pending = FALSE;
    request->active->size_generation++;
    pending_operations--;
    /* Keep the persisted recovery entry on failure too: a failure after an
     * exchange or interrupted durability check must never hide original data. */
    operation_ui();
    if (!ok) { show_error(error->message); g_error_free(error); }
    g_free(request);
}

static gboolean prepare_recovery(Session *s, Session **snapshot, GError **error)
{
    char *id = g_uuid_string_random();
    char *name = g_strdup_printf("%s (before clear)", s->name);
    Session *copy = new_session(id, name, s->browser);
    g_free(id); g_free(name);
    copy->archived = TRUE;
    copy->archived_at = g_get_real_time() / G_USEC_PER_SEC;
    g_ptr_array_add(retained_sessions, copy);
    /* Persist BEFORE touching directories. A crash leaves an accessible
     * recovery entry whether the exchange happened or not. */
    if (!save_sessions(error)) {
        g_ptr_array_remove_index(retained_sessions, retained_sessions->len - 1);
        return FALSE;
    }
    *snapshot = copy;
    return TRUE;
}

static void cb_clear(GtkButton *btn, gpointer data)
{
    (void)btn;
    Session *s = data;
    if (s->running || s->busy || pending_operations) return;
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(main_win), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO, "Reset %s?", s->name);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg),
        "Start with a fresh profile. The current cookies, logins, history and other data will be kept in Settings > Recovery. This does not free disk space.");
    gtk_window_set_title(GTK_WINDOW(dlg), "Confirm Clear");
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_NO);
    int response = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (response != GTK_RESPONSE_YES || s->running || s->busy || pending_operations) return;
    GError *error = NULL;
    Session *snapshot = NULL;
    if (!prepare_recovery(s, &snapshot, &error)) {
        show_error(error->message); g_error_free(error); return;
    }
    ResetRequest *request = g_new0(ResetRequest, 1);
    request->active = s; request->snapshot = snapshot;
    s->busy = TRUE; pending_operations++;
    operation_ui();
    GTask *task = g_task_new(NULL, NULL, reset_done, request);
    g_task_set_task_data(task, request, NULL);
    g_task_run_in_thread(task, reset_worker);
    g_object_unref(task);
}

/* Removing a session only changes metadata. Browser files remain intact. */
static gboolean remove_session(Session *s, gboolean unused, GError **error)
{
    (void)unused;
    if (pending_operations || s->busy || s->running || session_unavailable(s)) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_AGAIN,
            "Close this session's browser and wait for profile work before removing it.");
        return FALSE;
    }
    guint index;
    if (!g_ptr_array_find(sessions, s, &index)) return FALSE;
    g_ptr_array_steal_index(sessions, index);
    gint64 previous_date = s->archived_at;
    s->archived = TRUE;
    s->archived_at = g_get_real_time() / G_USEC_PER_SEC;
    g_ptr_array_add(retained_sessions, s);
    if (!save_sessions(error)) {
        g_ptr_array_steal_index(retained_sessions, retained_sessions->len - 1);
        s->archived = FALSE;
        s->archived_at = previous_date;
        g_ptr_array_insert(sessions, index, s);
        return FALSE;
    }
    return TRUE;
}

static gboolean restore_session(Session *s, GError **error)
{
    guint index;
    if (pending_operations || !g_ptr_array_find(retained_sessions, s, &index)) return FALSE;
    if (s->deletion_started) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
            "Permanent deletion has started. This entry cannot be restored; retry deletion or forget it after its files are gone.");
        return FALSE;
    }
    int root = open_profile_directory(s->profile_dir, error);
    if (root < 0) {
        if (error && *error) g_prefix_error(error, "Recovery data is missing or inaccessible. Nothing was restored. ");
        return FALSE;
    }
    close(root);
    g_ptr_array_steal_index(retained_sessions, index);
    s->archived = FALSE;
    g_ptr_array_add(sessions, s);
    if (!save_sessions(error)) {
        g_ptr_array_steal_index(sessions, sessions->len - 1);
        s->archived = TRUE;
        g_ptr_array_insert(retained_sessions, index, s);
        return FALSE;
    }
    return TRUE;
}

static gboolean rename_session(Session *s, const char *name, GError **error)
{
    char *label = g_strdup(name);
    g_strstrip(label);
    if (!*label || !g_utf8_validate(label, -1, NULL) || g_utf8_strlen(label, -1) > 80) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Use a name between 1 and 80 characters.");
        g_free(label); return FALSE;
    }
    char *old = s->name;
    s->name = label;
    if (!save_sessions(error)) { s->name = old; g_free(label); return FALSE; }
    g_free(old);
    return TRUE;
}

static void cb_rename(GtkButton *button, gpointer data)
{
    (void)button;
    Session *s = data;
    if (pending_operations) return;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Rename session", GTK_WINDOW(main_win),
        GTK_DIALOG_MODAL, "Cancel", GTK_RESPONSE_CANCEL, "Rename", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry), 80);
    gtk_entry_set_text(GTK_ENTRY(entry), s->name);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_container_set_border_width(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), entry, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        GError *error = NULL;
        if (!rename_session(s, gtk_entry_get_text(GTK_ENTRY(entry)), &error)) {
            show_error(error->message); g_error_free(error);
        } else {
            gtk_label_set_text(GTK_LABEL(s->name_lbl), s->name);
            gtk_widget_set_tooltip_text(s->name_lbl, s->name);
        }
    }
    gtk_widget_destroy(dialog);
}

static void cb_remove(GtkButton *button, gpointer data)
{
    (void)button;
    Session *s = data;
    if (s->running || s->busy || pending_operations) return;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(main_win), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "Remove %s?", s->name);
    gtk_window_set_title(GTK_WINDOW(dialog), "Remove session");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
        "Its browser data will be kept. Restore this session from Settings > Recovery. This does not free disk space.");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL, "Remove", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_ACCEPT) return;
    GError *error = NULL;
    if (!remove_session(s, FALSE, &error)) {
        if (error) { show_error(error->message); g_error_free(error); }
        update_session_ui(s); return;
    }
    gtk_widget_destroy(s->frame);
    s->name_lbl = s->frame = s->status_lbl = s->launch_btn = s->clear_btn = s->remove_btn = s->rename_btn = NULL;
    s->size_generation++; s->size_pending = FALSE;
    for (guint i = 0; i < sessions->len; i++) {
        Session *other = g_ptr_array_index(sessions, i);
        gtk_container_child_set(GTK_CONTAINER(session_grid), other->frame,
            "left-attach", (int)(i % 2), "top-attach", (int)(i / 2), NULL);
    }
    gtk_widget_set_visible(empty_label, sessions->len == 0);
}

/* ═══════════════════════════════════════════════════════
   build a single session card widget
═══════════════════════════════════════════════════════ */
/* Popovers provide GTK's native opening/closing transitions, keyboard
 * navigation and outside-click dismissal without an animation timer. */
static void cb_menu_toggle(GtkToggleButton *button, gpointer data)
{
    if (gtk_toggle_button_get_active(button)) gtk_popover_popup(GTK_POPOVER(data));
    else gtk_popover_popdown(GTK_POPOVER(data));
}

static void cb_menu_closed(GtkPopover *popover, gpointer data)
{
    GtkToggleButton *button = data;
    g_signal_handlers_block_by_func(button, cb_menu_toggle, popover);
    gtk_toggle_button_set_active(button, FALSE);
    g_signal_handlers_unblock_by_func(button, cb_menu_toggle, popover);
}

static void cb_menu_action(GtkButton *button, gpointer data)
{
    Session *s = data;
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
    gtk_popover_popdown(GTK_POPOVER(popover));
    if (GTK_WIDGET(button) == s->rename_btn) cb_rename(button, s);
    else if (GTK_WIDGET(button) == s->clear_btn) cb_clear(button, s);
    else if (GTK_WIDGET(button) == s->remove_btn) cb_remove(button, s);
}

static GtkWidget *session_menu_action(const char *label, Session *s)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_label_set_xalign(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), 0.0);
    g_signal_connect(button, "clicked", G_CALLBACK(cb_menu_action), s);
    return button;
}

static GtkWidget *create_card(Session *s)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    s->frame = frame;
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "isotab-card");
    gtk_widget_set_margin_start(frame, 4);
    gtk_widget_set_margin_end(frame, 4);
    gtk_widget_set_margin_top(frame, 4);
    gtk_widget_set_margin_bottom(frame, 4);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    s->name_lbl = gtk_label_new(s->name);
    gtk_label_set_xalign(GTK_LABEL(s->name_lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(s->name_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(s->name_lbl), 26);
    gtk_widget_set_tooltip_text(s->name_lbl, s->name);
    gtk_style_context_add_class(gtk_widget_get_style_context(s->name_lbl), "session-name");
    gtk_box_pack_start(GTK_BOX(header), s->name_lbl, TRUE, TRUE, 0);

    GtkWidget *more = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(more), svg_icon(ICON_MORE, 18));
    gtk_button_set_relief(GTK_BUTTON(more), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(more, "Session actions");
    atk_object_set_name(gtk_widget_get_accessible(more), "Session actions");
    GtkWidget *menu = gtk_popover_new(more);
    gtk_popover_set_position(GTK_POPOVER(menu), GTK_POS_BOTTOM);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(actions), 6);
    s->rename_btn = session_menu_action("Rename...", s);
    s->clear_btn = session_menu_action("Clear profile...", s);
    s->remove_btn = session_menu_action("Remove session...", s);
    gtk_box_pack_start(GTK_BOX(actions), s->rename_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(actions), s->clear_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), s->remove_btn, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(menu), actions);
    gtk_widget_show_all(actions);
    g_signal_connect(more, "toggled", G_CALLBACK(cb_menu_toggle), menu);
    g_signal_connect_object(menu, "closed", G_CALLBACK(cb_menu_closed), more, 0);
    g_object_set_data(G_OBJECT(more), "session-popover", menu);
    gtk_box_pack_end(GTK_BOX(header), more, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);

    GtkWidget *browser_label = gtk_label_new(s->browser->name);
    gtk_label_set_xalign(GTK_LABEL(browser_label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(browser_label), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), browser_label, FALSE, FALSE, 0);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    s->status_lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(s->status_lbl), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(s->status_lbl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(s->status_lbl), 25);
    gtk_style_context_add_class(gtk_widget_get_style_context(s->status_lbl), "dim-label");
    gtk_box_pack_start(GTK_BOX(footer), s->status_lbl, TRUE, TRUE, 0);
    s->launch_btn = gtk_button_new_with_label("Launch");
    gtk_widget_set_tooltip_text(s->launch_btn, "Launch or stop this session's browser");
    g_signal_connect(s->launch_btn, "clicked", G_CALLBACK(cb_launch), s);
    gtk_box_pack_end(GTK_BOX(footer), s->launch_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), footer, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), box);
    update_session_ui(s);
    return frame;
}

static void cb_add_session(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    if (pending_operations) return;
    detect_browsers();
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Add session", GTK_WINDOW(main_win),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL, "Add", GTK_RESPONSE_OK, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_set_spacing(GTK_BOX(box), 8);
    GtkWidget *combo = gtk_combo_box_text_new();
    int count = 0;
    for (guint i = 0; i < G_N_ELEMENTS(browsers); i++) {
        if (!browsers[i].executable) continue;
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), browsers[i].id, browsers[i].name);
        count++;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    if (preferences.default_browser && find_browser(preferences.default_browser)->executable)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), preferences.default_browser);
    GtkWidget *name = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(name), 80);
    gtk_entry_set_placeholder_text(GTK_ENTRY(name), "Session name (optional)");
    gtk_entry_set_activates_default(GTK_ENTRY(name), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(count ? "Browser" :
        "No supported browser found. Install Firefox, Chromium, Chrome or Brave."), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), name, FALSE, FALSE, 0);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_OK, count > 0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        Browser *browser = find_browser(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)));
        char *label = g_strdup(gtk_entry_get_text(GTK_ENTRY(name)));
        g_strstrip(label);
        char *id = g_uuid_string_random();
        Session *s = new_session(id, *label ? label : browser->name, browser);
        g_ptr_array_add(sessions, s);
        GError *error = NULL;
        if (!save_sessions(&error)) {
            g_ptr_array_remove_index(sessions, sessions->len - 1);
            show_error(error->message);
            g_error_free(error);
        } else {
            guint i = sessions->len - 1;
            gtk_grid_attach(GTK_GRID(session_grid), create_card(s), i % 2, i / 2, 1, 1);
            gtk_widget_show_all(session_grid);
            gtk_widget_hide(empty_label);
        }
        g_free(id);
        g_free(label);
    }
    gtk_widget_destroy(dialog);
}

static GtkWidget *settings_text(const char *text, gboolean selectable)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 62);
    gtk_label_set_selectable(GTK_LABEL(label), selectable);
    return label;
}

static GtkWidget *settings_page(GtkWidget *notebook, const char *title)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), box);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll, gtk_label_new(title));
    return box;
}

static void open_data_folder(GtkButton *button, gpointer data)
{
    (void)button;
    GError *error = NULL;
    char *uri = g_filename_to_uri(data_dir, NULL, &error);
    if (uri) gtk_show_uri_on_window(GTK_WINDOW(data), uri, GDK_CURRENT_TIME, &error);
    g_free(uri);
    if (error) { show_error(error->message); g_error_free(error); }
}

static gboolean recovery_files_missing(Session *s)
{
    char *trash = recovery_trash_path(s->id);
    gboolean missing = path_missing(s->profile_dir) && path_missing(trash);
    g_free(trash); return missing;
}

/* Success transfers ownership to the caller, which first destroys row widgets. */
static gboolean forget_recovery(Session *s, GError **error)
{
    guint index;
    if (!recovery_files_missing(s)) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_AGAIN,
            "Profile files still exist or are inaccessible. Use Delete permanently instead of forgetting this entry.");
        return FALSE;
    }
    if (!g_ptr_array_find(retained_sessions, s, &index)) return FALSE;
    g_ptr_array_steal_index(retained_sessions, index);
    if (!save_sessions(error)) { g_ptr_array_insert(retained_sessions, index, s); return FALSE; }
    return TRUE;
}

static gboolean mark_deletion_started(Session *s, GError **error)
{
    gboolean previous = s->deletion_started;
    s->deletion_started = TRUE;
    if (!save_sessions(error)) { s->deletion_started = previous; return FALSE; }
    s->size_generation++; s->size_pending = FALSE; s->size_known = FALSE;
    return TRUE;
}

static void recovery_row_destroy(GtkWidget *row, gpointer data)
{
    Session *s = data;
    if (s->recovery_row == row) {
        s->recovery_row = s->recovery_name_lbl = s->recovery_info_lbl = s->recovery_size_lbl = NULL;
        s->recovery_restore_btn = s->recovery_cleanup_btn = NULL;
    }
}

static void update_recovery_row(Session *s)
{
    if (!s->recovery_row) return;
    gboolean missing = recovery_files_missing(s);
    char *path = recovery_data_path(s);
    GDateTime *date = s->archived_at > 0 ? g_date_time_new_from_unix_local(s->archived_at) : NULL;
    char *when = date ? g_date_time_format(date, "%Y-%m-%d %H:%M:%S") : g_strdup("Date unavailable");
    gtk_label_set_text(GTK_LABEL(s->recovery_name_lbl), s->name);
    char *description = g_strdup_printf("%s · %s\n%s", s->browser->name, when, path);
    gtk_label_set_text(GTK_LABEL(s->recovery_info_lbl), description);
    g_free(description); g_free(path); g_free(when);
    if (date) g_date_time_unref(date);
    char *size = s->busy ? g_strdup("Deleting permanently...") : missing ? g_strdup("Profile folder missing") :
        s->deletion_started ? g_strdup("Deletion incomplete; remaining files cannot be restored") :
        !s->size_known ? g_strdup("Calculating size...") : s->size_mb < 0 ? g_strdup("Size unavailable") :
        g_strdup_printf("%.0f MB retained", s->size_mb);
    gtk_label_set_text(GTK_LABEL(s->recovery_size_lbl), size);
    g_free(size);
    gtk_widget_set_sensitive(s->recovery_restore_btn, !pending_operations && !missing && !s->deletion_started);
    gtk_widget_set_sensitive(s->recovery_cleanup_btn, !pending_operations);
    gtk_button_set_label(GTK_BUTTON(s->recovery_cleanup_btn), missing ? "Forget entry" :
        s->deletion_started ? "Retry delete" : "Delete permanently...");
    if (!missing && !s->busy) request_profile_size(s);
}

typedef struct { Session *session; GtkWidget *row, *dialog; gboolean was_started, detached; } PurgeRequest;
static void purge_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)cancel;
    PurgeRequest *request = data;
    GError *error = NULL;
    Session *s = request->session;
    if (!purge_profile(s->profile_dir, s->id, s->browser->family, &request->detached, &error)) g_task_return_error(task, error);
    else g_task_return_boolean(task, TRUE);
}

static void purge_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    PurgeRequest *request = data;
    Session *s = request->session;
    GError *error = NULL;
    gboolean ok = g_task_propagate_boolean(G_TASK(result), &error);
    s->busy = FALSE; s->size_known = FALSE; s->size_pending = FALSE; s->size_generation++;
    pending_operations--;
    if (!ok && !request->was_started && !request->detached) {
        s->deletion_started = FALSE;
        GError *rollback = NULL;
        if (!save_sessions(&rollback)) {
            s->deletion_started = TRUE;
            g_prefix_error(&error, "The deletion status could not be rolled back (%s). ", rollback->message);
            g_error_free(rollback);
        }
    }
    if (ok) ok = forget_recovery(s, &error);
    if (ok) { gtk_widget_destroy(request->row); session_free(s); }
    for (guint i = 0; i < retained_sessions->len; i++) update_recovery_row(g_ptr_array_index(retained_sessions, i));
    gtk_dialog_set_response_sensitive(GTK_DIALOG(request->dialog), GTK_RESPONSE_ACCEPT, TRUE);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(request->dialog), GTK_RESPONSE_CANCEL, TRUE);
    operation_ui();
    if (!ok) {
        char *message = g_strdup_printf("%s\n\n%s", request->detached ?
            "Permanent deletion did not finish. Some files may already be removed. The entry remains for retry or cleanup." :
            "Permanent deletion could not start. No profile files were deleted.", error->message);
        show_error(message); g_free(message); g_error_free(error);
    }
    g_object_unref(request->row); g_object_unref(request->dialog); g_free(request);
}

static void cb_recovery_cleanup(GtkButton *button, gpointer data)
{
    Session *s = data;
    if (pending_operations) return;
    GtkWidget *dialog = gtk_widget_get_toplevel(GTK_WIDGET(button));
    gboolean missing = recovery_files_missing(s);
    GtkWidget *confirm = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, missing ? "Forget %s?" : "Permanently delete %s?", s->name);
    gtk_window_set_title(GTK_WINDOW(confirm), "Recovery cleanup");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(confirm), missing ?
        "The profile folders are missing. This only removes the stale recovery entry." :
        "This permanently removes the remaining cookies, logins, history and other profile files. It cannot be undone. If deletion stops partway through, you can retry it from Recovery.");
    gtk_dialog_add_buttons(GTK_DIALOG(confirm), "Cancel", GTK_RESPONSE_CANCEL,
        missing ? "Forget entry" : "Delete permanently", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(confirm), GTK_RESPONSE_CANCEL);
    int response = gtk_dialog_run(GTK_DIALOG(confirm));
    gtk_widget_destroy(confirm);
    if (response != GTK_RESPONSE_ACCEPT || pending_operations) return;
    GError *error = NULL;
    if (missing) {
        if (!forget_recovery(s, &error)) {
            if (error) { show_error(error->message); g_error_free(error); }
            update_recovery_row(s); return;
        }
        gtk_widget_destroy(s->recovery_row); session_free(s); return;
    }
    gboolean was_started = s->deletion_started;
    if (!mark_deletion_started(s, &error)) { show_error(error->message); g_error_free(error); return; }
    s->busy = TRUE; pending_operations++;
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, FALSE);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL, FALSE);
    for (guint i = 0; i < retained_sessions->len; i++) update_recovery_row(g_ptr_array_index(retained_sessions, i));
    operation_ui();
    PurgeRequest *request = g_new0(PurgeRequest, 1);
    request->session = s; request->was_started = was_started;
    request->row = g_object_ref(s->recovery_row); request->dialog = g_object_ref(dialog);
    GTask *task = g_task_new(NULL, NULL, purge_done, request);
    g_task_set_task_data(task, request, NULL);
    g_task_run_in_thread(task, purge_worker); g_object_unref(task);
}

static gboolean recovery_dialog_close(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void)widget; (void)event; (void)data;
    return pending_operations != 0;
}

static void cb_restore(GtkButton *button, gpointer data)
{
    Session *s = data;
    GError *error = NULL;
    if (!restore_session(s, &error)) {
        if (error) { show_error(error->message); g_error_free(error); }
        return;
    }
    guint index = sessions->len - 1;
    gtk_grid_attach(GTK_GRID(session_grid), create_card(s), index % 2, index / 2, 1, 1);
    gtk_widget_show_all(session_grid);
    gtk_widget_hide(empty_label);
    gtk_widget_destroy(s->recovery_row);
}

static gboolean known_session_id(const char *id)
{
    if (session_by_id(id)) return TRUE;
    for (guint i = 0; i < retained_sessions->len; i++)
        if (!strcmp(((Session *)g_ptr_array_index(retained_sessions, i))->id, id)) return TRUE;
    return FALSE;
}

static void offer_import(GtkComboBoxText *combo, const char *id)
{
    if (!valid_session_id(id) || known_session_id(id)) return;
    Session *probe = new_session(id, id, find_browser("firefox"));
    int root = open_profile_directory(probe->profile_dir, NULL);
    if (root >= 0) { gtk_combo_box_text_append(combo, id, probe->profile_dir); close(root); }
    session_free(probe);
}

static void import_selection_changed(GtkComboBox *combo, gpointer data)
{
    GtkComboBox *profiles = g_object_get_data(G_OBJECT(data), "import-profiles");
    const char *id = gtk_combo_box_get_active_id(combo);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(data), GTK_RESPONSE_ACCEPT,
        id && *id && gtk_combo_box_get_active_id(profiles) != NULL);
}

static void cb_import(GtkButton *button, gpointer data)
{
    (void)button;
    if (pending_operations) return;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Import unlisted profile", GTK_WINDOW(data),
        GTK_DIALOG_MODAL, "Cancel", GTK_RESPONSE_CANCEL, "Import", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_set_spacing(GTK_BOX(box), 8);
    gtk_box_pack_start(GTK_BOX(box), settings_text(
        "Import a retained folder from an older IsoTab version. Select the browser originally used with it. Its files will stay in place.", FALSE), FALSE, FALSE, 0);
    GtkWidget *profiles = gtk_combo_box_text_new();
    char *parent = g_build_filename(data_dir, "profiles", NULL);
    GDir *directory = g_dir_open(parent, 0, NULL);
    const char *entry;
    if (directory) {
        while ((entry = g_dir_read_name(directory))) offer_import(GTK_COMBO_BOX_TEXT(profiles), entry);
        g_dir_close(directory);
    }
    g_free(parent);
    for (int i = 0; i < 10; i++) {
        char *id = g_strdup_printf("legacy_%d", i);
        offer_import(GTK_COMBO_BOX_TEXT(profiles), id);
        g_free(id);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(profiles), 0);
    GtkWidget *browser = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(browser), "", "Choose original browser...");
    for (guint i = 0; i < G_N_ELEMENTS(browsers); i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(browser), browsers[i].id, browsers[i].name);
    gtk_combo_box_set_active(GTK_COMBO_BOX(browser), 0);
    gtk_box_pack_start(GTK_BOX(box), profiles, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("Original browser"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), browser, FALSE, FALSE, 0);
    GtkWidget *name = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(name), 80);
    gtk_entry_set_placeholder_text(GTK_ENTRY(name), "Session name (optional)");
    gtk_box_pack_start(GTK_BOX(box), name, FALSE, FALSE, 0);
    gboolean available = gtk_combo_box_get_active_id(GTK_COMBO_BOX(profiles)) != NULL;
    if (!available) gtk_box_pack_start(GTK_BOX(box), gtk_label_new("No unlisted profile folders found."), FALSE, FALSE, 0);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, FALSE);
    g_object_set_data(G_OBJECT(dialog), "import-profiles", profiles);
    g_signal_connect(browser, "changed", G_CALLBACK(import_selection_changed), dialog);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(profiles));
        Browser *b = find_browser(gtk_combo_box_get_active_id(GTK_COMBO_BOX(browser)));
        if (!b) {
            show_error("Choose the original browser before importing this profile.");
        } else if (g_str_has_prefix(id, "legacy_") && b->family != FAMILY_FIREFOX) {
            show_error("Legacy session_N folders require a Firefox-family browser.");
        } else if (!known_session_id(id)) {
            char *label = g_strdup(gtk_entry_get_text(GTK_ENTRY(name)));
            g_strstrip(label);
            Session *s = new_session(id, *label ? label : "Imported profile", b);
            g_free(label);
            GError *error = NULL;
            if (!validate_import_profile(s->profile_dir, b, &error)) {
                show_error(error->message); g_error_free(error); session_free(s);
            } else {
                g_ptr_array_add(sessions, s);
                if (!save_sessions(&error)) {
                    g_ptr_array_remove_index(sessions, sessions->len - 1);
                    show_error(error->message); g_error_free(error);
                } else {
                    guint index = sessions->len - 1;
                    gtk_grid_attach(GTK_GRID(session_grid), create_card(s), index % 2, index / 2, 1, 1);
                    gtk_widget_show_all(session_grid);
                    gtk_widget_hide(empty_label);
                }
            }
        }
    }
    gtk_widget_destroy(dialog);
}

static void cb_settings(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    if (pending_operations) return;
    detect_browsers();
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Settings", GTK_WINDOW(main_win),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 620, 480);
    g_signal_connect(dialog, "delete-event", G_CALLBACK(recovery_dialog_close), NULL);
    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
        notebook, TRUE, TRUE, 0);
    GtkWidget *general = settings_page(notebook, "Preferences");
    gtk_box_pack_start(GTK_BOX(general), settings_text("Default browser for new sessions", FALSE), FALSE, FALSE, 0);
    GtkWidget *browser = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(browser), "", "First available browser");
    for (guint i = 0; i < G_N_ELEMENTS(browsers); i++) {
        Browser *b = &browsers[i];
        if (!b->executable && g_strcmp0(preferences.default_browser, b->id)) continue;
        char *label = g_strdup_printf("%s%s", b->name, b->executable ? "" : " (not installed)");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(browser), b->id, label);
        g_free(label);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(browser),
        preferences.default_browser ? preferences.default_browser : "");
    gtk_box_pack_start(GTK_BOX(general), browser, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(general), settings_text(
        "Used when adding a session. Existing sessions keep their assigned browser.", FALSE), FALSE, FALSE, 0);
    GtkWidget *sizes = gtk_check_button_new_with_label("Show profile sizes on session cards");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sizes), preferences.show_sizes);
    gtk_box_pack_start(GTK_BOX(general), sizes, FALSE, FALSE, 0);
    GtkWidget *shortcuts = gtk_check_button_new_with_label("Enable Alt+1 through Alt+0 quick launch");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(shortcuts), preferences.shortcuts);
    gtk_box_pack_start(GTK_BOX(general), shortcuts, FALSE, FALSE, 0);

    GtkWidget *storage = settings_page(notebook, "Storage");
    char *description = g_strdup_printf(
        "Profile storage\n%s\n\nNew profiles\n%s/profiles/<session ID>\n\nSession list and preferences\n%s/sessions.ini",
        data_dir, data_dir, data_dir);
    gtk_box_pack_start(GTK_BOX(storage), settings_text(description, TRUE), FALSE, FALSE, 0);
    g_free(description);
    GtkWidget *open = gtk_button_new_with_label("Open data folder");
    g_signal_connect(open, "clicked", G_CALLBACK(open_data_folder), dialog);
    gtk_box_pack_start(GTK_BOX(storage), open, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(storage), settings_text(
        "Close your browsers before backing up this folder. Existing Firefox profiles stay in their original session_N folders.", FALSE), FALSE, FALSE, 0);
    for (guint i = 0; i < sessions->len; i++) {
        Session *s = g_ptr_array_index(sessions, i);
        char *line = g_strdup_printf("%s (%s)\n%s", s->name, s->browser->name, s->profile_dir);
        gtk_box_pack_start(GTK_BOX(storage), settings_text(line, TRUE), FALSE, FALSE, 0);
        g_free(line);
    }

    GtkWidget *recovery = settings_page(notebook, "Recovery");
    gtk_box_pack_start(GTK_BOX(recovery), settings_text(
        "Restore adds a separate session. Delete permanently frees disk space and cannot be undone. Missing folders can be forgotten.", FALSE), FALSE, FALSE, 0);
    GtkWidget *import = gtk_button_new_with_label("Import unlisted profile");
    g_signal_connect(import, "clicked", G_CALLBACK(cb_import), dialog);
    gtk_box_pack_start(GTK_BOX(recovery), import, FALSE, FALSE, 0);
    if (!retained_sessions->len)
        gtk_box_pack_start(GTK_BOX(recovery), settings_text("No recovery profiles yet.", FALSE), FALSE, FALSE, 0);
    for (guint i = 0; i < retained_sessions->len; i++) {
        Session *s = g_ptr_array_index(retained_sessions, i);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        s->recovery_row = row;
        g_signal_connect(row, "destroy", G_CALLBACK(recovery_row_destroy), s);
        s->recovery_name_lbl = settings_text(s->name, FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(s->recovery_name_lbl), "session-name");
        s->recovery_info_lbl = settings_text("", TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(s->recovery_info_lbl), "dim-label");
        gtk_box_pack_start(GTK_BOX(row), s->recovery_name_lbl, FALSE, FALSE, 0);
        s->recovery_size_lbl = settings_text("", FALSE);
        gtk_box_pack_start(GTK_BOX(row), s->recovery_info_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), s->recovery_size_lbl, FALSE, FALSE, 0);
        GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        s->recovery_restore_btn = gtk_button_new_with_label("Restore");
        g_signal_connect(s->recovery_restore_btn, "clicked", G_CALLBACK(cb_restore), s);
        s->recovery_cleanup_btn = gtk_button_new_with_label("Delete permanently...");
        g_signal_connect(s->recovery_cleanup_btn, "clicked", G_CALLBACK(cb_recovery_cleanup), s);
        gtk_box_pack_start(GTK_BOX(actions), s->recovery_restore_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(actions), s->recovery_cleanup_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), actions, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);
        gtk_box_pack_start(GTK_BOX(recovery), row, FALSE, FALSE, 0);
        update_recovery_row(s);
    }

    GtkWidget *tips = settings_page(notebook, "Tips");
    gtk_box_pack_start(GTK_BOX(tips), settings_text(
        "Add session chooses a browser and an optional name. Different sessions can run at the same time.\n\n"
        "Clear starts fresh and keeps the original profile in Recovery. Remove moves the session to Recovery. Both ask for confirmation and retain disk usage. Rename changes its label.\n\n"
        "Close a session's browser before clearing or removing it. Closing IsoTab leaves browser windows open; reopening reconnects verified browser sessions and restores Stop.\n\n"
        "Alt+1 through Alt+0 launch the first ten sessions when enabled. Ctrl+Q closes IsoTab.\n\n"
        "Supported browsers: Firefox, Firefox ESR, Chromium, Chrome, Brave, Edge and Vivaldi. Install a native browser in PATH and it will be detected automatically. Tor, LibreWolf, Flatpak and Snap adapters are not included.\n\n"
        "Separate profiles keep browser data apart. They do not change your IP address or add another operating-system sandbox.", FALSE), FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    int response;
    do { response = gtk_dialog_run(GTK_DIALOG(dialog)); } while (pending_operations);
    if (response == GTK_RESPONSE_ACCEPT) {
        Preferences old = preferences;
        preferences.show_sizes = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sizes));
        preferences.shortcuts = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(shortcuts));
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(browser));
        preferences.default_browser = id && *id ? g_strdup(id) : NULL;
        GError *error = NULL;
        if (!save_sessions(&error)) {
            g_free(preferences.default_browser);
            preferences = old;
            show_error(error->message);
            g_error_free(error);
        } else {
            g_free(old.default_browser);
            for (guint i = 0; i < sessions->len; i++)
                update_session_ui(g_ptr_array_index(sessions, i));
        }
    }
    gtk_widget_destroy(dialog);
}

/* ═══════════════════════════════════════════════════════
   keyboard shortcuts
═══════════════════════════════════════════════════════ */
static gboolean cb_close(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void)widget; (void)event; (void)data;
    return pending_operations != 0;
}

static gboolean cb_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;

    /* Alt+1..Alt+0 → launch session 1..10 */
    if (preferences.shortcuts && (ev->state & GDK_MOD1_MASK)) {
        int idx = -1;
        if (ev->keyval >= GDK_KEY_1 && ev->keyval <= GDK_KEY_9)
            idx = ev->keyval - GDK_KEY_1;
        else if (ev->keyval == GDK_KEY_0)
            idx = 9;

        if (idx >= 0 && (guint)idx < sessions->len && !((Session *)g_ptr_array_index(sessions, idx))->running &&
            !((Session *)g_ptr_array_index(sessions, idx))->external_pid) {
            cb_launch(NULL, g_ptr_array_index(sessions, idx));
            return TRUE;
        }
    }
    /* Ctrl+Q → quit */
    if ((ev->state & GDK_CONTROL_MASK) &&
        (ev->keyval == GDK_KEY_q || ev->keyval == GDK_KEY_Q)) {
        if (!pending_operations) gtk_main_quit();
        return TRUE;
    }
    return FALSE;
}

static gboolean refresh_sessions(gpointer data)
{
    (void)data;
    detect_browsers();
    for (guint i = 0; i < sessions->len; i++) {
        Session *s = g_ptr_array_index(sessions, i);
        if (!s->busy && !s->running && (s->unavailable != session_unavailable(s) ||
            s->external_pid != recovered_session_pid(s) ||
            s->installed != (s->browser->executable != NULL)))
            update_session_ui(s);
    }
    return G_SOURCE_CONTINUE;
}

/* ═══════════════════════════════════════════════════════
   main
═══════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    if (geteuid() == 0) {
        g_printerr("Run IsoTab as your regular desktop user, not as root.\n");
        return 1;
    }
    gtk_init(&argc, &argv);
    /* Keep copyable paths from appearing selected when a settings tab opens. */
    g_object_set(gtk_settings_get_default(), "gtk-label-select-on-focus", FALSE, NULL);
    gtk_window_set_default_icon_name("isotab");
    GdkPixbuf *app_icon = gdk_pixbuf_new_from_resource_at_scale(
        "/org/isotab/isotab.svg", 128, 128, TRUE, NULL);
    if (app_icon) {
        gtk_window_set_default_icon(app_icon);
        g_object_unref(app_icon);
    }

    detect_browsers();
    data_dir = g_build_filename(g_get_home_dir(), ".isotab", NULL);
    if (g_mkdir_with_parents(data_dir, 0700) != 0) {
        show_error(g_strerror(errno));
        return 1;
    }
    int data_fd = open_profile_directory(data_dir, NULL);
    struct stat root_stat;
    if (data_fd < 0 || fstat(data_fd, &root_stat) != 0 || (root_stat.st_mode & 0077)) {
        if (data_fd >= 0) close(data_fd);
        show_error("IsoTab's data folder must be owned by you and private (mode 0700), without symlink path components.");
        return 1;
    }
    int app_fd = open_lock_at(data_fd, "launcher.lock");
    close(data_fd);
    if (app_fd < 0 || flock(app_fd, LOCK_EX | LOCK_NB) != 0) {
        show_error("IsoTab is already open, or its settings directory is inaccessible.");
        if (app_fd >= 0) close(app_fd);
        return 1;
    }
    GError *error = NULL;
    if (!load_sessions(&error)) {
        show_error(error->message);
        g_error_free(error);
        close(app_fd);
        return 1;
    }

    /* ── CSS theme ── */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window {"
        "  background: @theme_bg_color;"
        "}"
        ".isotab-card > border {"
        "  border-radius: 8px;"
        "}"
        ".session-name {"
        "  font-weight: bold;"
        "  font-size: 15px;"
        "}"
        , -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* ── main window ── */
    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), APP_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(main_win), 720, 500);
    g_signal_connect(main_win, "delete-event", G_CALLBACK(cb_close), NULL);
    g_signal_connect(main_win, "destroy",
        G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(main_win, "key-press-event",
        G_CALLBACK(cb_key), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(root,  14);
    gtk_widget_set_margin_end(root,    14);
    gtk_widget_set_margin_top(root,    10);
    gtk_widget_set_margin_bottom(root, 10);

    /* ── header ── */
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
        "<span font_size='x-large' font_weight='bold'>"
        "IsoTab</span>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);

    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(heading), svg_icon(NULL, 32), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(heading), title, FALSE, FALSE, 0);

    GtkWidget *sub = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(sub),
        "<span foreground='#888'>"
        "Your browsers, separate profiles.</span>");
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(sub), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(sub), 80);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 6);
    gtk_widget_set_margin_bottom(sep, 10);

    session_grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(session_grid), TRUE);
    for (guint i = 0; i < sessions->len; i++)
        gtk_grid_attach(GTK_GRID(session_grid), create_card(g_ptr_array_index(sessions, i)),
            i % 2, i / 2, 1, 1);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), session_grid);
    empty_label = gtk_label_new("Add a session to get started.");
    gtk_widget_set_no_show_all(empty_label, TRUE);
    header_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_end(GTK_BOX(heading), header_actions, FALSE, FALSE, 0);
    GtkWidget *settings_button = gtk_button_new_with_label("Settings");
    set_action(settings_button, "", ICON_SETTINGS);
    gtk_widget_set_tooltip_text(settings_button, "Settings and recovery");
    atk_object_set_name(gtk_widget_get_accessible(settings_button), "Settings and recovery");
    gtk_box_pack_end(GTK_BOX(header_actions), settings_button, FALSE, FALSE, 0);
    g_signal_connect(settings_button, "clicked", G_CALLBACK(cb_settings), NULL);
    GtkWidget *add_button = gtk_button_new_with_label("Add session");
    gtk_box_pack_end(GTK_BOX(header_actions), add_button, FALSE, FALSE, 0);
    g_signal_connect(add_button, "clicked", G_CALLBACK(cb_add_session), NULL);

    /* ── assemble ── */
    gtk_box_pack_start(GTK_BOX(root), heading, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), sub,   FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(root), sep,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), empty_label, FALSE, FALSE, 12);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(main_win), root);
    gtk_widget_show_all(main_win);
    gtk_widget_set_visible(empty_label, sessions->len == 0);

    g_timeout_add_seconds(2, refresh_sessions, NULL);
    gtk_main();
    close(app_fd);
    return 0;
}
