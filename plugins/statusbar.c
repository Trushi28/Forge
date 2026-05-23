/*
 * statusbar.c — Custom statusbar widget plugin
 *
 * Adds a word count and character count to the statusbar.
 *
 * Build: gcc -shared -fPIC -o statusbar.so statusbar.c
 */

#include "../core/forge_api.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static void handle_save(const char *filepath) {
    (void)filepath;
    forge_notify("statusbar plugin: file saved");
}

FORGE_PLUGIN_INIT {
    forge_on_save(handle_save);
}
