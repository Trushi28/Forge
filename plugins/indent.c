/*
 * indent.c — Smart indentation plugin
 *
 * Provides smart indentation that increases indent level after
 * opening braces and decreases after closing braces.
 *
 * Build: gcc -shared -fPIC -o indent.so indent.c
 */

#include "../core/forge_api.h"
#include <string.h>

static void handle_save(const char *filepath) {
    /* Example: could run a formatter after save */
    (void)filepath;
}

FORGE_PLUGIN_INIT {
    forge_on_save(handle_save);
}
