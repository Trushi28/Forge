#include "render.h"
#include "buffer.h"
#include "theme.h"
#include "ui.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ══════════════════════════════════════════════════════════════
   Output stream — batches all ANSI output into one write()
   ══════════════════════════════════════════════════════════════ */

static void stream_grow(RenderState *r, int need) {
    if (r->out_len + need <= r->out_cap) return;
    r->out_cap = (r->out_len + need) * 2 + 1024;
    r->output_stream = realloc(r->output_stream, r->out_cap);
}

static void stream_append(RenderState *r, const char *s, int len) {
    stream_grow(r, len);
    memcpy(r->output_stream + r->out_len, s, len);
    r->out_len += len;
}

static void stream_str(RenderState *r, const char *s) {
    stream_append(r, s, (int)strlen(s));
}

static void stream_printf(RenderState *r, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) stream_append(r, tmp, n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1);
}

/* Emit truecolor foreground */
static void stream_fg(RenderState *r, ThemeColor c) {
    stream_printf(r, "\x1b[38;2;%u;%u;%um", c.r, c.g, c.b);
}

/* Emit truecolor background */
static void stream_bg(RenderState *r, ThemeColor c) {
    stream_printf(r, "\x1b[48;2;%u;%u;%um", c.r, c.g, c.b);
}

/* ══════════════════════════════════════════════════════════════
   Cell buffer helpers
   ══════════════════════════════════════════════════════════════ */

static void alloc_cell_bufs(RenderState *r) {
    r->front_buffer = malloc(r->height * sizeof(char *));
    r->back_buffer  = malloc(r->height * sizeof(char *));
    for (int i = 0; i < r->height; i++) {
        r->front_buffer[i] = malloc(r->width + 1);
        r->back_buffer[i]  = malloc(r->width + 1);
        memset(r->front_buffer[i], 0,   r->width + 1);
        memset(r->back_buffer[i],  ' ', r->width);
        r->back_buffer[i][r->width] = '\0';
    }
}

static void free_cell_bufs(RenderState *r) {
    for (int i = 0; i < r->height; i++) {
        free(r->front_buffer[i]);
        free(r->back_buffer[i]);
    }
    free(r->front_buffer);
    free(r->back_buffer);
}

/* ── Lifecycle ───────────────────────────────────────────────*/

void render_init(RenderState *r, int width, int height) {
    r->width         = width;
    r->height        = height;
    r->scroll_row    = 0;
    r->scroll_col    = 0;
    r->out_cap       = 65536;
    r->out_len       = 0;
    r->output_stream = malloc(r->out_cap);
    r->status_msg[0] = '\0';
    r->full_redraw   = true;
    r->theme         = NULL;
    r->git           = NULL;
    r->diag_count    = 0;
    alloc_cell_bufs(r);
}

void render_free(RenderState *r) {
    free_cell_bufs(r);
    free(r->output_stream);
}

void render_resize(RenderState *r, int width, int height) {
    free_cell_bufs(r);
    free(r->output_stream);
    r->width         = width;
    r->height        = height;
    r->out_cap       = 65536;
    r->out_len       = 0;
    r->output_stream = malloc(r->out_cap);
    r->full_redraw   = true;
    alloc_cell_bufs(r);
}

void render_set_status(RenderState *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->status_msg, sizeof(r->status_msg), fmt, ap);
    va_end(ap);
}

void render_set_theme(RenderState *r, ForgeTheme *theme) {
    r->theme = theme;
    r->full_redraw = true;
}

/* ── Diagnostics ─────────────────────────────────────────────*/

void render_clear_diagnostics(RenderState *r) {
    r->diag_count = 0;
}

void render_add_diagnostic(RenderState *r, int line, int severity,
                           const char *msg) {
    if (r->diag_count >= MAX_DIAGNOSTICS) return;
    Diagnostic *d = &r->diagnostics[r->diag_count++];
    d->line     = line;
    d->severity = severity;
    strncpy(d->message, msg, sizeof(d->message) - 1);
    d->message[sizeof(d->message) - 1] = '\0';
}

int render_get_diag_severity(RenderState *r, int line) {
    int worst = DIAG_NONE;
    for (int i = 0; i < r->diag_count; i++) {
        if (r->diagnostics[i].line == line) {
            if (r->diagnostics[i].severity < worst || worst == DIAG_NONE)
                worst = r->diagnostics[i].severity;
        }
    }
    return worst;
}

/* ── Scroll adjustment ───────────────────────────────────────*/

static void adjust_scroll(RenderState *r, UIRegistry *ui, int cx, int cy, int text_rows) {
    if (cy < r->scroll_row)
        r->scroll_row = cy;
    if (cy >= r->scroll_row + text_rows)
        r->scroll_row = cy - text_rows + 1;

    int gutter_w = ui->slots[SLOT_GUTTER].visible ? ui->slots[SLOT_GUTTER].width : 0;
    int text_cols = r->width - gutter_w;
    if (cx < r->scroll_col)
        r->scroll_col = cx;
    if (cx >= r->scroll_col + text_cols)
        r->scroll_col = cx - text_cols + 1;
    if (r->scroll_col < 0) r->scroll_col = 0;
}



/* ══════════════════════════════════════════════════════════════
   Syntax highlighting — simple state-machine tokenizer

   Recognizes: C/Rust/Go/JS keywords, strings, comments,
   numbers, preprocessor directives, and operators.
   Fast single-pass per line; no regex or AST needed.
   ══════════════════════════════════════════════════════════════ */

typedef enum {
    HL_NORMAL,
    HL_KEYWORD,
    HL_TYPE,
    HL_STRING,
    HL_NUMBER,
    HL_COMMENT,
    HL_OPERATOR,
    HL_PREPROC,
    HL_FUNCTION,
    HL_CONSTANT
} HighlightKind;

static const char *c_keywords[] = {
    "auto", "break", "case", "const", "continue", "default", "do",
    "else", "enum", "extern", "for", "goto", "if", "inline",
    "register", "restrict", "return", "sizeof", "static", "struct",
    "switch", "typedef", "union", "volatile", "while",
    /* Rust/JS/Go additions */
    "fn", "let", "mut", "pub", "impl", "trait", "match", "use", "mod",
    "crate", "self", "super", "async", "await", "yield",
    "function", "var", "class", "new", "this", "throw", "try", "catch",
    "finally", "import", "export", "from", "extends",
    "func", "package", "defer", "go", "range", "select", "chan",
    NULL
};

static const char *c_types[] = {
    "int", "char", "void", "float", "double", "long", "short", "unsigned",
    "signed", "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "ssize_t", "bool",
    "true", "false", "NULL",
    /* Rust types */
    "i8", "i16", "i32", "i64", "i128", "u8", "u16", "u32", "u64", "u128",
    "f32", "f64", "usize", "isize", "String", "Vec", "Option", "Result",
    "Box", "Self",
    /* Go types */
    "string", "byte", "rune", "error", "nil",
    NULL
};

static const char *c_constants[] = {
    "TRUE", "FALSE", "NULL", "EOF", "STDIN_FILENO", "STDOUT_FILENO",
    "STDERR_FILENO", "EXIT_SUCCESS", "EXIT_FAILURE",
    "None", "Some", "Ok", "Err", "undefined", "null",
    NULL
};

static bool is_word_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static bool word_match(const char *word, int len, const char *list[]) {
    for (int i = 0; list[i]; i++) {
        int wl = (int)strlen(list[i]);
        if (wl == len && memcmp(word, list[i], len) == 0)
            return true;
    }
    return false;
}

/* Classify each character of `line` into hl_out[].
   Handles // comments, "strings", 'chars', numbers, keywords.  */
static void highlight_line(const char *line, int len, HighlightKind *hl_out,
                           bool *in_block_comment) {
    int i = 0;

    /* If we're inside a block comment from a previous line */
    if (*in_block_comment) {
        while (i < len) {
            hl_out[i] = HL_COMMENT;
            if (i + 1 < len && line[i] == '*' && line[i + 1] == '/') {
                hl_out[i + 1] = HL_COMMENT;
                i += 2;
                *in_block_comment = false;
                break;
            }
            i++;
        }
    }

    while (i < len) {
        /* Preprocessor: # at start of line (after whitespace) */
        if (line[i] == '#') {
            bool at_start = true;
            for (int j = 0; j < i; j++) {
                if (line[j] != ' ' && line[j] != '\t') {
                    at_start = false;
                    break;
                }
            }
            if (at_start) {
                while (i < len) hl_out[i++] = HL_PREPROC;
                break;
            }
        }

        /* Block comment start */
        if (i + 1 < len && line[i] == '/' && line[i + 1] == '*') {
            *in_block_comment = true;
            while (i < len) {
                hl_out[i] = HL_COMMENT;
                if (i + 1 < len && line[i] == '*' && line[i + 1] == '/') {
                    hl_out[i + 1] = HL_COMMENT;
                    i += 2;
                    *in_block_comment = false;
                    break;
                }
                i++;
            }
            continue;
        }

        /* Line comment: // */
        if (i + 1 < len && line[i] == '/' && line[i + 1] == '/') {
            while (i < len) hl_out[i++] = HL_COMMENT;
            break;
        }

        /* String: "..." */
        if (line[i] == '"') {
            hl_out[i++] = HL_STRING;
            while (i < len && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < len) {
                    hl_out[i++] = HL_STRING;
                }
                hl_out[i++] = HL_STRING;
            }
            if (i < len) hl_out[i++] = HL_STRING;
            continue;
        }

        /* Char: '...' */
        if (line[i] == '\'') {
            hl_out[i++] = HL_STRING;
            while (i < len && line[i] != '\'') {
                if (line[i] == '\\' && i + 1 < len) {
                    hl_out[i++] = HL_STRING;
                }
                hl_out[i++] = HL_STRING;
            }
            if (i < len) hl_out[i++] = HL_STRING;
            continue;
        }

        /* Numbers */
        if (isdigit((unsigned char)line[i]) ||
            (line[i] == '.' && i + 1 < len && isdigit((unsigned char)line[i + 1]))) {
            /* Don't highlight numbers that are part of identifiers */
            if (i > 0 && is_word_char(line[i - 1])) {
                hl_out[i++] = HL_NORMAL;
                continue;
            }
            while (i < len && (isdigit((unsigned char)line[i]) ||
                               line[i] == '.' || line[i] == 'x' ||
                               line[i] == 'X' ||
                               (line[i] >= 'a' && line[i] <= 'f') ||
                               (line[i] >= 'A' && line[i] <= 'F') ||
                               line[i] == 'u' || line[i] == 'U' ||
                               line[i] == 'l' || line[i] == 'L'))
                hl_out[i++] = HL_NUMBER;
            continue;
        }

        /* Identifiers / keywords / types */
        if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            int start = i;
            while (i < len && is_word_char(line[i])) i++;
            int wlen = i - start;

            /* Check if followed by '(' → function call */
            int j = i;
            while (j < len && (line[j] == ' ' || line[j] == '\t')) j++;
            bool is_func = (j < len && line[j] == '(');

            HighlightKind kind = HL_NORMAL;
            if (word_match(line + start, wlen, c_keywords))
                kind = HL_KEYWORD;
            else if (word_match(line + start, wlen, c_types))
                kind = HL_TYPE;
            else if (word_match(line + start, wlen, c_constants))
                kind = HL_CONSTANT;
            else if (is_func)
                kind = HL_FUNCTION;

            for (int k = start; k < i; k++)
                hl_out[k] = kind;
            continue;
        }

        /* Operators */
        if (strchr("+-*/%=!<>&|^~?:;,.(){}[]", line[i])) {
            hl_out[i++] = HL_OPERATOR;
            continue;
        }

        hl_out[i++] = HL_NORMAL;
    }
}

static ThemeColor hl_to_color(ForgeTheme *t, HighlightKind kind) {
    switch (kind) {
        case HL_KEYWORD:   return t->keyword;
        case HL_TYPE:      return t->type_color;
        case HL_STRING:    return t->string_color;
        case HL_NUMBER:    return t->number_color;
        case HL_COMMENT:   return t->comment;
        case HL_OPERATOR:  return t->operator_color;
        case HL_PREPROC:   return t->preprocessor;
        case HL_FUNCTION:  return t->function_color;
        case HL_CONSTANT:  return t->constant;
        default:           return t->fg;
    }
}

#define ESC_RESET       "\x1b[0m"
#define ESC_BOLD        "\x1b[1m"
#define ESC_ITALIC      "\x1b[3m"
#define ESC_HIDE_CURSOR "\x1b[?25l"
#define ESC_SHOW_CURSOR "\x1b[?25h"

static void gutter_widget_render(Widget *self, RenderState *r, Buffer *b) {
    (void)self;
    int text_rows = r->height - 1;
    size_t total_lines = buffer_line_count(b);
    int gutter_w = GUTTER_WIDTH; /* default; overridden if cfg available */
    if (r->cfg) {
        /* Dynamically compute gutter width from config widget list */
        gutter_w = 5; /* base: 4 digits + 1 separator */
    }

    for (int y = 0; y < text_rows; y++) {
        int logical = r->scroll_row + y;
        char *row = r->back_buffer[y];

        if ((size_t)logical < total_lines) {
            int line_no = logical + 1;
            unsigned n = (unsigned)(line_no < 1 ? 1 : line_no > 9999 ? 9999 : line_no);
            row[gutter_w - 1] = ' ';
            row[gutter_w - 2] = (char)('0' + n % 10); n /= 10;
            row[gutter_w - 3] = n ? (char)('0' + n % 10) : ' '; n /= 10;
            row[gutter_w - 4] = n ? (char)('0' + n % 10) : ' '; n /= 10;
            if (gutter_w >= 5)
                row[gutter_w - 5] = n ? (char)('0' + n % 10) : ' ';
        } else {
            row[0] = '~';
            for (int i = 1; i < gutter_w; i++) row[i] = ' ';
        }
    }
}

static void content_widget_render(Widget *self, RenderState *r, Buffer *b) {
    (void)self;
    int text_rows = r->height - 1;
    size_t total_lines = buffer_line_count(b);
    int gutter_w = GUTTER_WIDTH;

    for (int y = 0; y < text_rows; y++) {
        int logical = r->scroll_row + y;
        char *row = r->back_buffer[y];

        if ((size_t)logical < total_lines) {
            char *line = buffer_get_line(b, logical);
            if (line) {
                int text_cols = r->width - gutter_w;
                int llen      = (int)strlen(line);
                int avail     = llen - r->scroll_col;
                if (avail > 0) {
                    int copy = avail < text_cols ? avail : text_cols;
                    memcpy(row + gutter_w, line + r->scroll_col, copy);
                }
                free(line);
            }
        }
    }
}

static void statusbar_widget_render(Widget *self, RenderState *r, Buffer *b) {
    (void)self;
    (void)b;
    int cy = r->cy;
    int cx = r->cx;

    stream_printf(r, "\x1b[%d;1H", r->height);

    if (r->theme) {
        ForgeTheme *t = r->theme;

        /* ── Config-driven statusbar: iterate r->cfg->statusbar_widgets ── */
        /* Build left and right sections from widget names */
        char left_buf[512] = "";
        char right_buf[256] = "";
        int left_len = 0, right_len = 0;

        /* Always show the mode indicator as leftmost */
        stream_bg(r, t->statusbar_accent);
        stream_fg(r, t->statusbar_bg);
        stream_str(r, ESC_BOLD);
        stream_str(r, "  FORGE  ");
        stream_str(r, ESC_RESET);

        stream_bg(r, t->statusbar_bg);
        stream_fg(r, t->statusbar_accent);
        stream_str(r, " │ ");
        left_len = 9 + 3;

        /* Iterate config widgets for left-side items */
        if (r->cfg) {
            for (int i = 0; i < r->cfg->statusbar_widget_count; i++) {
                const char *wname = r->cfg->statusbar_widgets[i];
                if (strcmp(wname, "mode_indicator") == 0) {
                    /* Already rendered above */
                } else if (strcmp(wname, "filename") == 0) {
                    const char *msg = r->status_msg[0] ? r->status_msg : "ready";
                    int n = snprintf(left_buf + left_len - 12, sizeof(left_buf) - left_len, "%s", msg);
                    (void)n;
                    stream_fg(r, t->statusbar_fg);
                    stream_bg(r, t->statusbar_bg);
                    stream_str(r, msg);
                    left_len += (int)strlen(msg);
                } else if (strcmp(wname, "lsp_status") == 0) {
                    if (r->lsp) {
                        /* LSP status shown as part of right section */
                    }
                }
                /* git_branch and cursor_pos are right-aligned below */
            }
        } else {
            stream_fg(r, t->statusbar_fg);
            stream_bg(r, t->statusbar_bg);
            const char *msg = r->status_msg[0] ? r->status_msg : "ready";
            stream_str(r, msg);
            left_len += (int)strlen(msg);
        }

        /* Right section: git_branch, theme, cursor_pos */
        const char *branch = (r->git && r->git->repo_open && r->git->branch[0])
                               ? r->git->branch : NULL;
        if (branch)
            right_len = snprintf(right_buf, sizeof(right_buf), "  %s │ %s │ Ln %d, Col %d  ",
                          branch, t->name, cy + 1, cx + 1);
        else
            right_len = snprintf(right_buf, sizeof(right_buf), " %s │ Ln %d, Col %d  ",
                          t->name, cy + 1, cx + 1);

        int pad = r->width - left_len - right_len;
        for (int i = 0; i < pad; i++) stream_append(r, " ", 1);

        stream_fg(r, t->statusbar_fg);
        stream_append(r, right_buf, right_len < (int)sizeof(right_buf) - 1 ? right_len : (int)sizeof(right_buf) - 1);
    } else {
        stream_str(r, "\x1b[7m\x1b[1m");

        char left[256];
        int ll = snprintf(left, sizeof(left), "  FORGE  │  %s",
                          r->status_msg[0] ? r->status_msg : "ready");
        stream_append(r, left, ll < (int)sizeof(left) - 1 ? ll : (int)sizeof(left) - 1);

        char right[64];
        int rl = snprintf(right, sizeof(right), " Ln %d, Col %d  ", cy + 1, cx + 1);
        int pad = r->width - ll - rl;
        for (int i = 0; i < pad; i++) stream_append(r, " ", 1);
        stream_append(r, right, rl < (int)sizeof(right) - 1 ? rl : (int)sizeof(right) - 1);
    }

    stream_str(r, ESC_RESET);
}

static Widget gutter_widget;
static Widget content_widget;
static Widget statusbar_widget;
static Widget timeline_widget_inst;

/* ── Timeline scrubber widget ───────────────────────────────── */

static void timeline_widget_render(Widget *self, RenderState *r, Buffer *b) {
    (void)self; (void)b;
    if (!r->git || !r->git->timeline_visible || r->git->commit_count == 0)
        return;

    ForgeTheme *t = r->theme;
    if (!t) return;

    int timeline_row = r->height - TIMELINE_HEIGHT;
    int width = r->width;
    GitState *gs = r->git;

    /* ── Top border ──────────────────────────────────────── */
    stream_printf(r, "\x1b[%d;1H", timeline_row);
    stream_bg(r, t->gutter_bg);
    stream_fg(r, t->accent);
    stream_str(r, "─");
    stream_str(r, " TimeLine ");
    for (int i = 11; i < width; i++) stream_str(r, "─");
    stream_str(r, ESC_RESET);

    /* ── Commit strip ────────────────────────────────────── */
    stream_printf(r, "\x1b[%d;1H", timeline_row + 1);
    stream_bg(r, t->gutter_bg);
    stream_fg(r, t->gutter_fg);
    stream_str(r, " ◄ ");

    /* Calculate visible range of commits */
    int avail = width - 6;  /* 3 left arrow, 3 right arrow */
    int entry_width = 14;   /* space per commit entry */
    int visible_count = avail / entry_width;
    if (visible_count < 1) visible_count = 1;
    if (visible_count > gs->commit_count) visible_count = gs->commit_count;

    /* Center selected commit in view */
    int start = gs->timeline_selected - visible_count / 2;
    if (start < 0) start = 0;
    if (start + visible_count > gs->commit_count)
        start = gs->commit_count - visible_count;
    if (start < 0) start = 0;

    int chars_used = 3;  /* initial " ◄ " */

    for (int i = 0; i < visible_count && chars_used + entry_width < width - 3; i++) {
        int idx = start + i;
        bool is_selected = (idx == gs->timeline_selected);

        if (is_selected) {
            stream_fg(r, t->accent);
            stream_str(r, ESC_BOLD);
            stream_str(r, "[");
        } else {
            stream_fg(r, t->gutter_fg);
            stream_str(r, " ");
        }

        /* Show short SHA + abbreviated message */
        char entry[16];
        snprintf(entry, sizeof(entry), "%.7s", gs->commits[idx].short_sha);
        stream_str(r, entry);

        if (is_selected) {
            stream_str(r, "]");
            stream_str(r, ESC_RESET);
            stream_bg(r, t->gutter_bg);
        } else {
            stream_str(r, " ");
        }

        /* Separator */
        stream_fg(r, t->gutter_fg);
        if (i < visible_count - 1)
            stream_str(r, " ── ");
        else
            stream_str(r, " ");

        chars_used += entry_width;
    }

    /* Fill rest + right arrow */
    for (int i = chars_used; i < width - 3; i++)
        stream_append(r, " ", 1);
    stream_fg(r, t->gutter_fg);
    stream_str(r, " ► ");
    stream_str(r, ESC_RESET);

    /* ── Detail row (message + author + date) ────────────── */
    stream_printf(r, "\x1b[%d;1H", timeline_row + 2);
    stream_bg(r, t->gutter_bg);
    stream_fg(r, t->statusbar_fg);

    if (gs->timeline_selected >= 0 && gs->timeline_selected < gs->commit_count) {
        GitCommit *c = &gs->commits[gs->timeline_selected];

        /* Format relative date */
        char datestr[32];
        time_t now = time(NULL);
        long diff_secs = (long)(now - c->date);
        if (diff_secs < 60)
            snprintf(datestr, sizeof(datestr), "%lds ago", diff_secs);
        else if (diff_secs < 3600)
            snprintf(datestr, sizeof(datestr), "%ldm ago", diff_secs / 60);
        else if (diff_secs < 86400)
            snprintf(datestr, sizeof(datestr), "%ldh ago", diff_secs / 3600);
        else
            snprintf(datestr, sizeof(datestr), "%ldd ago", diff_secs / 86400);

        char detail[512];
        int dlen = snprintf(detail, sizeof(detail),
                            "  %s  │  %s  │  %s",
                            c->short_sha, c->author, c->message);

        if (dlen > width - 20) dlen = width - 20;
        stream_append(r, detail, dlen > 0 ? dlen : 0);

        /* Right-align the date */
        int date_len = (int)strlen(datestr);
        int pad = width - dlen - date_len - 2;
        for (int i = 0; i < pad; i++) stream_append(r, " ", 1);
        stream_fg(r, t->accent);
        stream_str(r, datestr);
        stream_append(r, "  ", 2);
    } else {
        for (int i = 0; i < width; i++) stream_append(r, " ", 1);
    }

    stream_str(r, ESC_RESET);
}

/* ── Inline blame rendering ─────────────────────────────────── */

static void blame_render(RenderState *r, int cy) {
    if (!r->git || !r->git->blame_visible || r->git->blame_count == 0)
        return;

    ForgeTheme *t = r->theme;
    if (!t) return;

    int text_rows = r->height - 1;
    if (r->git->timeline_visible) text_rows -= TIMELINE_HEIGHT;

    for (int y = 0; y < text_rows; y++) {
        int logical = r->scroll_row + y;
        if (logical < 0 || logical >= r->git->blame_count) continue;

        GitBlameLine *bl = &r->git->blame[logical];

        /* Position at end of line area */
        int blame_col = r->width - BLAME_COL_WIDTH;
        if (blame_col < GUTTER_WIDTH + 20) continue;  /* too narrow */

        stream_printf(r, "\x1b[%d;%dH", y + 1, blame_col);

        /* Format relative time */
        char timestr[16];
        time_t now = time(NULL);
        long diff_secs = (long)(now - bl->date);
        if (diff_secs < 60)
            snprintf(timestr, sizeof(timestr), "%lds", diff_secs);
        else if (diff_secs < 3600)
            snprintf(timestr, sizeof(timestr), "%ldm", diff_secs / 60);
        else if (diff_secs < 86400)
            snprintf(timestr, sizeof(timestr), "%ldh", diff_secs / 3600);
        else if (diff_secs < 2592000)
            snprintf(timestr, sizeof(timestr), "%ldd", diff_secs / 86400);
        else
            snprintf(timestr, sizeof(timestr), "%ldmo", diff_secs / 2592000);

        /* Author name (truncated) */
        char author[12];
        snprintf(author, sizeof(author), "%.10s", bl->author);

        ThemeColor blame_bg = (logical == cy) ? t->line_highlight : t->bg;
        stream_bg(r, blame_bg);
        stream_fg(r, t->comment);
        stream_str(r, ESC_ITALIC);

        char blame_str[BLAME_COL_WIDTH + 1];
        int blen = snprintf(blame_str, sizeof(blame_str),
                            " %s • %s ago", author, timestr);
        /* Pad to fixed width */
        for (int i = blen; i < BLAME_COL_WIDTH; i++)
            blame_str[i] = ' ';
        blame_str[BLAME_COL_WIDTH] = '\0';

        stream_str(r, blame_str);
        stream_str(r, ESC_RESET);
    }
}

void ui_register_builtins(UIRegistry *ui) {
    memset(&gutter_widget, 0, sizeof(gutter_widget));
    strncpy(gutter_widget.name, "gutter", sizeof(gutter_widget.name) - 1);
    gutter_widget.visible = true;
    gutter_widget.priority = 10;
    gutter_widget.render = gutter_widget_render;
    ui_register_widget(ui, SLOT_GUTTER, &gutter_widget);

    memset(&content_widget, 0, sizeof(content_widget));
    strncpy(content_widget.name, "content", sizeof(content_widget.name) - 1);
    content_widget.visible = true;
    content_widget.priority = 10;
    content_widget.render = content_widget_render;
    ui_register_widget(ui, SLOT_CONTENT, &content_widget);

    memset(&statusbar_widget, 0, sizeof(statusbar_widget));
    strncpy(statusbar_widget.name, "statusbar", sizeof(statusbar_widget.name) - 1);
    statusbar_widget.visible = true;
    statusbar_widget.priority = 10;
    statusbar_widget.render = statusbar_widget_render;
    ui_register_widget(ui, SLOT_STATUSBAR, &statusbar_widget);

    memset(&timeline_widget_inst, 0, sizeof(timeline_widget_inst));
    strncpy(timeline_widget_inst.name, "git_timeline", sizeof(timeline_widget_inst.name) - 1);
    timeline_widget_inst.visible = true;
    timeline_widget_inst.priority = 20;
    timeline_widget_inst.render = timeline_widget_render;
    ui_register_widget(ui, SLOT_BOTTOMBAR, &timeline_widget_inst);
}

/* ══════════════════════════════════════════════════════════════
   render_frame — the main render loop

   Now theme-aware with syntax highlighting, current-line
   highlight, themed gutter, diagnostic markers, and themed
   status bar.
   ══════════════════════════════════════════════════════════════ */

void render_frame(RenderState *r, Buffer *b, UIRegistry *ui,
                  int cx, int cy) {
    int text_rows = r->height - 1;
    /* Reduce text area when timeline is visible */
    if (r->git && r->git->timeline_visible && r->git->commit_count > 0)
        text_rows -= TIMELINE_HEIGHT;
    adjust_scroll(r, ui, cx, cy, text_rows);

    r->out_len = 0;
    stream_str(r, ESC_HIDE_CURSOR);

    /* Full clear on first paint / resize */
    if (r->full_redraw) {
        stream_str(r, "\x1b[2J");
        /* Set terminal background color for the whole screen */
        if (r->theme) {
            stream_bg(r, r->theme->bg);
            /* Fill every cell with background */
            for (int y = 0; y < r->height; y++) {
                stream_printf(r, "\x1b[%d;1H", y + 1);
                for (int x = 0; x < r->width; x++)
                    stream_append(r, " ", 1);
            }
        }
        r->full_redraw = false;
        /* Poison front buffer so every line repaints */
        for (int y = 0; y < r->height; y++)
            memset(r->front_buffer[y], 0, r->width + 1);
    }

    size_t total_lines = buffer_line_count(b);

    /* Track block comment state across lines */
    bool in_block_comment = false;

    /* We need to process highlight state from scroll_row=0 to handle
       block comments that start above the viewport. For efficiency,
       just scan from the beginning up to scroll_row. */
    if (r->scroll_row > 0) {
        for (int y = 0; y < r->scroll_row && (size_t)y < total_lines; y++) {
            char *line = buffer_get_line(b, y);
            if (line) {
                int llen = (int)strlen(line);
                HighlightKind *hl = malloc(llen * sizeof(HighlightKind));
                memset(hl, 0, llen * sizeof(HighlightKind));
                highlight_line(line, llen, hl, &in_block_comment);
                free(hl);
                free(line);
            }
        }
    }

    /* ── Build back-buffer (text rows) ───────────────────────*/
    r->cx = cx;
    r->cy = cy;

    for (int y = 0; y < text_rows; y++) {
        char *row = r->back_buffer[y];
        memset(row, ' ', r->width);
        row[r->width] = '\0';
    }

    /* Call slot-based widgets to populate gutter & content */
    ui_render_slots(ui, r, b);

    /* ── Diff & emit text rows with syntax highlighting ──────*/
    /* Reset block comment state for actual rendering pass */
    in_block_comment = false;
    if (r->scroll_row > 0) {
        for (int y = 0; y < r->scroll_row && (size_t)y < total_lines; y++) {
            char *line = buffer_get_line(b, y);
            if (line) {
                int llen = (int)strlen(line);
                HighlightKind *hl = malloc(llen * sizeof(HighlightKind));
                memset(hl, 0, llen * sizeof(HighlightKind));
                highlight_line(line, llen, hl, &in_block_comment);
                free(hl);
                free(line);
            }
        }
    }

    for (int y = 0; y < text_rows; y++) {
        if (memcmp(r->front_buffer[y], r->back_buffer[y], r->width) == 0
            && !r->theme)
            continue;

        stream_printf(r, "\x1b[%d;1H", y + 1);

        int logical = r->scroll_row + y;
        int is_cursor_row = (logical == cy);
        char *row = r->back_buffer[y];

        if (r->theme) {
            ForgeTheme *t = r->theme;

            /* Background for this row */
            ThemeColor row_bg = is_cursor_row ? t->line_highlight : t->bg;

            /* ── Gutter (dynamic width from slot bounds) ────*/
            int gutter_w = ui->slots[SLOT_GUTTER].visible ? ui->slots[SLOT_GUTTER].width : GUTTER_WIDTH;
            stream_bg(r, t->gutter_bg);
            if (is_cursor_row)
                stream_fg(r, t->gutter_active);
            else
                stream_fg(r, t->gutter_fg);

            if ((size_t)logical < total_lines) {
                stream_append(r, row, gutter_w);
            } else {
                /* Past EOF: dim tilde */
                stream_fg(r, t->gutter_fg);
                stream_str(r, "~");
                for (int gi = 1; gi < gutter_w; gi++) stream_append(r, " ", 1);
            }

            /* ── Diagnostic / Git diff marker after gutter ──*/
            int diag = render_get_diag_severity(r, logical);
            if (diag == DIAG_ERROR) {
                stream_fg(r, t->error);
                stream_bg(r, row_bg);
                stream_str(r, "●");
            } else if (diag == DIAG_WARNING) {
                stream_fg(r, t->warning);
                stream_bg(r, row_bg);
                stream_str(r, "▲");
            } else if (r->git && r->git->repo_open) {
                /* Show git diff marker if no diagnostic */
                GitDiffStatus ds = git_line_diff_status(r->git, logical);
                if (ds == GIT_DIFF_ADDED) {
                    stream_fg(r, t->hint);  /* green */
                    stream_bg(r, row_bg);
                    stream_str(r, "┃");
                } else if (ds == GIT_DIFF_MODIFIED) {
                    stream_fg(r, t->warning);  /* yellow */
                    stream_bg(r, row_bg);
                    stream_str(r, "┃");
                } else if (ds == GIT_DIFF_DELETED) {
                    stream_fg(r, t->error);  /* red */
                    stream_bg(r, row_bg);
                    stream_str(r, "▁");
                } else {
                    stream_bg(r, row_bg);
                    stream_append(r, " ", 1);
                }
            } else {
                stream_bg(r, row_bg);
                stream_append(r, " ", 1);
            }

            /* ── Text portion with syntax highlighting ──────*/
            if ((size_t)logical < total_lines) {
                char *line = buffer_get_line(b, logical);
                int llen = line ? (int)strlen(line) : 0;

                /* Highlight the full line */
                HighlightKind *hl = NULL;
                if (llen > 0) {
                    hl = malloc(llen * sizeof(HighlightKind));
                    memset(hl, 0, llen * sizeof(HighlightKind));
                    highlight_line(line, llen, hl, &in_block_comment);
                } else {
                    /* Empty line — still need to maintain block comment state */
                    HighlightKind dummy;
                    highlight_line("", 0, &dummy, &in_block_comment);
                }

                int text_cols = r->width - gutter_w - 1; /* -1 for diag col */
                int start_col = r->scroll_col;
                ThemeColor prev_color = t->fg;
                bool first = true;

                stream_bg(r, row_bg);

                for (int x = 0; x < text_cols; x++) {
                    int src_col = start_col + x;
                    if (src_col < llen && line) {
                        HighlightKind kind = hl[src_col];
                        ThemeColor fc = hl_to_color(t, kind);

                        if (first || fc.r != prev_color.r ||
                            fc.g != prev_color.g || fc.b != prev_color.b) {
                            stream_fg(r, fc);
                            if (kind == HL_KEYWORD)
                                stream_str(r, ESC_BOLD);
                            else if (kind == HL_COMMENT)
                                stream_str(r, ESC_ITALIC);
                            prev_color = fc;
                            first = false;
                        }
                        char ch = line[src_col];
                        stream_append(r, &ch, 1);
                    } else {
                        stream_append(r, " ", 1);
                    }
                }

                if (hl) free(hl);
                if (line) free(line);
            } else {
                /* Past EOF: fill with background */
                int fill = r->width - gutter_w - 1;
                stream_bg(r, row_bg);
                for (int x = 0; x < fill; x++)
                    stream_append(r, " ", 1);
            }

            stream_str(r, ESC_RESET);
        } else {
            /* Fallback: no theme, basic ANSI colors */
            if (is_cursor_row)
                stream_str(r, "\x1b[0;33m");
            else
                stream_str(r, "\x1b[2;37m");

            int gutter_w_fb = ui->slots[SLOT_GUTTER].visible ? ui->slots[SLOT_GUTTER].width : GUTTER_WIDTH;
            stream_append(r, row, gutter_w_fb);
            stream_str(r, ESC_RESET);
            stream_append(r, row + gutter_w_fb, r->width - gutter_w_fb);
        }

        memcpy(r->front_buffer[y], row, r->width);
    }

    /* ── Render blame overlay ─────────────────────────────────*/
    blame_render(r, cy);

    /* ── Place cursor ────────────────────────────────────────*/
    int screen_row = cy - r->scroll_row + 1;
    int final_gutter_w = ui->slots[SLOT_GUTTER].visible ? ui->slots[SLOT_GUTTER].width : GUTTER_WIDTH;
    int screen_col = cx - r->scroll_col + final_gutter_w + 1 + 1; /* +1 for diag col */
    stream_printf(r, "\x1b[%d;%dH", screen_row, screen_col);
    stream_str(r, ESC_SHOW_CURSOR);

    if (r->out_len > 0)
        (void)write(STDOUT_FILENO, r->output_stream, r->out_len);
}
