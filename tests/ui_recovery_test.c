/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#define main isotab_application_main
#include "../browser.c"
#undef main

static GtkWidget *settings_dialog;
static char *kept_cookie, *purged_path;
static GtkWidget *find_button(GtkWidget *widget, const char *label)
{
    if (GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label)) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *result = NULL;
    for (GList *l = children; l && !result; l = l->next) result = find_button(l->data, label);
    g_list_free(children); return result;
}
static gboolean answer_cleanup(gpointer data)
{
    GList *windows = gtk_window_list_toplevels();
    for (GList *l = windows; l; l = l->next) {
        if (GTK_IS_DIALOG(l->data) && !g_strcmp0(gtk_window_get_title(l->data), "Recovery cleanup")) {
            gtk_dialog_response(l->data, GPOINTER_TO_INT(data));
            g_list_free(windows); return G_SOURCE_REMOVE;
        }
    }
    g_list_free(windows); return G_SOURCE_CONTINUE;
}
static gboolean after_delete(gpointer data)
{
    (void)data;
    if (pending_operations) return G_SOURCE_CONTINUE;
    g_assert_cmpuint(retained_sessions->len, ==, 1);
    g_assert_true(path_missing(purged_path));
    g_assert_true(g_file_test(kept_cookie, G_FILE_TEST_EXISTS));
    Session *missing = g_ptr_array_index(retained_sessions, 0);
    g_assert_false(gtk_widget_get_sensitive(missing->recovery_restore_btn));
    GtkWidget *forget = find_button(settings_dialog, "Forget entry");
    g_assert_nonnull(forget);
    g_timeout_add(30, answer_cleanup, GINT_TO_POINTER(GTK_RESPONSE_ACCEPT));
    gtk_button_clicked(GTK_BUTTON(forget));
    g_assert_cmpuint(retained_sessions->len, ==, 0);
    gtk_dialog_response(GTK_DIALOG(settings_dialog), GTK_RESPONSE_ACCEPT);
    return G_SOURCE_REMOVE;
}
static gboolean start_delete(gpointer data)
{
    (void)data;
    GList *windows = gtk_window_list_toplevels();
    for (GList *l = windows; l; l = l->next)
        if (GTK_IS_DIALOG(l->data) && !g_strcmp0(gtk_window_get_title(l->data), "Settings")) settings_dialog = l->data;
    g_list_free(windows);
    g_assert_nonnull(settings_dialog);
    Session *s = g_ptr_array_index(retained_sessions, 0);
    if (!s->size_known) return G_SOURCE_CONTINUE;
    g_assert_cmpfloat(s->size_mb, >=, 1);
    GtkWidget *cleanup = find_button(settings_dialog, "Delete permanently...");
    g_assert_nonnull(cleanup);
    g_timeout_add(30, answer_cleanup, GINT_TO_POINTER(GTK_RESPONSE_CANCEL));
    gtk_button_clicked(GTK_BUTTON(cleanup));
    g_assert_cmpuint(retained_sessions->len, ==, 2);
    g_assert_false(s->deletion_started);
    g_assert_false(s->busy);
    g_assert_true(profile_exists(s->profile_dir));
    g_timeout_add(30, answer_cleanup, GINT_TO_POINTER(GTK_RESPONSE_ACCEPT));
    gtk_button_clicked(GTK_BUTTON(cleanup));
    g_assert_cmpuint(pending_operations, ==, 1);
    g_assert_true(s->deletion_started);
    g_assert_false(gtk_widget_get_sensitive(s->recovery_restore_btn));
    g_assert_false(gtk_widget_get_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(settings_dialog), GTK_RESPONSE_CANCEL)));
    g_assert_true(cb_close(NULL, NULL, NULL));
    /* A forced dialog response while work is pending must not destroy rows
     * referenced by the worker; the settings event loop waits for completion. */
    gtk_dialog_response(GTK_DIALOG(settings_dialog), GTK_RESPONSE_CANCEL);
    g_timeout_add(20, after_delete, NULL);
    return G_SOURCE_REMOVE;
}
static gboolean exercise(gpointer data)
{
    (void)data;
    char *id = g_uuid_string_random();
    Session *chrome = new_session(id, "Delete this backup", find_browser("chrome"));
    g_free(id);
    chrome->archived = TRUE;
    chrome->archived_at = g_get_real_time() / G_USEC_PER_SEC;
    g_ptr_array_add(retained_sessions, chrome);
    g_assert_cmpint(g_mkdir_with_parents(chrome->profile_dir, 0700), ==, 0);
    purged_path = g_strdup(chrome->profile_dir);
    char *file = g_build_filename(chrome->profile_dir, "Local State", NULL);
    g_assert_true(g_file_set_contents(file, "{}", -1, NULL));
    g_free(file);
    file = g_build_filename(chrome->profile_dir, "Data", NULL);
    char *bytes = g_malloc0(1024 * 1024);
    g_assert_true(g_file_set_contents(file, bytes, 1024 * 1024, NULL));
    g_free(bytes); g_free(file);
    id = g_uuid_string_random();
    Session *missing = new_session(id, "Missing backup", find_browser("firefox"));
    g_free(id); missing->archived = TRUE;
    g_ptr_array_add(retained_sessions, missing);
    g_assert_true(save_sessions(NULL));
    g_timeout_add(100, start_delete, NULL);
    cb_settings(NULL, NULL);
    g_assert_cmpuint(retained_sessions->len, ==, 0);
    gtk_main_quit(); return G_SOURCE_REMOVE;
}
int main(int argc, char **argv)
{
    char *home = g_dir_make_tmp("isotab-ui-recovery-XXXXXX", NULL);
    g_setenv("HOME", home, TRUE);
    char *profile = g_build_filename(home, ".isotab", "session_0", NULL);
    g_mkdir_with_parents(profile, 0700);
    kept_cookie = g_build_filename(profile, "cookies.sqlite", NULL);
    g_assert_true(g_file_set_contents(kept_cookie, "kept", -1, NULL));
    g_timeout_add(250, exercise, NULL);
    int result = isotab_application_main(argc, argv);
    g_assert_cmpint(result, ==, 0);
    g_ptr_array_unref(sessions); g_ptr_array_unref(retained_sessions);
    g_assert_true(load_sessions(NULL));
    g_assert_cmpuint(retained_sessions->len, ==, 0);
    g_assert_cmpuint(sessions->len, ==, 1);
    g_assert_true(g_file_test(kept_cookie, G_FILE_TEST_EXISTS));
    g_ptr_array_unref(sessions); g_ptr_array_unref(retained_sessions);
    GFile *tree = g_file_new_for_path(home);
    g_assert_true(delete_tree(tree, NULL)); g_object_unref(tree);
    g_free(purged_path); g_free(kept_cookie); g_free(profile); g_free(home);
    g_print("GTK recovery sizes, cancel/confirm deletion, pending-dialog protection and forgetting missing entries passed.\n");
    return 0;
}
