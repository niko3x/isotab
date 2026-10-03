/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#define main isotab_application_main
#include "../browser.c"
#undef main

static void write_file(const char *dir, const char *name)
{
    char *path = g_build_filename(dir, name, NULL);
    g_assert_true(g_file_set_contents(path, "test data", -1, NULL));
    g_free(path);
}

int main(void)
{
    GError *error = NULL;
    char *root = g_dir_make_tmp("isotab-test-XXXXXX", &error);
    g_assert_no_error(error);
    char *profile = g_build_filename(root, "profile's $(literal)", NULL);
    g_assert_cmpint(g_mkdir_with_parents(profile, 0700), ==, 0);
    write_file(profile, "cookies.sqlite");
    g_assert_cmpfloat(profile_size_mb(profile), >=, 0);
    g_assert_false(profile_unavailable(profile));

    /* A separate process must observe the lock, as after launcher restart. */
    int fd = lock_profile(profile, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    pid_t child = fork();
    g_assert_cmpint(child, >=, 0);
    if (child == 0) {
        g_assert_true(profile_unavailable(profile));
        g_assert_false(clear_profile(profile, &error));
        g_assert_nonnull(error);
        _exit(0);
    }
    int status;
    g_assert_cmpint(waitpid(child, &status, 0), ==, child);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), ==, 0);
    close(fd);
    g_assert_false(profile_unavailable(profile));

    char *outside = g_build_filename(root, "outside", NULL);
    g_assert_cmpint(g_mkdir_with_parents(outside, 0700), ==, 0);
    write_file(outside, "keep");
    char *link = g_build_filename(profile, "external-link", NULL);
    g_assert_cmpint(symlink(outside, link), ==, 0);
    char *nested = g_build_filename(profile, "nested", "directory", NULL);
    g_assert_cmpint(g_mkdir_with_parents(nested, 0700), ==, 0);
    write_file(nested, "history");
    g_assert_true(clear_profile(profile, &error));
    g_assert_no_error(error);
    char *kept = g_build_filename(outside, "keep", NULL);
    g_assert_true(g_file_test(kept, G_FILE_TEST_IS_REGULAR));
    g_assert_false(g_file_test(nested, G_FILE_TEST_EXISTS));
    char *lock = g_build_filename(profile, ".parentlock", NULL);
    g_assert_true(g_file_test(lock, G_FILE_TEST_IS_REGULAR));
    g_assert_true(clear_profile(profile, &error));
    g_assert_no_error(error);

    /* Fail closed for legacy locks, including dangling links. */
    char *legacy = g_build_filename(profile, "lock", NULL);
    g_assert_cmpint(symlink("missing-target", legacy), ==, 0);
    g_assert_true(profile_unavailable(profile));
    g_assert_false(clear_profile(profile, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_cmpint(unlink(legacy), ==, 0);

    /* Modern Firefox leaves IP:+PID links behind after crashes. An
     * acquired fcntl lock proves those markers are stale, even if the PID
     * happens to have been reused by a living process. */
    char *marker = g_strdup_printf("127.0.1.1:+%ld", (long)getpid());
    g_assert_cmpint(symlink(marker, legacy), ==, 0);
    g_assert_false(profile_unavailable(profile));
    g_assert_true(g_file_test(legacy, G_FILE_TEST_IS_SYMLINK));
    fd = lock_profile(profile, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    child = fork();
    g_assert_cmpint(child, >=, 0);
    if (child == 0) {
        g_assert_true(profile_unavailable(profile));
        g_assert_false(clear_profile(profile, &error));
        _exit(0);
    }
    g_assert_cmpint(waitpid(child, &status, 0), ==, child);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), ==, 0);
    close(fd);
    g_assert_true(clear_profile(profile, &error));
    g_assert_no_error(error);
    g_free(marker);

    /* Reject symlink profile roots and symlink lock files. */
    char *alias = g_build_filename(root, "alias", NULL);
    g_assert_cmpint(symlink(profile, alias), ==, 0);
    g_assert_false(clear_profile(alias, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_cmpint(unlink(lock), ==, 0);
    g_assert_cmpint(symlink(kept, lock), ==, 0);
    g_assert_false(clear_profile(profile, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);

    GFile *tree = g_file_new_for_path(root);
    g_assert_true(delete_tree(tree, &error));
    g_assert_no_error(error);
    g_object_unref(tree);
    g_free(alias); g_free(legacy); g_free(lock); g_free(kept);
    g_free(nested); g_free(link); g_free(outside); g_free(profile); g_free(root);
    g_print("Profile safety checks passed.\n");
    return 0;
}
