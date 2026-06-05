/*
 * autopairs.c — Auto-close brackets, quotes, and parentheses
 *
 * When a user types an opening bracket/quote, automatically insert
 * the matching closing character and position the cursor between
 * the pair.
 *
 * Build: gcc -shared -fPIC -o autopairs.so autopairs.c
 */

#include "../core/forge_api.h"
#include <string.h>

static const char pairs[][2] = {
    { '(', ')' },
    { '[', ']' },
    { '{', '}' },
    { '"', '"' },
    { '\'', '\'' },
    { '`', '`' },
};

static const int pair_count = sizeof(pairs) / sizeof(pairs[0]);

static char closing_for(char c) {
    for (int i = 0; i < pair_count; i++) {
        if (pairs[i][0] == c) return pairs[i][1];
    }
    return 0;
}

static void handle_keypress(int key, ForgeBuffer *buf) {
    (void)buf;

    if (key >= 32 && key <= 126) {
        char c = (char)key;
        char closer = closing_for(c);
        if (closer) {
            char close_str[2] = { closer, '\0' };
            forge_insert(close_str);
            /* Move cursor back so it sits between the pair:
               typing '(' inserts ')' then moves left → cursor is at (|) */
            forge_cursor_move(-1, 0);
        }
    }
}

FORGE_PLUGIN_INIT {
    forge_on_keypress(handle_keypress);
}
