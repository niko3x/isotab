/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#include <unistd.h>
static ssize_t injected_readlinkat(int fd, const char *name, char *buffer, size_t size);
#define readlinkat injected_readlinkat
#define main isotab_application_main
#include "../browser.c"
#undef main
#undef readlinkat

static guint reads, replace_after;
static const char *replacement;
static ssize_t injected_readlinkat(int fd, const char *name, char *buffer, size_t size)
{
    ssize_t result = readlinkat(fd, name, buffer, size);
    if (replace_after && !strcmp(name, "SingletonLock") && ++reads == replace_after) {
        g_assert_cmpint(unlinkat(fd, name, 0), ==, 0);
        g_assert_cmpint(symlinkat(replacement, fd, name), ==, 0);
    }
    return result;
}

int main(void)
{
    data_dir = g_dir_make_tmp("isotab-lock-race-XXXXXX", NULL);
    int root = open_profile_directory(data_dir, NULL);
    pid_t child = fork();
    if (!child) _exit(0);
    g_assert_cmpint(child, >, 0);
    waitpid(child, NULL, 0);
    char *stale = g_strdup_printf("%s-%ld", g_get_host_name(), (long)child);
    char *live = g_strdup_printf("%s-%ld", g_get_host_name(), (long)getpid());
    char *token = g_strdup_printf("%s.isotab-clear-%ld", g_get_host_name(), (long)getpid());
    const char *bad[] = {"foreign-host-2147483647", "malformed", live, "foreign-host-1trailing"};
    for (guint phase = 1; phase <= 2; phase++) {
        for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
            g_assert_cmpint(symlinkat(stale, root, "SingletonLock"), ==, 0);
            reads = 0; replace_after = phase; replacement = bad[i];
            GError *error = NULL;
            g_assert_false(reserve_chromium_at(root, token, &error));
            g_assert_nonnull(error); g_clear_error(&error);
            replace_after = 0;
            char *current = read_link_at(root, "SingletonLock");
            g_assert_cmpstr(current, ==, bad[i]);
            g_free(current);
            g_assert_cmpint(unlinkat(root, "SingletonLock", 0), ==, 0);
        }
    }
    close(root);
    GFile *tree = g_file_new_for_path(data_dir);
    g_assert_true(delete_tree(tree, NULL));
    g_object_unref(tree); g_free(data_dir); g_free(stale); g_free(live); g_free(token);
    g_print("Chromium lock race checks passed for foreign, malformed and live replacement markers.\n");
    return 0;
}
