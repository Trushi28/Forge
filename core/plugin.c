#include "plugin.h"
#include "forge_api.h"
#include "buffer.h"
#include "render.h"
#include "utf8.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static bool plugin_user_dir(char *out, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/') {
        int n = snprintf(out, cap, "%s/forge/plugins", xdg);
        return n >= 0 && (size_t)n < cap;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) return false;
    int n = snprintf(out, cap, "%s/.config/forge/plugins", home);
    return n >= 0 && (size_t)n < cap;
}

static bool mkdir_parents(const char *path) {
    char temp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(temp)) return false;
    memcpy(temp, path, len + 1);
    for (char *p = temp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(temp, 0700) != 0 && errno != EEXIST) return false;
        struct stat st;
        if (stat(temp, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
        *p = '/';
    }
    if (mkdir(temp, 0700) != 0 && errno != EEXIST) return false;
    struct stat st;
    return stat(temp, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool build_worker_c(const char *source) {
    if (!source || !*source) return false;
    const char *base = strrchr(source, '/');
    base = base ? base + 1 : source;
    size_t base_len = strlen(base);
    if (base_len <= 2 || strcmp(base + base_len - 2, ".c") != 0) {
        fprintf(stderr, "forge: plugin build expects a .c source file\n");
        return false;
    }
    char dir[1024], target[1024], temp[1024];
    if (!plugin_user_dir(dir, sizeof(dir)) || !mkdir_parents(dir)) {
        fprintf(stderr, "forge: cannot create plugin directory\n");
        return false;
    }
    int n = snprintf(target, sizeof(target), "%s/%.*s.forge-plugin",
                     dir, (int)(base_len - 2), base);
    if (n < 0 || (size_t)n >= sizeof(target)) return false;
    n = snprintf(temp, sizeof(temp), "%s/.plugin-build-XXXXXX", dir);
    if (n < 0 || (size_t)n >= sizeof(temp)) return false;
    int tmpfd = mkstemp(temp);
    if (tmpfd < 0) {
        perror("forge: create plugin build file");
        return false;
    }
    close(tmpfd);

    pid_t pid = fork();
    if (pid == 0) {
        execlp("cc", "cc", "-std=c11", "-O2", "-Wall", "-Wextra",
               source, "-o", temp, (char *)NULL);
        perror("forge: execute C compiler");
        _exit(127);
    }
    if (pid < 0) {
        perror("forge: fork compiler");
        unlink(temp);
        return false;
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        unlink(temp);
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || chmod(temp, 0755) != 0 ||
        rename(temp, target) != 0) {
        fprintf(stderr, "forge: plugin worker build failed\n");
        unlink(temp);
        return false;
    }
    printf("forge: built worker -> %s\n", target);
    return true;
}

int plugin_cli(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "build") == 0)
        return build_worker_c(argv[2]) ? 0 : 1;
    fprintf(stderr, "Usage: forge plugin build <source.c>\n");
    return 2;
}

static void worker_close(PluginWorker *worker, bool terminate) {
    if (!worker || !worker->running) return;
    if (worker->input_fd >= 0) close(worker->input_fd);
    if (worker->output_fd >= 0) close(worker->output_fd);
    worker->input_fd = worker->output_fd = -1;
    if (terminate && worker->pid > 0) {
        if (kill(worker->pid, SIGTERM) == 0) {
            int status;
            if (waitpid(worker->pid, &status, WNOHANG) == 0) {
                kill(worker->pid, SIGKILL);
                while (waitpid(worker->pid, &status, 0) < 0 && errno == EINTR) {}
            }
        }
    }
    worker->running = false;
    worker->pid = 0;
    worker->read_len = 0;
}

static bool json_escape(const char *src, char *dst, size_t cap, size_t *used) {
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
        char escaped[6];
        size_t len = 1;
        escaped[0] = (char)*p;
        if (*p == '"' || *p == '\\') {
            escaped[0] = '\\'; escaped[1] = (char)*p; len = 2;
        } else if (*p < 0x20) {
            escaped[0] = '\\'; escaped[1] = 'u'; escaped[2] = '0'; escaped[3] = '0';
            escaped[4] = hex[*p >> 4]; escaped[5] = hex[*p & 15]; len = 6;
        }
        if (n + len >= cap) return false;
        memcpy(dst + n, escaped, len);
        n += len;
    }
    *used = n;
    return true;
}

static bool worker_send_event(PluginWorker *worker, const char *event,
                              const char *filepath, int key) {
    char line[PLUGIN_WORKER_MAX_LINE];
    int head = snprintf(line, sizeof(line), "{\"protocol\":2,\"event\":\"%s\",\"key\":%d,\"file\":\"",
                        event, key);
    if (head < 0 || (size_t)head >= sizeof(line)) return false;
    size_t escaped = 0;
    if (!json_escape(filepath, line + head, sizeof(line) - (size_t)head, &escaped))
        return false;
    size_t len = (size_t)head + escaped;
    if (len + 3 >= sizeof(line)) return false;
    line[len++] = '"'; line[len++] = '}'; line[len++] = '\n';
    ssize_t written = write(worker->input_fd, line, len);
    if (written == (ssize_t)len) return true;
    /* Pipes are nonblocking: a full pipe drops the event instead of stalling
       the editor. A partial line would desynchronize the stream, so stop worker. */
    if (written >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        worker_close(worker, true);
    return false;
}

static bool json_get_string(const char *json, const char *field,
                            char *out, size_t out_cap) {
    char key[64];
    if (snprintf(key, sizeof(key), "\"%s\"", field) >= (int)sizeof(key)) return false;
    const char *p = strstr(json, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != ':') return false;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '"') return false;
    size_t n = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            if (!*p) return false;
            c = (unsigned char)*p++;
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c == 'u') {
                uint32_t cp = 0;
                for (int i = 0; i < 4; i++) {
                    if (!*p) return false;
                    unsigned char h = (unsigned char)*p++;
                    int v = h >= '0' && h <= '9' ? h - '0' :
                            h >= 'a' && h <= 'f' ? h - 'a' + 10 :
                            h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                    if (v < 0) return false;
                    cp = (cp << 4) | (uint32_t)v;
                }
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (!p[0] || !p[1] || p[0] != '\\' || p[1] != 'u') return false;
                    p += 2;
                    uint32_t low = 0;
                    for (int i = 0; i < 4; i++) {
                        if (!*p) return false;
                        unsigned char h = (unsigned char)*p++;
                        int v = h >= '0' && h <= '9' ? h - '0' :
                                h >= 'a' && h <= 'f' ? h - 'a' + 10 :
                                h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                        if (v < 0) return false;
                        low = (low << 4) | (uint32_t)v;
                    }
                    if (low < 0xdc00 || low > 0xdfff) return false;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    return false;
                }
                if (cp == 0) return false;
                char encoded[4];
                size_t encoded_len = utf8_encode(cp, encoded);
                if (n + encoded_len >= out_cap) return false;
                memcpy(out + n, encoded, encoded_len);
                n += encoded_len;
                continue;
            } else if (c != '"' && c != '\\' && c != '/') return false;
        }
        if (c == 0) return false;
        if (n + 1 >= out_cap) return false;
        out[n++] = (char)c;
    }
    if (*p != '"') return false;
    out[n] = '\0';
    return true;
}

static bool worker_handle_line(const char *line) {
    if (line[0] != '{') return false;
    const char *protocol = strstr(line, "\"protocol\"");
    if (!protocol) return false;
    protocol = strchr(protocol, ':');
    if (!protocol || strtol(protocol + 1, NULL, 10) != 2) return false;
    char action[32], message[257];
    if (!json_get_string(line, "action", action, sizeof(action))) return false;
    if (strcmp(action, "notify") == 0) {
        if (!json_get_string(line, "message", message, sizeof(message))) return false;
        for (char *p = message; *p; p++)
            if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f) *p = ' ';
        if (g_editor_ctx && g_editor_ctx->render) {
            render_set_status(g_editor_ctx->render, "%s", message);
            g_editor_ctx->render->full_redraw = true;
        }
        return true;
    }
    if (strcmp(action, "insert") == 0) {
        char text[1025];
        if (!json_get_string(line, "text", text, sizeof(text))) return false;
        size_t len = strlen(text);
        for (size_t i = 0; i < len;) {
            uint32_t cp;
            size_t n = utf8_decode(text + i, len - i, &cp);
            if (!n || (cp == 0xfffd && n == 1)) return false;
            i += n;
        }
        if (len && g_editor_ctx && g_editor_ctx->worker_insert)
            g_editor_ctx->worker_insert(text, len);
        return true;
    }
    return false;
}

static void worker_read_available(PluginWorker *worker) {
    char incoming[512];
    size_t budget = 8192;
    for (;;) {
        size_t request = budget < sizeof(incoming) ? budget : sizeof(incoming);
        ssize_t got = read(worker->output_fd, incoming, request);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (got <= 0) {
            worker_close(worker, true);
            return;
        }
        budget -= (size_t)got;
        for (ssize_t i = 0; i < got; i++) {
            char c = incoming[i];
            if (c == '\n') {
                worker->read_buffer[worker->read_len] = '\0';
                if (worker->read_len && !worker_handle_line(worker->read_buffer)) {
                    fprintf(stderr, "forge: invalid worker response from %s\n", worker->name);
                    worker_close(worker, true);
                    return;
                }
                worker->read_len = 0;
            } else if (worker->read_len + 1 < sizeof(worker->read_buffer)) {
                worker->read_buffer[worker->read_len++] = c;
            } else {
                fprintf(stderr, "forge: worker response too large: %s\n", worker->name);
                worker_close(worker, true);
                return;
            }
        }
        if (budget == 0) break;
    }
    int status;
    pid_t result = waitpid(worker->pid, &status, WNOHANG);
    if (result == worker->pid || (result < 0 && errno == ECHILD))
        worker_close(worker, false);
}

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

    /* Expand $FILE as one safely quoted shell argument. In config, write
       `formatter $FILE` without adding quotes around the placeholder. */
    char expanded[4096];
    size_t elen = 0, clen = strlen(command);
    for (size_t i = 0; i < clen; i++) {
        bool file_token = command[i] == '$' && i + 5 <= clen &&
                          memcmp(command + i + 1, "FILE", 4) == 0 &&
                          (i + 5 == clen || !(isalnum((unsigned char)command[i + 5]) ||
                                               command[i + 5] == '_'));
        if (!file_token) {
            if (elen + 1 >= sizeof(expanded)) return;
            expanded[elen++] = command[i];
            continue;
        }

        const char *value = filepath ? filepath : "";
        if (elen + 2 >= sizeof(expanded)) return;
        expanded[elen++] = '\'';
        for (const char *p = value; *p; p++) {
            if (*p == '\'') {
                if (elen + 4 >= sizeof(expanded)) return;
                memcpy(expanded + elen, "'\\''", 4);
                elen += 4;
            } else {
                if (elen + 1 >= sizeof(expanded)) return;
                expanded[elen++] = *p;
            }
        }
        if (elen + 1 >= sizeof(expanded)) return;
        expanded[elen++] = '\'';
        i += 4;
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
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
    }
}

/* ══════════════════════════════════════════════════════════════
   Legacy in-process C plugin loading

   This path is intentionally opt-in: dlopen plugins run with the
   editor's full privileges and can crash or corrupt the editor.
   ══════════════════════════════════════════════════════════════ */

void plugin_host_init(PluginHost *ph) {
    memset(ph, 0, sizeof(*ph));
    ph->initialized = true;
    g_plugin_host = ph;

    /* Default per-user search path, honoring the XDG config directory. */
    if (plugin_user_dir(ph->search_paths[0], sizeof(ph->search_paths[0]))) {
        ph->search_path_count = 1;
    }
}

static bool worker_start(PluginHost *ph, const char *path) {
    if (!ph || ph->worker_count >= PLUGIN_MAX_WORKERS || !path) return false;
    for (int i = 0; i < ph->worker_count; i++)
        if (ph->workers[i].running && strcmp(ph->workers[i].path, path) == 0)
            return true;
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || access(path, X_OK) != 0)
        return false;

    int to_child[2], from_child[2];
    if (pipe2(to_child, O_CLOEXEC) != 0) return false;
    if (pipe2(from_child, O_CLOEXEC) != 0) {
        close(to_child[0]); close(to_child[1]); return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        (void)dup2(to_child[0], STDIN_FILENO);
        (void)dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    close(to_child[0]); close(from_child[1]);
    if (pid < 0) {
        close(to_child[1]); close(from_child[0]); return false;
    }

    int in_flags = fcntl(to_child[1], F_GETFL, 0);
    int out_flags = fcntl(from_child[0], F_GETFL, 0);
    if (in_flags < 0 || out_flags < 0 ||
        fcntl(to_child[1], F_SETFL, in_flags | O_NONBLOCK) < 0 ||
        fcntl(from_child[0], F_SETFL, out_flags | O_NONBLOCK) < 0) {
        close(to_child[1]); close(from_child[0]);
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        return false;
    }

    PluginWorker *worker = &ph->workers[ph->worker_count++];
    memset(worker, 0, sizeof(*worker));
    snprintf(worker->path, sizeof(worker->path), "%s", path);
    const char *base = strrchr(path, '/');
    snprintf(worker->name, sizeof(worker->name), "%s", base ? base + 1 : path);
    worker->pid = pid;
    worker->input_fd = to_child[1];
    worker->output_fd = from_child[0];
    worker->running = true;
    fprintf(stderr, "forge: started plugin worker: %s\n", worker->name);
    return true;
}

int plugin_workers_load_dir(PluginHost *ph, const char *dir) {
    if (!ph || !ph->initialized || !dir) return 0;
    for (int i = 0; i < ph->worker_count;) {
        if (ph->workers[i].running) { i++; continue; }
        ph->workers[i] = ph->workers[--ph->worker_count];
        memset(&ph->workers[ph->worker_count], 0, sizeof(ph->workers[0]));
    }
    DIR *d = opendir(dir);
    if (!d) return 0;
    int loaded = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".forge-plugin") != 0) continue;
        char full_path[1024];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(full_path)) continue;
        if (worker_start(ph, full_path)) loaded++;
    }
    closedir(d);
    return loaded;
}

static void workers_send_event(PluginHost *ph, const char *event,
                               const char *filepath, int key) {
    for (int i = 0; i < ph->worker_count; i++) {
        PluginWorker *worker = &ph->workers[i];
        if (worker->running)
            (void)worker_send_event(worker, event, filepath, key);
    }
}

void plugin_workers_poll(PluginHost *ph) {
    if (!ph || !ph->initialized) return;
    for (int i = 0; i < ph->worker_count; i++)
        if (ph->workers[i].running) worker_read_available(&ph->workers[i]);
}

void plugin_workers_stop_all(PluginHost *ph) {
    if (!ph) return;
    for (int i = 0; i < ph->worker_count; i++)
        worker_close(&ph->workers[i], true);
    ph->worker_count = 0;
}

void plugin_host_free(PluginHost *ph) {
    plugin_workers_stop_all(ph);
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
    workers_send_event(ph, "save", filepath, 0);
    for (int i = 0; i < ph->on_save_count; i++) {
        if (ph->on_save_cbs[i])
            ph->on_save_cbs[i](filepath);
    }
}

void plugin_on_open(PluginHost *ph, const char *filepath) {
    if (!ph || !ph->initialized) return;
    workers_send_event(ph, "open", filepath, 0);
    for (int i = 0; i < ph->on_open_count; i++) {
        if (ph->on_open_cbs[i])
            ph->on_open_cbs[i](filepath);
    }
}

void plugin_on_close(PluginHost *ph, const char *filepath) {
    if (!ph || !ph->initialized) return;
    workers_send_event(ph, "close", filepath, 0);
    for (int i = 0; i < ph->on_close_count; i++) {
        if (ph->on_close_cbs[i])
            ph->on_close_cbs[i](filepath);
    }
}

void plugin_on_keypress(PluginHost *ph, int key, void *buf) {
    if (!ph || !ph->initialized) return;
    workers_send_event(ph, "keypress",
        g_editor_ctx && g_editor_ctx->filepath ? *g_editor_ctx->filepath : NULL, key);
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

/* ── Legacy widget API ─────────────────────────────────────── */

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
