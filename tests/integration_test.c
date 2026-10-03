/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#define main isotab_application_main
#include "../browser.c"
#undef main

static void stop_child(GPid pid)
{
    kill(pid, SIGTERM);
    for (int i = 0; i < 100; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid) { g_spawn_close_pid(pid); return; }
        g_usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    g_spawn_close_pid(pid);
}

int main(void)
{
    detect_browsers();
    Browser *ff = find_browser("firefox");
    Browser *chrome = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(browsers); i++)
        if (browsers[i].family == FAMILY_CHROMIUM && browsers[i].executable) {
            chrome = &browsers[i]; break;
        }
    if (!ff->executable || !chrome) {
        g_print("SKIP: install Firefox and a supported Chromium browser for integration checks.\n");
        return 0;
    }
    GError *error = NULL;
    char *root = g_dir_make_tmp("isotab-live-test-XXXXXX", &error);
    g_assert_no_error(error);
    data_dir = root;
    Session test[3] = {{0}};
    gboolean ok = TRUE;
    for (int i = 0; i < 3; i++) {
        test[i].browser = i == 0 ? ff : chrome;
        test[i].profile_dir = g_strdup_printf("%s/profile-%d", root, i);
        g_mkdir_with_parents(test[i].profile_dir, 0700);
        char *sentinel = g_build_filename(test[i].profile_dir, "isotab-test-sentinel", NULL);
        g_assert_true(g_file_set_contents(sentinel, "preserved", -1, NULL));
        g_free(sentinel);
        char **argv = browser_arguments(test[i].browser, test[i].profile_dir);
        argv = g_realloc_n(argv, 7, sizeof(char *));
        argv[4] = g_strdup("--headless");
        argv[5] = g_strdup("about:blank");
        argv[6] = NULL;
        ok = g_spawn_async(NULL, argv, NULL,
            G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
            NULL, NULL, &test[i].pid, &error);
        g_strfreev(argv);
        if (!ok) { g_printerr("Launch failed: %s\n", error->message); break; }
    }
    for (int retry = 0; ok && retry < 150; retry++) {
        gboolean all_locked = TRUE;
        for (int i = 0; i < 3; i++)
            all_locked &= session_unavailable(&test[i]);
        if (all_locked) break;
        if (retry == 149) { g_printerr("Browsers did not acquire all three profile locks.\n"); ok = FALSE; }
        g_usleep(100000);
    }
    if (ok) {
        for (int i = 0; i < 3; i++) {
            char *destination = g_strdup_printf("%s/active-recovery-%d", root, i);
            ProfileReset *reset = reset_prepare(test[i].profile_dir, destination,
                test[i].browser->family, &error);
            if (reset) { reset_release(reset); g_printerr("An active profile was reset!\n"); ok = FALSE; }
            g_free(destination);
            g_clear_error(&error);
            char *purge_id = g_uuid_string_random();
            gboolean began = FALSE;
            g_assert_false(purge_profile(test[i].profile_dir, purge_id, test[i].browser->family, &began, &error));
            g_assert_false(began);
            g_clear_error(&error); g_free(purge_id);
        }
    }
    if (ok) {
        /* A fresh launcher has no child-watch state or saved process IDs. */
        pid_t observer = fork();
        g_assert_cmpint(observer, >=, 0);
        if (observer == 0) {
            for (int i = 0; i < 3; i++) {
                Session reopened = test[i];
                reopened.running = FALSE;
                reopened.pid = 0;
                reopened.external_pid = recovered_session_pid(&reopened);
                for (int retry = 0; !reopened.external_pid && retry < 100; retry++) {
                    g_usleep(100000);
                    reopened.external_pid = recovered_session_pid(&reopened);
                }
                g_assert_cmpint(reopened.external_pid, >, 0);
                g_assert_cmpint(reopened.external_pid, ==, profile_owner(&reopened));
                g_assert_true(stop_recovered_session(&reopened, &error));
                g_assert_no_error(error);
            }
            _exit(0);
        }
        int status = 0;
        g_assert_cmpint(waitpid(observer, &status, 0), ==, observer);
        ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (ok) g_print("Reopened launcher recognized and safely stopped all three browsers.\n");
    }
    for (int i = 0; i < 3; i++) if (test[i].pid) stop_child(test[i].pid);
    if (ok) {
        for (int i = 0; i < 3; i++) {
            char *destination = g_strdup_printf("%s/stopped-recovery-%d", root, i);
            ProfileReset *reset = reset_prepare(test[i].profile_dir, destination,
                test[i].browser->family, &error);
            if (!reset) { g_printerr("Closed profile could not be reset: %s\n", error->message); ok = FALSE; }
            else {
                reset_release(reset);
                char *sentinel = g_build_filename(destination, "isotab-test-sentinel", NULL);
                g_assert_true(g_file_test(sentinel, G_FILE_TEST_EXISTS));
                g_free(sentinel);
                char *purge_id = g_uuid_string_random();
                gboolean began = FALSE;
                g_assert_true(purge_profile(destination, purge_id, test[i].browser->family, &began, &error));
                g_assert_true(began);
                g_assert_no_error(error);
                g_assert_true(path_missing(destination));
                g_free(purge_id);
            }
            g_free(destination);
            g_clear_error(&error);
        }
    }
    if (ok) {
        /* A browser must refuse the reservation used during Chromium Clear. */
        char *lock = g_build_filename(test[1].profile_dir, "SingletonLock", NULL);
        char *token = g_strdup_printf("%s.isotab-clear-%ld", g_get_host_name(), (long)getpid());
        g_assert_cmpint(symlink(token, lock), ==, 0);
        char **argv = browser_arguments(chrome, test[1].profile_dir);
        argv = g_realloc_n(argv, 7, sizeof(char *));
        argv[4] = g_strdup("--headless");
        argv[5] = g_strdup("about:blank");
        argv[6] = NULL;
        GPid pid = 0;
        char **env = g_get_environ();
        env = g_environ_unsetenv(env, "DISPLAY");
        env = g_environ_unsetenv(env, "WAYLAND_DISPLAY");
        env = g_environ_setenv(env, "GDK_BACKEND", "x11", TRUE);
        ok = g_spawn_async(NULL, argv, env,
            G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
            NULL, NULL, &pid, &error);
        g_strfreev(env);
        g_strfreev(argv);
        gboolean exited = FALSE;
        int status = 0;
        for (int retry = 0; ok && retry < 100; retry++) {
            if (waitpid(pid, &status, WNOHANG) == pid) { exited = TRUE; break; }
            g_usleep(100000);
        }
        if (pid && !exited) stop_child(pid);
        else if (pid) g_spawn_close_pid(pid);
        char *current = g_file_read_link(lock, NULL);
        ok = ok && exited && WIFEXITED(status) && WEXITSTATUS(status) != 0 &&
            g_strcmp0(current, token) == 0;
        if (!ok) g_printerr("Chromium did not respect the Clear reservation.\n");
        g_free(current); g_free(token); g_free(lock);
    }
    /* Only remove the generated test directory after all managed processes exit. */
    GFile *tree = g_file_new_for_path(root);
    if (ok) ok = delete_tree(tree, &error);
    if (ok) g_print("Live integration passed: Firefox + two %s sessions, active-profile protection, reset and permanent deletion after exit.\n", chrome->name);
    else g_printerr("Test profiles retained at %s\n", root);
    g_object_unref(tree);
    for (int i = 0; i < 3; i++) g_free(test[i].profile_dir);
    g_free(root);
    return ok ? 0 : 1;
}
