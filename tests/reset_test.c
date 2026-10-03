/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#define main isotab_application_main
#include "../browser.c"
#undef main

static void reload(void)
{
    g_ptr_array_unref(sessions);
    g_ptr_array_unref(retained_sessions);
    GError *error = NULL;
    g_assert_true(load_sessions(&error));
    g_assert_no_error(error);
}

int main(void)
{
    GError *error = NULL;
    data_dir = g_dir_make_tmp("isotab-reset-test-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_true(load_sessions(&error));
    char *id = g_uuid_string_random();
    Session *active = new_session(id, "Original", find_browser("firefox"));
    g_ptr_array_add(sessions, active);
    g_assert_cmpint(g_mkdir_with_parents(active->profile_dir, 0700), ==, 0);
    char *cookie = g_build_filename(active->profile_dir, "cookies.sqlite", NULL);
    g_assert_true(g_file_set_contents(cookie, "original-data", -1, NULL));
    Session *snapshot = NULL;
    g_assert_true(prepare_recovery(active, &snapshot, &error));
    /* A restart before the exchange retains both entries and all original bytes. */
    reload();
    active = g_ptr_array_index(sessions, 0);
    snapshot = g_ptr_array_index(retained_sessions, 0);
    g_assert_true(g_file_test(cookie, G_FILE_TEST_EXISTS));
    ProfileReset *reset = reset_prepare(active->profile_dir, snapshot->profile_dir, FAMILY_FIREFOX, &error);
    g_assert_no_error(error);
    g_assert_nonnull(reset);
    char *saved = g_build_filename(snapshot->profile_dir, "cookies.sqlite", NULL);
    g_assert_false(g_file_test(cookie, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(saved, G_FILE_TEST_EXISTS));
    /* Metadata has already committed; a restart after exchange finds the original. */
    reset_release(reset);
    reload();
    snapshot = g_ptr_array_index(retained_sessions, 0);
    g_assert_true(restore_session(snapshot, &error));
    g_assert_no_error(error);
    char *contents = NULL;
    g_assert_true(g_file_get_contents(saved, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "original-data");
    g_free(contents);

    /* Failed destination creation leaves every source byte intact. */
    reset = reset_prepare(snapshot->profile_dir, snapshot->profile_dir, FAMILY_FIREFOX, &error);
    g_assert_null(reset);
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_true(g_file_test(saved, G_FILE_TEST_EXISTS));

    char *next_id = g_uuid_string_random();
    Session *next = new_session(next_id, "Rollback", find_browser("firefox"));
    reset = reset_prepare(snapshot->profile_dir, next->profile_dir, FAMILY_FIREFOX, &error);
    g_assert_nonnull(reset);
    g_assert_no_error(error);
    g_assert_true(reset_rollback(reset, &error));
    g_assert_no_error(error);
    reset_release(reset);
    g_assert_true(g_file_test(saved, G_FILE_TEST_EXISTS));
    session_free(next);
    g_free(next_id);

    /* Clear after Chromium Stop accepts a genuinely dead local owner, but
     * rejects live, malformed, and foreign markers without changing data. */
    char *chrome_id = g_uuid_string_random();
    Session *chrome = new_session(chrome_id, "Chrome", find_browser("chrome"));
    g_ptr_array_add(sessions, chrome);
    g_assert_cmpint(g_mkdir_with_parents(chrome->profile_dir, 0700), ==, 0);
    char *chrome_data = g_build_filename(chrome->profile_dir, "Keep", NULL);
    g_assert_true(g_file_set_contents(chrome_data, "keep", -1, NULL));
    char *lock = g_build_filename(chrome->profile_dir, "SingletonLock", NULL);
    const char *bad[] = {"foreign-host-123", "malformed"};
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        g_assert_cmpint(symlink(bad[i], lock), ==, 0);
        g_assert_true(prepare_recovery(chrome, &snapshot, &error));
        g_assert_null(reset_prepare(chrome->profile_dir, snapshot->profile_dir, FAMILY_CHROMIUM, &error));
        g_assert_nonnull(error); g_clear_error(&error);
        g_assert_true(g_file_test(chrome_data, G_FILE_TEST_EXISTS));
        g_assert_cmpint(unlink(lock), ==, 0);
    }
    pid_t child = fork();
    g_assert_cmpint(child, >=, 0);
    if (!child) _exit(0);
    g_assert_cmpint(waitpid(child, NULL, 0), ==, child);
    char *token = g_strdup_printf("%s-%ld", g_get_host_name(), (long)child);
    g_assert_cmpint(symlink(token, lock), ==, 0);
    g_assert_true(prepare_recovery(chrome, &snapshot, &error));
    reset = reset_prepare(chrome->profile_dir, snapshot->profile_dir, FAMILY_CHROMIUM, &error);
    g_assert_nonnull(reset);
    g_assert_no_error(error);
    reset_release(reset);
    char *chrome_saved = g_build_filename(snapshot->profile_dir, "Keep", NULL);
    g_assert_true(g_file_test(chrome_saved, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(chrome_data, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(lock, G_FILE_TEST_IS_SYMLINK));
    /* An interrupted reset can leave our foreign-host reservation. The next
     * launcher removes only a verified dead reservation before native launch. */
    g_free(token);
    token = g_strdup_printf("%s.isotab-clear-%ld", g_get_host_name(), (long)child);
    g_assert_cmpint(symlink(token, lock), ==, 0);
    g_assert_true(prepare_chromium_launch(chrome->profile_dir, &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(lock, G_FILE_TEST_IS_SYMLINK));
    reload();
    g_assert_cmpuint(retained_sessions->len, ==, 3);
    g_ptr_array_unref(sessions); g_ptr_array_unref(retained_sessions);
    GFile *root = g_file_new_for_path(data_dir);
    g_assert_true(delete_tree(root, &error));
    g_assert_no_error(error);
    g_object_unref(root);
    g_free(chrome_saved); g_free(chrome_data); g_free(token); g_free(lock);
    g_free(chrome_id); g_free(saved); g_free(cookie); g_free(id); g_free(data_dir);
    g_print("Recoverable reset, interruption, rollback, restore and stale Chromium lock checks passed.\n");
    return 0;
}
