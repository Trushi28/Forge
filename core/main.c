#include "arena.h"
#include "buffer.h"
#include "completion.h"
#include "config.h"
#include "input.h"
#include "lsp.h"
#include "palette.h"
#include "render.h"
#include "theme.h"
#include "ui.h"
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    const char    *filepath;
    char           file_uri[LSP_MAX_URI];
    int            cx, cy;
    bool           running;
    bool           modified;
    bool           show_hover;
    char           hover_text[4096];
    int            hover_row, hover_col;

    int            config_poll_counter;
    int            lsp_change_counter;
} EditorState;

static EditorState E;

/* ── Resize signal ──────────────────────────────────────────── */
static volatile int g_resized = 0;
static void sigwinch_handler(int sig) {
    (void)sig;
    g_resized = 1;
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

/* ── Palette command callbacks ──────────────────────────────── */

static void cmd_save(void *ctx) {
    (void)ctx;
    if (E.filepath) {
        if (buffer_save(E.buf, E.filepath) == 0) {
            render_set_status(&E.render, "Saved  %s", E.filepath);
            E.modified = false;
        } else {
            render_set_status(&E.render, "ERROR: could not save  %s", E.filepath);
        }
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

static void lsp_sync_buffer(void) {
    if (!E.lsp || !E.lsp->running || !E.lsp->initialized) return;

    char *text = buffer_get_text(E.buf);
    if (text) {
        lsp_send_did_change(E.lsp, E.file_uri, text);
        free(text);
    }
}

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

static bool is_completion_trigger(int key) {
    return key == '.' || key == '>' || key == ':' || key == '/';
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

    /* ── Load theme ───────────────────────────────────────── */
    theme_load(&E.theme, E.cfg.theme_name);

    E.buf = E.filepath ? load_file(E.filepath) : buffer_new("", 0);

    ui_init(&E.ui, term_cols, term_rows);
    ui_register_builtins(&E.ui);
    render_init(&E.render, term_cols, term_rows);
    render_set_theme(&E.render, &E.theme);

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

    if (E.filepath)
        render_set_status(&E.render, "%s", E.filepath);
    else
        render_set_status(&E.render, "[new file]   ^S save   ^Q quit   ^P palette");

    /* Initial full clear */
    (void)write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);

    E.cx = 0;
    E.cy = 0;
    E.running  = true;
    E.modified = false;
    E.show_hover = false;

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

        int key = input_read_key();

        /* Dismiss hover on any key */
        if (E.show_hover) {
            E.show_hover = false;
            E.render.full_redraw = true;
            continue;
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
                    E.cx += tlen;
                    E.modified = true;
                    render_set_status(&E.render, "%s [+]",
                                      E.filepath ? E.filepath : "[new file]");
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

        /* ── Ctrl+G: Go-to-definition ────────────────────── */
        if (key == KEY_CTRL_G) {
            if (E.lsp && E.lsp->running && E.lsp->initialized) {
                lsp_request_definition(E.lsp, E.file_uri, E.cy, E.cx);
                render_set_status(&E.render, "Finding definition...");
            } else {
                /* No LSP: open goto-line instead */
                palette_show(&E.palette);
                E.palette.mode = PALETTE_GOTO_LINE;
                E.render.full_redraw = true;
            }
            continue;
        }

        /* ── Quit ─────────────────────────────────────────── */
        if (key == KEY_CTRL_Q) {
            E.running = false;

        /* ── Save ─────────────────────────────────────────── */
        } else if (key == KEY_CTRL_S) {
            cmd_save(NULL);
            E.render.full_redraw = true;
            render_frame(&E.render, E.buf, &E.ui, E.cx, E.cy);
            continue;

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
            E.cy++;
            E.cx = indent;
            E.modified = true;
            E.lsp_change_counter++;
            render_set_status(&E.render, "%s [+]",
                              E.filepath ? E.filepath : "[new file]");

        /* ── Backspace ────────────────────────────────────── */
        } else if (key == KEY_BACKSPACE) {
            size_t pos = buffer_get_offset(E.buf, E.cx, E.cy);
            if (pos > 0) {
                buffer_delete(E.buf, pos - 1, 1);
                if (E.cx > 0) {
                    E.cx--;
                } else if (E.cy > 0) {
                    E.cy--;
                    E.cx = line_len(E.buf, E.cy);
                }
                E.modified = true;
                E.lsp_change_counter++;
                render_set_status(&E.render, "%s [+]",
                                  E.filepath ? E.filepath : "[new file]");
            }

            /* Update completion filter if visible */
            if (E.completion.visible) {
                char word[128];
                get_word_at_cursor(word, sizeof(word));
                completion_update_filter(&E.completion, word);
                if (E.completion.filtered_count == 0)
                    completion_hide(&E.completion);
                E.render.full_redraw = true;
            }

        /* ── Delete (forward) ─────────────────────────────── */
        } else if (key == KEY_DELETE) {
            size_t pos = buffer_get_offset(E.buf, E.cx, E.cy);
            char *text = buffer_get_text(E.buf);
            size_t tlen = text ? strlen(text) : 0;
            free(text);
            if (pos < tlen) {
                buffer_delete(E.buf, pos, 1);
                E.modified = true;
                E.lsp_change_counter++;
                render_set_status(&E.render, "%s [+]",
                                  E.filepath ? E.filepath : "[new file]");
            }

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
                    E.cx += tlen;
                    E.modified = true;
                }
                completion_hide(&E.completion);
                E.render.full_redraw = true;
            } else {
                int spaces = E.cfg.tab_width - (E.cx % E.cfg.tab_width);
                size_t pos = buffer_get_offset(E.buf, E.cx, E.cy);
                if (E.cfg.use_spaces) {
                    for (int i = 0; i < spaces; i++)
                        buffer_insert(E.buf, pos + i, " ", 1);
                    E.cx += spaces;
                } else {
                    buffer_insert(E.buf, pos, "\t", 1);
                    E.cx++;
                }
                E.modified = true;
                E.lsp_change_counter++;
                render_set_status(&E.render, "%s [+]",
                                  E.filepath ? E.filepath : "[new file]");
            }

        /* ── Printable characters ─────────────────────────── */
        } else if (key >= 32 && key <= 126) {
            char c = (char)key;
            size_t pos = buffer_get_offset(E.buf, E.cx, E.cy);
            buffer_insert(E.buf, pos, &c, 1);
            E.cx++;
            E.modified = true;
            E.lsp_change_counter++;
            render_set_status(&E.render, "%s [+]",
                              E.filepath ? E.filepath : "[new file]");

            /* Trigger completion on specific characters */
            if (E.lsp && E.lsp->running && E.lsp->initialized &&
                E.cfg.lsp_completion && is_completion_trigger(key)) {
                lsp_request_completion(E.lsp, E.file_uri, E.cy, E.cx);
            }

            /* Update completion filter if visible */
            if (E.completion.visible) {
                char word[128];
                get_word_at_cursor(word, sizeof(word));
                completion_update_filter(&E.completion, word);
                E.render.full_redraw = true;
            }
        }

        /* Sync buffer to LSP periodically (every 5 edits) */
        if (E.lsp_change_counter >= 5) {
            E.lsp_change_counter = 0;
            lsp_sync_buffer();
        }
    }

    /* ── Cleanup ──────────────────────────────────────────── */
    if (E.lsp) {
        if (E.file_uri[0])
            lsp_send_did_close(E.lsp, E.file_uri);
        lsp_stop(E.lsp);
    }

    (void)write(STDOUT_FILENO, "\x1b[?25h", 6);     /* show cursor  */
    (void)write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7); /* clear screen */
    (void)write(STDOUT_FILENO, "\x1b[0m", 4);        /* reset colors */

    render_free(&E.render);
    buffer_free(E.buf);
    arena_free(session_arena);

    if (E.modified && E.filepath)
        fprintf(stderr, "forge: warning: unsaved changes in %s\n", E.filepath);

    return 0;
}
