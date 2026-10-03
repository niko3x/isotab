/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#define main isotab_application_main
#include "../browser.c"
#undef main

/* Real Snap browsers need their real user's common directory. All profile
 * names are fresh UUIDs; this test never loads or saves the session list. */
int main(void)
{
    detect_browsers();
    const char *ids[] = {"firefox-snap", "chromium-snap", "brave-snap"};
    Session *fixtures[6] = {0};
    guint count = 0;
    gboolean ok = TRUE;
    GError *error = NULL;
    data_dir = g_dir_make_tmp("isotab-snap-live-XXXXXX", &error);
    g_assert_no_error(error);
    for (guint b = 0; b < G_N_ELEMENTS(ids); b++) {
        Browser *browser = find_browser(ids[b]);
        if (!browser->executable) continue;
        int parent = ensure_browser_profile_root(browser, data_dir, &error);
        g_assert_no_error(error); g_assert_cmpint(parent, >=, 0); close(parent);
        for (int n = 0; n < 2; n++) {
            char *id = g_uuid_string_random();
            Session *s = new_session(id, "Snap integration fixture", browser);
            g_free(id); fixtures[count++] = s;
            int root = ensure_profile_directory(s->profile_dir, &error);
            g_assert_no_error(error); g_assert_cmpint(root, >=, 0); close(root);
            g_assert_true(prepare_browser_profile(browser, s->profile_dir, &error));
            char *sentinel = g_build_filename(s->profile_dir, "isotab-test-sentinel", NULL);
            g_assert_true(g_file_set_contents(sentinel, "preserved", -1, &error));
            g_free(sentinel);
            char **argv = browser_arguments(browser, s->profile_dir);
            guint length = g_strv_length(argv);
            argv = g_realloc_n(argv, length + 3, sizeof(char *));
            argv[length] = g_strdup("--headless");
            argv[length + 1] = g_strdup("about:blank"); argv[length + 2] = NULL;
            ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
                NULL, NULL, &s->pid, &error);
            g_strfreev(argv);
            if (!ok) { g_printerr("Launch failed: %s\n", error->message); goto cleanup; }
        }
    }
    if (!count) { g_printerr("No supported Snap browser installed.\n"); ok = FALSE; goto cleanup; }
    for (int retry = 0; retry < 300; retry++) {
        gboolean ready = TRUE;
        for (guint i = 0; i < count; i++) {
            fixtures[i]->external_pid = recovered_session_pid(fixtures[i]);
            ready &= fixtures[i]->external_pid > 0;
        }
        if (ready) break;
        if (retry == 299) { g_printerr("Snap browsers did not acquire recognizable profile locks.\n"); ok = FALSE; goto cleanup; }
        g_usleep(100000);
    }
    for (guint i = 0; i < count; i++) {
        Session *s = fixtures[i];
        for (guint j = 0; j < i; j++) g_assert_cmpint(s->external_pid, !=, fixtures[j]->external_pid);
        char *id = g_uuid_string_random();
        Session *backup = new_session(id, "Snapshot", s->browser); g_free(id);
        g_assert_null(reset_prepare(s->profile_dir, backup->profile_dir, s->browser->family, &error));
        g_assert_nonnull(error); g_clear_error(&error);
        gboolean started = FALSE;
        g_assert_false(purge_profile(s->profile_dir, s->id, s->browser->family, &started, &error));
        g_assert_false(started); g_clear_error(&error);
        /* reset_prepare may leave its empty destination on a refused reset. */
        if (!path_missing(backup->profile_dir)) {
            g_assert_true(purge_profile(backup->profile_dir, backup->id, backup->browser->family, &started, &error));
            g_assert_no_error(error);
        }
        session_free(backup);
        /* Simulate launcher reopening without child-watch state. */
        Session observer = *s; observer.running = FALSE; observer.pid = 0;
        observer.external_pid = recovered_session_pid(&observer);
        g_assert_cmpint(observer.external_pid, >, 0);
        g_assert_true(stop_recovered_session(&observer, &error)); g_assert_no_error(error);
    }
cleanup:
    for (guint i = 0; i < count; i++) {
        Session *s = fixtures[i];
        if (s->pid) {
            int status = 0; gboolean exited = FALSE;
            kill(s->pid, SIGTERM);
            for (int retry = 0; retry < 100; retry++) {
                if (waitpid(s->pid, &status, WNOHANG) == s->pid) { exited = TRUE; break; }
                g_usleep(100000);
            }
            if (!exited) { kill(s->pid, SIGKILL); waitpid(s->pid, &status, 0); }
            g_spawn_close_pid(s->pid);
        }
        for (int retry = 0; session_unavailable(s) && retry < 100; retry++) g_usleep(100000);
        if (ok) {
            char *id = g_uuid_string_random();
            Session *backup = new_session(id, "Snapshot", s->browser); g_free(id);
            ProfileReset *reset = reset_prepare(s->profile_dir, backup->profile_dir, s->browser->family, &error);
            g_assert_nonnull(reset); g_assert_no_error(error); reset_release(reset);
            char *sentinel = g_build_filename(backup->profile_dir, "isotab-test-sentinel", NULL);
            g_assert_true(g_file_test(sentinel, G_FILE_TEST_EXISTS)); g_free(sentinel);
            gboolean started = FALSE;
            g_assert_true(purge_profile(backup->profile_dir, backup->id, backup->browser->family, &started, &error));
            g_assert_no_error(error); session_free(backup);
        }
        gboolean started = FALSE;
        g_clear_error(&error);
        if (!purge_profile(s->profile_dir, s->id, s->browser->family, &started, &error)) {
            g_printerr("Fixture retained at %s: %s\n", s->profile_dir, error ? error->message : "cleanup failed");
            ok = FALSE; g_clear_error(&error);
        }
        session_free(s);
    }
    GFile *tree = g_file_new_for_path(data_dir);
    g_assert_true(delete_tree(tree, &error)); g_assert_no_error(error); g_object_unref(tree);
    g_free(data_dir);
    if (ok) g_print("Live Snap integration passed: %u simultaneous profiles, reconnect/Stop, active reset/purge refusal, preserved recovery and cleanup.\n", count);
    return ok ? 0 : 1;
}
