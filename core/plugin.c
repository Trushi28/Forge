#include "plugin.h"
#include "forge_api.h"
#include "buffer.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <dlfcn.h>
#include <ctype.h>

/* ══════════════════════════════════════════════════════════════
   Global plugin host pointer — used by forge_api functions
   to register callbacks from within loaded plugins.
   ══════════════════════════════════════════════════════════════ */

static PluginHost *g_plugin_host = NULL;

/* ══════════════════════════════════════════════════════════════
   Editor context — set by main.c so plugin API functions can
   access the active buffer, cursor, render state, etc.
   ══════════════════════════════════════════════════════════════ */

static EditorContext *g_editor_ctx = NULL;

void plugin_set_editor_context(EditorContext *ctx) {
    g_editor_ctx = ctx;
}

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

/* ── Event registration ────────────────────────────────────── */

void forge_on_save(forge_save_cb cb)     { plugin_register_on_save(cb); }
void forge_on_open(forge_open_cb cb)     { plugin_register_on_open(cb); }
void forge_on_close(forge_close_cb cb)   { plugin_register_on_close(cb); }
void forge_on_keypress(forge_keypress_cb cb) {
    /* Cast: forge_keypress_cb uses ForgeBuffer*, plugin_keypress_cb uses void*.
       They are ABI-compatible (both pointer-sized). */
    plugin_register_on_keypress((plugin_keypress_cb)cb);
}

/* ── Editor actions ────────────────────────────────────────── */

void forge_notify(const char *message) {
    if (!message) return;
    if (g_editor_ctx && g_editor_ctx->render) {
        render_set_status(g_editor_ctx->render, "%s", message);
    } else {
        /* Fallback if no editor context yet (e.g. during plugin init) */
        fprintf(stderr, "forge-plugin: %s\n", message);
    }
}

void forge_insert(const char *text) {
    if (!text || !g_editor_ctx || !g_editor_ctx->buf) return;
    int len = (int)strlen(text);
    if (len == 0) return;

    size_t pos = buffer_get_offset(g_editor_ctx->buf,
                                   *g_editor_ctx->cx,
                                   *g_editor_ctx->cy);
    buffer_insert(g_editor_ctx->buf, pos, text, len);
    *g_editor_ctx->cx += len;
}

void forge_delete(int n) {
    if (n <= 0 || !g_editor_ctx || !g_editor_ctx->buf) return;

    size_t pos = buffer_get_offset(g_editor_ctx->buf,
                                   *g_editor_ctx->cx,
                                   *g_editor_ctx->cy);
    if (pos == 0) return;
    size_t del = (size_t)n < pos ? (size_t)n : pos;
    buffer_delete(g_editor_ctx->buf, pos - del, del);
    *g_editor_ctx->cx -= (int)del;
    if (*g_editor_ctx->cx < 0) *g_editor_ctx->cx = 0;
}

void forge_cursor_move(int col_delta, int row_delta) {
    if (!g_editor_ctx) return;
    if (col_delta != 0 && g_editor_ctx->cx) {
        *g_editor_ctx->cx += col_delta;
        if (*g_editor_ctx->cx < 0) *g_editor_ctx->cx = 0;
    }
    if (row_delta != 0 && g_editor_ctx->cy) {
        *g_editor_ctx->cy += row_delta;
        if (*g_editor_ctx->cy < 0) *g_editor_ctx->cy = 0;
    }
}

void forge_run(const char *cmd) {
    plugin_run_hook(cmd, NULL);
}

const char *forge_get_filepath(void) {
    if (g_editor_ctx && g_editor_ctx->filepath)
        return *g_editor_ctx->filepath;
    return NULL;
}

/* ── Buffer query ──────────────────────────────────────────── */

/* ForgeBuffer is opaque to plugins. The API functions ignore the
   buf parameter and use the global editor context instead. */
struct ForgeBuffer { int _unused; };

size_t forge_buffer_line_count(ForgeBuffer *buf) {
    (void)buf;  /* use editor context's active buffer */
    if (!g_editor_ctx || !g_editor_ctx->buf) return 0;
    return buffer_line_count(g_editor_ctx->buf);
}

char *forge_buffer_get_line(ForgeBuffer *buf, int line) {
    (void)buf;
    if (!g_editor_ctx || !g_editor_ctx->buf) return NULL;
    return buffer_get_line(g_editor_ctx->buf, line);
}

void forge_buffer_get_cursor(ForgeBuffer *buf, int *line, int *col) {
    (void)buf;
    if (!g_editor_ctx) {
        if (line) *line = 0;
        if (col) *col = 0;
        return;
    }
    if (line) *line = g_editor_ctx->cy ? *g_editor_ctx->cy : 0;
    if (col)  *col  = g_editor_ctx->cx ? *g_editor_ctx->cx : 0;
}

size_t forge_buffer_char_count(ForgeBuffer *buf) {
    (void)buf;
    if (!g_editor_ctx || !g_editor_ctx->buf) return 0;
    return buffer_total_len(g_editor_ctx->buf);
}

size_t forge_buffer_word_count(ForgeBuffer *buf) {
    (void)buf;
    if (!g_editor_ctx || !g_editor_ctx->buf) return 0;

    char *text = buffer_get_text(g_editor_ctx->buf);
    if (!text) return 0;

    size_t words = 0;
    bool in_word = false;
    for (size_t i = 0; text[i]; i++) {
        if (isspace((unsigned char)text[i])) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            words++;
        }
    }
    free(text);
    return words;
}

/* ── Widget API — STUBS ────────────────────────────────────── */
/*
 * The widget API is declared in forge_api.h but the render system
 * does not yet have infrastructure to iterate and draw registered
 * widgets. These are explicit no-op stubs. Plugins that use the
 * widget API will get stderr warnings at runtime.
 *
 * To fully support widgets, render.c would need a widget registry
 * and iteration in render_frame(). That is a separate feature.
 */

ForgeWidget *forge_widget_new(const char *name, int slot, int priority) {
    ForgeWidget *w = calloc(1, sizeof(ForgeWidget));
    if (w) {
        if (name) strncpy(w->name, name, sizeof(w->name) - 1);
        w->slot = slot;
        w->priority = priority;
        w->dirty = true;
    }
    return w;
}

void forge_widget_set_render_cb(ForgeWidget *w, forge_widget_render_cb cb) {
    if (w) w->render_cb = cb;
}

void forge_widget_set_key_cb(ForgeWidget *w, forge_widget_key_cb cb) {
    if (w) w->key_cb = cb;
}

void forge_widget_mark_dirty(ForgeWidget *w) {
    if (!w) return;
    w->dirty = true;
    if (g_editor_ctx && g_editor_ctx->render && g_editor_ctx->render->ui) {
        RenderState *r = g_editor_ctx->render;
        int slot_id = w->slot;
        if (slot_id >= 0 && slot_id < SLOT_COUNT) {
            int start_row = r->ui->slots[slot_id].row - 1;
            int num_rows = r->ui->slots[slot_id].height;
            render_mark_dirty(r, start_row, num_rows);
        }
    }
}

void forge_widget_register(ForgeWidget *w) {
    if (w && g_editor_ctx && g_editor_ctx->render) {
        render_register_widget(g_editor_ctx->render, w);
    }
}

void forge_widget_write(ForgeWidget *w, int row, int col,
                        const char *text, unsigned int fg_color,
                        unsigned int bg_color) {
    if (!w || !text) return;
    if (!g_editor_ctx || !g_editor_ctx->render || !g_editor_ctx->render->ui) return;
    
    RenderState *r = g_editor_ctx->render;
    int slot_id = w->slot;
    if (slot_id < 0 || slot_id >= SLOT_COUNT) return;
    
    int slot_row = r->ui->slots[slot_id].row;
    int slot_col = r->ui->slots[slot_id].col;
    int screen_row = slot_row + row;
    int screen_col = slot_col + col;
    
    unsigned int r_fg = (fg_color >> 16) & 0xFF;
    unsigned int g_fg = (fg_color >> 8) & 0xFF;
    unsigned int b_fg = fg_color & 0xFF;
    
    unsigned int r_bg = (bg_color >> 16) & 0xFF;
    unsigned int g_bg = (bg_color >> 8) & 0xFF;
    unsigned int b_bg = bg_color & 0xFF;
    
    char buf[4096];
    int blen = snprintf(buf, sizeof(buf), "\x1b[%d;%dH\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um%s\x1b[0m",
                        screen_row, screen_col, r_fg, g_fg, b_fg, r_bg, g_bg, b_bg, text);
    if (blen > 0) {
        (void)write(STDOUT_FILENO, buf, blen);
    }
}
