#ifndef FORGE_RENDER_H
#define FORGE_RENDER_H

#include "buffer.h"
#include "git.h"
#include "ui.h"
#include "theme.h"
#include "config.h"
#include <stdbool.h>
#include <stdarg.h>

#define GUTTER_WIDTH 5   /* 4 digits + 1 separator space */
#define TIMELINE_HEIGHT 3 /* rows for the git timeline scrubber */
#define BLAME_COL_WIDTH 28 /* width of inline blame annotations */

/* ── Diagnostic markers for gutter ──────────────────────────── */
#define DIAG_NONE    0
#define DIAG_ERROR   1
#define DIAG_WARNING 2
#define DIAG_INFO    3
#define DIAG_HINT    4

#define MAX_DIAGNOSTICS 4096

typedef struct {
    int line;          /* 0-indexed */
    int severity;      /* DIAG_ERROR, DIAG_WARNING, etc. */
    char message[256];
} Diagnostic;

typedef struct RenderState {
    int   width, height;
    int   scroll_row;
    int   scroll_col;
    bool  full_redraw;       /* force full repaint on next frame  */
    char **front_buffer;     /* what is currently on screen       */
    char **back_buffer;      /* what we want to draw this frame   */
    char  *output_stream;
    int    out_len, out_cap;
    char   status_msg[256];

    /* Theme */
    ForgeTheme *theme;

    /* Diagnostics from LSP */
    Diagnostic diagnostics[MAX_DIAGNOSTICS];
    int        diag_count;

    /* Cursor positions for UI widgets */
    int        cx, cy;

    /* Git state pointer (set by main, read by widgets) */
    GitState  *git;

    /* Config and LSP Client pointers */
    ForgeConfig *cfg;
    struct LSPClient *lsp;
} RenderState;

void ui_register_builtins(UIRegistry *ui);

void render_init      (RenderState *r, int width, int height);
void render_free      (RenderState *r);
void render_resize    (RenderState *r, int width, int height);
void render_set_status(RenderState *r, const char *fmt, ...);
void render_set_theme (RenderState *r, ForgeTheme *theme);

void render_frame     (RenderState *r, Buffer *b, UIRegistry *ui, int cx, int cy);

/* Diagnostic management */
void render_clear_diagnostics(RenderState *r);
void render_add_diagnostic(RenderState *r, int line, int severity,
                           const char *msg);
int  render_get_diag_severity(RenderState *r, int line);

#endif
