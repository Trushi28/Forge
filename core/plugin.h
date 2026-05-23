#ifndef FORGE_PLUGIN_H
#define FORGE_PLUGIN_H

#include <stdbool.h>

/* ── Shell hooks ────────────────────────────────────────────── */

/* Execute a shell hook command. $FILE is replaced with filepath.
   Non-blocking: fires and forgets. */
void plugin_run_hook(const char *command, const char *filepath);

/* ── Plugin host (future: dlopen-based C plugins) ───────────── */

typedef struct {
    bool initialized;
} PluginHost;

void plugin_host_init(PluginHost *ph);
void plugin_host_free(PluginHost *ph);

/* Fire events */
void plugin_on_save(PluginHost *ph, const char *filepath);
void plugin_on_open(PluginHost *ph, const char *filepath);
void plugin_on_close(PluginHost *ph, const char *filepath);

#endif
