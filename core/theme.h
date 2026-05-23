#ifndef FORGE_THEME_H
#define FORGE_THEME_H

#include <stdint.h>
#include <stdbool.h>

/* ── RGB color ──────────────────────────────────────────────── */

typedef struct {
    uint8_t r, g, b;
} ThemeColor;

/* ── Full theme ─────────────────────────────────────────────── */

typedef struct {
    char name[64];

    /* UI chrome */
    ThemeColor bg;               /* editor background           */
    ThemeColor fg;               /* default foreground text      */
    ThemeColor accent;           /* highlights, active elements  */
    ThemeColor gutter_fg;        /* line number color             */
    ThemeColor gutter_active;    /* active line number            */
    ThemeColor gutter_bg;        /* gutter background             */
    ThemeColor statusbar_bg;     /* status bar background         */
    ThemeColor statusbar_fg;     /* status bar text               */
    ThemeColor statusbar_accent; /* status bar accent (mode, etc) */
    ThemeColor line_highlight;   /* current line background tint  */
    ThemeColor selection;        /* visual selection              */

    /* Diagnostics */
    ThemeColor error;
    ThemeColor warning;
    ThemeColor info;
    ThemeColor hint;

    /* Syntax */
    ThemeColor keyword;          /* if, for, while, return        */
    ThemeColor type_color;       /* int, char, void, struct       */
    ThemeColor function_color;   /* function names                */
    ThemeColor string_color;     /* "strings"                     */
    ThemeColor number_color;     /* numeric literals              */
    ThemeColor comment;          /* // comments                   */
    ThemeColor operator_color;   /* + - * / = etc                 */
    ThemeColor preprocessor;     /* #include, #define             */
    ThemeColor constant;         /* TRUE, FALSE, NULL             */
} ForgeTheme;

/* ── API ────────────────────────────────────────────────────── */

/* Load a named built-in theme. Returns false if name unknown. */
bool theme_load(ForgeTheme *t, const char *name);

/* Get the list of available theme names (NULL-terminated array) */
const char **theme_list(void);

/* Emit ANSI truecolor foreground escape: \x1b[38;2;R;G;Bm
   Writes into buf (must be >= 24 bytes). Returns buf. */
char *theme_fg_str(char *buf, ThemeColor c);

/* Emit ANSI truecolor background escape: \x1b[48;2;R;G;Bm */
char *theme_bg_str(char *buf, ThemeColor c);

/* Emit combined fg+bg escape */
char *theme_color_str(char *buf, ThemeColor fg, ThemeColor bg);

/* Convenience: ThemeColor from hex like 0x1e1e2e */
ThemeColor theme_hex(uint32_t hex);

#endif
