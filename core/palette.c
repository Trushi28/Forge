#include "palette.h"
#include "input.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ══════════════════════════════════════════════════════════════
   Initialization
   ══════════════════════════════════════════════════════════════ */

void palette_init(PaletteState *ps) {
    memset(ps, 0, sizeof(*ps));
}

void palette_add_command(PaletteState *ps, const char *name,
                         const char *desc, const char *shortcut,
                         PaletteAction action, void *ctx) {
    if (ps->command_count >= PALETTE_MAX_COMMANDS) return;
    PaletteCommand *c = &ps->commands[ps->command_count++];
    strncpy(c->name, name, sizeof(c->name) - 1);
    strncpy(c->description, desc, sizeof(c->description) - 1);
    if (shortcut) strncpy(c->shortcut, shortcut, sizeof(c->shortcut) - 1);
    c->action = action;
    c->ctx    = ctx;
}

/* ══════════════════════════════════════════════════════════════
   Show / Hide
   ══════════════════════════════════════════════════════════════ */

void palette_show(PaletteState *ps) {
    ps->visible    = true;
    ps->mode       = PALETTE_COMMANDS;
    ps->input_len  = 0;
    ps->input[0]   = '\0';
    ps->cursor     = 0;
    ps->selected   = 0;
    ps->accepted   = false;

    /* Show all commands initially */
    ps->filtered_count = ps->command_count;
    for (int i = 0; i < ps->command_count; i++)
        ps->filtered[i] = i;
}

void palette_hide(PaletteState *ps) {
    ps->visible = false;
}

/* ══════════════════════════════════════════════════════════════
   Fuzzy matching (same algorithm as completion)
   ══════════════════════════════════════════════════════════════ */

static int fuzzy_score(const char *pattern, const char *text) {
    if (!pattern[0]) return 0;

    int score = 0;
    int pi = 0;
    int plen = (int)strlen(pattern);
    int prev_match = -1;

    for (int ti = 0; text[ti] && pi < plen; ti++) {
        if (tolower((unsigned char)pattern[pi]) ==
            tolower((unsigned char)text[ti])) {
            if (prev_match == ti - 1) score += 3;
            if (ti == 0 || text[ti - 1] == '_' || text[ti - 1] == '/' ||
                text[ti - 1] == '.')
                score += 2;
            if (pattern[pi] == text[ti]) score += 1;
            prev_match = ti;
            pi++;
        }
    }

    return (pi == plen) ? score : -1;
}

static void filter_commands(PaletteState *ps) {
    const char *query = ps->input;
    if (query[0] == '\0') {
        ps->filtered_count = ps->command_count;
        for (int i = 0; i < ps->command_count; i++)
            ps->filtered[i] = i;
        return;
    }

    typedef struct { int idx; int score; } SI;
    SI scored[PALETTE_MAX_COMMANDS];
    int n = 0;

    for (int i = 0; i < ps->command_count; i++) {
        int s = fuzzy_score(query, ps->commands[i].name);
        if (s >= 0) {
            scored[n].idx   = i;
            scored[n].score = s;
            n++;
        }
    }

    /* Sort descending by score */
    for (int i = 1; i < n; i++) {
        SI tmp = scored[i];
        int j = i - 1;
        while (j >= 0 && scored[j].score < tmp.score) {
            scored[j + 1] = scored[j]; j--;
        }
        scored[j + 1] = tmp;
    }

    ps->filtered_count = n;
    for (int i = 0; i < n; i++)
        ps->filtered[i] = scored[i].idx;

    if (ps->selected >= ps->filtered_count)
        ps->selected = ps->filtered_count > 0 ? ps->filtered_count - 1 : 0;
}

static void filter_files(PaletteState *ps) {
    const char *query = ps->input;
    if (query[0] == '\0') {
        int max = ps->file_count < PALETTE_MAX_FILES ? ps->file_count : PALETTE_MAX_FILES;
        ps->filtered_count = max;
        for (int i = 0; i < max; i++)
            ps->filtered[i] = i;
        return;
    }

    typedef struct { int idx; int score; } SI;
    SI scored[PALETTE_MAX_FILES];
    int n = 0;

    for (int i = 0; i < ps->file_count; i++) {
        /* Match against both basename and full path */
        int s1 = fuzzy_score(query, ps->files[i].basename);
        int s2 = fuzzy_score(query, ps->files[i].path);
        int s = (s1 > s2) ? s1 : s2;
        if (s >= 0 && n < PALETTE_MAX_FILES) {
            scored[n].idx   = i;
            scored[n].score = s;
            n++;
        }
    }

    for (int i = 1; i < n; i++) {
        SI tmp = scored[i];
        int j = i - 1;
        while (j >= 0 && scored[j].score < tmp.score) {
            scored[j + 1] = scored[j]; j--;
        }
        scored[j + 1] = tmp;
    }

    ps->filtered_count = n;
    for (int i = 0; i < n; i++)
        ps->filtered[i] = scored[i].idx;

    if (ps->selected >= ps->filtered_count)
        ps->selected = ps->filtered_count > 0 ? ps->filtered_count - 1 : 0;
}

static void update_filter(PaletteState *ps) {
    if (ps->mode == PALETTE_COMMANDS)
        filter_commands(ps);
    else if (ps->mode == PALETTE_FILES)
        filter_files(ps);
}

/* ══════════════════════════════════════════════════════════════
   Key handling
   ══════════════════════════════════════════════════════════════ */

bool palette_handle_key(PaletteState *ps, int key) {
    if (!ps->visible) return false;

    if (key == KEY_ESC) {
        palette_hide(ps);
        return true;
    }

    if (key == KEY_ARROW_UP || key == KEY_CTRL_P) {
        if (ps->selected > 0) ps->selected--;
        return true;
    }

    if (key == KEY_ARROW_DOWN || key == KEY_CTRL_N) {
        if (ps->selected < ps->filtered_count - 1)
            ps->selected++;
        return true;
    }

    if (key == '\r' || key == '\n') {
        ps->accepted = true;

        if (ps->mode == PALETTE_GOTO_LINE) {
            ps->goto_line = atoi(ps->input);
            palette_hide(ps);
        } else if (ps->mode == PALETTE_FILES) {
            if (ps->filtered_count > 0 && ps->selected < ps->filtered_count) {
                int idx = ps->filtered[ps->selected];
                strncpy(ps->result_path, ps->files[idx].path,
                        sizeof(ps->result_path) - 1);
            }
            palette_hide(ps);
        } else if (ps->mode == PALETTE_COMMANDS) {
            if (ps->filtered_count > 0 && ps->selected < ps->filtered_count) {
                int idx = ps->filtered[ps->selected];
                ps->result_index = idx;
                PaletteCommand *cmd = &ps->commands[idx];
                palette_hide(ps);
                if (cmd->action)
                    cmd->action(cmd->ctx);
            } else {
                palette_hide(ps);
            }
        }
        return true;
    }

    if (key == KEY_BACKSPACE) {
        if (ps->input_len > 0) {
            ps->input[--ps->input_len] = '\0';
            ps->cursor = ps->input_len;

            /* Check mode transition */
            if (ps->input_len == 0 && ps->mode != PALETTE_COMMANDS) {
                ps->mode = PALETTE_COMMANDS;
            }
            update_filter(ps);
        }
        return true;
    }

    if (key >= 32 && key <= 126) {
        if (ps->input_len < PALETTE_MAX_INPUT - 1) {
            /* Mode detection on first character */
            if (ps->input_len == 0) {
                if (key == ':') {
                    ps->mode = PALETTE_GOTO_LINE;
                    return true;  /* Don't add ':' to input */
                }
                if (key == '>') {
                    ps->mode = PALETTE_FILES;
                    if (!ps->files_loaded) {
                        /* Files will be loaded by main.c */
                    }
                    return true;  /* Don't add '>' to input */
                }
            }

            ps->input[ps->input_len++] = (char)key;
            ps->input[ps->input_len]   = '\0';
            ps->cursor = ps->input_len;
            update_filter(ps);
        }
        return true;
    }

    return true;  /* Consume all keys when palette is visible */
}

/* ══════════════════════════════════════════════════════════════
   File scanning
   ══════════════════════════════════════════════════════════════ */

static const char *skip_dirs[] = {
    ".git", "node_modules", ".svn", ".hg", "__pycache__",
    ".venv", "target", "build", ".cache", ".idea", ".vscode", NULL
};

static bool should_skip(const char *name) {
    for (int i = 0; skip_dirs[i]; i++)
        if (strcmp(name, skip_dirs[i]) == 0) return true;
    return false;
}

static void scan_dir_recursive(PaletteState *ps, const char *base,
                                const char *prefix) {
    if (ps->file_count >= PALETTE_MAX_FILES) return;

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", base, prefix);

    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) && ps->file_count < PALETTE_MAX_FILES) {
        if (ent->d_name[0] == '.') continue;  /* Skip hidden */
        if (should_skip(ent->d_name)) continue;

        char rel[1024];
        if (prefix[0])
            snprintf(rel, sizeof(rel), "%s/%s", prefix, ent->d_name);
        else
            snprintf(rel, sizeof(rel), "%s", ent->d_name);

        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", base, rel);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_dir_recursive(ps, base, rel);
        } else if (S_ISREG(st.st_mode)) {
            PaletteFile *pf = &ps->files[ps->file_count++];
            memset(pf, 0, sizeof(*pf));
            snprintf(pf->path, sizeof(pf->path), "%s", rel);
            /* Extract basename */
            const char *slash = strrchr(rel, '/');
            snprintf(pf->basename, sizeof(pf->basename), "%s",
                     slash ? slash + 1 : rel);
        }
    }

    closedir(d);
}

void palette_scan_files(PaletteState *ps, const char *root_dir) {
    ps->file_count  = 0;
    ps->files_loaded = true;

    char dir[1024];
    if (root_dir)
        snprintf(dir, sizeof(dir), "%s", root_dir);
    else if (!getcwd(dir, sizeof(dir)))
        return;

    scan_dir_recursive(ps, dir, "");
}

/* ══════════════════════════════════════════════════════════════
   Render — centered overlay
   ══════════════════════════════════════════════════════════════ */

void palette_render(PaletteState *ps, ForgeTheme *theme,
                    int term_width, int term_height) {
    if (!ps->visible) return;

    /* Dimensions */
    int popup_width = 80;
    int limit = term_width - 20;
    if (popup_width > limit) popup_width = limit;
    if (popup_width < 40) popup_width = 40;
    if (popup_width > term_width) popup_width = term_width;

    int max_results = ps->filtered_count < PALETTE_MAX_VISIBLE
                        ? ps->filtered_count : PALETTE_MAX_VISIBLE;
    int popup_height = max_results + 4;

    /* Center horizontally and vertically */
    int col = (term_width - popup_width) / 2 + 1;
    int row = (term_height - popup_height) / 2 + 1;
    if (row < 1) row = 1;
    if (col < 1) col = 1;

    char buf[16384];
    int blen = 0;

    #define BPRINTF(...) blen += snprintf(buf + blen, sizeof(buf) - blen, __VA_ARGS__)

    BPRINTF("\x1b[s"); /* Save cursor */

    /* Colors */
    ThemeColor popup_bg, popup_fg, border_c, input_bg, sel_bg, sel_fg;
    if (theme) {
        popup_bg  = theme->gutter_bg;
        popup_fg  = theme->fg;
        border_c  = theme->gutter_fg;
        input_bg  = theme->gutter_bg;
        sel_bg    = theme->statusbar_accent;
        sel_fg    = theme->bg;
    } else {
        popup_bg  = (ThemeColor){40, 40, 40};
        popup_fg  = (ThemeColor){200, 200, 200};
        border_c  = (ThemeColor){150, 150, 150};
        input_bg  = (ThemeColor){40, 40, 40};
        sel_bg    = (ThemeColor){100, 150, 255};
        sel_fg    = (ThemeColor){20, 20, 20};
    }

    /* ── Top border ─────────────────────────────────────────── */
    BPRINTF("\x1b[%d;%dH", row, col);
    BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
            border_c.r, border_c.g, border_c.b,
            popup_bg.r, popup_bg.g, popup_bg.b);

    /* Mode indicator + title */
    const char *title = " Commands ";
    if (ps->mode == PALETTE_FILES) title = " Files ";
    else if (ps->mode == PALETTE_GOTO_LINE) title = " Go to Line ";

    /* Top bar with title */
    BPRINTF("┌");
    int title_len = (int)strlen(title);
    int dash_before = (popup_width - 2 - title_len) / 2;
    int dash_after  = popup_width - 2 - title_len - dash_before;
    for (int i = 0; i < dash_before; i++) BPRINTF("─");
    BPRINTF("\x1b[1m%s\x1b[22m", title);
    // Restore border color in case the title format/font reset colors
    BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
            border_c.r, border_c.g, border_c.b,
            popup_bg.r, popup_bg.g, popup_bg.b);
    for (int i = 0; i < dash_after; i++) BPRINTF("─");
    BPRINTF("┐");

    /* ── Input row ──────────────────────────────────────────── */
    BPRINTF("\x1b[%d;%dH", row + 1, col);
    // Draw left border
    BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
            border_c.r, border_c.g, border_c.b,
            popup_bg.r, popup_bg.g, popup_bg.b);
    BPRINTF("│");

    // Draw content padding and colors
    BPRINTF("\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um",
            input_bg.r, input_bg.g, input_bg.b,
            popup_fg.r, popup_fg.g, popup_fg.b);
    BPRINTF(" ");

    /* Prompt icon */
    if (ps->mode == PALETTE_GOTO_LINE)
        BPRINTF("\x1b[38;2;%u;%u;%um: \x1b[38;2;%u;%u;%um",
                border_c.r, border_c.g, border_c.b,
                popup_fg.r, popup_fg.g, popup_fg.b);
    else if (ps->mode == PALETTE_FILES)
        BPRINTF("\x1b[38;2;%u;%u;%um> \x1b[38;2;%u;%u;%um",
                border_c.r, border_c.g, border_c.b,
                popup_fg.r, popup_fg.g, popup_fg.b);
    else
        BPRINTF("⌘ ");

    /* Input text */
    int input_space = popup_width - 7;
    int show_len = ps->input_len < input_space ? ps->input_len : input_space;
    for (int i = 0; i < show_len; i++)
        BPRINTF("%c", ps->input[i]);

    /* Cursor indicator */
    BPRINTF("▎");

    /* Fill rest */
    int used = show_len + 1;
    for (int i = used; i < input_space; i++) BPRINTF(" ");
    BPRINTF(" ");

    // Draw right border
    BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
            border_c.r, border_c.g, border_c.b,
            popup_bg.r, popup_bg.g, popup_bg.b);
    BPRINTF("│");

    /* ── Separator ──────────────────────────────────────────── */
    BPRINTF("\x1b[%d;%dH", row + 2, col);
    BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
            border_c.r, border_c.g, border_c.b,
            popup_bg.r, popup_bg.g, popup_bg.b);
    BPRINTF("├");
    for (int i = 0; i < popup_width - 2; i++) BPRINTF("─");
    BPRINTF("┤");

    /* ── Result rows ────────────────────────────────────────── */
    /* Scrolling window */
    int scroll_start = 0;
    if (ps->selected >= scroll_start + max_results)
        scroll_start = ps->selected - max_results + 1;
    if (ps->selected < scroll_start)
        scroll_start = ps->selected;

    for (int i = 0; i < max_results; i++) {
        int fi = scroll_start + i;
        if (fi >= ps->filtered_count) break;

        bool selected = (fi == ps->selected);

        BPRINTF("\x1b[%d;%dH", row + 3 + i, col);

        // Draw left border
        BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
                border_c.r, border_c.g, border_c.b,
                popup_bg.r, popup_bg.g, popup_bg.b);
        BPRINTF("│");

        // Set row colors
        if (selected) {
            BPRINTF("\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um\x1b[1m",
                    sel_bg.r, sel_bg.g, sel_bg.b,
                    sel_fg.r, sel_fg.g, sel_fg.b);
        } else {
            BPRINTF("\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um",
                    popup_bg.r, popup_bg.g, popup_bg.b,
                    popup_fg.r, popup_fg.g, popup_fg.b);
        }

        BPRINTF(" ");

        int content_width = popup_width - 4;

        if (ps->mode == PALETTE_COMMANDS) {
            int idx = ps->filtered[fi];
            PaletteCommand *cmd = &ps->commands[idx];

            /* Command name */
            int nl = (int)strlen(cmd->name);
            int shortcut_len = (int)strlen(cmd->shortcut);
            int name_space = content_width - shortcut_len - 2;
            int show_name = nl < name_space ? nl : name_space;

            for (int j = 0; j < show_name; j++)
                BPRINTF("%c", cmd->name[j]);
            for (int j = show_name; j < name_space; j++)
                BPRINTF(" ");

            /* Shortcut (right-aligned, dimmer) */
            if (!selected && shortcut_len > 0) {
                if (theme) {
                    BPRINTF("\x1b[38;2;%u;%u;%um",
                            theme->comment.r, theme->comment.g, theme->comment.b);
                }
            }
            BPRINTF("  %s", cmd->shortcut);
            // Restore selection or popup colors
            if (selected) {
                BPRINTF("\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um\x1b[1m",
                        sel_bg.r, sel_bg.g, sel_bg.b,
                        sel_fg.r, sel_fg.g, sel_fg.b);
            } else {
                BPRINTF("\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um",
                        popup_bg.r, popup_bg.g, popup_bg.b,
                        popup_fg.r, popup_fg.g, popup_fg.b);
            }

        } else if (ps->mode == PALETTE_FILES) {
            int idx = ps->filtered[fi];
            PaletteFile *pf = &ps->files[idx];
            int pl = (int)strlen(pf->path);
            int show_path = pl < content_width ? pl : content_width;
            for (int j = 0; j < show_path; j++)
                BPRINTF("%c", pf->path[j]);
            for (int j = show_path; j < content_width; j++)
                BPRINTF(" ");

        } else if (ps->mode == PALETTE_GOTO_LINE) {
            for (int j = 0; j < content_width; j++)
                BPRINTF(" ");
        }

        // Draw right padding and right border
        BPRINTF(" ");
        BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
                border_c.r, border_c.g, border_c.b,
                popup_bg.r, popup_bg.g, popup_bg.b);
        BPRINTF("│");
    }

    /* ── Bottom border with hint bar inside ─────────────────── */
    BPRINTF("\x1b[%d;%dH", row + 3 + max_results, col);
    BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
            border_c.r, border_c.g, border_c.b,
            popup_bg.r, popup_bg.g, popup_bg.b);
    BPRINTF("└");

    const char *hint = " ↑↓ navigate  ⏎ accept  esc close  > files  : goto line ";
    int hint_len = (int)strlen(hint);
    if (popup_width - 2 < hint_len) {
        hint = " ↑↓  ⏎  esc ";
        hint_len = (int)strlen(hint);
    }

    dash_before = (popup_width - 2 - hint_len) / 2;
    dash_after  = popup_width - 2 - hint_len - dash_before;

    for (int i = 0; i < dash_before; i++) BPRINTF("─");

    // Print hint in comment/dim color
    if (theme) {
        BPRINTF("\x1b[38;2;%u;%u;%um", theme->comment.r, theme->comment.g, theme->comment.b);
    } else {
        BPRINTF("\x1b[2m");
    }
    BPRINTF("%s", hint);

    // Restore border color
    BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
            border_c.r, border_c.g, border_c.b,
            popup_bg.r, popup_bg.g, popup_bg.b);
    for (int i = 0; i < dash_after; i++) BPRINTF("─");
    BPRINTF("┘");

    BPRINTF("\x1b[0m\x1b[u"); /* Reset + restore cursor */

    #undef BPRINTF

    (void)write(STDOUT_FILENO, buf, blen);
}
