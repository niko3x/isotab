/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#include <stddef.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#define main isotab_application_main
#include "../browser.c"
#undef main

static void wait_success(pid_t pid)
{
    int status;
    g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
    g_assert_true(WIFEXITED(status)); g_assert_cmpint(WEXITSTATUS(status), ==, 0);
}

/* Only the forked test process receives these filters. Simulate old kernels
 * and restrictive sandboxes without weakening the host or the application. */
static void deny_syscall(int number, int error)
{
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, number, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | error),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {.len = G_N_ELEMENTS(code), .filter = code};
    g_assert_cmpint(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0), ==, 0);
    g_assert_cmpint(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program), ==, 0);
}

int main(void)
{
    /* Exercise actual process descriptors on the running kernel. */
    pid_t browser = fork(); g_assert_cmpint(browser, >=, 0);
    if (!browser) { for (;;) pause(); }
    int fd = syscall(SYS_pidfd_open, browser, 0);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(syscall(SYS_pidfd_send_signal, fd, 0, NULL, 0), ==, 0);
    close(fd);

    const int failures[] = {ENOSYS, EPERM};
    for (guint i = 0; i < G_N_ELEMENTS(failures); i++) {
        pid_t child = fork(); g_assert_cmpint(child, >=, 0);
        if (!child) {
            deny_syscall(SYS_pidfd_open, failures[i]);
            Session observer = {.external_pid = browser};
            GError *error = NULL;
            g_assert_false(stop_recovered_session(&observer, &error));
            g_assert_nonnull(error); g_clear_error(&error);
            g_assert_cmpint(kill(browser, 0), ==, 0);
            _exit(0);
        }
        wait_success(child);
    }
    fd = syscall(SYS_pidfd_open, browser, 0); g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(syscall(SYS_pidfd_send_signal, fd, SIGTERM, NULL, 0), ==, 0);
    close(fd);
    int status; g_assert_cmpint(waitpid(browser, &status, 0), ==, browser);
    g_assert_true(WIFSIGNALED(status)); g_assert_cmpint(WTERMSIG(status), ==, SIGTERM);

    const int rename_failures[] = {ENOSYS, EOPNOTSUPP};
    for (guint i = 0; i < G_N_ELEMENTS(rename_failures); i++) {
        data_dir = g_dir_make_tmp("isotab-kernel-test-XXXXXX", NULL);
        char *old = g_build_filename(data_dir, "original", NULL);
        char *backup = g_build_filename(data_dir, "backup", NULL);
        g_assert_cmpint(mkdir(old, 0700), ==, 0);
        char *sentinel = g_build_filename(old, "sentinel", NULL);
        g_assert_true(g_file_set_contents(sentinel, "preserved", -1, NULL));
        pid_t child = fork(); g_assert_cmpint(child, >=, 0);
        if (!child) {
            deny_syscall(SYS_renameat2, rename_failures[i]);
            GError *error = NULL;
            g_assert_null(reset_prepare(old, backup, FAMILY_FIREFOX, &error));
            g_assert_nonnull(error); g_clear_error(&error);
            gboolean started = FALSE;
            char *id = g_uuid_string_random();
            g_assert_false(purge_profile(old, id, FAMILY_FIREFOX, &started, &error));
            g_assert_false(started); g_assert_nonnull(error);
            _exit(0);
        }
        wait_success(child);
        char *contents = NULL;
        g_assert_true(g_file_get_contents(sentinel, &contents, NULL, NULL));
        g_assert_cmpstr(contents, ==, "preserved"); g_free(contents);
        GFile *tree = g_file_new_for_path(data_dir);
        g_assert_true(delete_tree(tree, NULL)); g_object_unref(tree);
        g_free(sentinel); g_free(backup); g_free(old); g_free(data_dir);
    }
    g_print("Kernel checks passed: actual pidfd signaling and safe refusal when pidfd/renameat2 are unavailable or denied.\n");
    return 0;
}
