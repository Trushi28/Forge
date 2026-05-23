#include "completion.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ══════════════════════════════════════════════════════════════
   Initialization
   ══════════════════════════════════════════════════════════════ */

void completion_init(CompletionState *cs) {
    memset(cs, 0, sizeof(*cs));
}

void completion_show(CompletionState *cs, LSPCompletionItem *items,
                     int count, int cx, int cy) {
    cs->visible   = true;
    cs->selected  = 0;
    cs->anchor_cx = cx;
    cs->anchor_cy = cy;
    cs->filter_len = 0;
    cs->filter[0]  = '\0';

    cs->count = count < LSP_MAX_COMPLETIONS ? count : LSP_MAX_COMPLETIONS;
    memcpy(cs->items, items, cs->count * sizeof(LSPCompletionItem));

    /* Initially all items are visible */
    cs->filtered_count = cs->count;
    for (int i = 0; i < cs->count; i++)
        cs->filtered[i] = i;
}

void completion_hide(CompletionState *cs) {
    cs->visible = false;
    cs->count   = 0;
    cs->filtered_count = 0;
}

/* ══════════════════════════════════════════════════════════════
   Fuzzy matching
   ══════════════════════════════════════════════════════════════ */

/* Simple subsequence fuzzy match: every character of pattern must
   appear in text in order (case-insensitive). Returns score (higher
   is better) or -1 for no match. */
static int fuzzy_match(const char *pattern, const char *text) {
    if (!pattern[0]) return 0;

    int score = 0;
    int pi = 0;
    int plen = (int)strlen(pattern);
    int prev_match = -1;

    for (int ti = 0; text[ti] && pi < plen; ti++) {
        if (tolower((unsigned char)pattern[pi]) ==
            tolower((unsigned char)text[ti])) {
            /* Bonus for consecutive matches */
            if (prev_match == ti - 1) score += 3;
            /* Bonus for matching at word boundary */
            if (ti == 0 || text[ti - 1] == '_' || text[ti - 1] == '.')
                score += 2;
            /* Exact case match bonus */
            if (pattern[pi] == text[ti]) score += 1;
            prev_match = ti;
            pi++;
        }
    }

    return (pi == plen) ? score : -1;
}

void completion_update_filter(CompletionState *cs, const char *prefix) {
    if (!prefix) prefix = "";
    int len = (int)strlen(prefix);
    if (len >= (int)sizeof(cs->filter)) len = (int)sizeof(cs->filter) - 1;
    memcpy(cs->filter, prefix, len);
    cs->filter[len] = '\0';
    cs->filter_len = len;

    if (len == 0) {
        /* No filter — show all */
        cs->filtered_count = cs->count;
        for (int i = 0; i < cs->count; i++)
            cs->filtered[i] = i;
        return;
    }

    /* Score and sort */
    typedef struct { int idx; int score; } ScoredItem;
    ScoredItem scored[LSP_MAX_COMPLETIONS];
    int n = 0;

    for (int i = 0; i < cs->count; i++) {
        int s = fuzzy_match(prefix, cs->items[i].label);
        if (s >= 0) {
            scored[n].idx   = i;
            scored[n].score = s;
            n++;
        }
    }

    /* Simple insertion sort by score (descending) */
    for (int i = 1; i < n; i++) {
        ScoredItem tmp = scored[i];
        int j = i - 1;
        while (j >= 0 && scored[j].score < tmp.score) {
            scored[j + 1] = scored[j];
            j--;
        }
        scored[j + 1] = tmp;
    }

    cs->filtered_count = n;
    for (int i = 0; i < n; i++)
        cs->filtered[i] = scored[i].idx;

    /* Clamp selection */
    if (cs->selected >= cs->filtered_count)
        cs->selected = cs->filtered_count > 0 ? cs->filtered_count - 1 : 0;
}

/* ══════════════════════════════════════════════════════════════
   Navigation
   ══════════════════════════════════════════════════════════════ */

void completion_move_up(CompletionState *cs) {
    if (cs->selected > 0) cs->selected--;
}

void completion_move_down(CompletionState *cs) {
    if (cs->selected < cs->filtered_count - 1)
        cs->selected++;
}

const LSPCompletionItem *completion_get_selected(CompletionState *cs) {
    if (!cs->visible || cs->filtered_count == 0) return NULL;
    int idx = cs->filtered[cs->selected];
    return &cs->items[idx];
}

/* ══════════════════════════════════════════════════════════════
   Kind icons
   ══════════════════════════════════════════════════════════════ */

const char *completion_kind_icon(int kind) {
    switch (kind) {
        case LSP_KIND_METHOD:
        case LSP_KIND_FUNCTION:    return "ƒ";
        case LSP_KIND_CONSTRUCTOR: return "⊕";
        case LSP_KIND_FIELD:
        case LSP_KIND_PROPERTY:
        case LSP_KIND_VARIABLE:    return "ν";
        case LSP_KIND_CLASS:
        case LSP_KIND_STRUCT:
        case LSP_KIND_INTERFACE:   return "τ";
        case LSP_KIND_MODULE:      return "▦";
        case LSP_KIND_KEYWORD:     return "κ";
        case LSP_KIND_SNIPPET:     return "Σ";
        case LSP_KIND_ENUM:        return "∈";
        case LSP_KIND_CONSTANT:    return "π";
        case LSP_KIND_TYPE_PARAM:  return "T";
        default:                   return "·";
    }
}

/* ══════════════════════════════════════════════════════════════
   Render — draws overlay popup directly to terminal
   ══════════════════════════════════════════════════════════════ */

void completion_render(CompletionState *cs, ForgeTheme *theme,
                       int screen_row, int screen_col,
                       int term_width, int term_height) {
    if (!cs->visible || cs->filtered_count == 0) return;

    /* Popup dimensions */
    int popup_width  = 40;
    int popup_height = cs->filtered_count < COMPLETION_MAX_VISIBLE
                         ? cs->filtered_count : COMPLETION_MAX_VISIBLE;

    /* Position: prefer below cursor, shift up if near bottom */
    int popup_row = screen_row + 1;
    if (popup_row + popup_height > term_height) {
        popup_row = screen_row - popup_height;
        if (popup_row < 1) popup_row = 1;
    }

    int popup_col = screen_col;
    if (popup_col + popup_width > term_width)
        popup_col = term_width - popup_width;
    if (popup_col < 1) popup_col = 1;

    /* Scroll window */
    int scroll_start = 0;
    if (cs->selected >= scroll_start + popup_height)
        scroll_start = cs->selected - popup_height + 1;
    if (cs->selected < scroll_start)
        scroll_start = cs->selected;

    char buf[8192];
    int blen = 0;

    #define BPRINTF(...) blen += snprintf(buf + blen, sizeof(buf) - blen, __VA_ARGS__)

    /* Save cursor position */
    BPRINTF("\x1b[s");

    for (int i = 0; i < popup_height; i++) {
        int fi = scroll_start + i;
        if (fi >= cs->filtered_count) break;

        int idx = cs->filtered[fi];
        LSPCompletionItem *item = &cs->items[idx];
        bool selected = (fi == cs->selected);

        /* Move to row */
        BPRINTF("\x1b[%d;%dH", popup_row + i, popup_col);

        if (theme) {
            if (selected) {
                /* Selected: accent bg */
                BPRINTF("\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um",
                        theme->accent.r, theme->accent.g, theme->accent.b,
                        theme->bg.r, theme->bg.g, theme->bg.b);
                BPRINTF("\x1b[1m");
            } else {
                /* Normal: surface bg */
                BPRINTF("\x1b[48;2;%u;%u;%um\x1b[38;2;%u;%u;%um",
                        theme->statusbar_bg.r, theme->statusbar_bg.g, theme->statusbar_bg.b,
                        theme->fg.r, theme->fg.g, theme->fg.b);
            }
        } else {
            BPRINTF(selected ? "\x1b[7m" : "\x1b[48;5;236m");
        }

        /* Kind icon */
        const char *icon = completion_kind_icon(item->kind);
        BPRINTF(" %s ", icon);

        /* Label (left-aligned, truncated) */
        int label_width = popup_width - 5;  /* icon + padding */
        int ll = (int)strlen(item->label);
        int show = ll < label_width ? ll : label_width;
        for (int j = 0; j < show; j++)
            BPRINTF("%c", item->label[j]);
        for (int j = show; j < label_width; j++)
            BPRINTF(" ");

        BPRINTF("\x1b[0m");
    }

    /* Restore cursor */
    BPRINTF("\x1b[u");

    #undef BPRINTF

    (void)write(STDOUT_FILENO, buf, blen);
}
