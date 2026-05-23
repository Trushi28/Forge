#ifndef FORGE_COMPLETION_H
#define FORGE_COMPLETION_H

#include "lsp.h"
#include "theme.h"
#include <stdbool.h>

#define COMPLETION_MAX_VISIBLE 10

typedef struct {
    bool visible;
    int  selected;         /* currently highlighted index */

    /* Items (from LSP) */
    LSPCompletionItem items[LSP_MAX_COMPLETIONS];
    int               count;

    /* Filtered view */
    int  filtered[LSP_MAX_COMPLETIONS]; /* indices into items[] */
    int  filtered_count;

    /* Position anchor */
    int  anchor_cx, anchor_cy;  /* cursor pos when popup opened */

    /* Filter prefix (what user typed since popup opened) */
    char filter[128];
    int  filter_len;
} CompletionState;

/* Initialize completion state */
void completion_init(CompletionState *cs);

/* Show completion popup with new items */
void completion_show(CompletionState *cs, LSPCompletionItem *items,
                     int count, int cx, int cy);

/* Hide the popup */
void completion_hide(CompletionState *cs);

/* Update filter and re-filter items */
void completion_update_filter(CompletionState *cs, const char *prefix);

/* Navigate selection */
void completion_move_up(CompletionState *cs);
void completion_move_down(CompletionState *cs);

/* Get the currently selected item, or NULL */
const LSPCompletionItem *completion_get_selected(CompletionState *cs);

/* Render the popup overlay (writes directly to terminal) */
void completion_render(CompletionState *cs, ForgeTheme *theme,
                       int screen_row, int screen_col,
                       int term_width, int term_height);

/* Get kind icon character for a completion kind */
const char *completion_kind_icon(int kind);

#endif
