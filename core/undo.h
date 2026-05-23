#ifndef FORGE_UNDO_H
#define FORGE_UNDO_H

#include <stdbool.h>
#include <stddef.h>

/* ── Undo entry types ───────────────────────────────────────── */

typedef enum {
    UNDO_INSERT,     /* text was inserted — undo = delete it */
    UNDO_DELETE      /* text was deleted  — undo = re-insert it */
} UndoType;

/* ── Single undo entry ──────────────────────────────────────── */

typedef struct {
    UndoType  type;
    size_t    pos;       /* byte position in document */
    char     *text;      /* the text that was inserted or deleted */
    size_t    len;       /* length of text */
    int       cx, cy;    /* cursor position before the operation */
} UndoEntry;

/* ── Undo stack (linear history with redo) ──────────────────── */

#define UNDO_MAX_ENTRIES 8192

typedef struct {
    UndoEntry entries[UNDO_MAX_ENTRIES];
    int       count;     /* total entries (including redo-able ones) */
    int       current;   /* index of next entry to undo (0 = nothing to undo) */
                         /* entries[0..current-1] = undo-able */
                         /* entries[current..count-1] = redo-able */
} UndoStack;

/* ── Public API ─────────────────────────────────────────────── */

void undo_init(UndoStack *u);
void undo_free(UndoStack *u);

/* Record an edit operation.
   For INSERT: record the inserted text (so undo can delete it).
   For DELETE: record the deleted text (so undo can re-insert it).
   Calling this after an undo discards the redo history. */
void undo_record(UndoStack *u, UndoType type, size_t pos,
                 const char *text, size_t len, int cx, int cy);

/* Try to merge with the previous entry (e.g. consecutive single-char inserts).
   Returns true if merged, false if a new entry was needed. */
bool undo_try_merge(UndoStack *u, UndoType type, size_t pos,
                    const char *text, size_t len, int cx, int cy);

/* Undo the most recent operation.
   Returns the entry that was undone, or NULL if nothing to undo.
   The caller must apply the reverse operation to the buffer. */
UndoEntry *undo_pop(UndoStack *u);

/* Redo the most recently undone operation.
   Returns the entry to redo, or NULL if nothing to redo.
   The caller must re-apply the operation to the buffer. */
UndoEntry *undo_redo(UndoStack *u);

/* Check if undo/redo is available */
bool undo_can_undo(UndoStack *u);
bool undo_can_redo(UndoStack *u);

#endif
