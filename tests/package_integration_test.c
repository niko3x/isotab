/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#define _GNU_SOURCE
#define main isotab_application_main
#include "../browser.c"
#undef main

static pid_t tor_daemon_for(const char *profile)
{
    char *data = g_build_filename(profile, "tor-data", NULL);
    GDir *processes = g_dir_open("/proc", 0, NULL);
    const char *entry;
    pid_t pid = 0;
    while (processes && !pid && (entry = g_dir_read_name(processes))) {
        if (!g_ascii_isdigit(*entry)) continue;
        char *path = g_build_filename("/proc", entry, "cmdline", NULL);
        char *args = NULL; gsize length = 0;
        if (g_file_get_contents(path, &args, &length, NULL)) {
            gboolean after_data = FALSE;
            for (gsize i = 0; i < length;) {
                const char *arg = args + i;
                const char *end = memchr(arg, '\0', length - i);
                if (!end) break;
                if (after_data && !strcmp(arg, data)) { pid = atoi(entry); break; }
                after_data = !strcmp(arg, "DataDirectory");
                i = end - args + 1;
            }
        }
        g_free(args); g_free(path);
    }
    if (processes) g_dir_close(processes);
    g_free(data); return pid;
}

static void terminate_test_child(GPid pid)
{
    if (!pid) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 100; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid) { g_spawn_close_pid(pid); return; }
        g_usleep(100000);
    }
    kill(pid, SIGKILL); waitpid(pid, NULL, 0); g_spawn_close_pid(pid);
}

static gboolean test_tor(Browser *tor)
{
    GError *error = NULL;
    data_dir = g_dir_make_tmp("isotab-tor-live-XXXXXX", &error);
    g_assert_no_error(error);
    int parent = ensure_browser_profile_root(tor, data_dir, &error);
    g_assert_no_error(error); close(parent);
    Session *profiles[2] = {NULL};
    pid_t daemons[2] = {0};
    int daemon_fds[2] = {-1, -1};
    gboolean ok = TRUE;
    for (int i = 0; i < 2; i++) {
        char *id = g_uuid_string_random();
        profiles[i] = new_session(id, "Tor test", tor); g_free(id);
        int root = ensure_profile_directory(profiles[i]->profile_dir, &error);
        g_assert_no_error(error); close(root);
        g_assert_true(prepare_browser_profile(tor, profiles[i]->profile_dir, &error));
        char *prefs = g_build_filename(profiles[i]->profile_dir, "user.js", NULL);
        int fd = open(prefs, O_WRONLY | O_APPEND);
        const char *disable_update = "user_pref(\"app.update.disabledForTesting\", true);\n";
        g_assert_cmpint(write(fd, disable_update, strlen(disable_update)), ==, (ssize_t)strlen(disable_update));
        close(fd); g_free(prefs);
        char **argv = browser_arguments(tor, profiles[i]->profile_dir);
        argv = g_realloc_n(argv, 7, sizeof(char *));
        argv[4] = g_strdup("--headless"); argv[5] = g_strdup("about:blank"); argv[6] = NULL;
        char **env = browser_environment(tor);
        char *cwd = g_path_get_dirname(tor->executable);
        ok = g_spawn_async(cwd, argv, env,
            G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
            NULL, NULL, &profiles[i]->pid, &error);
        g_strfreev(argv); g_strfreev(env); g_free(cwd);
        if (!ok) { g_printerr("Tor launch failed: %s\n", error->message); break; }
    }
    for (int retry = 0; ok && retry < 300; retry++) {
        gboolean running = TRUE;
        for (int i = 0; i < 2; i++) {
            daemons[i] = tor_daemon_for(profiles[i]->profile_dir);
            profiles[i]->external_pid = recovered_session_pid(profiles[i]);
            running &= daemons[i] > 0 && profiles[i]->external_pid > 0;
        }
        if (running) break;
        if (retry == 299) { g_printerr("Two isolated Tor browser/daemon pairs did not start.\n"); ok = FALSE; }
        g_usleep(100000);
    }
    if (ok) {
        g_assert_cmpint(daemons[0], !=, daemons[1]);
        g_assert_cmpint(profiles[0]->external_pid, !=, profiles[1]->external_pid);
        for (int i = 0; i < 2; i++) daemon_fds[i] = syscall(SYS_pidfd_open, daemons[i], 0);
        for (int i = 0; i < 2; i++) {
            char *id = g_uuid_string_random();
            Session *snapshot = new_session(id, "Snapshot", tor); g_free(id);
            g_assert_null(reset_prepare(profiles[i]->profile_dir, snapshot->profile_dir, FAMILY_FIREFOX, &error));
            g_assert_nonnull(error); g_clear_error(&error); session_free(snapshot);
            g_assert_true(stop_recovered_session(profiles[i], &error)); g_assert_no_error(error);
        }
    }
    for (int i = 0; i < 2; i++) if (profiles[i]) terminate_test_child(profiles[i]->pid);
    for (int i = 0; i < 2; i++) if (daemons[i]) {
        int root = open_profile_directory(profiles[i]->profile_dir, NULL);
        int guard = -1;
        for (int retry = 0; retry < 100 && guard == -1; retry++) {
            guard = tor_data_guard_at(root, NULL);
            if (guard == -1) g_usleep(100000);
        }
        if (guard == -1) {
            g_printerr("Tor daemon retained its data lock after Stop.\n");
            if (daemon_fds[i] >= 0) syscall(SYS_pidfd_send_signal, daemon_fds[i], SIGTERM, NULL, 0);
            ok = FALSE;
        } else if (guard >= 0) close(guard);
        if (root >= 0) close(root);
        if (daemon_fds[i] >= 0) close(daemon_fds[i]);
    }
    if (ok) for (int i = 0; i < 2; i++) {
        char *id = g_uuid_string_random();
        Session *snapshot = new_session(id, "Snapshot", tor); g_free(id);
        ProfileReset *reset = reset_prepare(profiles[i]->profile_dir, snapshot->profile_dir, FAMILY_FIREFOX, &error);
        g_assert_nonnull(reset); g_assert_no_error(error); reset_release(reset);
        gboolean started = FALSE;
        g_assert_true(purge_profile(snapshot->profile_dir, snapshot->id, FAMILY_FIREFOX, &started, &error));
        g_assert_true(started); g_assert_no_error(error); session_free(snapshot);
    }
    if (ok) {
        GFile *tree = g_file_new_for_path(data_dir);
        g_assert_true(delete_tree(tree, &error)); g_assert_no_error(error); g_object_unref(tree);
        g_print("Live Tor integration passed: two browser/daemon pairs, reconnect/Stop, live reset refusal, reset and purge after exit.\n");
    } else g_printerr("Tor test profiles retained at %s\n", data_dir);
    for (int i = 0; i < 2; i++) if (profiles[i]) session_free(profiles[i]);
    g_free(data_dir); return ok;
}

/* Use an installed Flatpak's runtime with an inert command, not its application.
 * It writes a genuine inner-PID marker and opens the explicitly granted fixture. */
static gboolean test_flatpak(const char *application)
{
    GError *error = NULL;
    char *flatpak = g_find_program_in_path("flatpak");
    if (!flatpak) { g_print("SKIP: Flatpak is unavailable.\n"); return TRUE; }
    char *cache = g_build_filename(g_get_user_cache_dir(), "isotab-package-test-XXXXXX", NULL);
    char *base = g_mkdtemp_full(cache, 0700);
    g_assert_nonnull(base);
    int root = open_profile_directory(base, &error); g_assert_no_error(error);
    int tag = open_lock_at(root, ".isotab-sandbox"); close(tag);
    char *grant = g_strdup_printf("--filesystem=%s:rw", base);
    char *script = g_build_filename(base, "marker.sh", NULL);
    g_assert_true(g_file_set_contents(script,
        "#!/bin/sh\nprofile=$1\nexec 9<\"$profile\"\nln -s \"$(hostname)-$$\" \"$profile/SingletonLock\"\nwhile :; do sleep 1; done\n", -1, NULL));
    char *argv[] = {flatpak, "run", grant, "--command=sh", (char *)application, script, base, NULL};
    GPid wrapper = 0;
    gboolean ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &wrapper, &error);
    Session observer = {.profile_dir = base, .browser = find_browser("chromium-flatpak")};
    pid_t owner = 0;
    for (int retry = 0; ok && !owner && retry < 100; retry++) { g_usleep(100000); owner = profile_owner(&observer); }
    if (!owner) { g_printerr("Flatpak test command did not acquire its sandbox marker.\n"); ok = FALSE; }
    if (owner) {
        g_assert_false(chromium_available_at(root, &error)); g_clear_error(&error);
        gboolean uncertain = FALSE;
        g_assert_true(process_uses_root(owner, root, &uncertain));
        /* This is a known inert test shell, deliberately not accepted by Stop. */
        g_assert_false(process_matches_session(&observer, owner));
        kill(owner, SIGTERM);
    }
    terminate_test_child(wrapper);
    if (ok) {
        g_assert_true(prepare_chromium_launch(base, &error)); g_assert_no_error(error);
        GFile *tree = g_file_new_for_path(base);
        g_assert_true(delete_tree(tree, &error)); g_assert_no_error(error); g_object_unref(tree);
        g_print("Live Flatpak namespace integration passed: narrow profile grant, inner-to-host PID resolution, active protection and stale recovery.\n");
    } else g_printerr("Flatpak test fixture retained at %s\n", base);
    close(root); g_free(cache); g_free(flatpak); g_free(grant); g_free(script); return ok;
}

int main(void)
{
    alarm(120);
    detect_browsers();
    Browser *tor = find_browser("tor-browser");
    gboolean ok = TRUE;
    if (tor->executable) ok = test_tor(tor);
    else g_print("SKIP: install Tor Browser 15+ for live Tor integration.\n");
    const char *flatpak_app = g_getenv("ISOTAB_TEST_FLATPAK_APP");
    if (flatpak_app && *flatpak_app) ok = test_flatpak(flatpak_app) && ok;
    else g_print("SKIP: set ISOTAB_TEST_FLATPAK_APP to an installed app ID to test a real Flatpak namespace.\n");
    return ok ? 0 : 1;
}
