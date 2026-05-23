#ifndef FORGE_PLUGIN_H
#define FORGE_PLUGIN_H

#include <stdbool.h>
#include <sys/types.h>

/* ── Shell hooks ────────────────────────────────────────────── */

/* Execute a shell hook command. $FILE is replaced with filepath.
   Non-blocking: fires and forgets. */
void plugin_run_hook(const char *command, const char *filepath);

/* ── Loaded plugin info ─────────────────────────────────────── */

#define PLUGIN_MAX_LOADED  32
#define PLUGIN_MAX_CALLBACKS 16

typedef void (*plugin_save_cb)(const char *filepath);
typedef void (*plugin_open_cb)(const char *filepath);
typedef void (*plugin_close_cb)(const char *filepath);
typedef void (*plugin_keypress_cb)(int key, void *buf);

typedef struct {
    char   name[128];      /* plugin filename (e.g. "autopairs.so") */
    char   path[512];      /* full path to .so file */
    void  *handle;         /* dlopen handle */
    bool   loaded;
} LoadedPlugin;

/* ── Plugin host ────────────────────────────────────────────── */

typedef struct {
    bool initialized;

    /* Loaded .so plugins */
    LoadedPlugin  plugins[PLUGIN_MAX_LOADED];
    int           plugin_count;

    /* Registered callbacks from all plugins */
    plugin_save_cb      on_save_cbs[PLUGIN_MAX_CALLBACKS];
    int                 on_save_count;

    plugin_open_cb      on_open_cbs[PLUGIN_MAX_CALLBACKS];
    int                 on_open_count;

    plugin_close_cb     on_close_cbs[PLUGIN_MAX_CALLBACKS];
    int                 on_close_count;

    plugin_keypress_cb  on_keypress_cbs[PLUGIN_MAX_CALLBACKS];
    int                 on_keypress_count;

    /* Plugin search paths */
    char  search_paths[4][512];
    int   search_path_count;
} PluginHost;

/* ── Plugin host lifecycle ──────────────────────────────────── */

void plugin_host_init(PluginHost *ph);
void plugin_host_free(PluginHost *ph);

/* Scan a directory for .so plugins and load them */
int  plugin_host_load_dir(PluginHost *ph, const char *dir);

/* Load a single plugin by path */
bool plugin_host_load(PluginHost *ph, const char *path);

/* Unload a plugin by name */
void plugin_host_unload(PluginHost *ph, const char *name);

/* ── Fire events to all plugins ─────────────────────────────── */

void plugin_on_save(PluginHost *ph, const char *filepath);
void plugin_on_open(PluginHost *ph, const char *filepath);
void plugin_on_close(PluginHost *ph, const char *filepath);
void plugin_on_keypress(PluginHost *ph, int key, void *buf);

/* ── Plugin API registration (called by plugins via forge_api) ── */

void plugin_register_on_save(plugin_save_cb cb);
void plugin_register_on_open(plugin_open_cb cb);
void plugin_register_on_close(plugin_close_cb cb);
void plugin_register_on_keypress(plugin_keypress_cb cb);

#endif
