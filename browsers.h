/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 niko3x. See LICENSE for terms and warranty disclaimer.
 */
#ifndef ISOTAB_BROWSERS_H
#define ISOTAB_BROWSERS_H
#include <glib.h>

typedef enum { FAMILY_FIREFOX, FAMILY_CHROMIUM } BrowserFamily;
typedef struct {
    const char *id;
    const char *name;
    BrowserFamily family;
    const char *commands[4];
    char *executable;
} Browser;
static Browser browsers[] = {
    {"firefox", "Firefox", FAMILY_FIREFOX, {"firefox", "firefox-bin", NULL}, NULL},
    {"firefox-esr", "Firefox ESR", FAMILY_FIREFOX, {"firefox-esr", NULL}, NULL},
    {"chromium", "Chromium", FAMILY_CHROMIUM, {"chromium", "chromium-browser", NULL}, NULL},
    {"chrome", "Google Chrome", FAMILY_CHROMIUM, {"google-chrome-stable", "google-chrome", NULL}, NULL},
    {"brave", "Brave", FAMILY_CHROMIUM, {"brave-browser", "brave", "brave-browser-stable", NULL}, NULL},
    {"edge", "Microsoft Edge", FAMILY_CHROMIUM, {"microsoft-edge-stable", "microsoft-edge", NULL}, NULL},
    {"vivaldi", "Vivaldi", FAMILY_CHROMIUM, {"vivaldi-stable", "vivaldi", NULL}, NULL}
};

static Browser *find_browser(const char *id)
{
    for (guint i = 0; i < G_N_ELEMENTS(browsers); i++)
        if (g_strcmp0(id, browsers[i].id) == 0) return &browsers[i];
    return NULL;
}

static void detect_browsers(void)
{
    for (guint i = 0; i < G_N_ELEMENTS(browsers); i++) {
        Browser *b = &browsers[i];
        g_clear_pointer(&b->executable, g_free);
        for (int j = 0; b->commands[j] && !b->executable; j++)
            b->executable = g_find_program_in_path(b->commands[j]);
    }
}

static char **browser_arguments(Browser *browser, const char *profile)
{
    char **argv = g_new0(char *, 6);
    argv[0] = g_strdup(browser->executable);
    if (browser->family == FAMILY_FIREFOX) {
        argv[1] = g_strdup("--profile");
        argv[2] = g_strdup(profile);
        argv[3] = g_strdup("--no-remote");
    } else {
        argv[1] = g_strdup_printf("--user-data-dir=%s", profile);
        argv[2] = g_strdup("--no-first-run");
        argv[3] = g_strdup("--no-default-browser-check");
    }
    return argv;
}
#endif
