/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#define _GNU_SOURCE
#define main isotab_application_main
#include "../browser.c"
#undef main

static void write_fixture(const char *path, const char *contents, gboolean executable)
{
    g_assert_true(g_file_set_contents(path, contents, -1, NULL));
    /* GLib versions differ in permission preservation on replacement. A
     * fixture must stay private even with the VM user's default umask 0002. */
    g_assert_cmpint(chmod(path, executable ? 0700 : 0600), ==, 0);
}

int main(void)
{
    alarm(40);
    GError *error = NULL;
    data_dir = g_dir_make_tmp("isotab-adapters-XXXXXX", &error);
    g_assert_no_error(error);
    Browser *flatpak = find_browser("firefox-flatpak");
    flatpak->executable = g_strdup("/test/flatpak with spaces");
    char **argv = browser_arguments(flatpak, "/home/test/a ' quoted profile");
    g_assert_cmpstr(argv[1], ==, "run");
    g_assert_cmpstr(argv[2], ==, "--filesystem=/home/test/a ' quoted profile:rw");
    g_assert_cmpstr(argv[3], ==, "org.mozilla.firefox");
    g_assert_cmpstr(argv[4], ==, "--profile");
    g_assert_cmpstr(argv[5], ==, "/home/test/a ' quoted profile");
    g_assert_cmpstr(argv[6], ==, "--no-remote");
    g_assert_null(argv[7]); g_strfreev(argv);
    Browser *snap = find_browser("chromium-snap");
    snap->executable = g_strdup("/test/snap");
    argv = browser_arguments(snap, "/home/test/snap/chromium/common/isotab/profiles/id");
    g_assert_cmpstr(argv[1], ==, "run");
    g_assert_cmpstr(argv[2], ==, "chromium");
    g_assert_cmpstr(argv[3], ==, "--user-data-dir=/home/test/snap/chromium/common/isotab/profiles/id");
    g_assert_null(argv[6]); g_strfreev(argv);
    Session *snap_session = new_session("a113f875-8232-4e1d-b23a-eaa491a08cab", "Snap", snap);
    char *snap_root = g_build_filename(g_get_home_dir(), "snap", "chromium", "common", "isotab", "profiles", NULL);
    g_assert_true(g_str_has_prefix(snap_session->profile_dir, snap_root));
    char *snap_trash = recovery_trash_path_for_profile(snap_session->profile_dir, snap_session->id);
    char *expected_trash = g_build_filename(g_get_home_dir(), "snap", "chromium", "common", "isotab", "trash", snap_session->id, NULL);
    g_assert_cmpstr(snap_trash, ==, expected_trash);
    g_free(snap_trash); g_free(expected_trash);
    session_free(snap_session); g_free(snap_root);

    /* Snap operations stay within its revision-independent store and trash. */
    char *snap_home = g_build_filename(data_dir, "snap-test-home", NULL);
    int snap_fd = ensure_profile_directory(snap_home, &error); g_assert_no_error(error); close(snap_fd);
    snap_fd = ensure_browser_profile_root_for_home(snap, data_dir, snap_home, &error);
    g_assert_no_error(error); close(snap_fd);
    snap_root = browser_profile_root_for_home(snap, data_dir, snap_home);
    char *snap_original = g_build_filename(snap_root, "active", NULL);
    char *snap_snapshot = g_build_filename(snap_root, "recovery", NULL);
    snap_fd = ensure_profile_directory(snap_original, &error); g_assert_no_error(error); close(snap_fd);
    char *snap_cookie = g_build_filename(snap_original, "sentinel", NULL);
    write_fixture(snap_cookie, "preserved", FALSE); g_free(snap_cookie);
    ProfileReset *snap_reset = reset_prepare(snap_original, snap_snapshot, FAMILY_CHROMIUM, &error);
    g_assert_nonnull(snap_reset); g_assert_no_error(error); reset_release(snap_reset);
    snap_cookie = g_build_filename(snap_snapshot, "sentinel", NULL);
    g_assert_true(g_file_test(snap_cookie, G_FILE_TEST_IS_REGULAR)); g_free(snap_cookie);
    gboolean snap_started = FALSE;
    g_assert_true(purge_profile(snap_snapshot, "recovery", FAMILY_CHROMIUM, &snap_started, &error));
    g_assert_true(snap_started); g_assert_no_error(error);
    g_assert_true(path_missing(snap_snapshot));
    g_free(snap_home); g_free(snap_root); g_free(snap_original); g_free(snap_snapshot);

    /* Discovery is asynchronous and only exact installed application IDs match. */
    char *fake_flatpak = g_build_filename(data_dir, "fake-flatpak", NULL);
    write_fixture(fake_flatpak, "#!/bin/sh\nprintf '%s\\n' org.mozilla.firefox io.gitlab.librewolf-community org.chromium.Chromium-extra\n", TRUE);
    flatpak_last_scan = 0;
    refresh_flatpak_apps(fake_flatpak);
    g_assert_true(flatpak_scan_pending);
    while (flatpak_scan_pending) g_main_context_iteration(NULL, TRUE);
    g_assert_true(g_hash_table_contains(flatpak_apps, "org.mozilla.firefox"));
    g_assert_true(g_hash_table_contains(flatpak_apps, "io.gitlab.librewolf-community"));
    g_assert_false(g_hash_table_contains(flatpak_apps, "org.chromium.Chromium"));
    char *fake_snap = g_build_filename(data_dir, "snap", NULL);
    write_fixture(fake_snap, "#!/bin/sh\nexit 0\n", TRUE);
    char *wrapper = g_build_filename(data_dir, "firefox", NULL);
    g_assert_cmpint(symlink(fake_snap, wrapper), ==, 0);
    g_assert_true(packaged_wrapper(wrapper));
    char *flatpak_wrapper = g_build_filename(data_dir, "flatpak-firefox", NULL);
    write_fixture(flatpak_wrapper, "#!/bin/sh\nexec flatpak run org.mozilla.firefox \"$@\"\n", TRUE);
    g_assert_true(packaged_wrapper(flatpak_wrapper)); g_free(flatpak_wrapper);

    /* A new Tor profile gets only session-local paths, not another user's prefs. */
    char *bundle = g_build_filename(data_dir, "bundle", NULL);
    char *tor_dir = g_build_filename(bundle, "TorBrowser", "Tor", NULL);
    g_assert_cmpint(g_mkdir_with_parents(tor_dir, 0700), ==, 0);
    char *exe = g_build_filename(bundle, "firefox", NULL);
    char *tor_exe = g_build_filename(tor_dir, "tor", NULL);
    char *ini = g_build_filename(bundle, "application.ini", NULL);
    char *version = g_build_filename(bundle, "tbb_version.json", NULL);
    write_fixture(exe, "#!/bin/sh\nexit 0\n", TRUE);
    write_fixture(tor_exe, "#!/bin/sh\nexit 0\n", TRUE);
    write_fixture(ini, "; Mozilla INI comment\n[App]\nVendor=Tor Project\n", FALSE);
    write_fixture(version, "{\"version\":\"15.0.22\"}", FALSE);
    char *found = tor_bundle_executable(bundle);
    g_assert_cmpstr(found, ==, exe); g_free(found);
    write_fixture(version, "{\"version\":\"14.0.0\"}", FALSE);
    g_assert_null(tor_bundle_executable(bundle));
    g_assert_cmpint(unlink(version), ==, 0);
    g_assert_cmpint(mkfifo(version, 0600), ==, 0);
    g_assert_null(tor_bundle_executable(bundle));
    g_assert_cmpint(unlink(version), ==, 0);
    write_fixture(version, "{\"version\":\"15.0.22\"}", FALSE);
    Browser *tor = find_browser("tor-browser");
    tor->executable = g_strdup(exe);
    Browser *tor_flatpak = find_browser("tor-browser-flatpak");
    tor_flatpak->executable = g_strdup("/test/flatpak");
    tor_flatpak->bundle_executable = g_strdup(exe);
    argv = browser_arguments(tor_flatpak, "/test/isolated-profile");
    char *command = g_strdup_printf("--command=%s", exe);
    g_assert_cmpstr(argv[3], ==, command); g_free(command);
    g_assert_cmpstr(argv[5], ==, "--env=TOR_SOCKS_PORT=0");
    g_assert_cmpstr(argv[6], ==, "org.torproject.torbrowser-launcher");
    g_assert_cmpstr(argv[7], ==, "--profile");
    g_assert_cmpstr(argv[8], ==, "/test/isolated-profile");
    g_assert_null(argv[10]); g_strfreev(argv);
    int parent = ensure_browser_profile_root(tor, data_dir, &error);
    g_assert_no_error(error); close(parent);
    Session *active = new_session("bc25827d-9d78-4b11-90ba-8b9f20b89b92", "Tor", tor);
    int root = ensure_profile_directory(active->profile_dir, &error);
    g_assert_no_error(error); close(root);
    gboolean prepared = prepare_browser_profile(tor, active->profile_dir, &error);
    g_assert_no_error(error);
    g_assert_true(prepared);
    char *prefs = g_build_filename(active->profile_dir, "user.js", NULL);
    char *contents = NULL;
    g_assert_true(g_file_get_contents(prefs, &contents, NULL, NULL));
    g_assert_true(g_str_has_prefix(contents, TOR_PREF_BEGIN));
    g_assert_nonnull(strstr(contents, active->profile_dir));
    char *custom = g_strconcat(contents, "// custom preference retained\n", NULL);
    write_fixture(prefs, custom, FALSE); g_free(custom); g_free(contents);
    prepared = prepare_browser_profile(tor, active->profile_dir, &error);
    g_assert_no_error(error);
    g_assert_true(prepared);
    g_assert_true(g_file_get_contents(prefs, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "// custom preference retained")); g_free(contents);
    g_assert_true(validate_import_profile(active->profile_dir, tor, &error));
    g_assert_false(validate_import_profile(active->profile_dir, find_browser("firefox"), &error));
    g_assert_nonnull(error); g_clear_error(&error);
    char *unmanaged = g_build_filename(data_dir, "profiles", "unmanaged", NULL);
    int unmanaged_fd = ensure_profile_directory(unmanaged, &error); g_assert_no_error(error); close(unmanaged_fd);
    char *unmanaged_prefs = g_build_filename(unmanaged, "user.js", NULL);
    write_fixture(unmanaged_prefs, "// original user preferences\n", FALSE);
    g_assert_false(prepare_browser_profile(tor, unmanaged, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_assert_true(g_file_get_contents(unmanaged_prefs, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "// original user preferences\n"); g_free(contents);
    char *firefox_prefs = g_build_filename(unmanaged, "prefs.js", NULL);
    write_fixture(firefox_prefs, "// Firefox profile\n", FALSE);
    g_assert_false(validate_import_profile(unmanaged, tor, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_free(firefox_prefs); g_free(unmanaged_prefs); g_free(unmanaged);
    char **env = browser_environment(tor);
    g_assert_cmpstr(g_environ_getenv(env, "TOR_SOCKS_PORT"), ==, "0");
    g_assert_null(g_environ_getenv(env, "TOR_SKIP_LAUNCH")); g_strfreev(env);
    char *quoted = js_path_string("/tmp/a\"b\\c\n");
    g_assert_cmpstr(quoted, ==, "\"/tmp/a\\\"b\\\\c\\u000a\""); g_free(quoted);

    /* A still-running Tor daemon blocks management after Firefox exits. */
    int ready[2]; g_assert_cmpint(pipe(ready), ==, 0);
    pid_t child = fork(); g_assert_cmpint(child, >=, 0);
    if (!child) {
        close(ready[0]);
        int profile = open_profile_directory(active->profile_dir, NULL);
        int guard = tor_data_guard_at(profile, NULL);
        g_assert_cmpint(guard, >=, 0);
        g_assert_cmpint(write(ready[1], "x", 1), ==, 1);
        for (;;) pause();
    }
    close(ready[1]); char byte;
    g_assert_cmpint(read(ready[0], &byte, 1), ==, 1); close(ready[0]);
    g_assert_true(session_unavailable(active));
    char *snapshot = g_build_filename(data_dir, "profiles", "snapshot", NULL);
    g_assert_null(reset_prepare(active->profile_dir, snapshot, FAMILY_FIREFOX, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    kill(child, SIGTERM); waitpid(child, NULL, 0);
    g_assert_false(session_unavailable(active));
    ProfileReset *reset = reset_prepare(active->profile_dir, snapshot, FAMILY_FIREFOX, &error);
    /* The failed reset retains its empty snapshot; use a distinct destination. */
    g_assert_null(reset); g_clear_error(&error); g_free(snapshot);
    snapshot = g_build_filename(data_dir, "profiles", "snapshot-2", NULL);
    reset = reset_prepare(active->profile_dir, snapshot, FAMILY_FIREFOX, &error);
    g_assert_nonnull(reset); g_assert_no_error(error); reset_release(reset);
    g_assert_true(prepare_browser_profile(tor, snapshot, &error));
    char *restored_prefs = g_build_filename(snapshot, "user.js", NULL);
    g_assert_true(g_file_get_contents(restored_prefs, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, snapshot)); g_free(contents); g_free(restored_prefs);
    gboolean started = FALSE;
    g_assert_true(purge_profile(snapshot, "tor-purge", FAMILY_FIREFOX, &started, &error));
    g_assert_true(started); g_assert_no_error(error);

    g_assert_cmpint(namespace_pid_from_status("Name: browser\nNSpid:\t12345\t5\n"), ==, 5);
    g_assert_cmpint(namespace_pid_from_status("NSpid: 2147483648\n"), ==, -1);
    g_assert_cmpint(namespace_pid_from_status("NSpid: 5junk\n"), ==, -1);
    char *chrome_path = g_build_filename(data_dir, "sandbox-profile", NULL);
    root = ensure_profile_directory(chrome_path, &error);
    g_assert_no_error(error);
    int tag = open_lock_at(root, ".isotab-sandbox"); close(tag);
    g_assert_cmpint(pipe(ready), ==, 0);
    child = fork(); g_assert_cmpint(child, >=, 0);
    if (!child) {
        close(ready[0]);
        char *token = g_strdup_printf("%s-%ld", g_get_host_name(), (long)getpid());
        g_assert_cmpint(symlinkat(token, root, "SingletonLock"), ==, 0);
        g_assert_cmpint(write(ready[1], "x", 1), ==, 1);
        for (;;) pause();
    }
    close(ready[1]); g_assert_cmpint(read(ready[0], &byte, 1), ==, 1); close(ready[0]);
    g_assert_false(chromium_available_at(root, &error)); g_clear_error(&error);
    Session observer = {.profile_dir = chrome_path, .browser = find_browser("chromium-flatpak")};
    g_assert_cmpint(profile_owner(&observer), ==, child);
    kill(child, SIGTERM); waitpid(child, NULL, 0);
    g_assert_true(prepare_chromium_launch(chrome_path, &error)); g_assert_no_error(error);
    close(root);

    GFile *tree = g_file_new_for_path(data_dir);
    g_assert_true(delete_tree(tree, &error)); g_assert_no_error(error); g_object_unref(tree);
    session_free(active); g_free(snapshot); g_free(prefs); g_free(chrome_path);
    g_free(bundle); g_free(tor_dir); g_free(exe); g_free(tor_exe); g_free(ini); g_free(version);
    g_free(fake_flatpak); g_free(fake_snap); g_free(wrapper); g_free(data_dir);
    g_print("Adapter checks passed: explicit package arguments, discovery, Snap storage, Tor isolation/daemon locks and sandbox marker ownership.\n");
    return 0;
}
