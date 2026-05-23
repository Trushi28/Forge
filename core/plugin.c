#include "plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <dlfcn.h>

/* ══════════════════════════════════════════════════════════════
   Global plugin host pointer — used by forge_api functions
   to register callbacks from within loaded plugins.
   ══════════════════════════════════════════════════════════════ */

static PluginHost *g_plugin_host = NULL;

/* ══════════════════════════════════════════════════════════════
   Shell hooks — execute shell commands on editor events

   The command string can contain $FILE which is replaced with
   the actual file path. Commands run in the background (forked)
   so they don't block the editor.
   ══════════════════════════════════════════════════════════════ */

void plugin_run_hook(const char *command, const char *filepath) {
    if (!command || command[0] == '\0') return;

    /* Expand $FILE in the command */
    char expanded[1024];
    int  elen = 0;
    int  clen = (int)strlen(command);

    for (int i = 0; i < clen && elen < (int)sizeof(expanded) - 1; i++) {
        if (command[i] == '$' && i + 4 < clen &&
            memcmp(command + i + 1, "FILE", 4) == 0) {
            /* Replace $FILE with filepath */
            if (filepath) {
                int flen = (int)strlen(filepath);
                if (elen + flen < (int)sizeof(expanded) - 1) {
                    memcpy(expanded + elen, filepath, flen);
                    elen += flen;
                }
            }
            i += 4;  /* skip FILE */
        } else {
            expanded[elen++] = command[i];
        }
    }
    expanded[elen] = '\0';

    /* Fork and exec in background */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child process */
        setsid();  /* detach from terminal */

        /* Redirect stdout/stderr to /dev/null */
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        execl("/bin/sh", "sh", "-c", expanded, (char *)NULL);
        _exit(127);
    }

    /* Parent: don't wait (fire and forget) */
    /* Reap zombie later */
    if (pid > 0) {
        /* Non-blocking wait to reap any previous zombies */
        waitpid(-1, NULL, WNOHANG);
    }
}

/* ══════════════════════════════════════════════════════════════
   Plugin host — dlopen-based C plugin loading

   Plugins are shared objects (.so) that contain a
   forge_plugin_init() function. When loaded, this function
   is called automatically (it's a constructor), which lets
   the plugin register its callbacks.
   ══════════════════════════════════════════════════════════════ */

void plugin_host_init(PluginHost *ph) {
    memset(ph, 0, sizeof(*ph));
    ph->initialized = true;
    g_plugin_host = ph;

    /* Default search paths */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(ph->search_paths[0], sizeof(ph->search_paths[0]),
                 "%s/.config/forge/plugins", home);
        ph->search_path_count = 1;
    }
}

void plugin_host_free(PluginHost *ph) {
    /* Unload all plugins */
    for (int i = 0; i < ph->plugin_count; i++) {
        if (ph->plugins[i].handle) {
            dlclose(ph->plugins[i].handle);
            ph->plugins[i].handle = NULL;
        }
        ph->plugins[i].loaded = false;
    }
    ph->plugin_count = 0;
    ph->on_save_count = 0;
    ph->on_open_count = 0;
    ph->on_close_count = 0;
    ph->on_keypress_count = 0;
    ph->initialized = false;
    g_plugin_host = NULL;
}

bool plugin_host_load(PluginHost *ph, const char *path) {
    if (!ph || !ph->initialized) return false;
    if (ph->plugin_count >= PLUGIN_MAX_LOADED) return false;

    /* Check if already loaded */
    for (int i = 0; i < ph->plugin_count; i++) {
        if (strcmp(ph->plugins[i].path, path) == 0 && ph->plugins[i].loaded)
            return true;  /* already loaded */
    }

    /* dlopen the plugin */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "forge: plugin load error: %s: %s\n", path, dlerror());
        return false;
    }

    /* Register in our list */
    LoadedPlugin *lp = &ph->plugins[ph->plugin_count];
    strncpy(lp->path, path, sizeof(lp->path) - 1);

    /* Extract basename for name */
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;
    strncpy(lp->name, basename, sizeof(lp->name) - 1);

    lp->handle = handle;
    lp->loaded = true;
    ph->plugin_count++;

    /* The plugin's constructor (FORGE_PLUGIN_INIT) has already run
       during dlopen, so callbacks are already registered via the
       plugin_register_on_* functions. */

    fprintf(stderr, "forge: loaded plugin: %s\n", lp->name);
    return true;
}

void plugin_host_unload(PluginHost *ph, const char *name) {
    for (int i = 0; i < ph->plugin_count; i++) {
        if (strcmp(ph->plugins[i].name, name) == 0 && ph->plugins[i].loaded) {
            if (ph->plugins[i].handle)
                dlclose(ph->plugins[i].handle);
            ph->plugins[i].handle = NULL;
            ph->plugins[i].loaded = false;
            fprintf(stderr, "forge: unloaded plugin: %s\n", name);
            return;
        }
    }
}

int plugin_host_load_dir(PluginHost *ph, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;

    int loaded = 0;
    struct dirent *entry;

    while ((entry = readdir(d)) != NULL) {
        /* Only load .so files */
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".so") != 0) continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);

        if (plugin_host_load(ph, full_path))
            loaded++;
    }

    closedir(d);
    return loaded;
}

/* ══════════════════════════════════════════════════════════════
   Fire events to all registered plugin callbacks
   ══════════════════════════════════════════════════════════════ */

void plugin_on_save(PluginHost *ph, const char *filepath) {
    if (!ph || !ph->initialized) return;
    for (int i = 0; i < ph->on_save_count; i++) {
        if (ph->on_save_cbs[i])
            ph->on_save_cbs[i](filepath);
    }
}

void plugin_on_open(PluginHost *ph, const char *filepath) {
    if (!ph || !ph->initialized) return;
    for (int i = 0; i < ph->on_open_count; i++) {
        if (ph->on_open_cbs[i])
            ph->on_open_cbs[i](filepath);
    }
}

void plugin_on_close(PluginHost *ph, const char *filepath) {
    if (!ph || !ph->initialized) return;
    for (int i = 0; i < ph->on_close_count; i++) {
        if (ph->on_close_cbs[i])
            ph->on_close_cbs[i](filepath);
    }
}

void plugin_on_keypress(PluginHost *ph, int key, void *buf) {
    if (!ph || !ph->initialized) return;
    for (int i = 0; i < ph->on_keypress_count; i++) {
        if (ph->on_keypress_cbs[i])
            ph->on_keypress_cbs[i](key, buf);
    }
}

/* ══════════════════════════════════════════════════════════════
   Plugin API registration — called by plugins via forge_api.h

   These are the implementations of forge_on_save() etc. that
   the loaded .so plugins call during their init.
   ══════════════════════════════════════════════════════════════ */

void plugin_register_on_save(plugin_save_cb cb) {
    if (!g_plugin_host) return;
    if (g_plugin_host->on_save_count < PLUGIN_MAX_CALLBACKS)
        g_plugin_host->on_save_cbs[g_plugin_host->on_save_count++] = cb;
}

void plugin_register_on_open(plugin_open_cb cb) {
    if (!g_plugin_host) return;
    if (g_plugin_host->on_open_count < PLUGIN_MAX_CALLBACKS)
        g_plugin_host->on_open_cbs[g_plugin_host->on_open_count++] = cb;
}

void plugin_register_on_close(plugin_close_cb cb) {
    if (!g_plugin_host) return;
    if (g_plugin_host->on_close_count < PLUGIN_MAX_CALLBACKS)
        g_plugin_host->on_close_cbs[g_plugin_host->on_close_count++] = cb;
}

void plugin_register_on_keypress(plugin_keypress_cb cb) {
    if (!g_plugin_host) return;
    if (g_plugin_host->on_keypress_count < PLUGIN_MAX_CALLBACKS)
        g_plugin_host->on_keypress_cbs[g_plugin_host->on_keypress_count++] = cb;
}

/* ══════════════════════════════════════════════════════════════
   Forge API implementations — called by plugins

   These provide the actual forge_on_save(), forge_insert(), etc.
   function bodies that match the declarations in forge_api.h.
   ══════════════════════════════════════════════════════════════ */

void forge_on_save(plugin_save_cb cb)     { plugin_register_on_save(cb); }
void forge_on_open(plugin_open_cb cb)     { plugin_register_on_open(cb); }
void forge_on_close(plugin_close_cb cb)   { plugin_register_on_close(cb); }

void forge_notify(const char *message) {
    /* For now, print to stderr — in full implementation this would
       send an IPC message to the editor to update the status bar. */
    if (message)
        fprintf(stderr, "forge-plugin: %s\n", message);
}

void forge_run(const char *cmd) {
    plugin_run_hook(cmd, NULL);
}
