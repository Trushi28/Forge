#ifndef FORGE_PALETTE_H
#define FORGE_PALETTE_H

#include "theme.h"
#include <stdbool.h>

#define PALETTE_MAX_COMMANDS  64
#define PALETTE_MAX_FILES     512
#define PALETTE_MAX_INPUT     256
#define PALETTE_MAX_VISIBLE   12

/* ── Command callback ───────────────────────────────────────── */

typedef void (*PaletteAction)(void *ctx);

typedef struct {
    char           name[128];
    char           description[128];
    char           shortcut[32];
    PaletteAction  action;
    void          *ctx;
} PaletteCommand;

/* ── File entry (for fuzzy finder) ──────────────────────────── */

typedef struct {
    char path[512];       /* relative path */
    char basename[128];   /* filename only */
} PaletteFile;

/* ── Palette mode ───────────────────────────────────────────── */

typedef enum {
    PALETTE_COMMANDS,     /* default: search commands */
    PALETTE_FILES,        /* file finder mode (> prefix) */
    PALETTE_GOTO_LINE     /* go to line (: prefix) */
} PaletteMode;

/* ── Palette state ──────────────────────────────────────────── */

typedef struct {
    bool          visible;
    PaletteMode   mode;

    /* Input */
    char          input[PALETTE_MAX_INPUT];
    int           input_len;
    int           cursor;

    /* Commands */
    PaletteCommand commands[PALETTE_MAX_COMMANDS];
    int            command_count;

    /* Files (lazily populated) */
    PaletteFile   files[PALETTE_MAX_FILES];
    int           file_count;
    bool          files_loaded;

    /* Filtered results (indices) */
    int           filtered[PALETTE_MAX_FILES]; /* max of files or commands */
    int           filtered_count;
    int           selected;

    /* Result (set when user accepts) */
    bool          accepted;
    int           result_index;    /* index into filtered[] */
    int           goto_line;       /* for GOTO_LINE mode */
    char          result_path[512]; /* for FILES mode */
} PaletteState;

/* ── API ────────────────────────────────────────────────────── */

void palette_init(PaletteState *ps);

/* Register a command */
void palette_add_command(PaletteState *ps, const char *name,
                         const char *desc, const char *shortcut,
                         PaletteAction action, void *ctx);

/* Open/close */
void palette_show(PaletteState *ps);
void palette_hide(PaletteState *ps);

/* Handle keypress; returns true if consumed */
bool palette_handle_key(PaletteState *ps, int key);

/* Render overlay */
void palette_render(PaletteState *ps, ForgeTheme *theme,
                    int term_width, int term_height);

/* Scan directory for files (for fuzzy finder) */
void palette_scan_files(PaletteState *ps, const char *root_dir);

#endif
