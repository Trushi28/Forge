#include "config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

/* ══════════════════════════════════════════════════════════════
   Defaults
   ══════════════════════════════════════════════════════════════ */

void config_default(ForgeConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    /* [editor] */
    cfg->tab_width        = 4;
    cfg->use_spaces        = true;
    cfg->show_line_numbers = true;
    cfg->mouse_enabled     = true;
    cfg->scrolloff         = 8;
    strncpy(cfg->theme_name, "catppuccin", sizeof(cfg->theme_name) - 1);

    /* [lsp] */
    cfg->lsp_auto_detect  = true;
    cfg->lsp_show_hints   = true;
    cfg->lsp_completion   = true;
    cfg->lsp_diagnostics  = true;

    /* [git] */
    cfg->git_show_gutter  = true;
    cfg->git_show_blame   = false;
    cfg->git_timeline     = true;

    /* [guild] */
    strncpy(cfg->guild_handle, "anon", sizeof(cfg->guild_handle) - 1);
    strncpy(cfg->guild_name,   "",     sizeof(cfg->guild_name) - 1);
    strncpy(cfg->guild_color,  "cyan", sizeof(cfg->guild_color) - 1);
    cfg->guild_announce_lan = true;

    /* [lang.c] */
    cfg->lang_c_indent = 4;
    strncpy(cfg->lang_c_formatter, "clang-format -i $FILE",
            sizeof(cfg->lang_c_formatter) - 1);
    strncpy(cfg->lang_c_lsp, "clangd", sizeof(cfg->lang_c_lsp) - 1);

    /* [lang.python] */
    cfg->lang_python_indent = 4;
    strncpy(cfg->lang_python_formatter, "black $FILE",
            sizeof(cfg->lang_python_formatter) - 1);
    strncpy(cfg->lang_python_lsp, "pyright", sizeof(cfg->lang_python_lsp) - 1);

    /* [lang.rust] */
    cfg->lang_rust_indent = 4;
    strncpy(cfg->lang_rust_formatter, "rustfmt $FILE",
            sizeof(cfg->lang_rust_formatter) - 1);
    strncpy(cfg->lang_rust_lsp, "rust-analyzer",
            sizeof(cfg->lang_rust_lsp) - 1);

    /* Slot-based UI defaults */
    cfg->gutter_widget_count = 3;
    strcpy(cfg->gutter_widgets[0], "line_numbers");
    strcpy(cfg->gutter_widgets[1], "git_diff_gutter");
    strcpy(cfg->gutter_widgets[2], "diagnostics_gutter");

    cfg->statusbar_widget_count = 5;
    strcpy(cfg->statusbar_widgets[0], "mode_indicator");
    strcpy(cfg->statusbar_widgets[1], "filename");
    strcpy(cfg->statusbar_widgets[2], "git_branch");
    strcpy(cfg->statusbar_widgets[3], "lsp_status");
    strcpy(cfg->statusbar_widgets[4], "cursor_pos");

    cfg->bottombar_widget_count = 1;
    strcpy(cfg->bottombar_widgets[0], "git_timeline");

    cfg->topbar_widget_count = 0;
    cfg->topbar_visible = false;

    cfg->right_panel_widget_count = 1;
    strcpy(cfg->right_panel_widgets[0], "guild_panel");
    cfg->right_panel_visible = false;
    cfg->right_panel_width = 35;
}

/* ══════════════════════════════════════════════════════════════
   TOML subset parser

   Supports:
     [section]
     [section.subsection]
     key = "string"
     key = 123
     key = true / false
     # comments
   ══════════════════════════════════════════════════════════════ */

static void trim(char *s) {
    /* Trim trailing whitespace/newline */
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' '  || s[len - 1] == '\t'))
        s[--len] = '\0';

    /* Trim leading whitespace */
    char *start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
}

static void strip_quotes(char *val) {
    size_t len = strlen(val);
    if (len >= 2 && val[0] == '"' && val[len - 1] == '"') {
        memmove(val, val + 1, len - 2);
        val[len - 2] = '\0';
    }
}

static void set_string(char *dst, size_t dstsz, const char *src) {
    size_t slen = strlen(src);
    size_t copy = slen < dstsz - 1 ? slen : dstsz - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
}

static bool parse_bool(const char *val) {
    return (strcmp(val, "true") == 0 || strcmp(val, "1") == 0 ||
            strcmp(val, "yes") == 0);
}

static int parse_string_array(const char *val, char out[][64], int max_sz) {
    int count = 0;
    const char *p = val;
    while (*p && count < max_sz) {
        while (*p && *p != '"' && *p != '\'') p++;
        if (!*p) break;
        char quote = *p;
        p++;
        int len = 0;
        while (*p && *p != quote && len < 63) {
            out[count][len++] = *p++;
        }
        out[count][len] = '\0';
        count++;
        if (*p == quote) p++;
    }
    return count;
}

static void apply_setting(ForgeConfig *cfg, const char *section,
                           const char *key, char *val) {
    strip_quotes(val);
    trim(val);

    /* [editor] */
    if (strcmp(section, "editor") == 0) {
        if (strcmp(key, "tab_width") == 0)
            cfg->tab_width = atoi(val);
        else if (strcmp(key, "use_spaces") == 0)
            cfg->use_spaces = parse_bool(val);
        else if (strcmp(key, "line_numbers") == 0)
            cfg->show_line_numbers = parse_bool(val);
        else if (strcmp(key, "mouse") == 0)
            cfg->mouse_enabled = parse_bool(val);
        else if (strcmp(key, "scrolloff") == 0)
            cfg->scrolloff = atoi(val);
        else if (strcmp(key, "theme") == 0)
            set_string(cfg->theme_name, sizeof(cfg->theme_name), val);
    }
    /* [lsp] */
    else if (strcmp(section, "lsp") == 0) {
        if (strcmp(key, "auto_detect") == 0)
            cfg->lsp_auto_detect = parse_bool(val);
        else if (strcmp(key, "show_hints") == 0)
            cfg->lsp_show_hints = parse_bool(val);
        else if (strcmp(key, "completion") == 0)
            cfg->lsp_completion = parse_bool(val);
        else if (strcmp(key, "diagnostics") == 0)
            cfg->lsp_diagnostics = parse_bool(val);
    }
    /* [git] */
    else if (strcmp(section, "git") == 0) {
        if (strcmp(key, "show_gutter") == 0)
            cfg->git_show_gutter = parse_bool(val);
        else if (strcmp(key, "show_blame") == 0)
            cfg->git_show_blame = parse_bool(val);
        else if (strcmp(key, "timeline") == 0)
            cfg->git_timeline = parse_bool(val);
    }
    /* [guild] */
    else if (strcmp(section, "guild") == 0) {
        if (strcmp(key, "handle") == 0)
            set_string(cfg->guild_handle, sizeof(cfg->guild_handle), val);
        else if (strcmp(key, "name") == 0)
            set_string(cfg->guild_name, sizeof(cfg->guild_name), val);
        else if (strcmp(key, "color") == 0)
            set_string(cfg->guild_color, sizeof(cfg->guild_color), val);
        else if (strcmp(key, "announce_lan") == 0)
            cfg->guild_announce_lan = parse_bool(val);
    }
    /* [hooks] */
    else if (strcmp(section, "hooks") == 0) {
        if (strcmp(key, "on_save") == 0)
            set_string(cfg->hook_on_save, sizeof(cfg->hook_on_save), val);
        else if (strcmp(key, "on_open") == 0)
            set_string(cfg->hook_on_open, sizeof(cfg->hook_on_open), val);
        else if (strcmp(key, "on_close") == 0)
            set_string(cfg->hook_on_close, sizeof(cfg->hook_on_close), val);
    }
    /* [lang.c] */
    else if (strcmp(section, "lang.c") == 0) {
        if (strcmp(key, "indent") == 0)
            cfg->lang_c_indent = atoi(val);
        else if (strcmp(key, "formatter") == 0)
            set_string(cfg->lang_c_formatter, sizeof(cfg->lang_c_formatter), val);
        else if (strcmp(key, "lsp") == 0)
            set_string(cfg->lang_c_lsp, sizeof(cfg->lang_c_lsp), val);
    }
    /* [lang.python] */
    else if (strcmp(section, "lang.python") == 0) {
        if (strcmp(key, "indent") == 0)
            cfg->lang_python_indent = atoi(val);
        else if (strcmp(key, "formatter") == 0)
            set_string(cfg->lang_python_formatter,
                       sizeof(cfg->lang_python_formatter), val);
        else if (strcmp(key, "lsp") == 0)
            set_string(cfg->lang_python_lsp, sizeof(cfg->lang_python_lsp), val);
    }
    /* [lang.rust] */
    else if (strcmp(section, "lang.rust") == 0) {
        if (strcmp(key, "indent") == 0)
            cfg->lang_rust_indent = atoi(val);
        else if (strcmp(key, "formatter") == 0)
            set_string(cfg->lang_rust_formatter,
                       sizeof(cfg->lang_rust_formatter), val);
        else if (strcmp(key, "lsp") == 0)
            set_string(cfg->lang_rust_lsp, sizeof(cfg->lang_rust_lsp), val);
    }
    /* [ui.slots.topbar] */
    else if (strcmp(section, "ui.slots.topbar") == 0) {
        if (strcmp(key, "widgets") == 0)
            cfg->topbar_widget_count = parse_string_array(val, cfg->topbar_widgets, 8);
        else if (strcmp(key, "visible") == 0)
            cfg->topbar_visible = parse_bool(val);
    }
    /* [ui.slots.gutter] */
    else if (strcmp(section, "ui.slots.gutter") == 0) {
        if (strcmp(key, "widgets") == 0)
            cfg->gutter_widget_count = parse_string_array(val, cfg->gutter_widgets, 8);
    }
    /* [ui.slots.statusbar] */
    else if (strcmp(section, "ui.slots.statusbar") == 0) {
        if (strcmp(key, "widgets") == 0)
            cfg->statusbar_widget_count = parse_string_array(val, cfg->statusbar_widgets, 8);
    }
    /* [ui.slots.bottombar] */
    else if (strcmp(section, "ui.slots.bottombar") == 0) {
        if (strcmp(key, "widgets") == 0)
            cfg->bottombar_widget_count = parse_string_array(val, cfg->bottombar_widgets, 8);
        else if (strcmp(key, "visible") == 0)
            cfg->git_timeline = parse_bool(val);
    }
    /* [ui.slots.right_panel] */
    else if (strcmp(section, "ui.slots.right_panel") == 0) {
        if (strcmp(key, "widgets") == 0)
            cfg->right_panel_widget_count = parse_string_array(val, cfg->right_panel_widgets, 8);
        else if (strcmp(key, "visible") == 0)
            cfg->right_panel_visible = parse_bool(val);
        else if (strcmp(key, "width") == 0)
            cfg->right_panel_width = atoi(val);
    }
}

/* ══════════════════════════════════════════════════════════════
   Load
   ══════════════════════════════════════════════════════════════ */

int config_load(ForgeConfig *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    /* Record path and mtime for live-reload polling */
    set_string(cfg->filepath, sizeof(cfg->filepath), path);
    struct stat st;
    if (stat(path, &st) == 0)
        cfg->last_mtime = st.st_mtime;

    char section[128] = "";
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        trim(line);

        /* Skip empty lines and comments */
        if (line[0] == '\0' || line[0] == '#') continue;

        /* Section header: [section] or [section.subsection] */
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                set_string(section, sizeof(section), line + 1);
            }
            continue;
        }

        /* Key = value */
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (key[0] && val[0])
            apply_setting(cfg, section, key, val);
    }

    fclose(f);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   Live-reload polling
   ══════════════════════════════════════════════════════════════ */

bool config_poll_reload(ForgeConfig *cfg) {
    if (cfg->filepath[0] == '\0') return false;

    struct stat st;
    if (stat(cfg->filepath, &st) != 0) return false;
    if (st.st_mtime == cfg->last_mtime) return false;

    /* File changed — reload.
       Save filepath/mtime first: config_default() zeros the struct. */
    char saved_path[FORGE_MAX_PATH];
    memcpy(saved_path, cfg->filepath, sizeof(saved_path));
    time_t saved_mtime = st.st_mtime;

    config_default(cfg);
    config_load(cfg, saved_path);
    cfg->last_mtime = saved_mtime;
    return true;
}

/* ══════════════════════════════════════════════════════════════
   Default path
   ══════════════════════════════════════════════════════════════ */

const char *config_default_path(void) {
    static char path[FORGE_MAX_PATH];
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    snprintf(path, sizeof(path), "%s/.config/forge/config.toml", home);
    return path;
}
