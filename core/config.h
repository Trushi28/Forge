#ifndef FORGE_CONFIG_H
#define FORGE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define FORGE_MAX_PATH 512

/* ── User-configurable settings ─────────────────────────────── */

typedef struct {
    /* [editor] */
    int   tab_width;
    bool  use_spaces;
    bool  show_line_numbers;
    bool  mouse_enabled;
    int   scrolloff;
    char  theme_name[64];

    /* [keybinds] — stored as name strings, resolved at runtime */

    /* [lsp] */
    bool  lsp_auto_detect;
    bool  lsp_show_hints;
    bool  lsp_completion;
    bool  lsp_diagnostics;

    /* [git] */
    bool  git_show_gutter;
    bool  git_show_blame;
    bool  git_timeline;

    /* [guild] */
    char  guild_handle[64];
    char  guild_name[128];
    char  guild_color[32];
    bool  guild_announce_lan;

    /* [hooks] */
    char  hook_on_save[256];
    char  hook_on_open[256];
    char  hook_on_close[256];

    /* [lang.c] etc — per-language overrides */
    int   lang_c_indent;
    char  lang_c_formatter[256];
    char  lang_c_lsp[64];

    int   lang_python_indent;
    char  lang_python_formatter[256];
    char  lang_python_lsp[64];

    int   lang_rust_indent;
    char  lang_rust_formatter[256];
    char  lang_rust_lsp[64];

    /* Internal */
    char  filepath[FORGE_MAX_PATH];
    time_t last_mtime;
} ForgeConfig;

/* Load config from path; returns 0 on success, -1 if file missing */
int  config_load(ForgeConfig *cfg, const char *path);

/* Fill with sensible defaults */
void config_default(ForgeConfig *cfg);

/* Check if file changed and reload; returns true if reloaded */
bool config_poll_reload(ForgeConfig *cfg);

/* Get the default config path (~/.config/forge/config.toml) */
const char *config_default_path(void);

#endif
