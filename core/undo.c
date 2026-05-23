#include "undo.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ══════════════════════════════════════════════════════════════
   Lifecycle
   ══════════════════════════════════════════════════════════════ */

void undo_init(UndoStack *u) {
    memset(u, 0, sizeof(*u));
}

void undo_free(UndoStack *u) {
    for (int i = 0; i < u->count; i++)
        free(u->entries[i].text);
    memset(u, 0, sizeof(*u));
}

/* ══════════════════════════════════════════════════════════════
   Internal helpers
   ══════════════════════════════════════════════════════════════ */

/* Free entries from index `from` to `to` (exclusive) */
static void free_range(UndoStack *u, int from, int to) {
    for (int i = from; i < to; i++)
        free(u->entries[i].text);
}

/* ══════════════════════════════════════════════════════════════
   Record
   ══════════════════════════════════════════════════════════════ */

void undo_record(UndoStack *u, UndoType type, size_t pos,
                 const char *text, size_t len, int cx, int cy) {
    /* Discard redo history (anything after current) */
    if (u->current < u->count)
        free_range(u, u->current, u->count);

    /* If stack is full, shift everything down by dropping oldest entry */
    if (u->current >= UNDO_MAX_ENTRIES) {
        free(u->entries[0].text);
        memmove(&u->entries[0], &u->entries[1],
                (UNDO_MAX_ENTRIES - 1) * sizeof(UndoEntry));
        u->current--;
    }

    UndoEntry *e = &u->entries[u->current];
    e->type = type;
    e->pos  = pos;
    e->len  = len;
    e->cx   = cx;
    e->cy   = cy;
    e->text = malloc(len + 1);
    memcpy(e->text, text, len);
    e->text[len] = '\0';

    u->current++;
    u->count = u->current;
}

/* ══════════════════════════════════════════════════════════════
   Merge — combine consecutive single-char inserts or deletes

   This makes Ctrl+Z undo a whole word at a time instead of
   character-by-character.  Merges happen when:
   1. Same operation type (both INSERT or both DELETE)
   2. Single character
   3. Positions are adjacent (for inserts: new pos == prev pos + prev len;
      for deletes: new pos == prev pos or prev pos - 1)
   4. Not separated by whitespace (typing "hello world" is two undo groups)
   ══════════════════════════════════════════════════════════════ */

bool undo_try_merge(UndoStack *u, UndoType type, size_t pos,
                    const char *text, size_t len, int cx, int cy) {
    if (u->current == 0 || len != 1) return false;

    UndoEntry *prev = &u->entries[u->current - 1];
    if (prev->type != type) return false;

    /* Don't merge across whitespace boundaries */
    char new_char = text[0];
    char prev_last = prev->text[prev->len - 1];

    /* Newlines always break the group */
    if (new_char == '\n' || prev_last == '\n') return false;

    /* Whitespace-to-non-whitespace or vice versa breaks the group */
    bool new_ws  = (new_char == ' ' || new_char == '\t');
    bool prev_ws = (prev_last == ' ' || prev_last == '\t');
    if (new_ws != prev_ws) return false;

    if (type == UNDO_INSERT) {
        /* Must be adjacent: typing sequentially */
        if (pos != prev->pos + prev->len) return false;

        /* Extend the previous entry */
        prev->text = realloc(prev->text, prev->len + 2);
        prev->text[prev->len] = new_char;
        prev->len++;
        prev->text[prev->len] = '\0';

        /* Discard redo history */
        if (u->current < u->count)
            free_range(u, u->current, u->count);
        u->count = u->current;
        return true;
    }

    if (type == UNDO_DELETE) {
        if (pos == prev->pos - 1) {
            /* Backspace: prepend character */
            prev->text = realloc(prev->text, prev->len + 2);
            memmove(prev->text + 1, prev->text, prev->len + 1);
            prev->text[0] = new_char;
            prev->len++;
            prev->pos = pos;
            prev->cx  = cx;
            prev->cy  = cy;

            if (u->current < u->count)
                free_range(u, u->current, u->count);
            u->count = u->current;
            return true;
        }
        if (pos == prev->pos) {
            /* Forward delete: append character */
            prev->text = realloc(prev->text, prev->len + 2);
            prev->text[prev->len] = new_char;
            prev->len++;
            prev->text[prev->len] = '\0';

            if (u->current < u->count)
                free_range(u, u->current, u->count);
            u->count = u->current;
            return true;
        }
    }

    return false;
}

/* ══════════════════════════════════════════════════════════════
   Undo / Redo
   ══════════════════════════════════════════════════════════════ */

UndoEntry *undo_pop(UndoStack *u) {
    if (u->current <= 0) return NULL;
    u->current--;
    return &u->entries[u->current];
}

UndoEntry *undo_redo(UndoStack *u) {
    if (u->current >= u->count) return NULL;
    UndoEntry *e = &u->entries[u->current];
    u->current++;
    return e;
}

bool undo_can_undo(UndoStack *u) { return u->current > 0; }
bool undo_can_redo(UndoStack *u) { return u->current < u->count; }
