#include "buffer.h"
#include "arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern Arena *session_arena;

/* ══════════════════════════════════════════════════════════════
   Helpers
   ══════════════════════════════════════════════════════════════ */

static size_t count_newlines(const char *s, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++)
        if (s[i] == '\n') n++;
    return n;
}

/* Walk from node n up to the root recomputing subtree_len /
   subtree_lines.  Call after every structural mutation.          */
static void fix_meta(Buffer *b, PieceNode *n) {
    while (n != b->nil) {
        const char *src = n->is_original ? b->original : b->add_buf;
        n->subtree_lines = count_newlines(src + n->buf_start, n->buf_len)
                         + n->left->subtree_lines
                         + n->right->subtree_lines;
        n->subtree_len   = n->buf_len
                         + n->left->subtree_len
                         + n->right->subtree_len;
        n = n->parent;
    }
}

/* ══════════════════════════════════════════════════════════════
   Line cache
   ══════════════════════════════════════════════════════════════ */

static void cache_dirty(Buffer *b) { b->line_cache_dirty = true; }

static void inorder_flatten(Buffer *b, PieceNode *n, char **ptr) {
    if (n == b->nil) return;
    inorder_flatten(b, n->left,  ptr);
    if (n->buf_len > 0) {
        const char *src = n->is_original ? b->original : b->add_buf;
        memcpy(*ptr, src + n->buf_start, n->buf_len);
        *ptr += n->buf_len;
    }
    inorder_flatten(b, n->right, ptr);
}

static void cache_rebuild(Buffer *b) {
    if (!b->line_cache_dirty) return;

    free(b->cached_text);
    free(b->line_starts);

    size_t total = (b->root != b->nil) ? b->root->subtree_len : 0;

    /* +2: unconditional guard '\n' + '\0'
       We ALWAYS append the guard even when text already ends with '\n'.
       This is critical: "foo\n" has two logical lines ("foo" and ""),
       so line_count must be 2.  If we skip the guard when text ends
       with '\n' we get line_count=1, and buffer_get_offset(0,1) clamps
       cy=1 back to line 0, inserting every subsequent character at
       offset 0 (i.e. the very start of the document).               */
    b->cached_text = malloc(total + 2);
    char *ptr = b->cached_text;
    inorder_flatten(b, b->root, &ptr);

    /* Unconditional guard newline */
    b->cached_text[total] = '\n';
    size_t scan_total = total + 1;       /* include the guard in scan  */
    b->cached_text[scan_total] = '\0';

    /* Count lines (each '\n' terminates one line) */
    size_t nlines = 0;
    for (size_t i = 0; i < scan_total; i++)
        if (b->cached_text[i] == '\n') nlines++;

    /* line_starts[i] = first byte of line i
       line_starts[nlines] = sentinel (one past last '\n')             */
    b->line_starts    = malloc((nlines + 1) * sizeof(size_t));
    b->line_starts[0] = 0;
    size_t li = 1;
    for (size_t i = 0; i < scan_total && li <= nlines; i++)
        if (b->cached_text[i] == '\n')
            b->line_starts[li++] = i + 1;

    b->line_count       = nlines;
    b->line_cache_dirty = false;
}

/* ══════════════════════════════════════════════════════════════
   Buffer lifecycle
   ══════════════════════════════════════════════════════════════ */

Buffer *buffer_new(const char *initial_content, size_t len) {
    Buffer *b = malloc(sizeof(Buffer));

    b->nil = malloc(sizeof(PieceNode));
    memset(b->nil, 0, sizeof(PieceNode));
    b->nil->color  = RB_BLACK;
    b->nil->left   = b->nil->right = b->nil->parent = b->nil;

    b->original_len = len;
    b->original     = NULL;
    if (len > 0) {
        b->original = malloc(len);
        memcpy(b->original, initial_content, len);
    }

    b->add_cap = 64 * 1024;
    b->add_len = 0;
    b->add_buf = malloc(b->add_cap);

    b->piece_count      = 0;
    b->cached_text      = NULL;
    b->line_starts      = NULL;
    b->line_count       = 0;
    b->line_cache_dirty = true;

    if (len > 0) {
        PieceNode *root = arena_alloc(session_arena, sizeof(PieceNode));
        root->is_original   = true;
        root->buf_start     = 0;
        root->buf_len       = len;
        root->subtree_len   = len;
        root->subtree_lines = count_newlines(initial_content, len);
        root->color         = RB_BLACK;
        root->left = root->right = root->parent = b->nil;
        b->root        = root;
        b->piece_count = 1;
    } else {
        b->root = b->nil;
    }
    return b;
}

void buffer_free(Buffer *b) {
    free(b->original);
    free(b->add_buf);
    free(b->cached_text);
    free(b->line_starts);
    free(b->nil);
    free(b);
}

/* ══════════════════════════════════════════════════════════════
   BST helpers
   ══════════════════════════════════════════════════════════════ */

/* Attach new_node under parent (or as root), then propagate metadata. */
static void link_node(Buffer *b, PieceNode *new_node,
                      PieceNode *parent, bool is_left) {
    new_node->parent = parent;
    if (parent == b->nil)      b->root         = new_node;
    else if (is_left)          parent->left    = new_node;
    else                       parent->right   = new_node;
    fix_meta(b, new_node);
}

/* Rightmost (in-order last) node of subtree rooted at n. */
static PieceNode *subtree_max(Buffer *b, PieceNode *n) {
    while (n->right != b->nil) n = n->right;
    return n;
}


/* ══════════════════════════════════════════════════════════════
   Insert

   Three bugs existed in the previous version:

   Bug A – strict `>` in the descent condition.
     With `char_pos > node_end`, equality (char_pos == node_end) fell
     into the `else` block with offset == buf_len.  When curr->right
     was non-nil the old code called link_node(z, curr, right) which
     **overwrote curr->right**, silently discarding the entire right
     subtree.  Every character typed past the first in a region was
     lost.  Fix: use `>=` so equality descends into the right subtree
     rather than stopping early.

   Bug B – offset == 0 with a non-nil left child.
     link_node(z, curr, left) overwrote curr->left, discarding the
     whole left subtree.  Fix: when offset == 0 and a left subtree
     exists, find the in-order predecessor (rightmost node of the left
     subtree, which by definition has a nil right child) and append z
     there.

   Bug C – fell off the tree.
     With `>=` and a valid tree the loop can only exit without finding
     a node when char_pos is past the total tree length.  We now track
     the last visited node (and direction) so we can insert there.
   ══════════════════════════════════════════════════════════════ */

void buffer_insert(Buffer *b, size_t char_pos, const char *text, size_t len) {
    if (len == 0) return;

    /* Grow add-buffer */
    if (b->add_len + len > b->add_cap) {
        b->add_cap = (b->add_len + len) * 2;
        b->add_buf = realloc(b->add_buf, b->add_cap);
    }
    size_t add_start = b->add_len;
    memcpy(b->add_buf + b->add_len, text, len);
    b->add_len += len;

    /* New piece node */
    PieceNode *z = arena_alloc(session_arena, sizeof(PieceNode));
    z->is_original   = false;
    z->buf_start     = add_start;
    z->buf_len       = len;
    z->subtree_len   = len;
    z->subtree_lines = count_newlines(text, len);
    z->color         = RB_RED;
    z->left = z->right = b->nil;

    b->piece_count++;
    cache_dirty(b);

    /* Empty tree */
    if (b->root == b->nil) {
        link_node(b, z, b->nil, false);
        b->root->color = RB_BLACK;
        return;
    }

    /* ── Descent ──────────────────────────────────────────────
       Invariant: `pos` is the byte offset of the FIRST byte of
       curr's own piece (not counting curr's left subtree).

       Condition `char_pos >= node_end` uses >=, not >.
       This means `char_pos == node_end` descends right instead of
       stopping with offset == buf_len — which would otherwise
       silently corrupt the right subtree (Bug A).
       ──────────────────────────────────────────────────────── */
    PieceNode *curr   = b->root;
    PieceNode *parent = b->nil;   /* last node visited before going nil */
    bool       p_left = false;    /* direction taken from parent        */
    size_t     pos    = 0;        /* start of curr's piece in doc space */

    while (curr != b->nil) {
        size_t left_len   = curr->left->subtree_len;
        size_t node_start = pos + left_len;
        size_t node_end   = node_start + curr->buf_len;

        if (char_pos < node_start) {
            parent = curr; p_left = true;
            curr   = curr->left;

        } else if (char_pos >= node_end) {   /* FIX A: was > (strict) */
            parent = curr; p_left = false;
            pos    = node_end;
            curr   = curr->right;

        } else {
            /* char_pos ∈ [node_start, node_end)
               offset ∈ [0, buf_len)  — never equals buf_len here    */
            size_t offset = char_pos - node_start;

            if (offset == 0) {
                /* Insert logically BEFORE curr.
                   FIX B: if curr->left exists, link_node(z, curr, left)
                   would overwrite curr->left.  Instead find the
                   in-order predecessor (rightmost of left subtree)
                   which has a nil right child, and attach z there.  */
                if (curr->left != b->nil) {
                    PieceNode *pred = subtree_max(b, curr->left);
                    link_node(b, z, pred, false);
                } else {
                    link_node(b, z, curr, true);
                }

            } else {
                /* Mid-node split: curr[0..offset) · z · curr[offset..end)
                   The right half (rh) takes curr's existing right subtree
                   so the BST shape is preserved.                      */
                PieceNode *old_right = curr->right;

                PieceNode *rh = arena_alloc(session_arena, sizeof(PieceNode));
                rh->is_original   = curr->is_original;
                rh->buf_start     = curr->buf_start + offset;
                rh->buf_len       = curr->buf_len   - offset;
                rh->color         = RB_RED;
                rh->left          = z;
                rh->right         = old_right;
                rh->parent        = curr;
                z->parent         = rh;

                if (old_right != b->nil) old_right->parent = rh;

                curr->buf_len = offset;
                curr->right   = rh;

                fix_meta(b, z);     /* propagates up through rh → curr → … */
                b->piece_count++;   /* one extra piece for rh               */
            }
            return;
        }
    }

    /* FIX C: fell off the tree (char_pos >= total document length).
       parent is the last node we visited; p_left is false (we went
       right), so parent->right is b->nil and it is safe to attach z. */
    link_node(b, z, parent, p_left);
}

/* ══════════════════════════════════════════════════════════════
   Delete
   ══════════════════════════════════════════════════════════════ */

void buffer_delete(Buffer *b, size_t char_pos, size_t del_len) {
    if (del_len == 0 || b->root == b->nil) return;
    cache_dirty(b);

    while (del_len > 0) {
        PieceNode *curr = b->root;
        size_t     pos  = 0;

        while (curr != b->nil) {
            size_t left_len = curr->left->subtree_len;
            if (char_pos < pos + left_len) {
                curr = curr->left;
            } else if (char_pos >= pos + left_len + curr->buf_len) {
                pos += left_len + curr->buf_len;
                curr = curr->right;
            } else {
                pos += left_len;
                break;
            }
        }
        if (curr == b->nil) break;

        size_t offset = char_pos - pos;
        size_t avail  = curr->buf_len - offset;
        size_t chunk  = del_len < avail ? del_len : avail;

        if (offset == 0 && chunk == curr->buf_len) {
            /* Case 1: whole node – zero out, tree shape intact */
            curr->buf_len = 0;

        } else if (offset == 0) {
            /* Case 2: trim from left */
            curr->buf_start += chunk;
            curr->buf_len   -= chunk;

        } else if (offset + chunk == curr->buf_len) {
            /* Case 3: trim from right */
            curr->buf_len = offset;

        } else {
            /* Case 4: mid-node delete – keep left edge + right edge */
            PieceNode *old_right = curr->right;

            PieceNode *rh = arena_alloc(session_arena, sizeof(PieceNode));
            rh->is_original = curr->is_original;
            rh->buf_start   = curr->buf_start + offset + chunk;
            rh->buf_len     = curr->buf_len   - (offset + chunk);
            rh->color       = RB_RED;
            rh->left        = b->nil;
            rh->right       = old_right;
            rh->parent      = curr;

            if (old_right != b->nil) old_right->parent = rh;

            curr->buf_len = offset;
            curr->right   = rh;
            fix_meta(b, rh);
            b->piece_count++;
        }

        fix_meta(b, curr);
        del_len -= chunk;
    }
}

/* ══════════════════════════════════════════════════════════════
   Public read API  (all backed by the line cache)
   ══════════════════════════════════════════════════════════════ */

/* doc_len: the actual document byte length, excluding the guard '\n' */
static size_t doc_len(Buffer *b) {
    return (b->root != b->nil) ? b->root->subtree_len : 0;
}

char *buffer_get_text(Buffer *b) {
    cache_rebuild(b);
    size_t len  = doc_len(b);
    char  *copy = malloc(len + 1);
    memcpy(copy, b->cached_text, len);
    copy[len] = '\0';
    return copy;
}

size_t buffer_line_count(Buffer *b) {
    cache_rebuild(b);
    return b->line_count;
}

char *buffer_get_line(Buffer *b, int line_idx) {
    cache_rebuild(b);
    if (line_idx < 0 || (size_t)line_idx >= b->line_count) return NULL;

    size_t start = b->line_starts[line_idx];
    size_t end   = b->line_starts[line_idx + 1];

    /* Strip the terminating '\n' from real document lines.
       The last line_starts entry points one past the guard '\n' we
       appended; if end > doc_len the terminator is our guard,
       not the document's — we still strip it so the caller always
       gets a bare string with no trailing newline.                  */
    if (end > start && b->cached_text[end - 1] == '\n') end--;

    size_t len    = end - start;
    char  *result = malloc(len + 1);
    memcpy(result, b->cached_text + start, len);
    result[len] = '\0';
    return result;
}

size_t buffer_get_offset(Buffer *b, int cx, int cy) {
    cache_rebuild(b);
    if (b->line_count == 0) return 0;

    /* Clamp cy to valid range */
    size_t line = (size_t)cy < b->line_count ? (size_t)cy
                                              : b->line_count - 1;
    size_t start = b->line_starts[line];
    size_t next  = b->line_starts[line + 1];

    /* Line length: strip the terminating '\n' if present */
    size_t line_len = (next > start && b->cached_text[next - 1] == '\n')
                          ? next - start - 1
                          : next - start;

    /* Don't let the cursor step past the document end */
    size_t dl  = doc_len(b);
    size_t col = (size_t)cx < line_len ? (size_t)cx : line_len;
    size_t off = start + col;
    return off < dl ? off : dl;
}

/* ══════════════════════════════════════════════════════════════
   Save
   ══════════════════════════════════════════════════════════════ */

int buffer_save(Buffer *b, const char *path) {
    size_t total = doc_len(b);
    char  *raw   = malloc(total + 1);
    char  *ptr   = raw;
    inorder_flatten(b, b->root, &ptr);
    raw[total] = '\0';

    FILE *f = fopen(path, "w");
    if (!f) { free(raw); return -1; }
    size_t written = fwrite(raw, 1, total, f);
    fclose(f);
    free(raw);
    return (written == total) ? 0 : -1;
}
