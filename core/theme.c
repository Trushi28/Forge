#include "theme.h"
#include <stdio.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════
   Helpers
   ══════════════════════════════════════════════════════════════ */

ThemeColor theme_hex(uint32_t hex) {
    ThemeColor c;
    c.r = (uint8_t)((hex >> 16) & 0xFF);
    c.g = (uint8_t)((hex >>  8) & 0xFF);
    c.b = (uint8_t)( hex        & 0xFF);
    return c;
}

char *theme_fg_str(char *buf, ThemeColor c) {
    sprintf(buf, "\x1b[38;2;%u;%u;%um", c.r, c.g, c.b);
    return buf;
}

char *theme_bg_str(char *buf, ThemeColor c) {
    sprintf(buf, "\x1b[48;2;%u;%u;%um", c.r, c.g, c.b);
    return buf;
}

char *theme_color_str(char *buf, ThemeColor fg, ThemeColor bg) {
    sprintf(buf, "\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um",
            fg.r, fg.g, fg.b, bg.r, bg.g, bg.b);
    return buf;
}

/* ══════════════════════════════════════════════════════════════
   Built-in Themes
   ══════════════════════════════════════════════════════════════ */

/* ── Catppuccin Mocha ────────────────────────────────────────
   Popular dark theme with rich, warm tones.
   Palette from https://catppuccin.com/palette                 */

static void theme_catppuccin(ForgeTheme *t) {
    strncpy(t->name, "catppuccin", sizeof(t->name) - 1);

    /* Base colors */
    t->bg               = theme_hex(0x1e1e2e);  /* Base      */
    t->fg               = theme_hex(0xcdd6f4);  /* Text      */
    t->accent           = theme_hex(0x89b4fa);  /* Blue      */

    /* Gutter */
    t->gutter_fg        = theme_hex(0x585b70);  /* Overlay0   */
    t->gutter_active    = theme_hex(0xf9e2af);  /* Yellow     */
    t->gutter_bg        = theme_hex(0x181825);  /* Mantle     */

    /* Status bar */
    t->statusbar_bg     = theme_hex(0x313244);  /* Surface0   */
    t->statusbar_fg     = theme_hex(0xcdd6f4);  /* Text       */
    t->statusbar_accent = theme_hex(0xcba6f7);  /* Mauve      */

    /* Current line */
    t->line_highlight   = theme_hex(0x262637);  /* slightly lighter than base */
    t->selection        = theme_hex(0x45475a);  /* Surface1   */

    /* Diagnostics */
    t->error            = theme_hex(0xf38ba8);  /* Red        */
    t->warning          = theme_hex(0xfab387);  /* Peach      */
    t->info             = theme_hex(0x89b4fa);  /* Blue       */
    t->hint             = theme_hex(0xa6e3a1);  /* Green      */

    /* Syntax */
    t->keyword          = theme_hex(0xcba6f7);  /* Mauve      */
    t->type_color       = theme_hex(0xf9e2af);  /* Yellow     */
    t->function_color   = theme_hex(0x89b4fa);  /* Blue       */
    t->string_color     = theme_hex(0xa6e3a1);  /* Green      */
    t->number_color     = theme_hex(0xfab387);  /* Peach      */
    t->comment          = theme_hex(0x6c7086);  /* Overlay0   */
    t->operator_color   = theme_hex(0x89dceb);  /* Sky        */
    t->preprocessor     = theme_hex(0xf38ba8);  /* Red        */
    t->constant         = theme_hex(0xfab387);  /* Peach      */
}

/* ── Forge Light ─────────────────────────────────────────────
   Clean, warm light theme inspired by GitHub Light            */

static void theme_forge_light(ForgeTheme *t) {
    strncpy(t->name, "light", sizeof(t->name) - 1);

    /* Base colors */
    t->bg               = theme_hex(0xfafafa);
    t->fg               = theme_hex(0x383a42);
    t->accent           = theme_hex(0x4078f2);

    /* Gutter */
    t->gutter_fg        = theme_hex(0x9da5b4);
    t->gutter_active    = theme_hex(0x383a42);
    t->gutter_bg        = theme_hex(0xf0f0f0);

    /* Status bar */
    t->statusbar_bg     = theme_hex(0x21252b);
    t->statusbar_fg     = theme_hex(0xd7dae0);
    t->statusbar_accent = theme_hex(0x61afef);

    /* Current line */
    t->line_highlight   = theme_hex(0xecedee);
    t->selection        = theme_hex(0xd7d8db);

    /* Diagnostics */
    t->error            = theme_hex(0xe45649);
    t->warning          = theme_hex(0xc18401);
    t->info             = theme_hex(0x4078f2);
    t->hint             = theme_hex(0x50a14f);

    /* Syntax */
    t->keyword          = theme_hex(0xa626a4);
    t->type_color       = theme_hex(0xc18401);
    t->function_color   = theme_hex(0x4078f2);
    t->string_color     = theme_hex(0x50a14f);
    t->number_color     = theme_hex(0x986801);
    t->comment          = theme_hex(0xa0a1a7);
    t->operator_color   = theme_hex(0x0184bc);
    t->preprocessor     = theme_hex(0xe45649);
    t->constant         = theme_hex(0x986801);
}

/* ── Gruvbox Dark ────────────────────────────────────────────
   Retro-groove warm dark theme                                */

static void theme_gruvbox(ForgeTheme *t) {
    strncpy(t->name, "gruvbox", sizeof(t->name) - 1);

    t->bg               = theme_hex(0x282828);
    t->fg               = theme_hex(0xebdbb2);
    t->accent           = theme_hex(0x83a598);

    t->gutter_fg        = theme_hex(0x665c54);
    t->gutter_active    = theme_hex(0xfabd2f);
    t->gutter_bg        = theme_hex(0x1d2021);

    t->statusbar_bg     = theme_hex(0x3c3836);
    t->statusbar_fg     = theme_hex(0xebdbb2);
    t->statusbar_accent = theme_hex(0xfe8019);

    t->line_highlight   = theme_hex(0x32302f);
    t->selection        = theme_hex(0x504945);

    t->error            = theme_hex(0xfb4934);
    t->warning          = theme_hex(0xfe8019);
    t->info             = theme_hex(0x83a598);
    t->hint             = theme_hex(0xb8bb26);

    t->keyword          = theme_hex(0xfb4934);
    t->type_color       = theme_hex(0xfabd2f);
    t->function_color   = theme_hex(0x83a598);
    t->string_color     = theme_hex(0xb8bb26);
    t->number_color     = theme_hex(0xd3869b);
    t->comment          = theme_hex(0x928374);
    t->operator_color   = theme_hex(0x8ec07c);
    t->preprocessor     = theme_hex(0xfe8019);
    t->constant         = theme_hex(0xd3869b);
}

/* ── Solarized Dark ──────────────────────────────────────────*/

static void theme_solarized(ForgeTheme *t) {
    strncpy(t->name, "solarized", sizeof(t->name) - 1);

    t->bg               = theme_hex(0x002b36);
    t->fg               = theme_hex(0x839496);
    t->accent           = theme_hex(0x268bd2);

    t->gutter_fg        = theme_hex(0x586e75);
    t->gutter_active    = theme_hex(0x93a1a1);
    t->gutter_bg        = theme_hex(0x002028);

    t->statusbar_bg     = theme_hex(0x073642);
    t->statusbar_fg     = theme_hex(0x93a1a1);
    t->statusbar_accent = theme_hex(0x2aa198);

    t->line_highlight   = theme_hex(0x003847);
    t->selection        = theme_hex(0x073642);

    t->error            = theme_hex(0xdc322f);
    t->warning          = theme_hex(0xb58900);
    t->info             = theme_hex(0x268bd2);
    t->hint             = theme_hex(0x859900);

    t->keyword          = theme_hex(0x859900);
    t->type_color       = theme_hex(0xb58900);
    t->function_color   = theme_hex(0x268bd2);
    t->string_color     = theme_hex(0x2aa198);
    t->number_color     = theme_hex(0xd33682);
    t->comment          = theme_hex(0x586e75);
    t->operator_color   = theme_hex(0x93a1a1);
    t->preprocessor     = theme_hex(0xcb4b16);
    t->constant         = theme_hex(0xd33682);
}

/* ── Tokyo Night ─────────────────────────────────────────────
   Modern VS Code-inspired dark theme                          */

static void theme_tokyo_night(ForgeTheme *t) {
    strncpy(t->name, "tokyo_night", sizeof(t->name) - 1);

    t->bg               = theme_hex(0x1a1b26);
    t->fg               = theme_hex(0xa9b1d6);
    t->accent           = theme_hex(0x7aa2f7);

    t->gutter_fg        = theme_hex(0x3b4261);
    t->gutter_active    = theme_hex(0xc0caf5);
    t->gutter_bg        = theme_hex(0x16161e);

    t->statusbar_bg     = theme_hex(0x24283b);
    t->statusbar_fg     = theme_hex(0xa9b1d6);
    t->statusbar_accent = theme_hex(0x7dcfff);

    t->line_highlight   = theme_hex(0x20212e);
    t->selection        = theme_hex(0x283457);

    t->error            = theme_hex(0xf7768e);
    t->warning          = theme_hex(0xe0af68);
    t->info             = theme_hex(0x7aa2f7);
    t->hint             = theme_hex(0x9ece6a);

    t->keyword          = theme_hex(0x9d7cd8);
    t->type_color       = theme_hex(0xe0af68);
    t->function_color   = theme_hex(0x7aa2f7);
    t->string_color     = theme_hex(0x9ece6a);
    t->number_color     = theme_hex(0xff9e64);
    t->comment          = theme_hex(0x565f89);
    t->operator_color   = theme_hex(0x89ddff);
    t->preprocessor     = theme_hex(0xf7768e);
    t->constant         = theme_hex(0xff9e64);
}

/* ── Dracula ─────────────────────────────────────────────────
   Popular dark theme with rich purples and greens.
   Palette from https://draculatheme.com/contribute             */

static void theme_dracula(ForgeTheme *t) {
    strncpy(t->name, "dracula", sizeof(t->name) - 1);

    t->bg               = theme_hex(0x282a36);  /* Background    */
    t->fg               = theme_hex(0xf8f8f2);  /* Foreground    */
    t->accent           = theme_hex(0xbd93f9);  /* Purple        */

    t->gutter_fg        = theme_hex(0x6272a4);  /* Comment       */
    t->gutter_active    = theme_hex(0xf1fa8c);  /* Yellow        */
    t->gutter_bg        = theme_hex(0x21222c);  /* Current Line  */

    t->statusbar_bg     = theme_hex(0x44475a);  /* Selection     */
    t->statusbar_fg     = theme_hex(0xf8f8f2);  /* Foreground    */
    t->statusbar_accent = theme_hex(0xff79c6);  /* Pink          */

    t->line_highlight   = theme_hex(0x2d2f3d);  /* Slightly lighter */
    t->selection        = theme_hex(0x44475a);  /* Selection     */

    t->error            = theme_hex(0xff5555);  /* Red           */
    t->warning          = theme_hex(0xffb86c);  /* Orange        */
    t->info             = theme_hex(0x8be9fd);  /* Cyan          */
    t->hint             = theme_hex(0x50fa7b);  /* Green         */

    t->keyword          = theme_hex(0xff79c6);  /* Pink          */
    t->type_color       = theme_hex(0x8be9fd);  /* Cyan          */
    t->function_color   = theme_hex(0x50fa7b);  /* Green         */
    t->string_color     = theme_hex(0xf1fa8c);  /* Yellow        */
    t->number_color     = theme_hex(0xbd93f9);  /* Purple        */
    t->comment          = theme_hex(0x6272a4);  /* Comment       */
    t->operator_color   = theme_hex(0xff79c6);  /* Pink          */
    t->preprocessor     = theme_hex(0xff5555);  /* Red           */
    t->constant         = theme_hex(0xbd93f9);  /* Purple        */
}

/* ══════════════════════════════════════════════════════════════
   Public API
   ══════════════════════════════════════════════════════════════ */

static const char *theme_names[] = {
    "catppuccin", "dracula", "tokyo_night", "gruvbox", "solarized", "light", NULL
};

const char **theme_list(void) {
    return theme_names;
}

bool theme_load(ForgeTheme *t, const char *name) {
    memset(t, 0, sizeof(*t));

    if (!name || strcmp(name, "catppuccin") == 0) {
        theme_catppuccin(t);
        return true;
    }
    if (strcmp(name, "light") == 0) {
        theme_forge_light(t);
        return true;
    }
    if (strcmp(name, "gruvbox") == 0) {
        theme_gruvbox(t);
        return true;
    }
    if (strcmp(name, "solarized") == 0) {
        theme_solarized(t);
        return true;
    }
    if (strcmp(name, "tokyo_night") == 0) {
        theme_tokyo_night(t);
        return true;
    }
    if (strcmp(name, "dracula") == 0) {
        theme_dracula(t);
        return true;
    }

    /* Unknown — fall back to catppuccin */
    theme_catppuccin(t);
    return false;
}
