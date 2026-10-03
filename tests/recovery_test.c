/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#define main isotab_application_main
#include "../browser.c"
#undef main

static Session *fixture(const char *browser)
{
    char *id = g_uuid_string_random();
    Session *s = new_session(id, "Recover me", find_browser(browser));
    g_free(id);
    s->archived = TRUE;
    s->archived_at = g_get_real_time() / G_USEC_PER_SEC;
    g_ptr_array_add(retained_sessions, s);
    return s;
}
static void reload(void)
{
    g_ptr_array_unref(sessions); g_ptr_array_unref(retained_sessions);
    g_assert_true(load_sessions(NULL));
}
int main(void)
{
    data_dir = g_dir_make_tmp("isotab-recovery-XXXXXX", NULL);
    g_assert_true(load_sessions(NULL));
    GError *error = NULL;
    Session *missing = fixture("firefox");
    g_assert_false(restore_session(missing, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_assert_cmpuint(sessions->len, ==, 0);
    g_assert_cmpuint(retained_sessions->len, ==, 1);
    g_assert_true(forget_recovery(missing, &error));
    g_assert_no_error(error); session_free(missing);

    /* Import rejects wrong families, mixed data and implicit selection. */
    Session *chrome = fixture("chrome");
    g_assert_cmpint(g_mkdir_with_parents(chrome->profile_dir, 0700), ==, 0);
    char *marker = g_build_filename(chrome->profile_dir, "Local State", NULL);
    g_assert_true(g_file_set_contents(marker, "{}", -1, NULL));
    g_assert_false(validate_import_profile(chrome->profile_dir, NULL, &error));
    g_clear_error(&error);
    g_assert_false(validate_import_profile(chrome->profile_dir, find_browser("firefox"), &error));
    g_clear_error(&error);
    g_assert_true(validate_import_profile(chrome->profile_dir, find_browser("brave"), &error));
    g_assert_no_error(error);
    char *prefs = g_build_filename(chrome->profile_dir, "prefs.js", NULL);
    g_assert_true(g_file_set_contents(prefs, "prefs", -1, NULL));
    g_assert_false(validate_import_profile(chrome->profile_dir, find_browser("chrome"), &error));
    g_clear_error(&error);
    g_assert_cmpint(unlink(prefs), ==, 0);
    g_assert_false(forget_recovery(chrome, &error));
    g_clear_error(&error);
    gint64 archived_at = chrome->archived_at;
    g_assert_true(mark_deletion_started(chrome, &error));
    reload();
    chrome = g_ptr_array_index(retained_sessions, 0);
    g_assert_cmpint(chrome->archived_at, ==, archived_at);
    g_assert_true(chrome->deletion_started);
    g_assert_false(restore_session(chrome, &error));
    g_clear_error(&error);

    /* A browser holding the native marker prevents any detach or deletion. */
    char *lock_path = g_build_filename(chrome->profile_dir, "SingletonLock", NULL);
    char *live = g_strdup_printf("%s-%ld", g_get_host_name(), (long)getpid());
    g_assert_cmpint(symlink(live, lock_path), ==, 0);
    gboolean started = FALSE;
    g_assert_false(purge_profile(chrome->profile_dir, chrome->id, FAMILY_CHROMIUM, &started, &error));
    g_assert_false(started); g_clear_error(&error);
    g_assert_true(g_file_test(marker, G_FILE_TEST_EXISTS));
    g_assert_cmpint(unlink(lock_path), ==, 0);

    /* Symlink contents never cause deletion outside the recovery directory. */
    char *sentinel = g_build_filename(data_dir, "outside-sentinel", NULL);
    g_assert_true(g_file_set_contents(sentinel, "safe", -1, NULL));
    char *link = g_build_filename(chrome->profile_dir, "outside", NULL);
    g_assert_cmpint(symlink(sentinel, link), ==, 0);
    g_assert_true(purge_profile(chrome->profile_dir, chrome->id, FAMILY_CHROMIUM, &started, &error));
    g_assert_true(started); g_assert_no_error(error);
    g_assert_true(g_file_test(sentinel, G_FILE_TEST_EXISTS));
    g_assert_true(recovery_files_missing(chrome));
    g_assert_true(forget_recovery(chrome, &error));
    session_free(chrome);

    /* Interrupted deletion is resumed from detached trash, never advertised
     * as a restorable backup. Simulate partial deletion using a depth limit. */
    Session *ff = fixture("firefox");
    g_assert_cmpint(g_mkdir_with_parents(ff->profile_dir, 0700), ==, 0);
    char *nested = g_strdup(ff->profile_dir);
    for (int i = 0; i < 130; i++) {
        char *next = g_build_filename(nested, "deep", NULL);
        g_free(nested); nested = next;
        g_assert_cmpint(mkdir(nested, 0700), ==, 0);
    }
    g_assert_true(mark_deletion_started(ff, &error));
    g_assert_false(purge_profile(ff->profile_dir, ff->id, FAMILY_FIREFOX, &started, &error));
    g_assert_true(started); g_assert_nonnull(error); g_clear_error(&error);
    char *trash = recovery_trash_path(ff->id);
    g_assert_true(g_file_test(trash, G_FILE_TEST_IS_DIR));
    g_assert_true(path_missing(ff->profile_dir));
    reload();
    ff = g_ptr_array_index(retained_sessions, 0);
    g_assert_false(restore_session(ff, &error)); g_clear_error(&error);
    /* Remove the deepest empty folders, then retry the guarded operation. */
    char *suffix = g_strdup(nested + strlen(ff->profile_dir));
    char *deep_trash = g_strconcat(trash, suffix, NULL);
    g_assert_cmpint(rmdir(deep_trash), ==, 0);
    char *parent = g_path_get_dirname(deep_trash);
    g_assert_cmpint(rmdir(parent), ==, 0);
    g_assert_true(purge_profile(ff->profile_dir, ff->id, FAMILY_FIREFOX, &started, &error));
    g_assert_no_error(error);
    g_assert_true(forget_recovery(ff, &error)); session_free(ff);
    g_assert_cmpuint(retained_sessions->len, ==, 0);
    reload();
    g_assert_cmpuint(retained_sessions->len, ==, 0);
    g_ptr_array_unref(sessions); g_ptr_array_unref(retained_sessions);
    char *config = g_build_filename(data_dir, "sessions.ini", NULL);
    char *bad_id = g_uuid_string_random();
    char *bad_config = g_strdup_printf("[IsoTab]\nversion=1\n[%s]\nname=Incomplete\nbrowser=chrome\narchived=true\ndeletion_started=garbage\n", bad_id);
    g_assert_true(g_file_set_contents(config, bad_config, -1, NULL));
    g_assert_false(load_sessions(&error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_ptr_array_unref(sessions); g_ptr_array_unref(retained_sessions);
    g_free(config); g_free(bad_id); g_free(bad_config);
    GFile *tree = g_file_new_for_path(data_dir);
    g_assert_true(delete_tree(tree, NULL)); g_object_unref(tree);
    g_free(parent); g_free(deep_trash); g_free(suffix); g_free(trash); g_free(nested);
    g_free(sentinel); g_free(link); g_free(lock_path); g_free(live); g_free(marker); g_free(prefs); g_free(data_dir);
    g_print("Missing recovery, import compatibility, timestamps, guarded deletion and interrupted-deletion retry checks passed.\n");
    return 0;
}
