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
    ps->scroll_offset = 0;

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

static void print_highlighted_match(char *buf, int *blen, int max_len, const char *text, const char *query,
                                    ThemeColor hl_fg, ThemeColor normal_fg, ThemeColor bg_color,
                                    bool bold_item, bool underline_match) {
    const char *q = query;
    #define HPRINTF(...) *blen += snprintf(buf + *blen, max_len - *blen, __VA_ARGS__)

    // Set background color once
    HPRINTF("\x1b[48;2;%u;%u;%um", bg_color.r, bg_color.g, bg_color.b);

    while (*text && *blen < max_len - 64) {
        bool is_match = false;
        if (*q && tolower((unsigned char)*text) == tolower((unsigned char)*q)) {
            is_match = true;
            q++;
        }

        if (is_match) {
            // Highlight color
            if (underline_match) {
                // Keep same fg but add underline
                HPRINTF("\x1b[38;2;%u;%u;%um\x1b[4m", normal_fg.r, normal_fg.g, normal_fg.b);
            } else {
                HPRINTF("\x1b[38;2;%u;%u;%um\x1b[1m", hl_fg.r, hl_fg.g, hl_fg.b);
            }
        } else {
            // Normal color
            HPRINTF("\x1b[38;2;%u;%u;%um", normal_fg.r, normal_fg.g, normal_fg.b);
            if (underline_match) {
                HPRINTF("\x1b[24m"); // remove underline
            }
            if (bold_item) {
                HPRINTF("\x1b[1m");
            } else {
                HPRINTF("\x1b[22m"); // normal weight
            }
        }

        buf[(*blen)++] = *text;
        text++;
    }

    // Clean up underline if we added it
    if (underline_match) {
        HPRINTF("\x1b[24m");
    }

    #undef HPRINTF
}

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

    /* Calculate dynamic popup height for proper centering */
    int popup_height = 4;
    if (ps->mode == PALETTE_COMMANDS) {
        popup_height += max_results * 2;
    } else {
        popup_height += max_results;
    }

    /* Position at upper quarter, not center */
    int col = (term_width - popup_width) / 2 + 1;
    int row = term_height / 4;
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
        input_bg  = theme->gutter_bg; /* Match popup background to avoid "transparency" cutout */
        sel_bg    = theme->statusbar_accent;
        sel_fg    = theme->bg;
    } else {
        popup_bg  = (ThemeColor){40, 40, 40};
        popup_fg  = (ThemeColor){200, 200, 200};
        border_c  = (ThemeColor){150, 150, 150};
        input_bg  = (ThemeColor){40, 40, 40}; /* Match popup background */
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

    // Draw content padding and colors (bg color with fg text)
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
    int input_space = popup_width - 8;
    int show_len = ps->input_len < input_space ? ps->input_len : input_space;
    for (int i = 0; i < show_len; i++)
        BPRINTF("%c", ps->input[i]);

    /* Cursor indicator (cursor as a block) */
    BPRINTF("\x1b[38;2;%u;%u;%um█", theme ? theme->accent.r : border_c.r, theme ? theme->accent.g : border_c.g, theme ? theme->accent.b : border_c.b);
    // Restore default input text color
    BPRINTF("\x1b[38;2;%u;%u;%um", popup_fg.r, popup_fg.g, popup_fg.b);

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
    /* Scrolling window — use ps->scroll_offset instead of static local */
    int scroll_start = ps->scroll_offset;
    if (ps->selected >= scroll_start + max_results)
        scroll_start = ps->selected - max_results + 1;
    if (ps->selected < scroll_start)
        scroll_start = ps->selected;
    ps->scroll_offset = scroll_start;
    int current_row = row + 3;

    for (int i = 0; i < max_results; i++) {
        int fi = scroll_start + i;
        if (fi >= ps->filtered_count) break;

        bool selected = (fi == ps->selected);

        ThemeColor item_bg = selected ? sel_bg : popup_bg;
        ThemeColor item_fg = selected ? sel_fg : popup_fg;

        int content_width = popup_width - 4;

        if (ps->mode == PALETTE_COMMANDS) {
            int idx = ps->filtered[fi];
            PaletteCommand *cmd = &ps->commands[idx];

            // ── Row 1: Name & Shortcut
            BPRINTF("\x1b[%d;%dH", current_row, col);

            // Draw left border
            BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um│",
                    border_c.r, border_c.g, border_c.b,
                    popup_bg.r, popup_bg.g, popup_bg.b);

            // Left padding space in item background
            BPRINTF("\x1b[48;2;%u;%u;%um ", item_bg.r, item_bg.g, item_bg.b);

            /* Command name with matched characters highlight */
            int nl = (int)strlen(cmd->name);
            int shortcut_len = (int)strlen(cmd->shortcut);
            int name_space = content_width - shortcut_len - 2;
            int show_name = nl < name_space ? nl : name_space;
            if (show_name < 0) show_name = 0;

            char trunc_name[128];
            memcpy(trunc_name, cmd->name, show_name);
            trunc_name[show_name] = '\0';

            print_highlighted_match(buf, &blen, sizeof(buf), trunc_name, ps->input,
                                    sel_bg, item_fg, item_bg, selected, selected);

            // Pad remaining space between name and shortcut
            BPRINTF("\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um\x1b[22m",
                    item_bg.r, item_bg.g, item_bg.b,
                    item_fg.r, item_fg.g, item_fg.b);
            if (selected) BPRINTF("\x1b[1m");
            for (int j = show_name; j < name_space; j++) BPRINTF(" ");
            BPRINTF("  ");

            /* Shortcut (dimmed gutter_fg, right-aligned) */
            if (selected) {
                BPRINTF("\x1b[38;2;%u;%u;%um\x1b[1m", item_fg.r, item_fg.g, item_fg.b);
            } else {
                BPRINTF("\x1b[38;2;%u;%u;%um\x1b[2m", border_c.r, border_c.g, border_c.b);
            }
            BPRINTF("%s", cmd->shortcut);
            BPRINTF("\x1b[22m\x1b[0m");

            // Right padding and right border
            BPRINTF("\x1b[48;2;%u;%u;%um ", item_bg.r, item_bg.g, item_bg.b);
            BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um│",
                    border_c.r, border_c.g, border_c.b,
                    popup_bg.r, popup_bg.g, popup_bg.b);

            current_row++;

            // ── Row 2: Description
            BPRINTF("\x1b[%d;%dH", current_row, col);

            // Left border
            BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um│",
                    border_c.r, border_c.g, border_c.b,
                    popup_bg.r, popup_bg.g, popup_bg.b);

            // Left padding space
            BPRINTF("\x1b[48;2;%u;%u;%um ", item_bg.r, item_bg.g, item_bg.b);

            /* Description text (dimmed gutter_fg, smaller indent) */
            int dl = (int)strlen(cmd->description);
            int desc_space = content_width - 2;
            int show_desc = dl < desc_space ? dl : desc_space;
            if (show_desc < 0) show_desc = 0;

            char trunc_desc[256];
            memcpy(trunc_desc, cmd->description, show_desc);
            trunc_desc[show_desc] = '\0';

            BPRINTF("\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um  %s",
                    item_bg.r, item_bg.g, item_bg.b,
                    border_c.r, border_c.g, border_c.b,
                    trunc_desc);

            // Pad remaining spaces
            for (int j = show_desc; j < desc_space; j++) BPRINTF(" ");
            BPRINTF(" ");

            // Right border
            BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um│",
                    border_c.r, border_c.g, border_c.b,
                    popup_bg.r, popup_bg.g, popup_bg.b);

            current_row++;

        } else if (ps->mode == PALETTE_FILES) {
            int idx = ps->filtered[fi];
            PaletteFile *pf = &ps->files[idx];

            BPRINTF("\x1b[%d;%dH", current_row, col);

            // Left border
            BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um│",
                    border_c.r, border_c.g, border_c.b,
                    popup_bg.r, popup_bg.g, popup_bg.b);

            // Left padding space
            BPRINTF("\x1b[48;2;%u;%u;%um ", item_bg.r, item_bg.g, item_bg.b);

            /* File path with matched characters highlight */
            int pl = (int)strlen(pf->path);
            int show_path = pl < content_width ? pl : content_width;
            if (show_path < 0) show_path = 0;

            char trunc_path[512];
            memcpy(trunc_path, pf->path, show_path);
            trunc_path[show_path] = '\0';

            print_highlighted_match(buf, &blen, sizeof(buf), trunc_path, ps->input,
                                    sel_bg, item_fg, item_bg, selected, selected);

            // Pad remaining space
            BPRINTF("\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um\x1b[22m",
                    item_bg.r, item_bg.g, item_bg.b,
                    item_fg.r, item_fg.g, item_fg.b);
            if (selected) BPRINTF("\x1b[1m");
            for (int j = show_path; j < content_width; j++) BPRINTF(" ");

            // Right padding and border
            BPRINTF(" ");
            BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um│",
                    border_c.r, border_c.g, border_c.b,
                    popup_bg.r, popup_bg.g, popup_bg.b);

            current_row++;

        } else if (ps->mode == PALETTE_GOTO_LINE) {
            BPRINTF("\x1b[%d;%dH", current_row, col);

            // Left border
            BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um│",
                    border_c.r, border_c.g, border_c.b,
                    popup_bg.r, popup_bg.g, popup_bg.b);

            // Left padding space
            BPRINTF("\x1b[48;2;%u;%u;%um ", item_bg.r, item_bg.g, item_bg.b);

            // Goto line just shows empty/background
            for (int j = 0; j < content_width; j++) BPRINTF(" ");

            // Right padding and border
            BPRINTF(" ");
            BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um│",
                    border_c.r, border_c.g, border_c.b,
                    popup_bg.r, popup_bg.g, popup_bg.b);

            current_row++;
        }
    }

    /* ── Bottom border with hint bar inside ─────────────────── */
    BPRINTF("\x1b[%d;%dH", current_row, col);
    BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
            border_c.r, border_c.g, border_c.b,
            popup_bg.r, popup_bg.g, popup_bg.b);
    BPRINTF("└");

    const char *hint = " ↑↓ navigate  ⏎ accept  esc close ";
    int hint_len = (int)strlen(hint);
    if (popup_width - 2 < hint_len) {
        hint = " ↑↓  ⏎  esc ";
        hint_len = (int)strlen(hint);
    }

    dash_before = (popup_width - 2 - hint_len) / 2;
    dash_after  = popup_width - 2 - hint_len - dash_before;

    for (int i = 0; i < dash_before; i++) BPRINTF("─");

    // Print hint in gutter_fg color (unselected / normal, not comments)
    BPRINTF("\x1b[38;2;%u;%u;%um\x1b[22m", border_c.r, border_c.g, border_c.b);
    BPRINTF("%s", hint);

    // Restore border color/line
    BPRINTF("\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
            border_c.r, border_c.g, border_c.b,
            popup_bg.r, popup_bg.g, popup_bg.b);
    for (int i = 0; i < dash_after; i++) BPRINTF("─");
    BPRINTF("┘");

    BPRINTF("\x1b[0m\x1b[u"); /* Reset + restore cursor */

    #undef BPRINTF

    (void)write(STDOUT_FILENO, buf, blen);
}
