/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#define main isotab_application_main
#include "../browser.c"
#undef main
static guint ticks;
static gboolean tick(gpointer data) { (void)data; ticks++; return G_SOURCE_CONTINUE; }
static void choose_import_browser(GtkWidget *widget, gpointer data)
{
    (void)data;
    if (GTK_IS_COMBO_BOX(widget)) gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), "firefox");
    if (GTK_IS_CONTAINER(widget)) gtk_container_foreach(GTK_CONTAINER(widget), choose_import_browser, NULL);
}
static gboolean answer(gpointer data)
{
    const char *title = data;
    GList *windows = gtk_window_list_toplevels();
    for (GList *l = windows; l; l = l->next) {
        if (GTK_IS_DIALOG(l->data) && !g_strcmp0(gtk_window_get_title(l->data), title)) {
            if (!strcmp(title, "Rename session")) {
                GList *children = gtk_container_get_children(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(l->data))));
                for (GList *c = children; c; c = c->next)
                    if (GTK_IS_ENTRY(c->data)) gtk_entry_set_text(GTK_ENTRY(c->data), "Recovered profile");
                g_list_free(children);
            }
            if (!strcmp(title, "Import unlisted profile")) choose_import_browser(l->data, NULL);
            gtk_dialog_response(l->data, !strcmp(title, "Confirm Clear") ? GTK_RESPONSE_YES : GTK_RESPONSE_ACCEPT);
            g_list_free(windows); return G_SOURCE_REMOVE;
        }
    }
    g_list_free(windows); return G_SOURCE_CONTINUE;
}
static GtkWidget *find_restore(GtkWidget *widget)
{
    if (GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), "Restore")) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *result = NULL;
    for (GList *l = children; l && !result; l = l->next) result = find_restore(l->data);
    g_list_free(children); return result;
}
static gboolean restore_dialog(gpointer data)
{
    (void)data;
    GList *windows = gtk_window_list_toplevels();
    for (GList *l = windows; l; l = l->next) {
        if (GTK_IS_DIALOG(l->data) && !g_strcmp0(gtk_window_get_title(l->data), "Settings")) {
            GtkWidget *button = find_restore(l->data);
            g_assert_nonnull(button);
            gtk_button_clicked(GTK_BUTTON(button));
            gtk_dialog_response(l->data, GTK_RESPONSE_ACCEPT);
            g_list_free(windows); return G_SOURCE_REMOVE;
        }
    }
    g_list_free(windows); return G_SOURCE_CONTINUE;
}
static gboolean finish(gpointer data)
{
    (void)data;
    if (pending_operations) return G_SOURCE_CONTINUE;
    g_assert_cmpuint(ticks, >, 0);
    g_assert_cmpuint(retained_sessions->len, ==, 1);
    Session *old = g_ptr_array_index(retained_sessions, 0);
    char *cookie = g_build_filename(old->profile_dir, "cookies.sqlite", NULL);
    g_assert_true(g_file_test(cookie, G_FILE_TEST_EXISTS));
    g_free(cookie);
    Session *active = g_ptr_array_index(sessions, 0);
    g_timeout_add(50, answer, "Remove session");
    g_assert_true(GTK_IS_BUTTON(active->remove_btn));
    gtk_button_clicked(GTK_BUTTON(active->remove_btn));
    g_assert_cmpuint(sessions->len, ==, 0);
    g_assert_cmpuint(retained_sessions->len, ==, 2);
    g_timeout_add(50, restore_dialog, NULL);
    cb_settings(NULL, NULL);
    g_assert_cmpuint(sessions->len, ==, 1);
    g_assert_true(g_ptr_array_index(sessions, 0) == old);
    g_timeout_add(50, answer, "Rename session");
    gtk_button_clicked(GTK_BUTTON(old->rename_btn));
    g_assert_cmpstr(old->name, ==, "Recovered profile");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(old->name_lbl)), ==, old->name);
    char *orphan_id = g_uuid_string_random();
    char *orphan_path = g_build_filename(data_dir, "profiles", orphan_id, NULL);
    g_assert_cmpint(g_mkdir_with_parents(orphan_path, 0700), ==, 0);
    char *orphan_file = g_build_filename(orphan_path, "Keep", NULL);
    g_assert_true(g_file_set_contents(orphan_file, "untouched", -1, NULL));
    g_timeout_add(50, answer, "Import unlisted profile");
    cb_import(NULL, main_win);
    g_assert_cmpuint(sessions->len, ==, 2);
    Session *imported = g_ptr_array_index(sessions, 1);
    g_assert_cmpstr(imported->id, ==, orphan_id);
    g_assert_true(g_file_test(orphan_file, G_FILE_TEST_EXISTS));
    g_free(orphan_file); g_free(orphan_path); g_free(orphan_id);
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}
static GtkWidget *find_menu_toggle(GtkWidget *widget)
{
    if (GTK_IS_TOGGLE_BUTTON(widget) && g_object_get_data(G_OBJECT(widget), "session-popover")) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *result = NULL;
    for (GList *l = children; l && !result; l = l->next) result = find_menu_toggle(l->data);
    g_list_free(children); return result;
}
static gboolean clear_from_menu(gpointer data)
{
    GtkWidget *more = data;
    GtkWidget *popover = g_object_get_data(G_OBJECT(more), "session-popover");
    g_assert_true(gtk_widget_get_visible(popover));
    Session *s = g_ptr_array_index(sessions, 0);
    g_timeout_add(50, answer, "Confirm Clear");
    gtk_button_clicked(GTK_BUTTON(s->clear_btn));
    g_assert_cmpuint(pending_operations, ==, 1);
    g_assert_true(s->busy);
    g_assert_false(gtk_widget_get_sensitive(s->launch_btn));
    g_assert_true(cb_close(NULL, NULL, NULL));
    g_timeout_add(20, finish, NULL);
    return G_SOURCE_REMOVE;
}
static gboolean menu_closed_check(gpointer data)
{
    GtkWidget *more = data;
    GtkWidget *popover = g_object_get_data(G_OBJECT(more), "session-popover");
    g_assert_false(gtk_widget_get_visible(popover));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(more)));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(more), TRUE);
    g_timeout_add(250, clear_from_menu, more);
    return G_SOURCE_REMOVE;
}
static gboolean close_menu(gpointer data)
{
    GtkWidget *more = data;
    GtkWidget *popover = g_object_get_data(G_OBJECT(more), "session-popover");
    g_assert_true(gtk_widget_get_visible(popover));
    g_assert_true(gtk_widget_get_mapped(popover));
    gtk_popover_popdown(GTK_POPOVER(popover));
    /* Native dismissal must synchronize the three-dot toggle too.
     * A closing transition keeps the popover visible until its frames finish. */
    g_assert_true(gtk_widget_get_visible(popover));
    g_timeout_add(250, menu_closed_check, more);
    return G_SOURCE_REMOVE;
}
static gboolean exercise(gpointer data)
{
    (void)data;
    Session *s = g_ptr_array_index(sessions, 0);
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", TRUE, NULL);
    GtkWidget *more = find_menu_toggle(s->frame);
    g_assert_nonnull(more);
    GtkWidget *popover = g_object_get_data(G_OBJECT(more), "session-popover");
    g_assert_true(GTK_IS_POPOVER(popover));
    gtk_popover_set_modal(GTK_POPOVER(popover), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(more), TRUE);
    g_assert_true(gtk_widget_get_visible(popover));
    g_timeout_add(250, close_menu, more);
    return G_SOURCE_REMOVE;
}
int main(int argc, char **argv)
{
    char *home = g_dir_make_tmp("isotab-release-ui-XXXXXX", NULL);
    g_setenv("HOME", home, TRUE);
    char *profile = g_build_filename(home, ".isotab", "session_0", NULL);
    g_mkdir_with_parents(profile, 0700);
    char *cookie = g_build_filename(profile, "cookies.sqlite", NULL);
    g_file_set_contents(cookie, "original", -1, NULL);
    g_timeout_add(5, tick, NULL);
    g_timeout_add(250, exercise, NULL);
    int result = isotab_application_main(argc, argv);
    g_assert_cmpint(result, ==, 0);
    g_ptr_array_unref(sessions); g_ptr_array_unref(retained_sessions);
    GError *error = NULL;
    g_assert_true(load_sessions(&error));
    g_assert_no_error(error);
    g_assert_cmpuint(sessions->len, ==, 2);
    g_assert_cmpstr(((Session *)g_ptr_array_index(sessions, 0))->name, ==, "Recovered profile");
    g_ptr_array_unref(sessions); g_ptr_array_unref(retained_sessions);
    GFile *root = g_file_new_for_path(home);
    g_assert_true(delete_tree(root, &error));
    g_assert_no_error(error);
    g_object_unref(root); g_free(cookie); g_free(profile); g_free(home);
    g_print("GTK async Clear, removal, recovery, rename and restart checks passed.\n");
    return 0;
}
