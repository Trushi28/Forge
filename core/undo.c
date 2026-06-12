#include "undo.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void free_node(UndoEntry *e) {
    if (!e) return;
    for (int i = 0; i < e->child_count; i++) {
        free_node(e->children[i]);
    }
    free(e->text);
    free(e);
}

static bool is_child(UndoEntry *parent, UndoEntry *child) {
    if (!parent || !child) return false;
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child) return true;
    }
    return false;
}

void undo_init(UndoStack *u) {
    if (!u) return;
    u->root = calloc(1, sizeof(UndoEntry));
    u->current = u->root;
    u->last_branch = NULL;
}

void undo_free(UndoStack *u) {
    if (!u) return;
    free_node(u->root);
    free_node(u->last_branch);
    u->root = NULL;
    u->current = NULL;
    u->last_branch = NULL;
}

void undo_record(UndoStack *u, UndoType type, size_t pos,
                 const char *text, size_t len, int cx, int cy) {
    if (!u || !u->current) return;

    /* Add child to current node */
    if (u->current->child_count >= 8) {
        UndoEntry *discarded = u->current->children[0];
        if (u->last_branch) {
            free_node(u->last_branch);
        }
        u->last_branch = discarded;
        
        for (int i = 1; i < 8; i++) {
            u->current->children[i - 1] = u->current->children[i];
        }
        u->current->child_count--;
    }

    UndoEntry *e = calloc(1, sizeof(UndoEntry));
    e->type = type;
    e->pos  = pos;
    e->len  = len;
    e->cx   = cx;
    e->cy   = cy;
    e->text = malloc(len + 1);
    memcpy(e->text, text, len);
    e->text[len] = '\0';
    e->parent = u->current;

    u->current->children[u->current->child_count++] = e;
    u->current->most_recent_child = e;
    u->current = e;
}

bool undo_try_merge(UndoStack *u, UndoType type, size_t pos,
                    const char *text, size_t len, int cx, int cy) {
    if (!u || u->current == u->root || len != 1) return false;

    UndoEntry *prev = u->current;
    if (prev->type != type) return false;

    char new_char = text[0];
    char prev_last = prev->text[prev->len - 1];

    if (new_char == '\n' || prev_last == '\n') return false;

    bool new_ws  = (new_char == ' ' || new_char == '\t');
    bool prev_ws = (prev_last == ' ' || prev_last == '\t');
    if (new_ws != prev_ws) return false;

    if (type == UNDO_INSERT) {
        if (pos != prev->pos + prev->len) return false;

        prev->text = realloc(prev->text, prev->len + 2);
        prev->text[prev->len] = new_char;
        prev->len++;
        prev->text[prev->len] = '\0';
        return true;
    }

    if (type == UNDO_DELETE) {
        if (pos == prev->pos - 1) {
            prev->text = realloc(prev->text, prev->len + 2);
            memmove(prev->text + 1, prev->text, prev->len + 1);
            prev->text[0] = new_char;
            prev->len++;
            prev->pos = pos;
            prev->cx  = cx;
            prev->cy  = cy;
            return true;
        }
        if (pos == prev->pos) {
            prev->text = realloc(prev->text, prev->len + 2);
            prev->text[prev->len] = new_char;
            prev->len++;
            prev->text[prev->len] = '\0';
            return true;
        }
    }

    return false;
}

UndoEntry *undo_pop(UndoStack *u) {
    if (!u || u->current == u->root) return NULL;
    UndoEntry *undone = u->current;
    UndoEntry *parent = undone->parent;
    parent->most_recent_child = undone;
    u->current = parent;
    return undone;
}

UndoEntry *undo_redo(UndoStack *u) {
    if (!u || u->current->child_count == 0) return NULL;
    UndoEntry *redo_node = u->current->most_recent_child;
    if (!redo_node || !is_child(u->current, redo_node)) {
        redo_node = u->current->children[u->current->child_count - 1];
    }
    u->current = redo_node;
    u->current->parent->most_recent_child = redo_node;
    return redo_node;
}

bool undo_can_undo(UndoStack *u) {
    return u && u->current != u->root;
}

bool undo_can_redo(UndoStack *u) {
    return u && u->current->child_count > 0;
}

UndoEntry* undo_get_last_branch(UndoStack *u) {
    return u ? u->last_branch : NULL;
}
