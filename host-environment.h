/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ISOTAB_HOST_ENVIRONMENT_H
#define ISOTAB_HOST_ENVIRONMENT_H
/* Keep in sync with packaging/AppRun. Preserve unset versus empty values. */
static const char *host_environment_keys[] = {
    "PATH", "LD_LIBRARY_PATH", "XDG_DATA_DIRS", "GTK_DATA_PREFIX",
    "GTK_EXE_PREFIX", "GTK_PATH", "GTK_IM_MODULE_FILE", "GTK_THEME",
    "GDK_BACKEND", "GDK_PIXBUF_MODULE_FILE", "GDK_PIXBUF_MODULEDIR",
    "GSETTINGS_SCHEMA_DIR", "GI_TYPELIB_PATH", "GIO_MODULE_DIR", NULL
};
static char **host_environment(void)
{
    char **env = g_get_environ();
    if (g_strcmp0(g_getenv("ISOTAB_APPIMAGE_ENV"), "1") != 0) return env;
    for (guint i = 0; host_environment_keys[i]; i++) {
        const char *key = host_environment_keys[i];
        char *saved = g_strconcat("ISOTAB_HOST_", key, NULL);
        const char *value = g_getenv(saved);
        env = value ? g_environ_setenv(env, key, value, TRUE) : g_environ_unsetenv(env, key);
        env = g_environ_unsetenv(env, saved);
        g_free(saved);
    }
    const char *unset[] = {"ISOTAB_APPIMAGE_ENV", "APPDIR", "APPIMAGE", "ARGV0", NULL};
    for (guint i = 0; unset[i]; i++) env = g_environ_unsetenv(env, unset[i]);
    return env;
}
#endif
