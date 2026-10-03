/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#define _GNU_SOURCE
#define main isotab_application_main
#include "../browser.c"
#undef main
#include <sys/syscall.h>
#include <linux/fs.h>

int main(void)
{
    alarm(30); /* FIFO regressions must fail rather than hang the test suite. */
    GError *error = NULL;
    char *base = g_dir_make_tmp("isotab-security-XXXXXX", &error);
    g_assert_no_error(error);
    char *profile = g_build_filename(base, "profile", NULL);
    g_assert_cmpint(g_mkdir_with_parents(profile, 0700), ==, 0);
    char *ff_lock = g_build_filename(profile, ".parentlock", NULL);
    g_assert_cmpint(mkfifo(ff_lock, 0600), ==, 0);
    g_assert_cmpint(lock_profile(profile, &error), ==, -1);
    g_assert_nonnull(error); g_clear_error(&error);
    g_assert_cmpint(unlink(ff_lock), ==, 0);
    char *outside = g_build_filename(base, "outside", NULL);
    g_assert_cmpint(g_mkdir_with_parents(outside, 0700), ==, 0);
    char *sentinel = g_build_filename(outside, "must-survive", NULL);
    g_assert_true(g_file_set_contents(sentinel, "preserve", -1, NULL));
    g_assert_cmpint(link(sentinel, ff_lock), ==, 0);
    g_assert_false(clear_profile(profile, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_assert_cmpint(unlink(ff_lock), ==, 0);
    char *chrome_lock = g_build_filename(profile, ".isotab.lock", NULL);
    g_assert_cmpint(mkfifo(chrome_lock, 0600), ==, 0);
    g_assert_false(clear_chromium(profile, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_assert_cmpint(unlink(chrome_lock), ==, 0);

    char *alias = g_build_filename(base, "alias", NULL);
    g_assert_cmpint(symlink(base, alias), ==, 0);
    char *via_alias = g_build_filename(alias, "profile", NULL);
    g_assert_false(clear_profile(via_alias, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_assert_cmpint(chmod(profile, 0777), ==, 0);
    g_assert_false(clear_profile(profile, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_assert_cmpint(chmod(profile, 0700), ==, 0);

    /* Concurrently exchange a real directory and an outside-pointing symlink.
     * Deletion may abort, but must never walk through the substituted link. */
    int root = open_profile_directory(profile, &error);
    g_assert_no_error(error);
    g_assert_cmpint(root, >=, 0);
    g_assert_cmpint(mkdirat(root, "entry", 0700), ==, 0);
    g_assert_cmpint(symlinkat(outside, root, "alternate"), ==, 0);
    pid_t child = fork();
    g_assert_cmpint(child, >=, 0);
    if (child == 0) {
        for (int i = 0; i < 200000; i++)
            syscall(SYS_renameat2, root, "entry", root, "alternate", RENAME_EXCHANGE);
        _exit(0);
    }
    struct stat st;
    g_assert_cmpint(fstat(root, &st), ==, 0);
    for (int i = 0; i < 1000; i++) {
        delete_entry_at(root, "entry", st.st_dev, 0, &error);
        g_clear_error(&error);
        mkdirat(root, "entry", 0700);
        int link_result = symlinkat(outside, root, "alternate");
        g_assert_true(link_result == 0 || errno == EEXIST);
        g_assert_true(g_file_test(sentinel, G_FILE_TEST_IS_REGULAR));
    }
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    close(root);

    /* A forged Chromium marker cannot restore Stop for an unrelated process. */
    char *marker = g_build_filename(profile, "SingletonLock", NULL);
    child = fork();
    g_assert_cmpint(child, >=, 0);
    if (child == 0) { for (;;) pause(); }
    char *token = g_strdup_printf("%s-%ld", g_get_host_name(), (long)child);
    g_assert_cmpint(symlink(token, marker), ==, 0);
    Session forged = { .profile_dir = profile, .browser = find_browser("chrome"), .external_pid = child };
    g_assert_cmpint(recovered_session_pid(&forged), ==, 0);
    g_assert_false(stop_recovered_session(&forged, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_assert_cmpint(kill(child, 0), ==, 0);
    kill(child, SIGTERM); waitpid(child, NULL, 0);
    g_assert_cmpint(unlink(marker), ==, 0);
    g_free(token); g_free(marker);

    data_dir = base;
    char *config = g_build_filename(base, "sessions.ini", NULL);
    g_assert_cmpint(mkfifo(config, 0600), ==, 0);
    g_assert_false(load_sessions(&error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_ptr_array_unref(sessions);
    g_assert_cmpint(unlink(config), ==, 0);
    g_assert_cmpint(symlink(sentinel, config), ==, 0);
    g_assert_false(load_sessions(&error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_ptr_array_unref(sessions);
    g_assert_cmpint(unlink(config), ==, 0);
    int huge = open(config, O_CREAT | O_WRONLY, 0600);
    g_assert_cmpint(huge, >=, 0);
    g_assert_cmpint(ftruncate(huge, 1024 * 1024 + 1), ==, 0);
    close(huge);
    g_assert_false(load_sessions(&error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_ptr_array_unref(sessions);
    char *contents = NULL;
    g_assert_true(g_file_get_contents(sentinel, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "preserve");
    g_free(contents);
    GFile *tree = g_file_new_for_path(base);
    g_assert_true(delete_tree(tree, &error));
    g_assert_no_error(error);
    g_object_unref(tree);
    g_free(config); g_free(via_alias); g_free(alias); g_free(chrome_lock);
    g_free(sentinel); g_free(outside); g_free(ff_lock); g_free(profile); g_free(base);
    alarm(0);
    g_print("Security checks passed: FIFO/hardlink/ancestor symlink rejection, settings validation and deletion race stress.\n");
    return 0;
}
