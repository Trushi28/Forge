#include "arena.h"
#include "buffer.h"
#include "completion.h"
#include "config.h"
#include "git.h"
#include "input.h"
#include "ipc.h"
#include "lsp.h"
#include "palette.h"
#include "plugin.h"
#include "forgescript.h"
#include "render.h"
#include "theme.h"
#include "ui.h"
#include "undo.h"
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

Arena *session_arena = NULL;

/* ── Global editor state ────────────────────────────────────── */

typedef struct {
    Buffer        *buf;
    RenderState    render;
    UIRegistry     ui;
    ForgeConfig    cfg;
    ForgeTheme     theme;
    LSPClient     *lsp;
    CompletionState completion;
    PaletteState   palette;
    UndoStack      undo;
    GitState       git;
    PluginHost     plugins;
    ForgeScriptVM  scripts;
    IPCBridge      ipc;

    const char    *filepath;
    char           file_uri[LSP_MAX_URI];
    int            cx, cy;
    bool           running;
    bool           modified;
    bool           show_hover;
    char           hover_text[4096];
    int            hover_row, hover_col;

    int            config_poll_counter;

    /* LSP time-based debounce: sync buffer after 300ms of idle */
    bool           lsp_dirty;       /* buffer changed since last sync? */
    struct timespec lsp_last_edit;  /* timestamp of last edit */

    /* Guild panel */
    bool           guild_panel_visible;
    char           guild_chat_input[512];
    int            guild_chat_input_len;
    int            guild_ipc_timer;    /* countdown for IPC reconnect */
} EditorState;

static EditorState E;

/* ── Resize signal ──────────────────────────────────────────── */
static volatile int g_resized = 0;
static void sigwinch_handler(int sig) {
    (void)sig;
    g_resized = 1;
}

/* ── Time helpers ───────────────────────────────────────────── */
static void get_time(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

/* Returns milliseconds elapsed since `since` */
static long ms_since(struct timespec *since) {
    struct timespec now;
    get_time(&now);
    return (now.tv_sec - since->tv_sec) * 1000 +
           (now.tv_nsec - since->tv_nsec) / 1000000;
}

/* ── File loading ───────────────────────────────────────────── */
static Buffer *load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return buffer_new("", 0);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        fclose(f);
        return buffer_new("", 0);
    }
    char *data = malloc((size_t)sz + 1);
    size_t got = fread(data, 1, (size_t)sz, f);
    fclose(f);
    data[got] = '\0';
    Buffer *b = buffer_new(data, got);
    free(data);
    return b;
}

/* ── Cursor helpers ─────────────────────────────────────────── */
static int line_len(Buffer *b, int cy) {
    char *l = buffer_get_line(b, cy);
    int n = l ? (int)strlen(l) : 0;
    if (l) free(l);
    return n;
}

static void clamp_cx(int *cx, int cy) {
    int max = line_len(E.buf, cy);
    if (*cx > max) *cx = max;
    if (*cx < 0)   *cx = 0;
}

/* ── Get word at cursor ─────────────────────────────────────── */
static void get_word_at_cursor(char *word, int max_len) {
    word[0] = '\0';
    char *line = buffer_get_line(E.buf, E.cy);
    if (!line) return;

    int len = (int)strlen(line);
    int start = E.cx, end = E.cx;

    /* Expand left */
    while (start > 0 && (line[start - 1] == '_' ||
           (line[start - 1] >= 'a' && line[start - 1] <= 'z') ||
           (line[start - 1] >= 'A' && line[start - 1] <= 'Z') ||
           (line[start - 1] >= '0' && line[start - 1] <= '9')))
        start--;

    /* Expand right */
    while (end < len && (line[end] == '_' ||
           (line[end] >= 'a' && line[end] <= 'z') ||
           (line[end] >= 'A' && line[end] <= 'Z') ||
           (line[end] >= '0' && line[end] <= '9')))
        end++;

    int wlen = end - start;
    if (wlen > 0 && wlen < max_len) {
        memcpy(word, line + start, wlen);
        word[wlen] = '\0';
    }

    free(line);
}

/* ── Mark buffer as modified (shared helper) ────────────────── */
static void mark_modified(void) {
    E.modified = true;
    E.lsp_dirty = true;
    get_time(&E.lsp_last_edit);
    render_set_status(&E.render, "%s [+]",
                      E.filepath ? E.filepath : "[new file]");
}

/* ── LSP time-based debounce ────────────────────────────────── */
static void lsp_debounce_sync(void) {
    if (!E.lsp || !E.lsp->running || !E.lsp->initialized) return;
    if (!E.lsp_dirty) return;

    /* Sync after 300ms of idle */
    if (ms_since(&E.lsp_last_edit) >= 300) {
        char *text = buffer_get_text(E.buf);
        if (text) {
            lsp_send_did_change(E.lsp, E.file_uri, text);
            free(text);
        }
        E.lsp_dirty = false;
    }
}

/* ── Request completion (auto-trigger) ──────────────────────── */
static bool is_identifier_char(int key) {
    return (key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
           (key >= '0' && key <= '9') || key == '_';
}

static bool is_completion_trigger(int key) {
    return key == '.' || key == '>' || key == ':' || key == '/';
}

static void maybe_request_completion(int key) {
    if (!E.lsp || !E.lsp->running || !E.lsp->initialized ||
        !E.cfg.lsp_completion)
        return;

    if (is_completion_trigger(key)) {
        /* Always trigger on special characters */
        lsp_request_completion(E.lsp, E.file_uri, E.cy, E.cx);
    } else if (is_identifier_char(key)) {
        /* Trigger on identifier chars if we have at least 2 chars typed */
        char word[128];
        get_word_at_cursor(word, sizeof(word));
        if ((int)strlen(word) >= 2) {
            lsp_request_completion(E.lsp, E.file_uri, E.cy, E.cx);
        }
    }
}

/* ── Update completion filter (shared for backspace + delete) ── */
static void update_completion_on_edit(void) {
    if (!E.completion.visible) return;

    char word[128];
    get_word_at_cursor(word, sizeof(word));
    completion_update_filter(&E.completion, word);
    if (E.completion.filtered_count == 0)
        completion_hide(&E.completion);
    E.render.full_redraw = true;
}

/* ── Palette command callbacks ──────────────────────────────── */

static void cmd_save(void *ctx) {
    (void)ctx;
    if (E.filepath) {
        if (buffer_save(E.buf, E.filepath) == 0) {
            render_set_status(&E.render, "Saved  %s", E.filepath);
            E.modified = false;

            /* Fire shell hook */
            if (E.cfg.hook_on_save[0])
                plugin_run_hook(E.cfg.hook_on_save, E.filepath);

            /* Fire plugin callbacks */
            plugin_on_save(&E.plugins, E.filepath);

            /* Fire ForgeScript event */
            fs_vm_fire_event(&E.scripts, FS_EVENT_SAVE, E.filepath);
        } else {
            render_set_status(&E.render, "ERROR: could not save  %s", E.filepath);
        }
    } else {
        render_set_status(&E.render, "No file path — use :save <path>");
    }
}

static void cmd_quit(void *ctx) {
    (void)ctx;
    E.running = false;
}

static void cmd_toggle_line_numbers(void *ctx) {
    (void)ctx;
    E.cfg.show_line_numbers = !E.cfg.show_line_numbers;
    E.render.full_redraw = true;
    render_set_status(&E.render, "Line numbers: %s",
                      E.cfg.show_line_numbers ? "on" : "off");
}

static void cmd_next_theme(void *ctx) {
    (void)ctx;
    const char **names = theme_list();
    int current = -1;
    for (int i = 0; names[i]; i++) {
        if (strcmp(names[i], E.theme.name) == 0) { current = i; break; }
    }
    int next = 0;
    if (current >= 0) {
        next = current + 1;
        if (!names[next]) next = 0;
    }
    theme_load(&E.theme, names[next]);
    render_set_theme(&E.render, &E.theme);
    render_set_status(&E.render, "Theme: %s", E.theme.name);
}

static void cmd_goto_line(void *ctx) {
    (void)ctx;
    /* Switch palette to goto mode */
    palette_show(&E.palette);
    E.palette.mode = PALETTE_GOTO_LINE;
}

static void cmd_open_file(void *ctx) {
    (void)ctx;
    if (!E.palette.files_loaded) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)))
            palette_scan_files(&E.palette, cwd);
    }
    palette_show(&E.palette);
    E.palette.mode = PALETTE_FILES;
    /* Trigger initial filter */
    E.palette.filtered_count = E.palette.file_count < PALETTE_MAX_FILES
                                 ? E.palette.file_count : PALETTE_MAX_FILES;
    for (int i = 0; i < E.palette.filtered_count; i++)
        E.palette.filtered[i] = i;
}

static void cmd_format(void *ctx) {
    (void)ctx;
    render_set_status(&E.render, "Format: not yet implemented");
}

static void cmd_lsp_restart(void *ctx) {
    (void)ctx;
    if (E.lsp) {
        lsp_stop(E.lsp);
        E.lsp = NULL;
    }
    if (E.filepath) {
        const char *server = lsp_detect_server(E.filepath);
        if (server) {
            E.lsp = lsp_start(server, NULL);
            render_set_status(&E.render, "LSP: restarting %s...", server);
        }
    }
}

static void cmd_toggle_blame(void *ctx) {
    (void)ctx;
    E.git.blame_visible = !E.git.blame_visible;
    if (E.git.blame_visible && E.git.repo_open && E.filepath) {
        git_refresh_blame(&E.git, E.filepath);
    }
    E.render.full_redraw = true;
    render_set_status(&E.render, "Blame: %s",
                      E.git.blame_visible ? "on" : "off");
}

static void cmd_toggle_timeline(void *ctx) {
    (void)ctx;
    E.git.timeline_visible = !E.git.timeline_visible;
    if (E.git.timeline_visible && E.git.repo_open && E.filepath) {
        git_refresh_log(&E.git, E.filepath);
    }
    E.render.full_redraw = true;
    render_set_status(&E.render, "Timeline: %s",
                      E.git.timeline_visible ? "on" : "off");
}

/* ── Guild commands ──────────────────────────────────────────── */

static void cmd_toggle_guild(void *ctx) {
    (void)ctx;
    E.guild_panel_visible = !E.guild_panel_visible;
    E.render.full_redraw = true;
    if (E.guild_panel_visible) {
        if (!E.ipc.connected)
            ipc_try_connect(&E.ipc);
        if (E.ipc.connected) {
            const char *json = "{\"type\":\"GUILD_STATUS\"}";
            ipc_send(&E.ipc, json, (int)strlen(json));
        }
        render_set_status(&E.render, "Guild panel — %s",
                          E.ipc.connected ? "connected" : "connecting...");
    } else {
        render_set_status(&E.render, "Guild panel closed");
    }
}

static void cmd_guild_ping(void *ctx) {
    (void)ctx;
    if (!E.ipc.connected) {
        render_set_status(&E.render, "Not connected to forge-net");
        return;
    }
    char json[512];
    int len = snprintf(json, sizeof(json),
        "{\"type\":\"PING\",\"target\":\"all\",\"file\":\"%s\",\"line\":%d}",
        E.filepath ? E.filepath : "", E.cy + 1);
    ipc_send(&E.ipc, json, len);
    render_set_status(&E.render, "Pinged all peers at line %d", E.cy + 1);
}

static void cmd_guild_share(void *ctx) {
    (void)ctx;
    if (!E.ipc.connected) {
        render_set_status(&E.render, "Not connected to forge-net");
        return;
    }
    if (!E.filepath) {
        render_set_status(&E.render, "No file to share");
        return;
    }
    const char *basename = strrchr(E.filepath, '/');
    basename = basename ? basename + 1 : E.filepath;

    FILE *f = fopen(E.filepath, "rb");
    if (!f) {
        render_set_status(&E.render, "Could not read file for sharing");
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        render_set_status(&E.render, "Could not size file for sharing");
        return;
    }
    long file_len = ftell(f);
    if (file_len < 0 || file_len > 10 * 1024 * 1024) {
        fclose(f);
        render_set_status(&E.render, "Share limit is 10MB");
        return;
    }
    rewind(f);

    unsigned char *data = malloc((size_t)file_len);
    if (!data) {
        fclose(f);
        render_set_status(&E.render, "Out of memory sharing file");
        return;
    }
    size_t read_len = fread(data, 1, (size_t)file_len, f);
    fclose(f);
    if (read_len != (size_t)file_len) {
        free(data);
        render_set_status(&E.render, "Could not read file for sharing");
        return;
    }

    char *b64 = base64_encode_bytes(data, read_len);
    free(data);
    if (!b64) {
        render_set_status(&E.render, "Out of memory encoding file");
        return;
    }

    size_t json_cap = strlen(basename) + strlen(b64) + 96;
    char *json = malloc(json_cap);
    if (!json) {
        free(b64);
        render_set_status(&E.render, "Out of memory sharing file");
        return;
    }
    int len = snprintf(json, json_cap,
        "{\"type\":\"FILE_SHARE\",\"name\":\"%s\",\"data_b64\":\"%s\"}", basename, b64);
    free(b64);
    ipc_send(&E.ipc, json, len);
    free(json);
    render_set_status(&E.render, "Shared: %s", basename);
}

static void cmd_guild_collab(void *ctx) {
    (void)ctx;
    if (!E.ipc.connected) {
        render_set_status(&E.render, "Not connected to forge-net");
        return;
    }
    render_set_status(&E.render, "Collab: use :collab <peer> in palette");
}

static void handle_net_message(const char *msg) {
    char type[64];
    if (!json_get_string(msg, "type", type, sizeof(type)))
        return;

    if (strcmp(type, "GUILD_STATUS_RESP") == 0) {
        json_get_string(msg, "guild_name", E.guild_name, sizeof(E.guild_name));
        json_get_string(msg, "my_handle", E.guild_handle, sizeof(E.guild_handle));
        json_get_int(msg, "peer_count", &E.guild_peer_count);
        snprintf(E.guild_last_event, sizeof(E.guild_last_event),
                 "%d peer%s online", E.guild_peer_count,
                 E.guild_peer_count == 1 ? "" : "s");
        E.render.full_redraw = true;
    } else if (strcmp(type, "CHAT_RECV") == 0 ||
               strcmp(type, "DM_RECV") == 0) {
        char from[64], text[128];
        json_get_string(msg, "from", from, sizeof(from));
        json_get_string(msg, "text", text, sizeof(text));
        snprintf(E.guild_last_event, sizeof(E.guild_last_event),
                 "%s: %s", from, text);
        render_set_status(&E.render, "%s", E.guild_last_event);
        E.render.full_redraw = true;
    } else if (strcmp(type, "PING_RECV") == 0) {
        char from[64], file[128];
        int line = 0;
        json_get_string(msg, "from", from, sizeof(from));
        json_get_string(msg, "file", file, sizeof(file));
        json_get_int(msg, "line", &line);
        snprintf(E.guild_last_event, sizeof(E.guild_last_event),
                 "Ping from %s: %s:%d", from, file, line);
        render_set_status(&E.render, "%s", E.guild_last_event);
        E.render.full_redraw = true;
    } else if (strcmp(type, "FILE_RECEIVED") == 0) {
        char from[64], name[128];
        int size = 0;
        json_get_string(msg, "from", from, sizeof(from));
        json_get_string(msg, "name", name, sizeof(name));
        json_get_int(msg, "size", &size);
        snprintf(E.guild_last_event, sizeof(E.guild_last_event),
                 "File from %s: %s (%d bytes)", from, name, size);
        render_set_status(&E.render, "%s", E.guild_last_event);
        E.render.full_redraw = true;
    } else if (strcmp(type, "INVITE_CREATED") == 0) {
        char fp[128];
        json_get_string(msg, "fingerprint", fp, sizeof(fp));
        snprintf(E.guild_last_event, sizeof(E.guild_last_event),
                 "Invite created; fingerprint %s", fp);
        render_set_status(&E.render, "%s", E.guild_last_event);
    } else if (strcmp(type, "TRUST_WARNING") == 0 ||
               strcmp(type, "ERROR") == 0) {
        char text[192];
        if (!json_get_string(msg, "message", text, sizeof(text)))
            json_get_string(msg, "received", text, sizeof(text));
        snprintf(E.guild_last_event, sizeof(E.guild_last_event),
                 "%s", text);
        render_set_status(&E.render, "forge-net: %s", text);
        E.render.full_redraw = true;
    }
}

/* ── Guild panel rendering ───────────────────────────────────── */

static void render_guild_panel(void) {
    if (!E.guild_panel_visible) return;

    ForgeTheme *t = E.render.theme;
    if (!t) return;

    int panel_width = 35;
    int panel_x = E.render.width - panel_width;
    if (panel_x < 30) return;

    int panel_height = E.render.height - 1;
    char buf[8192];
    int blen = 0;

    /* Panel background */
    for (int y = 0; y < panel_height && blen < (int)sizeof(buf) - 256; y++) {
        blen += snprintf(buf + blen, sizeof(buf) - blen,
            "\x1b[%d;%dH\x1b[48;2;%u;%u;%um",
            y + 1, panel_x, t->gutter_bg.r, t->gutter_bg.g, t->gutter_bg.b);
        for (int x = 0; x < panel_width && blen < (int)sizeof(buf) - 4; x++)
            buf[blen++] = ' ';
    }

    const char *guild_name = E.guild_name[0] ? E.guild_name : E.cfg.guild_name;
    const char *handle = E.guild_handle[0] ? E.guild_handle : E.cfg.guild_handle;

    /* Header */
    blen += snprintf(buf + blen, sizeof(buf) - blen,
        "\x1b[1;%dH\x1b[38;2;%u;%u;%um\x1b[1m GUILD: %.20s\x1b[0m",
        panel_x, t->accent.r, t->accent.g, t->accent.b,
        guild_name[0] ? guild_name : "local");

    /* Connection status */
    blen += snprintf(buf + blen, sizeof(buf) - blen,
        "\x1b[2;%dH\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um %s",
        panel_x, t->gutter_bg.r, t->gutter_bg.g, t->gutter_bg.b,
        t->gutter_fg.r, t->gutter_fg.g, t->gutter_fg.b,
        E.ipc.connected ? "● Connected" : "○ Disconnected");

    blen += snprintf(buf + blen, sizeof(buf) - blen,
        "\x1b[3;%dH\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um @%.18s  %d online",
        panel_x, t->gutter_bg.r, t->gutter_bg.g, t->gutter_bg.b,
        t->gutter_fg.r, t->gutter_fg.g, t->gutter_fg.b,
        handle[0] ? handle : "anon", E.guild_peer_count);

    /* Divider */
    blen += snprintf(buf + blen, sizeof(buf) - blen,
        "\x1b[4;%dH\x1b[38;2;%u;%u;%um",
        panel_x, t->gutter_fg.r, t->gutter_fg.g, t->gutter_fg.b);
    for (int i = 0; i < panel_width && blen < (int)sizeof(buf) - 8; i++)
        blen += snprintf(buf + blen, sizeof(buf) - blen, "─");

    /* Peers section */
    blen += snprintf(buf + blen, sizeof(buf) - blen,
        "\x1b[5;%dH\x1b[38;2;%u;%u;%um\x1b[1m Peers\x1b[0m",
        panel_x, t->statusbar_fg.r, t->statusbar_fg.g, t->statusbar_fg.b);

    blen += snprintf(buf + blen, sizeof(buf) - blen,
        "\x1b[6;%dH\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um  %s",
        panel_x, t->gutter_bg.r, t->gutter_bg.g, t->gutter_bg.b,
        t->comment.r, t->comment.g, t->comment.b,
        E.ipc.connected ? "Encrypted peer sync active" : "Waiting for forge-net...");

    blen += snprintf(buf + blen, sizeof(buf) - blen,
        "\x1b[8;%dH\x1b[38;2;%u;%u;%um\x1b[1m Activity\x1b[0m",
        panel_x, t->statusbar_fg.r, t->statusbar_fg.g, t->statusbar_fg.b);
    blen += snprintf(buf + blen, sizeof(buf) - blen,
        "\x1b[9;%dH\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um  %.30s",
        panel_x, t->gutter_bg.r, t->gutter_bg.g, t->gutter_bg.b,
        t->comment.r, t->comment.g, t->comment.b,
        E.guild_last_event[0] ? E.guild_last_event : "No network events yet");

    /* Commands help */
    int hr = panel_height - 4;
    blen += snprintf(buf + blen, sizeof(buf) - blen,
        "\x1b[%d;%dH\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um",
        hr, panel_x, t->gutter_bg.r, t->gutter_bg.g, t->gutter_bg.b,
        t->gutter_fg.r, t->gutter_fg.g, t->gutter_fg.b);
    for (int i = 0; i < panel_width && blen < (int)sizeof(buf) - 8; i++)
        blen += snprintf(buf + blen, sizeof(buf) - blen, "─");

    blen += snprintf(buf + blen, sizeof(buf) - blen,
        "\x1b[%d;%dH\x1b[38;2;%u;%u;%um :ping  :share  :collab",
        hr + 1, panel_x, t->comment.r, t->comment.g, t->comment.b);
    blen += snprintf(buf + blen, sizeof(buf) - blen,
        "\x1b[%d;%dH :grab  :drop   :chat", hr + 2, panel_x);
    blen += snprintf(buf + blen, sizeof(buf) - blen,
        "\x1b[%d;%dH Ctrl+G to close", hr + 3, panel_x);

    blen += snprintf(buf + blen, sizeof(buf) - blen, "\x1b[0m");
    if (blen > 0) (void)write(STDOUT_FILENO, buf, blen);
}

/* ── Hover rendering ────────────────────────────────────────── */

static void render_hover(void) {
    if (!E.show_hover || E.hover_text[0] == '\0') return;

    char buf[8192];
    int blen = 0;
    #define HPRINTF(...) blen += snprintf(buf + blen, sizeof(buf) - blen, __VA_ARGS__)

    HPRINTF("\x1b[s"); /* Save cursor */

    int popup_width = 60;
    int text_len = (int)strlen(E.hover_text);

    /* Count lines in hover text */
    int line_count = 1;
    for (int i = 0; i < text_len; i++)
        if (E.hover_text[i] == '\n') line_count++;
    if (line_count > 15) line_count = 15;

    int popup_height = line_count + 2;  /* borders */

    /* Position: above cursor if possible */
    int screen_row = E.cy - E.render.scroll_row + 1;
    int screen_col = E.cx - E.render.scroll_col + GUTTER_WIDTH + 2;
    int popup_row  = screen_row - popup_height;
    if (popup_row < 1) popup_row = screen_row + 1;
    if (popup_row + popup_height > E.render.height)
        popup_row = 1;

    if (screen_col + popup_width > E.render.width)
        screen_col = E.render.width - popup_width - 1;
    if (screen_col < 1) screen_col = 1;

    ThemeColor bg, fg, border;
    if (E.render.theme) {
        bg = E.render.theme->statusbar_bg;
        fg = E.render.theme->fg;
        border = E.render.theme->accent;
    } else {
        bg = (ThemeColor){40, 40, 40};
        fg = (ThemeColor){200, 200, 200};
        border = (ThemeColor){100, 150, 255};
    }

    /* Top border */
    HPRINTF("\x1b[%d;%dH\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
            popup_row, screen_col,
            border.r, border.g, border.b, bg.r, bg.g, bg.b);
    HPRINTF("╭");
    for (int i = 0; i < popup_width - 2; i++) HPRINTF("─");
    HPRINTF("╮");

    /* Content lines */
    int src_pos = 0;
    for (int y = 0; y < line_count && y < 15; y++) {
        HPRINTF("\x1b[%d;%dH\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um│ ",
                popup_row + 1 + y, screen_col,
                border.r, border.g, border.b, bg.r, bg.g, bg.b);

        HPRINTF("\x1b[38;2;%u;%u;%um", fg.r, fg.g, fg.b);

        int chars_written = 0;
        int content_width = popup_width - 4;
        while (src_pos < text_len && E.hover_text[src_pos] != '\n' &&
               chars_written < content_width) {
            if (E.hover_text[src_pos] >= 32)
                HPRINTF("%c", E.hover_text[src_pos]);
            else
                HPRINTF(" ");
            chars_written++;
            src_pos++;
        }
        if (src_pos < text_len && E.hover_text[src_pos] == '\n')
            src_pos++;

        for (int j = chars_written; j < content_width; j++)
            HPRINTF(" ");

        HPRINTF("\x1b[38;2;%u;%u;%um │",
                border.r, border.g, border.b);
    }

    /* Bottom border */
    HPRINTF("\x1b[%d;%dH\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
            popup_row + 1 + line_count, screen_col,
            border.r, border.g, border.b, bg.r, bg.g, bg.b);
    HPRINTF("╰");
    for (int i = 0; i < popup_width - 2; i++) HPRINTF("─");
    HPRINTF("╯");

    HPRINTF("\x1b[0m\x1b[u"); /* Reset + restore cursor */
    #undef HPRINTF

    (void)write(STDOUT_FILENO, buf, blen);
}

/* ── LSP helpers ────────────────────────────────────────────── */

static void lsp_update_diagnostics(void) {
    if (!E.lsp || !E.lsp->diagnostics_ready) return;

    render_clear_diagnostics(&E.render);
    for (int i = 0; i < E.lsp->diagnostic_count; i++) {
        LSPDiagnostic *d = &E.lsp->diagnostics[i];
        int severity;
        switch (d->severity) {
            case LSP_DIAG_ERROR:   severity = DIAG_ERROR;   break;
            case LSP_DIAG_WARNING: severity = DIAG_WARNING; break;
            case LSP_DIAG_INFO:    severity = DIAG_INFO;    break;
            default:               severity = DIAG_HINT;    break;
        }
        render_add_diagnostic(&E.render, d->start_line, severity, d->message);
    }
    E.lsp->diagnostics_ready = false;
    E.render.full_redraw = true;
}

/* ══════════════════════════════════════════════════════════════
   main
   ══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    E.filepath = (argc >= 2) ? argv[1] : NULL;

    session_arena = arena_new(1024 * 1024 * 16);

    signal(SIGWINCH, sigwinch_handler);
    input_enable_raw_mode();

    int term_rows, term_cols;
    if (input_get_term_size(&term_rows, &term_cols) == -1) {
        perror("forge: could not get terminal size");
        exit(1);
    }

    /* ── Load config ──────────────────────────────────────── */
    config_default(&E.cfg);
    config_load(&E.cfg, config_default_path());
    strncpy(E.guild_name, E.cfg.guild_name, sizeof(E.guild_name) - 1);
    strncpy(E.guild_handle, E.cfg.guild_handle, sizeof(E.guild_handle) - 1);
    strncpy(E.guild_last_event, "Not connected", sizeof(E.guild_last_event) - 1);

    /* ── Load theme ───────────────────────────────────────── */
    theme_load(&E.theme, E.cfg.theme_name);

    E.buf = E.filepath ? load_file(E.filepath) : buffer_new("", 0);

    ui_init(&E.ui, term_cols, term_rows);
    ui_register_builtins(&E.ui);
    render_init(&E.render, term_cols, term_rows);
    render_set_theme(&E.render, &E.theme);

    /* ── Initialize undo ──────────────────────────────────── */
    undo_init(&E.undo);

    /* ── Initialize git ───────────────────────────────────── */
    char cwd[1024];
    bool has_cwd = (getcwd(cwd, sizeof(cwd)) != NULL);
    if (has_cwd) {
        git_state_init(&E.git, cwd);
        if (E.git.repo_open && E.filepath) {
            git_refresh_diff(&E.git, E.filepath);
            git_refresh_branch(&E.git);
        }
    }
    E.render.git = &E.git;

    /* ── Initialize plugins ───────────────────────────────── */
    plugin_host_init(&E.plugins);
    /* Load plugins from default directories */
    for (int i = 0; i < E.plugins.search_path_count; i++)
        plugin_host_load_dir(&E.plugins, E.plugins.search_paths[i]);

    /* ── Initialize ForgeScript VM ────────────────────────── */
    fs_vm_init(&E.scripts);
    {
        char script_dir[1024];
        const char *home = getenv("HOME");
        if (home) {
            snprintf(script_dir, sizeof(script_dir),
                     "%s/.config/forge/scripts", home);
            fs_vm_load_dir(&E.scripts, script_dir);
        }
        /* Also load from project-local scripts */
        if (has_cwd) {
            snprintf(script_dir, sizeof(script_dir),
                     "%s/scripts/user", cwd);
            fs_vm_load_dir(&E.scripts, script_dir);
        }
    }

    /* ── Initialize IPC (try to connect to forge-net) ─────── */
    ipc_init(&E.ipc);
    /* Don't connect yet — forge-net may not be running */

    /* ── Initialize completion ────────────────────────────── */
    completion_init(&E.completion);

    /* ── Initialize palette ───────────────────────────────── */
    palette_init(&E.palette);
    palette_add_command(&E.palette, "Save File",         "Save current file",      "Ctrl+S",  cmd_save, NULL);
    palette_add_command(&E.palette, "Quit",              "Exit Forge",             "Ctrl+Q",  cmd_quit, NULL);
    palette_add_command(&E.palette, "Toggle Line Numbers","Show/hide line numbers", "",        cmd_toggle_line_numbers, NULL);
    palette_add_command(&E.palette, "Next Theme",        "Cycle through themes",   "",        cmd_next_theme, NULL);
    palette_add_command(&E.palette, "Go to Line",        "Jump to a line number",  "Ctrl+G",  cmd_goto_line, NULL);
    palette_add_command(&E.palette, "Open File",         "Open a file",            "",        cmd_open_file, NULL);
    palette_add_command(&E.palette, "Format File",       "Format with formatter",  "",        cmd_format, NULL);
    palette_add_command(&E.palette, "Restart LSP",       "Restart language server", "",       cmd_lsp_restart, NULL);
    palette_add_command(&E.palette, "Toggle Blame",      "Show/hide git blame",    "",        cmd_toggle_blame, NULL);
    palette_add_command(&E.palette, "Toggle Timeline",   "Show/hide git timeline", "Ctrl+T",  cmd_toggle_timeline, NULL);
    palette_add_command(&E.palette, "Guild Panel",       "Toggle guild panel",     "Ctrl+G",  cmd_toggle_guild, NULL);
    palette_add_command(&E.palette, "Ping",              "Ping peers at cursor",   "",         cmd_guild_ping, NULL);
    palette_add_command(&E.palette, "Share File",        "Share file with guild",  "",         cmd_guild_share, NULL);
    palette_add_command(&E.palette, "Collab",            "Start collab session",   "",         cmd_guild_collab, NULL);

    /* ── Start LSP ────────────────────────────────────────── */
    E.lsp = NULL;
    if (E.filepath && E.cfg.lsp_auto_detect) {
        const char *server = lsp_detect_server(E.filepath);
        if (server) {
            E.lsp = lsp_start(server, NULL);
            if (E.lsp) {
                lsp_path_to_uri(E.filepath, E.file_uri, sizeof(E.file_uri));
            }
        }
    }

    if (E.filepath) {
        if (E.git.repo_open)
            render_set_status(&E.render, "%s  [%s]",
                              E.filepath, E.git.branch);
        else
            render_set_status(&E.render, "%s", E.filepath);
    } else {
        render_set_status(&E.render, "[new file]   ^S save   ^Q quit   ^P palette");
    }

    /* Initial full clear */
    (void)write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);

    E.cx = 0;
    E.cy = 0;
    E.running  = true;
    E.modified = false;
    E.show_hover = false;
    E.lsp_dirty = false;
    get_time(&E.lsp_last_edit);

    /* Send didOpen once LSP initializes — checked in poll loop */
    bool lsp_did_open_sent = false;

    while (E.running) {

        /* Handle terminal resize */
        if (g_resized) {
            g_resized = 0;
            if (input_get_term_size(&term_rows, &term_cols) == 0) {
                render_resize(&E.render, term_cols, term_rows);
                render_set_theme(&E.render, &E.theme);
                ui_resize(&E.ui, term_cols, term_rows);
            }
        }

        /* Periodic config hot-reload */
        if (++E.config_poll_counter >= 200) {
            E.config_poll_counter = 0;
            if (config_poll_reload(&E.cfg)) {
                theme_load(&E.theme, E.cfg.theme_name);
                render_set_theme(&E.render, &E.theme);
                render_set_status(&E.render, "config reloaded");
            }
        }

        /* LSP time-based debounce — sync if idle for 300ms */
        lsp_debounce_sync();

        /* Poll LSP */
        if (E.lsp && E.lsp->running) {
            lsp_poll(E.lsp);

            /* Send didOpen after initialization */
            if (E.lsp->initialized && !lsp_did_open_sent && E.filepath) {
                char *text = buffer_get_text(E.buf);
                lsp_send_did_open(E.lsp, E.file_uri,
                                  lsp_language_id(E.filepath), text);
                free(text);
                lsp_did_open_sent = true;
                render_set_status(&E.render, "%s  [%s]",
                                  E.filepath, E.lsp->server_name);
            }

            /* Process completion results */
            if (E.lsp->completion_ready) {
                if (E.lsp->completion_count > 0) {
                    completion_show(&E.completion, E.lsp->completions,
                                    E.lsp->completion_count, E.cx, E.cy);
                }
                E.lsp->completion_ready = false;
            }

            /* Process hover results */
            if (E.lsp->hover_ready) {
                if (E.lsp->hover.valid) {
                    size_t hlen = strlen(E.lsp->hover.contents);
                    if (hlen >= sizeof(E.hover_text))
                        hlen = sizeof(E.hover_text) - 1;
                    memcpy(E.hover_text, E.lsp->hover.contents, hlen);
                    E.hover_text[hlen] = '\0';
                    E.show_hover = true;
                }
                E.lsp->hover_ready = false;
            }

            /* Process definition results */
            if (E.lsp->definition_ready) {
                if (E.lsp->definition.valid) {
                    /* For now, handle same-file jumps only */
                    E.cy = E.lsp->definition.line;
                    E.cx = E.lsp->definition.col;
                    clamp_cx(&E.cx, E.cy);
                    render_set_status(&E.render, "→ Ln %d, Col %d",
                                      E.cy + 1, E.cx + 1);
                    E.render.full_redraw = true;
                }
                E.lsp->definition_ready = false;
            }

            /* Process diagnostics */
            lsp_update_diagnostics();
        }

        /* ── Render ──────────────────────────────────────── */
        render_frame(&E.render, E.buf, &E.ui, E.cx, E.cy);

        /* Render overlays */
        if (E.completion.visible) {
            int screen_row = E.cy - E.render.scroll_row + 1;
            int screen_col = E.cx - E.render.scroll_col + GUTTER_WIDTH + 2;
            completion_render(&E.completion, &E.theme,
                              screen_row, screen_col,
                              E.render.width, E.render.height);
        }

        if (E.palette.visible) {
            palette_render(&E.palette, &E.theme,
                           E.render.width, E.render.height);
        }

        if (E.show_hover) {
            render_hover();
        }

        /* Guild panel overlay */
        if (E.guild_panel_visible) {
            render_guild_panel();
        }

        /* IPC polling and auto-reconnect */
        if (E.ipc.connected) {
            int msgs = ipc_poll(&E.ipc);
            while (msgs > 0) {
                char *msg = ipc_read_message(&E.ipc);
                if (msg) {
                    handle_net_message(msg);
                    free(msg);
                }
                msgs--;
            }
            if (E.guild_panel_visible) {
                if (E.guild_status_timer-- <= 0) {
                    const char *json = "{\"type\":\"GUILD_STATUS\"}";
                    ipc_send(&E.ipc, json, (int)strlen(json));
                    E.guild_status_timer = 120;
                }
            }
        } else if (E.guild_panel_visible) {
            /* Try reconnecting periodically */
            ipc_try_connect(&E.ipc);
        }

        int key = input_read_key();

        /* Dismiss hover on any key — but DON'T consume the key!
           Re-process it below instead of `continue`. */
        if (E.show_hover) {
            E.show_hover = false;
            E.render.full_redraw = true;
            /* Fall through to process the key normally */
        }

        /* ── Palette takes priority ──────────────────────── */
        if (E.palette.visible) {
            palette_handle_key(&E.palette, key);

            /* Check if palette produced a result */
            if (E.palette.accepted) {
                E.palette.accepted = false;

                if (E.palette.mode == PALETTE_GOTO_LINE ||
                    E.palette.goto_line > 0) {
                    int target = E.palette.goto_line - 1;
                    if (target < 0) target = 0;
                    size_t lc = buffer_line_count(E.buf);
                    if (lc > 0 && (size_t)target >= lc)
                        target = (int)lc - 1;
                    E.cy = target;
                    E.cx = 0;
                    clamp_cx(&E.cx, E.cy);
                    E.palette.goto_line = 0;
                }

                if (E.palette.result_path[0]) {
                    /* Open new file */
                    buffer_free(E.buf);
                    E.buf = load_file(E.palette.result_path);
                    E.filepath = E.palette.result_path;
                    E.cx = 0;
                    E.cy = 0;

                    /* Reset undo for new file */
                    undo_free(&E.undo);
                    undo_init(&E.undo);

                    /* Restart LSP for new file */
                    if (E.lsp) { lsp_stop(E.lsp); E.lsp = NULL; }
                    lsp_did_open_sent = false;
                    if (E.cfg.lsp_auto_detect) {
                        const char *server = lsp_detect_server(E.filepath);
                        if (server) {
                            E.lsp = lsp_start(server, NULL);
                            if (E.lsp)
                                lsp_path_to_uri(E.filepath, E.file_uri,
                                                sizeof(E.file_uri));
                        }
                    }

                    /* Refresh git for new file */
                    if (E.git.repo_open) {
                        git_refresh_diff(&E.git, E.filepath);
                        git_refresh_branch(&E.git);
                    }

                    /* Fire on_open hook */
                    if (E.cfg.hook_on_open[0])
                        plugin_run_hook(E.cfg.hook_on_open, E.filepath);

                    E.modified = false;
                    render_set_status(&E.render, "%s", E.filepath);
                    E.palette.result_path[0] = '\0';
                    E.render.full_redraw = true;
                }
            }
            E.render.full_redraw = true;
            continue;
        }

        /* ── Completion popup keys ───────────────────────── */
        if (E.completion.visible) {
            if (key == KEY_ARROW_UP) {
                completion_move_up(&E.completion);
                E.render.full_redraw = true;
                continue;
            }
            if (key == KEY_ARROW_DOWN) {
                completion_move_down(&E.completion);
                E.render.full_redraw = true;
                continue;
            }
            if (key == '\t' || key == '\r' || key == '\n') {
                /* Accept completion */
                const LSPCompletionItem *item =
                    completion_get_selected(&E.completion);
                if (item) {
                    /* Delete the prefix typed since anchor */
                    int prefix_len = E.cx - E.completion.anchor_cx;
                    if (prefix_len > 0) {
                        size_t pos = buffer_get_offset(E.buf, E.cx, E.cy);
                        char *deleted = buffer_get_line(E.buf, E.cy);
                        /* Record the delete for undo */
                        if (deleted) {
                            int dstart = E.completion.anchor_cx;
                            if (dstart >= 0 && dstart + prefix_len <= (int)strlen(deleted)) {
                                undo_record(&E.undo, UNDO_DELETE, pos - prefix_len,
                                            deleted + dstart, prefix_len, E.cx, E.cy);
                            }
                            free(deleted);
                        }
                        buffer_delete(E.buf, pos - prefix_len, prefix_len);
                        E.cx -= prefix_len;
                    }

                    /* Insert the completion text */
                    const char *text = item->insert_text[0]
                                         ? item->insert_text
                                         : item->label;
                    size_t pos = buffer_get_offset(E.buf, E.cx, E.cy);
                    int tlen = (int)strlen(text);
                    buffer_insert(E.buf, pos, text, tlen);
                    undo_record(&E.undo, UNDO_INSERT, pos, text, tlen, E.cx, E.cy);
                    E.cx += tlen;
                    mark_modified();
                }
                completion_hide(&E.completion);
                E.render.full_redraw = true;
                continue;
            }
            if (key == KEY_ESC) {
                completion_hide(&E.completion);
                E.render.full_redraw = true;
                continue;
            }
            /* Other keys: fall through to normal handling, but update filter */
        }

        /* ── Ctrl+P: Command Palette ─────────────────────── */
        if (key == KEY_CTRL_P) {
            palette_show(&E.palette);
            E.render.full_redraw = true;
            continue;
        }

        /* ── Ctrl+K: Hover docs ──────────────────────────── */
        if (key == KEY_CTRL_K) {
            if (E.lsp && E.lsp->running && E.lsp->initialized) {
                lsp_request_hover(E.lsp, E.file_uri, E.cy, E.cx);
                render_set_status(&E.render, "Fetching hover docs...");
            } else {
                render_set_status(&E.render, "No LSP server running");
            }
            continue;
        }

        /* ── Ctrl+G: Go-to-definition / Guild panel ─────── */
        if (key == KEY_CTRL_G) {
            if (E.lsp && E.lsp->running && E.lsp->initialized) {
                lsp_request_definition(E.lsp, E.file_uri, E.cy, E.cx);
                render_set_status(&E.render, "Finding definition...");
            } else {
                /* No LSP: toggle guild panel */
                cmd_toggle_guild(NULL);
            }
            continue;
        }

        /* ── Ctrl+T: Toggle Git Timeline ─────────────────── */
        if (key == KEY_CTRL_T) {
            cmd_toggle_timeline(NULL);
            continue;
        }

        /* ── Ctrl+B: Toggle Git Blame ────────────────────── */
        if (key == KEY_CTRL_B) {
            cmd_toggle_blame(NULL);
            continue;
        }

        /* ── Timeline navigation (when timeline is visible) ── */
        if (E.git.timeline_visible && E.git.commit_count > 0) {
            if (key == KEY_ARROW_LEFT && E.git.timeline_selected > 0) {
                E.git.timeline_selected--;
                E.render.full_redraw = true;
                GitCommit *c = &E.git.commits[E.git.timeline_selected];
                render_set_status(&E.render, "Timeline: %s — %s",
                                  c->short_sha, c->message);
                continue;
            }
            if (key == KEY_ARROW_RIGHT &&
                E.git.timeline_selected < E.git.commit_count - 1) {
                E.git.timeline_selected++;
                E.render.full_redraw = true;
                GitCommit *c = &E.git.commits[E.git.timeline_selected];
                render_set_status(&E.render, "Timeline: %s — %s",
                                  c->short_sha, c->message);
                continue;
            }
            /* Enter: load file at selected commit (read-only view) */
            if ((key == '\r' || key == '\n') && E.filepath) {
                GitCommit *c = &E.git.commits[E.git.timeline_selected];
                char *content = git_file_at_commit(&E.git, E.filepath, c->sha);
                if (content) {
                    /* Replace buffer with historical content (read-only) */
                    buffer_free(E.buf);
                    E.buf = buffer_new(content, strlen(content));
                    free(content);
                    E.git.timeline_viewing = true;
                    E.cx = 0; E.cy = 0;
                    render_set_status(&E.render,
                        "[READ-ONLY] %s @ %s — %s  (Esc to return)",
                        E.filepath, c->short_sha, c->message);
                    E.render.full_redraw = true;
                } else {
                    render_set_status(&E.render,
                        "Could not load %s at commit %s",
                        E.filepath, c->short_sha);
                }
                continue;
            }
            /* Escape: return to present */
            if (key == KEY_ESC) {
                if (E.git.timeline_viewing) {
                    /* Reload the current file from disk */
                    buffer_free(E.buf);
                    E.buf = E.filepath ? load_file(E.filepath)
                                       : buffer_new("", 0);
                    E.git.timeline_viewing = false;
                    E.cx = 0; E.cy = 0;
                    render_set_status(&E.render, "%s  [%s]",
                                      E.filepath ? E.filepath : "[new file]",
                                      E.git.branch);
                    E.render.full_redraw = true;
                } else {
                    /* Just close timeline */
                    E.git.timeline_visible = false;
                    E.render.full_redraw = true;
                    render_set_status(&E.render, "Timeline closed");
                }
                continue;
            }
        }

        /* ── Block editing when viewing historical file ────── */
        if (E.git.timeline_viewing) {
            /* Only allow navigation and escape, not editing */
            if (key == KEY_ARROW_UP || key == KEY_ARROW_DOWN ||
                key == KEY_ARROW_LEFT || key == KEY_ARROW_RIGHT ||
                key == KEY_PAGE_UP || key == KEY_PAGE_DOWN ||
                key == KEY_HOME || key == KEY_END) {
                /* Fall through to normal navigation */
            } else if (key == KEY_CTRL_Q) {
                E.running = false;
                continue;
            } else {
                render_set_status(&E.render,
                    "[READ-ONLY] Press Esc to return to present");
                continue;
            }
        }

        /* ── Ctrl+Z: Undo ────────────────────────────────── */
        if (key == (KEY_CTRL_A + 25)) {  /* Ctrl+Z = 26 */
            UndoEntry *e = undo_pop(&E.undo);
            if (e) {
                if (e->type == UNDO_INSERT) {
                    /* Was an insert: delete the text to undo */
                    buffer_delete(E.buf, e->pos, e->len);
                } else {
                    /* Was a delete: re-insert the text to undo */
                    buffer_insert(E.buf, e->pos, e->text, e->len);
                }
                E.cx = e->cx;
                E.cy = e->cy;
                clamp_cx(&E.cx, E.cy);
                E.render.full_redraw = true;
                E.lsp_dirty = true;
                get_time(&E.lsp_last_edit);
                render_set_status(&E.render, "Undo");
            } else {
                render_set_status(&E.render, "Nothing to undo");
            }
            continue;
        }

        /* ── Ctrl+Y: Redo ────────────────────────────────── */
        if (key == (KEY_CTRL_A + 24)) {  /* Ctrl+Y = 25 */
            UndoEntry *e = undo_redo(&E.undo);
            if (e) {
                if (e->type == UNDO_INSERT) {
                    /* Was an insert: re-insert */
                    buffer_insert(E.buf, e->pos, e->text, e->len);
                } else {
                    /* Was a delete: re-delete */
                    buffer_delete(E.buf, e->pos, e->len);
                }
                E.cx = e->cx;
                E.cy = e->cy;
                clamp_cx(&E.cx, E.cy);
                E.render.full_redraw = true;
                E.lsp_dirty = true;
                get_time(&E.lsp_last_edit);
                render_set_status(&E.render, "Redo");
            } else {
                render_set_status(&E.render, "Nothing to redo");
            }
            continue;
        }

        /* ── Quit ─────────────────────────────────────────── */
        if (key == KEY_CTRL_Q) {
            E.running = false;

        /* ── Save ─────────────────────────────────────────── */
        } else if (key == KEY_CTRL_S) {
            cmd_save(NULL);
            /* Immediately sync LSP buffer on save */
            if (E.lsp && E.lsp->running && E.lsp->initialized) {
                char *text = buffer_get_text(E.buf);
                if (text) {
                    lsp_send_did_change(E.lsp, E.file_uri, text);
                    free(text);
                }
                E.lsp_dirty = false;
            }
            /* Refresh git diff after save */
            if (E.git.repo_open && E.filepath)
                git_refresh_diff(&E.git, E.filepath);
            E.render.full_redraw = true;

        /* ── Navigation ───────────────────────────────────── */
        } else if (key == KEY_ARROW_UP) {
            if (E.cy > 0) { E.cy--; clamp_cx(&E.cx, E.cy); }

        } else if (key == KEY_ARROW_DOWN) {
            size_t lc = buffer_line_count(E.buf);
            if (lc > 0 && (size_t)E.cy < lc - 1) {
                E.cy++; clamp_cx(&E.cx, E.cy);
            }

        } else if (key == KEY_ARROW_LEFT) {
            if (E.cx > 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = line_len(E.buf, E.cy);
            }

        } else if (key == KEY_ARROW_RIGHT) {
            int ll = line_len(E.buf, E.cy);
            if (E.cx < ll) {
                E.cx++;
            } else {
                size_t lc = buffer_line_count(E.buf);
                if ((size_t)E.cy < lc - 1) { E.cy++; E.cx = 0; }
            }

        } else if (key == KEY_PAGE_UP) {
            E.cy -= E.render.height - 2;
            if (E.cy < 0) E.cy = 0;
            clamp_cx(&E.cx, E.cy);

        } else if (key == KEY_PAGE_DOWN) {
            size_t lc = buffer_line_count(E.buf);
            E.cy += E.render.height - 2;
            if (lc > 0 && (size_t)E.cy >= lc) E.cy = (int)lc - 1;
            clamp_cx(&E.cx, E.cy);

        } else if (key == KEY_HOME) {
            E.cx = 0;

        } else if (key == KEY_END) {
            E.cx = line_len(E.buf, E.cy);

        /* ── Enter ────────────────────────────────────────── */
        } else if (key == '\r' || key == '\n') {
            size_t pos = buffer_get_offset(E.buf, E.cx, E.cy);

            /* Auto-indent: copy leading whitespace from current line */
            char *curr_line = buffer_get_line(E.buf, E.cy);
            int indent = 0;
            if (curr_line) {
                while (curr_line[indent] == ' ' || curr_line[indent] == '\t')
                    indent++;
                free(curr_line);
            }

            /* Insert newline + indent */
            char insert_buf[256];
            insert_buf[0] = '\n';
            int ilen = 1;
            for (int i = 0; i < indent && ilen < 255; i++)
                insert_buf[ilen++] = ' ';
            insert_buf[ilen] = '\0';

            buffer_insert(E.buf, pos, insert_buf, ilen);
            undo_record(&E.undo, UNDO_INSERT, pos, insert_buf, ilen, E.cx, E.cy);
            E.cy++;
            E.cx = indent;
            mark_modified();

        /* ── Backspace ────────────────────────────────────── */
        } else if (key == KEY_BACKSPACE) {
            size_t pos = buffer_get_offset(E.buf, E.cx, E.cy);
            if (pos > 0) {
                /* Get the character being deleted for undo */
                char *text = buffer_get_text(E.buf);
                char deleted_char = text ? text[pos - 1] : '\0';
                free(text);

                int old_cx = E.cx, old_cy = E.cy;
                buffer_delete(E.buf, pos - 1, 1);

                if (E.cx > 0) {
                    E.cx--;
                } else if (E.cy > 0) {
                    E.cy--;
                    E.cx = line_len(E.buf, E.cy);
                }

                /* Record for undo (try merging consecutive backspaces) */
                if (!undo_try_merge(&E.undo, UNDO_DELETE, pos - 1,
                                    &deleted_char, 1, old_cx, old_cy)) {
                    undo_record(&E.undo, UNDO_DELETE, pos - 1,
                                &deleted_char, 1, old_cx, old_cy);
                }

                mark_modified();
            }

            /* Update completion filter if visible */
            update_completion_on_edit();

        /* ── Delete (forward) ─────────────────────────────── */
        } else if (key == KEY_DELETE) {
            size_t pos = buffer_get_offset(E.buf, E.cx, E.cy);
            size_t tlen = buffer_total_len(E.buf);  /* No alloc! */
            if (pos < tlen) {
                /* Get the character being deleted for undo */
                char *text = buffer_get_text(E.buf);
                char deleted_char = text ? text[pos] : '\0';
                free(text);

                buffer_delete(E.buf, pos, 1);

                /* Record for undo */
                if (!undo_try_merge(&E.undo, UNDO_DELETE, pos,
                                    &deleted_char, 1, E.cx, E.cy)) {
                    undo_record(&E.undo, UNDO_DELETE, pos,
                                &deleted_char, 1, E.cx, E.cy);
                }

                mark_modified();
            }

            /* FIX: Update completion filter on forward delete too */
            update_completion_on_edit();

        /* ── Tab ──────────────────────────────────────────── */
        } else if (key == '\t') {
            /* If completion is visible, tab accepts */
            if (E.completion.visible) {
                const LSPCompletionItem *item =
                    completion_get_selected(&E.completion);
                if (item) {
                    int prefix_len = E.cx - E.completion.anchor_cx;
                    if (prefix_len > 0) {
                        size_t pos = buffer_get_offset(E.buf, E.cx, E.cy);
                        buffer_delete(E.buf, pos - prefix_len, prefix_len);
                        E.cx -= prefix_len;
                    }
                    const char *ins = item->insert_text[0]
                                        ? item->insert_text : item->label;
                    size_t pos = buffer_get_offset(E.buf, E.cx, E.cy);
                    int tlen = (int)strlen(ins);
                    buffer_insert(E.buf, pos, ins, tlen);
                    undo_record(&E.undo, UNDO_INSERT, pos, ins, tlen, E.cx, E.cy);
                    E.cx += tlen;
                    mark_modified();
                }
                completion_hide(&E.completion);
                E.render.full_redraw = true;
            } else {
                int spaces = E.cfg.tab_width - (E.cx % E.cfg.tab_width);
                size_t pos = buffer_get_offset(E.buf, E.cx, E.cy);
                if (E.cfg.use_spaces) {
                    char tab_buf[16];
                    for (int i = 0; i < spaces && i < 15; i++)
                        tab_buf[i] = ' ';
                    tab_buf[spaces] = '\0';
                    buffer_insert(E.buf, pos, tab_buf, spaces);
                    undo_record(&E.undo, UNDO_INSERT, pos, tab_buf, spaces, E.cx, E.cy);
                    E.cx += spaces;
                } else {
                    buffer_insert(E.buf, pos, "\t", 1);
                    undo_record(&E.undo, UNDO_INSERT, pos, "\t", 1, E.cx, E.cy);
                    E.cx++;
                }
                mark_modified();
            }

        /* ── Printable characters ─────────────────────────── */
        } else if (key >= 32 && key <= 126) {
            char c = (char)key;
            size_t pos = buffer_get_offset(E.buf, E.cx, E.cy);
            buffer_insert(E.buf, pos, &c, 1);

            /* Try merging with previous undo entry for word-level undo */
            if (!undo_try_merge(&E.undo, UNDO_INSERT, pos, &c, 1, E.cx, E.cy)) {
                undo_record(&E.undo, UNDO_INSERT, pos, &c, 1, E.cx, E.cy);
            }

            E.cx++;
            mark_modified();

            /* Auto-trigger completion on identifier chars and trigger chars */
            maybe_request_completion(key);

            /* Update completion filter if visible */
            if (E.completion.visible) {
                char word[128];
                get_word_at_cursor(word, sizeof(word));
                completion_update_filter(&E.completion, word);
                E.render.full_redraw = true;
            }
        }
    }

    /* ── Cleanup ──────────────────────────────────────────── */
    if (E.lsp) {
        if (E.file_uri[0])
            lsp_send_did_close(E.lsp, E.file_uri);
        lsp_stop(E.lsp);
    }

    /* Fire on_close hook */
    if (E.cfg.hook_on_close[0] && E.filepath)
        plugin_run_hook(E.cfg.hook_on_close, E.filepath);

    (void)write(STDOUT_FILENO, "\x1b[?25h", 6);     /* show cursor  */
    (void)write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7); /* clear screen */
    (void)write(STDOUT_FILENO, "\x1b[0m", 4);        /* reset colors */

    render_free(&E.render);
    undo_free(&E.undo);
    git_state_free(&E.git);
    plugin_host_free(&E.plugins);
    fs_vm_free(&E.scripts);
    ipc_free(&E.ipc);
    buffer_free(E.buf);
    arena_free(session_arena);

    if (E.modified && E.filepath)
        fprintf(stderr, "forge: warning: unsaved changes in %s\n", E.filepath);

    return 0;
}
