/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#define main isotab_application_main
#include "../browser.c"
#undef main

static void remove_test_tree(const char *path)
{
    GFile *file = g_file_new_for_path(path);
    GError *error = NULL;
    g_assert_true(delete_tree(file, &error));
    g_assert_no_error(error);
    g_object_unref(file);
}

int main(void)
{
    GError *error = NULL;
    data_dir = g_dir_make_tmp("isotab-sessions-test-XXXXXX", &error);
    g_assert_no_error(error);
    char *old = g_build_filename(data_dir, "session_2", NULL);
    g_assert_cmpint(g_mkdir_with_parents(old, 0700), ==, 0);
    char *cookie = g_build_filename(old, "cookies.sqlite", NULL);
    g_assert_true(g_file_set_contents(cookie, "untouched", -1, NULL));
    g_assert_true(load_sessions(&error));
    g_assert_no_error(error);
    g_assert_cmpuint(sessions->len, ==, 1);
    Session *legacy = g_ptr_array_index(sessions, 0);
    g_assert_cmpstr(legacy->name, ==, "Session 3");
    g_assert_cmpstr(legacy->profile_dir, ==, old);
    g_assert_cmpstr(legacy->browser->id, ==, "firefox");
    char *uuid = g_uuid_string_random();
    Session *chrome = new_session(uuid, "Work <&> café", find_browser("chrome"));
    g_ptr_array_add(sessions, chrome);
    preferences.show_sizes = FALSE;
    preferences.shortcuts = FALSE;
    preferences.default_browser = g_strdup("chrome");
    g_assert_true(save_sessions(&error));
    g_assert_no_error(error);
    g_ptr_array_unref(sessions);
    g_ptr_array_unref(retained_sessions);
    g_assert_true(load_sessions(&error));
    g_assert_cmpuint(sessions->len, ==, 2);
    chrome = g_ptr_array_index(sessions, 1);
    g_assert_cmpstr(chrome->id, ==, uuid);
    g_assert_cmpstr(chrome->name, ==, "Work <&> café");
    g_assert_false(preferences.show_sizes);
    g_assert_false(preferences.shortcuts);
    g_assert_cmpstr(preferences.default_browser, ==, "chrome");
    g_assert_cmpstr(chrome->browser->id, ==, "chrome");
    char *contents = NULL;
    g_assert_true(g_file_get_contents(cookie, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "untouched");
    g_free(contents);
    g_assert_false(valid_session_id("../../outside"));
    g_assert_false(valid_session_id("legacy_0/other"));
    g_assert_false(valid_session_id("legacy_10"));

    Browser *b = find_browser("chrome");
    b->executable = g_strdup("/test/browser with spaces");
    char **argv = browser_arguments(b, "/tmp/a ' quoted profile");
    g_assert_cmpstr(argv[0], ==, b->executable);
    g_assert_cmpstr(argv[1], ==, "--user-data-dir=/tmp/a ' quoted profile");
    g_assert_null(argv[4]);
    g_strfreev(argv);
    b = find_browser("firefox");
    b->executable = g_strdup("/test/firefox");
    argv = browser_arguments(b, old);
    g_assert_cmpstr(argv[1], ==, "--profile");
    g_assert_cmpstr(argv[2], ==, old);
    g_assert_cmpstr(argv[3], ==, "--no-remote");
    g_strfreev(argv);

    g_assert_cmpint(g_mkdir_with_parents(chrome->profile_dir, 0700), ==, 0);
    char *lock = g_build_filename(chrome->profile_dir, "SingletonLock", NULL);
    char *token = g_strdup_printf("%s-%ld", g_get_host_name(), (long)getpid());
    g_assert_cmpint(symlink(token, lock), ==, 0);
    g_assert_false(chromium_available(chrome->profile_dir, NULL));
    g_assert_false(clear_chromium(chrome->profile_dir, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_cmpint(unlink(lock), ==, 0);
    g_free(token);
    g_assert_cmpint(symlink("foreign-host-12345", lock), ==, 0);
    g_assert_false(chromium_available(chrome->profile_dir, NULL));
    g_assert_cmpint(unlink(lock), ==, 0);
    g_assert_cmpint(symlink("malformed", lock), ==, 0);
    g_assert_false(chromium_available(chrome->profile_dir, NULL));
    g_assert_cmpint(unlink(lock), ==, 0);
    /* Use a reaped child's PID for a genuinely stale marker. */
    pid_t child = fork();
    g_assert_cmpint(child, >=, 0);
    if (child == 0) _exit(0);
    g_assert_cmpint(waitpid(child, NULL, 0), ==, child);
    token = g_strdup_printf("%s-%ld", g_get_host_name(), (long)child);
    g_assert_cmpint(symlink(token, lock), ==, 0);
    g_assert_true(chromium_available(chrome->profile_dir, NULL));
    char *outside_link = g_build_filename(chrome->profile_dir, "outside", NULL);
    g_assert_cmpint(symlink(old, outside_link), ==, 0);
    g_assert_false(clear_chromium(chrome->profile_dir, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    /* Browser-owned stale markers are recovered by launching the browser,
     * never unlinked by Clear. Simulate a normal browser shutdown here. */
    g_assert_cmpint(unlink(lock), ==, 0);
    g_assert_true(clear_chromium(chrome->profile_dir, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_test(cookie, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(lock, G_FILE_TEST_IS_SYMLINK));
    g_assert_false(g_file_test(outside_link, G_FILE_TEST_IS_SYMLINK));

    /* Running sessions cannot be removed, even without deleting data. */
    chrome->running = TRUE;
    g_assert_false(remove_session(chrome, FALSE, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    chrome->running = FALSE;
    g_assert_cmpuint(sessions->len, ==, 2);

    /* Forget a session without touching its profile. */
    char *saved = g_build_filename(chrome->profile_dir, "keep-me", NULL);
    g_assert_true(g_file_set_contents(saved, "keep", -1, NULL));
    g_assert_true(remove_session(chrome, FALSE, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(sessions->len, ==, 1);
    g_assert_true(g_file_test(saved, G_FILE_TEST_EXISTS));

    g_ptr_array_unref(sessions);
    g_ptr_array_unref(retained_sessions);
    g_assert_true(load_sessions(&error));
    g_assert_cmpuint(sessions->len, ==, 1);
    g_assert_cmpstr(((Session *)g_ptr_array_index(sessions, 0))->id, ==, "legacy_2");

    g_assert_cmpuint(retained_sessions->len, ==, 1);
    chrome = g_ptr_array_index(retained_sessions, 0);
    g_assert_true(restore_session(chrome, &error));
    g_assert_no_error(error);
    g_assert_true(rename_session(chrome, "Renamed profile", &error));
    g_assert_no_error(error);
    g_assert_cmpstr(chrome->name, ==, "Renamed profile");
    g_assert_true(remove_session(chrome, FALSE, &error));
    /* Removal is recoverable even for a migrated profile. */
    legacy = g_ptr_array_index(sessions, 0);
    g_assert_true(remove_session(legacy, TRUE, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_test(cookie, G_FILE_TEST_EXISTS));
    g_ptr_array_unref(sessions);
    g_ptr_array_unref(retained_sessions);
    g_assert_true(load_sessions(&error));
    g_assert_cmpuint(sessions->len, ==, 0);

    /* Failed persistence keeps the session in its original position. */
    char *rollback_id = g_uuid_string_random();
    Session *rollback = new_session(rollback_id, "Keep session", find_browser("chrome"));
    g_free(rollback_id);
    g_ptr_array_add(sessions, rollback);
    char *real_dir = data_dir;
    data_dir = g_build_filename(real_dir, "missing-parent", NULL);
    g_assert_false(remove_session(rollback, FALSE, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_cmpuint(sessions->len, ==, 1);
    g_assert_true(g_ptr_array_index(sessions, 0) == rollback);
    g_assert_false(rename_session(rollback, "Must roll back", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_cmpstr(rollback->name, ==, "Keep session");
    Session *failed_snapshot = NULL;
    g_assert_false(prepare_recovery(rollback, &failed_snapshot, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_null(failed_snapshot);
    g_free(data_dir);
    data_dir = real_dir;
    g_free(saved);

    /* A malformed config must fail without overwriting it. */
    char *config = g_build_filename(data_dir, "sessions.ini", NULL);
    const char *invalid = "[IsoTab]\nversion=1\n[../../escape]\nname=bad\nbrowser=chrome\n";
    g_assert_true(g_file_set_contents(config, invalid, -1, NULL));
    g_ptr_array_unref(sessions);
    g_ptr_array_unref(retained_sessions);
    g_assert_false(load_sessions(&error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_true(g_file_get_contents(config, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, invalid);
    g_free(contents);
    g_ptr_array_unref(sessions);
    g_ptr_array_unref(retained_sessions);
    remove_test_tree(data_dir);
    g_free(data_dir); g_free(config); g_free(outside_link); g_free(token);
    g_free(lock); g_free(uuid); g_free(cookie); g_free(old);
    g_print("Session migration, persistence, arguments and Chromium safety checks passed.\n");
    return 0;
}
