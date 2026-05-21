#include "render.h"
#include "buffer.h"
#include "ui.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ══════════════════════════════════════════════════════════════
   Output stream
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
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) stream_append(r, tmp, n < 128 ? n : 127);
}

/* ══════════════════════════════════════════════════════════════
   Cell buffer helpers
   Each row is (width) display bytes.  We keep a front and back
   buffer of these rows.  front is what is currently on-screen;
   back is what we want.  On every frame we diff and only write
   changed rows.  On the very first frame (or after a resize) we
   force a full repaint by poisoning front with 0x00.
   ══════════════════════════════════════════════════════════════ */

static void alloc_cell_bufs(RenderState *r) {
    r->front_buffer = malloc(r->height * sizeof(char *));
    r->back_buffer  = malloc(r->height * sizeof(char *));
    for (int i = 0; i < r->height; i++) {
        r->front_buffer[i] = malloc(r->width + 1);
        r->back_buffer[i]  = malloc(r->width + 1);
        /* poison front so every row is dirty on first paint */
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

/* ── Lifecycle ────────────────────────────────────────────────*/

void render_init(RenderState *r, int width, int height) {
    r->width         = width;
    r->height        = height;
    r->scroll_row    = 0;
    r->scroll_col    = 0;
    r->out_cap       = 32768;
    r->out_len       = 0;
    r->output_stream = malloc(r->out_cap);
    r->status_msg[0] = '\0';
    r->full_redraw   = true;
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
    r->out_cap       = 32768;
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

/* ── Scroll adjustment ────────────────────────────────────────*/

static void adjust_scroll(RenderState *r, int cx, int cy, int text_rows) {
    if (cy < r->scroll_row)
        r->scroll_row = cy;
    if (cy >= r->scroll_row + text_rows)
        r->scroll_row = cy - text_rows + 1;

    int text_cols = r->width - GUTTER_WIDTH;
    if (cx < r->scroll_col)
        r->scroll_col = cx;
    if (cx >= r->scroll_col + text_cols)
        r->scroll_col = cx - text_cols + 1;
    if (r->scroll_col < 0) r->scroll_col = 0;
}

/* ── Gutter ───────────────────────────────────────────────────*/

static void write_gutter(char *row, int line_no) {
    /* Right-align a 4-digit line number then a separator space.
       Hand-rolled to avoid snprintf truncation warning.          */
    unsigned n = (unsigned)(line_no < 1 ? 1
                          : line_no > 9999 ? 9999 : line_no);
    row[4] = ' ';
    row[3] = (char)('0' + n % 10); n /= 10;
    row[2] = n ? (char)('0' + n % 10) : ' '; n /= 10;
    row[1] = n ? (char)('0' + n % 10) : ' '; n /= 10;
    row[0] = n ? (char)('0' + n % 10) : ' ';
}

/* ══════════════════════════════════════════════════════════════
   render_frame
   ══════════════════════════════════════════════════════════════ */

/*  ANSI escape shorthand */
#define ESC_RESET       "\x1b[0m"
#define ESC_BOLD        "\x1b[1m"
#define ESC_REVERSE     "\x1b[7m"
#define ESC_HIDE_CURSOR "\x1b[?25l"
#define ESC_SHOW_CURSOR "\x1b[?25h"
/* Dim white for gutter numbers */
#define ESC_GUTTER_ON   "\x1b[2;37m"
/* Active line gutter highlight */
#define ESC_GUTTER_ACT  "\x1b[0;33m"

void render_frame(RenderState *r, Buffer *b, UIRegistry *ui,
                  int cx, int cy) {
    (void)ui;

    int text_rows = r->height - 1;   /* last row reserved for status bar */
    adjust_scroll(r, cx, cy, text_rows);

    r->out_len = 0;
    stream_str(r, ESC_HIDE_CURSOR);

    /* ── Clear on first paint / after resize ─────────────────*/
    if (r->full_redraw) {
        stream_str(r, "\x1b[2J");   /* erase whole display */
        r->full_redraw = false;
    }

    size_t total_lines = buffer_line_count(b);

    /* ── Build back-buffer (text rows) ───────────────────────*/
    for (int y = 0; y < text_rows; y++) {
        char *row = r->back_buffer[y];
        memset(row, ' ', r->width);
        row[r->width] = '\0';

        int logical = r->scroll_row + y;
        if ((size_t)logical < total_lines) {
            write_gutter(row, logical + 1);

            char *line = buffer_get_line(b, logical);
            if (line) {
                int text_cols = r->width - GUTTER_WIDTH;
                int llen      = (int)strlen(line);
                int avail     = llen - r->scroll_col;
                if (avail > 0) {
                    int copy = avail < text_cols ? avail : text_cols;
                    memcpy(row + GUTTER_WIDTH,
                           line + r->scroll_col, copy);
                }
                free(line);
            }
        } else {
            /* Past end-of-file: tilde placeholder */
            row[0] = '~';
        }
    }

    /* ── Diff & emit text rows ────────────────────────────────*/
    for (int y = 0; y < text_rows; y++) {
        if (memcmp(r->front_buffer[y], r->back_buffer[y], r->width) == 0)
            continue;

        /* Move to row, reset attrs, emit gutter with dim colour,
           then emit the text portion in default colour.          */
        stream_printf(r, "\x1b[%d;1H", y + 1);

        int is_cursor_row = ((r->scroll_row + y) == cy);

        /* Gutter */
        if (is_cursor_row)
            stream_str(r, ESC_GUTTER_ACT);
        else
            stream_str(r, ESC_GUTTER_ON);

        char *row = r->back_buffer[y];
        stream_append(r, row, GUTTER_WIDTH);

        /* Text */
        stream_str(r, ESC_RESET);
        stream_append(r, row + GUTTER_WIDTH, r->width - GUTTER_WIDTH);

        memcpy(r->front_buffer[y], row, r->width);
    }

    /* ── Status bar (always repaint) ─────────────────────────*/
    {
        stream_printf(r, "\x1b[%d;1H", r->height);
        stream_str(r, ESC_REVERSE ESC_BOLD);

        /* Left side: editor name + status message */
        char left[256];
        int  ll = snprintf(left, sizeof(left), "  FORGE  │  %s",
                           r->status_msg[0] ? r->status_msg : "ready");
        stream_append(r, left, ll < (int)sizeof(left) - 1 ? ll : (int)sizeof(left) - 1);

        /* Right side: position indicator */
        char right[64];
        int  rl = snprintf(right, sizeof(right), " Ln %d, Col %d  ",
                           cy + 1, cx + 1);

        /* Fill gap between left and right */
        int pad = r->width - ll - rl;
        for (int i = 0; i < pad; i++) stream_append(r, " ", 1);
        stream_append(r, right, rl < (int)sizeof(right) - 1 ? rl : (int)sizeof(right) - 1);

        stream_str(r, ESC_RESET);
    }

    /* ── Place cursor ─────────────────────────────────────────*/
    int screen_row = cy - r->scroll_row + 1;
    int screen_col = cx - r->scroll_col + GUTTER_WIDTH + 1;
    stream_printf(r, "\x1b[%d;%dH", screen_row, screen_col);
    stream_str(r, ESC_SHOW_CURSOR);

    if (r->out_len > 0)
        (void)write(STDOUT_FILENO, r->output_stream, r->out_len);
}
