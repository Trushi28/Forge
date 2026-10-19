#include "buffer.h"
#include "arena.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern Arena *session_arena;

/* ══════════════════════════════════════════════════════════════
   Piece tree

   An in-order sequence of pieces (original / add-buffer slices)
   held in a binary tree whose nodes cache subtree byte and line
   counts.  Position lookup is O(depth).

   Depth is kept logarithmic by compaction: whenever an insert
   descends deeper than a threshold, or too many zero-length
   nodes accumulate, the whole tree is flattened, adjacent
   contiguous pieces are merged, dead nodes are recycled, and a
   perfectly balanced tree is rebuilt in place (no allocation).
   Consecutive typing extends the previous add-buffer piece
   instead of creating a new node, so the piece count stays low.

   All tree walks are iterative (parent-pointer successor), so a
   degenerate tree can never overflow the C stack.
   ══════════════════════════════════════════════════════════════ */

typedef int (*piece_fn)(PieceNode *n, void *ctx);

static size_t count_newlines(const char *s, size_t len) {
    size_t n = 0;
    const char *p = s, *end = s + len;
    while (p < end && (p = memchr(p, '\n', (size_t)(end - p)))) { n++; p++; }
    return n;
}

static const char *piece_src(const Buffer *b, const PieceNode *n) {
    return n->is_original ? b->original : b->add_buf;
}

static size_t doc_len(Buffer *b) {
    return (b->root != b->nil) ? b->root->subtree_len : 0;
}

/* Recompute subtree_len / subtree_lines from n up to the root. */
static void fix_meta(Buffer *b, PieceNode *n) {
    while (n != b->nil) {
        n->subtree_lines = n->own_lines + n->left->subtree_lines
                         + n->right->subtree_lines;
        n->subtree_len   = n->buf_len + n->left->subtree_len
                         + n->right->subtree_len;
        n = n->parent;
    }
}

/* ── Node pool ──────────────────────────────────────────────── */

static PieceNode *node_alloc(Buffer *b) {
    PieceNode *n;
    if (b->free_list) {
        n = b->free_list;
        b->free_list = n->right;
    } else {
        n = arena_alloc(session_arena, sizeof(PieceNode));
    }
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->color = RB_RED;
    n->left = n->right = n->parent = b->nil;
    return n;
}

static void node_release(Buffer *b, PieceNode *n) {
    n->right = b->free_list;
    b->free_list = n;
}

/* ── Iterative in-order traversal ───────────────────────────── */

static int for_each_piece(Buffer *b, piece_fn fn, void *ctx) {
    PieceNode *n = b->root;
    if (n == b->nil) return 0;
    while (n->left != b->nil) n = n->left;
    while (n != b->nil) {
        int rc = fn(n, ctx);
        if (rc) return rc;
        if (n->right != b->nil) {
            n = n->right;
            while (n->left != b->nil) n = n->left;
        } else {
            PieceNode *p = n->parent;
            while (p != b->nil && n == p->right) { n = p; p = p->parent; }
            n = p;
        }
    }
    return 0;
}

typedef struct { Buffer *b; char *ptr; } FlattenCtx;

static int flatten_cb(PieceNode *n, void *c) {
    FlattenCtx *f = c;
    if (n->buf_len > 0) {
        memcpy(f->ptr, piece_src(f->b, n) + n->buf_start, n->buf_len);
        f->ptr += n->buf_len;
    }
    return 0;
}

static void flatten_into(Buffer *b, char *dst) {
    FlattenCtx f = { b, dst };
    for_each_piece(b, flatten_cb, &f);
}

/* ── Compaction / rebalancing ───────────────────────────────── */

typedef struct { PieceNode **arr; size_t n; } CollectCtx;

static int count_cb(PieceNode *n, void *c) { (void)n; (*(size_t *)c)++; return 0; }
static int collect_cb(PieceNode *n, void *c) {
    CollectCtx *cc = c;
    cc->arr[cc->n++] = n;
    return 0;
}

static PieceNode *build_balanced(Buffer *b, PieceNode **arr, size_t lo,
                                 size_t hi, PieceNode *parent) {
    if (lo >= hi) return b->nil;
    size_t mid = lo + (hi - lo) / 2;
    PieceNode *n = arr[mid];
    n->parent = parent;
    n->color  = RB_BLACK;
    n->left   = build_balanced(b, arr, lo, mid, n);
    n->right  = build_balanced(b, arr, mid + 1, hi, n);
    n->subtree_len   = n->buf_len   + n->left->subtree_len   + n->right->subtree_len;
    n->subtree_lines = n->own_lines + n->left->subtree_lines + n->right->subtree_lines;
    return n;
}

static void buffer_compact(Buffer *b) {
    if (b->root == b->nil) {
        b->piece_count = 0;
        b->dead_count  = 0;
        return;
    }
    size_t total = 0;
    for_each_piece(b, count_cb, &total);

    PieceNode **arr = malloc(total * sizeof(PieceNode *));
    if (!arr) return;                      /* keep the (valid) old tree */
    CollectCtx cc = { arr, 0 };
    for_each_piece(b, collect_cb, &cc);

    size_t m = 0;
    for (size_t i = 0; i < cc.n; i++) {
        PieceNode *n = arr[i];
        if (n->buf_len == 0) { node_release(b, n); continue; }
        if (m > 0) {
            PieceNode *p = arr[m - 1];
            if (p->is_original == n->is_original &&
                p->buf_start + p->buf_len == n->buf_start) {
                p->buf_len   += n->buf_len;
                p->own_lines += n->own_lines;
                node_release(b, n);
                continue;
            }
        }
        arr[m++] = n;
    }

    b->root = build_balanced(b, arr, 0, m, b->nil);
    if (b->root == b->nil) b->root->parent = b->nil; /* no-op, nil stays inert */
    b->piece_count = m;
    b->dead_count  = 0;
    free(arr);
}

static int ilog2(size_t n) { int r = 0; while (n > 1) { n >>= 1; r++; } return r; }

/* ══════════════════════════════════════════════════════════════
   Line cache
   ══════════════════════════════════════════════════════════════ */

static void cache_dirty(Buffer *b) { b->line_cache_dirty = true; }

static void cache_rebuild(Buffer *b) {
    if (!b->line_cache_dirty) return;

    free(b->cached_text);
    free(b->line_starts);

    size_t total = doc_len(b);

    /* +2: unconditional guard '\n' + '\0'.  "foo\n" has two logical
       lines ("foo" and ""), so the guard is ALWAYS appended.        */
    b->cached_text = malloc(total + 2);
    flatten_into(b, b->cached_text);
    b->cached_text[total] = '\n';
    size_t scan_total = total + 1;
    b->cached_text[scan_total] = '\0';

    size_t nlines = 0;
    for (size_t i = 0; i < scan_total; i++)
        if (b->cached_text[i] == '\n') nlines++;

    /* line_starts[nlines] = sentinel (one past the last '\n') */
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
    Buffer *b = calloc(1, sizeof(Buffer));

    b->nil = calloc(1, sizeof(PieceNode));
    b->nil->color  = RB_BLACK;
    b->nil->left   = b->nil->right = b->nil->parent = b->nil;

    b->original_len = len;
    if (len > 0) {
        b->original = malloc(len);
        memcpy(b->original, initial_content, len);
    }

    b->add_cap = 64 * 1024;
    b->add_buf = malloc(b->add_cap);
    b->line_cache_dirty = true;
    b->root = b->nil;

    if (len > 0) {
        PieceNode *root = node_alloc(b);
        if (root) {
            root->is_original   = true;
            root->buf_len       = len;
            root->own_lines     = count_newlines(initial_content, len);
            root->subtree_len   = len;
            root->subtree_lines = root->own_lines;
            root->color         = RB_BLACK;
            b->root        = root;
            b->piece_count = 1;
        }
    }
    return b;
}

void buffer_free(Buffer *b) {
    if (!b) return;
    free(b->original);
    free(b->add_buf);
    free(b->cached_text);
    free(b->line_starts);
    free(b->nil);
    free(b);
}

/* ══════════════════════════════════════════════════════════════
   Insert
   ══════════════════════════════════════════════════════════════ */

/* Node containing byte `pos` (start <= pos < start+len), or NULL. */
static PieceNode *find_piece(Buffer *b, size_t pos, size_t *out_start) {
    PieceNode *curr = b->root;
    size_t base = 0;
    while (curr != b->nil) {
        size_t ns = base + curr->left->subtree_len;
        size_t ne = ns + curr->buf_len;
        if (pos < ns)        curr = curr->left;
        else if (pos >= ne)  { base = ne; curr = curr->right; }
        else { *out_start = ns; return curr; }
    }
    return NULL;
}

/* Split a piece's newline count at byte offset `off` (scans the
   smaller side only). */
static void split_lines(const Buffer *b, const PieceNode *n, size_t off,
                        size_t *l, size_t *r) {
    const char *src = piece_src(b, n) + n->buf_start;
    if (off <= n->buf_len - off) {
        *l = count_newlines(src, off);
        *r = n->own_lines - *l;
    } else {
        *r = count_newlines(src + off, n->buf_len - off);
        *l = n->own_lines - *r;
    }
}

static void link_node(Buffer *b, PieceNode *new_node, PieceNode *parent,
                      bool is_left) {
    new_node->parent = parent;
    if (parent == b->nil)  b->root = new_node;
    else if (is_left)      parent->left  = new_node;
    else                   parent->right = new_node;
    fix_meta(b, new_node);
}

void buffer_insert(Buffer *b, size_t char_pos, const char *text, size_t len) {
    if (len == 0 || !text) return;

    size_t total = doc_len(b);
    if (char_pos > total) char_pos = total;

    /* Grow add-buffer */
    if (b->add_len + len > b->add_cap) {
        size_t ncap = (b->add_len + len) * 2;
        char *nb = realloc(b->add_buf, ncap);
        if (!nb) return;
        b->add_buf = nb;
        b->add_cap = ncap;
    }
    size_t add_start = b->add_len;
    memcpy(b->add_buf + add_start, text, len);
    b->add_len += len;
    cache_dirty(b);

    size_t z_lines = count_newlines(text, len);

    /* Fast path: typing continues the add piece that ends right here. */
    if (char_pos > 0) {
        size_t st = 0;
        PieceNode *prev = find_piece(b, char_pos - 1, &st);
        if (prev && !prev->is_original &&
            st + prev->buf_len == char_pos &&
            prev->buf_start + prev->buf_len == add_start) {
            prev->buf_len   += len;
            prev->own_lines += z_lines;
            fix_meta(b, prev);
            return;
        }
    }

    PieceNode *z = node_alloc(b);
    if (!z) { buffer_compact(b); z = node_alloc(b); }
    if (!z) return;                        /* out of nodes: drop the edit */
    z->is_original = false;
    z->buf_start   = add_start;
    z->buf_len     = len;
    z->own_lines   = z_lines;
    z->subtree_len = len;
    z->subtree_lines = z_lines;
    b->piece_count++;

    if (b->root == b->nil) {
        link_node(b, z, b->nil, false);
        b->root->color = RB_BLACK;
        return;
    }

    PieceNode *curr   = b->root;
    PieceNode *parent = b->nil;
    bool       p_left = false;
    size_t     pos    = 0;       /* doc offset of the start of curr's subtree */
    int        depth  = 0;

    while (curr != b->nil) {
        size_t node_start = pos + curr->left->subtree_len;
        size_t node_end   = node_start + curr->buf_len;
        depth++;

        if (char_pos < node_start) {
            parent = curr; p_left = true;
            curr = curr->left;
        } else if (char_pos >= node_end) {
            parent = curr; p_left = false;
            pos = node_end;
            curr = curr->right;
        } else {
            size_t offset = char_pos - node_start;   /* 0 <= offset < buf_len */

            if (offset == 0) {
                /* Insert logically before curr: rightmost of left subtree
                   (nil right child) or curr->left itself. */
                if (curr->left != b->nil) {
                    PieceNode *pred = curr->left;
                    while (pred->right != b->nil) pred = pred->right;
                    link_node(b, z, pred, false);
                } else {
                    link_node(b, z, curr, true);
                }
            } else {
                PieceNode *rh = node_alloc(b);
                if (!rh) {                           /* roll back */
                    node_release(b, z);
                    b->piece_count--;
                    return;
                }
                size_t l, r;
                split_lines(b, curr, offset, &l, &r);

                PieceNode *old_right = curr->right;
                rh->is_original = curr->is_original;
                rh->buf_start   = curr->buf_start + offset;
                rh->buf_len     = curr->buf_len - offset;
                rh->own_lines   = r;
                rh->left   = z;
                rh->right  = old_right;
                rh->parent = curr;
                z->parent  = rh;
                if (old_right != b->nil) old_right->parent = rh;

                curr->buf_len   = offset;
                curr->own_lines = l;
                curr->right     = rh;

                fix_meta(b, z);
                b->piece_count++;
                depth += 2;
            }
            goto done;
        }
    }
    link_node(b, z, parent, p_left);
    depth++;

done:
    if (depth > 2 * ilog2(b->piece_count + 1) + 16)
        buffer_compact(b);
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
        const char *src = piece_src(b, curr) + curr->buf_start;

        if (offset == 0 && chunk == curr->buf_len) {
            /* Whole node: zero it, tree shape intact. */
            curr->buf_len   = 0;
            curr->own_lines = 0;
            b->dead_count++;

        } else if (offset == 0) {
            curr->own_lines -= count_newlines(src, chunk);
            curr->buf_start += chunk;
            curr->buf_len   -= chunk;

        } else if (offset + chunk == curr->buf_len) {
            curr->own_lines -= count_newlines(src + offset, chunk);
            curr->buf_len    = offset;

        } else {
            /* Mid-node delete: keep left edge + right edge. */
            PieceNode *rh = node_alloc(b);
            if (!rh) {
                /* Compaction recycles nodes (and may merge `curr` away), so
                   restart the lookup afterwards instead of reusing curr. */
                buffer_compact(b);
                rh = node_alloc(b);
                if (!rh) break;
                node_release(b, rh);
                continue;
            }
            size_t tail_off = offset + chunk;
            size_t tail_len = curr->buf_len - tail_off;
            size_t d = count_newlines(src + offset, chunk), l, r;
            if (offset <= tail_len) {
                l = count_newlines(src, offset);
                r = curr->own_lines - d - l;
            } else {
                r = count_newlines(src + tail_off, tail_len);
                l = curr->own_lines - d - r;
            }

            PieceNode *old_right = curr->right;
            rh->is_original = curr->is_original;
            rh->buf_start   = curr->buf_start + tail_off;
            rh->buf_len     = tail_len;
            rh->own_lines   = r;
            rh->left        = b->nil;
            rh->right       = old_right;
            rh->parent      = curr;
            if (old_right != b->nil) old_right->parent = rh;

            curr->buf_len   = offset;
            curr->own_lines = l;
            curr->right     = rh;
            fix_meta(b, rh);
            b->piece_count++;
        }

        fix_meta(b, curr);
        del_len -= chunk;
    }

    if (b->dead_count > 64 && b->dead_count * 2 > b->piece_count)
        buffer_compact(b);
}

/* ══════════════════════════════════════════════════════════════
   Public read API  (all backed by the line cache)
   ══════════════════════════════════════════════════════════════ */

size_t buffer_total_len(Buffer *b) { return doc_len(b); }

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

    size_t line = cy < 0 ? 0
                : (size_t)cy < b->line_count ? (size_t)cy : b->line_count - 1;
    size_t start = b->line_starts[line];
    size_t next  = b->line_starts[line + 1];

    size_t line_len = (next > start && b->cached_text[next - 1] == '\n')
                          ? next - start - 1
                          : next - start;

    size_t dl  = doc_len(b);
    size_t col = cx < 0 ? 0 : ((size_t)cx < line_len ? (size_t)cx : line_len);
    size_t off = start + col;
    return off < dl ? off : dl;
}

/* ══════════════════════════════════════════════════════════════
   Save — write to a temp file in the same directory, then
   rename over the target, so a crash/full disk never truncates
   the user's file.  Falls back to a direct write when the
   directory is not writable.
   ══════════════════════════════════════════════════════════════ */

static int write_cb(PieceNode *n, void *c) {
    struct { Buffer *b; FILE *f; } *w = c;
    if (n->buf_len == 0) return 0;
    return fwrite(piece_src(w->b, n) + n->buf_start, 1, n->buf_len, w->f)
               == n->buf_len ? 0 : 1;
}

static int write_pieces(Buffer *b, FILE *f) {
    struct { Buffer *b; FILE *f; } w = { b, f };
    return for_each_piece(b, write_cb, &w);
}

int buffer_save(Buffer *b, const char *path) {
    char real[PATH_MAX];
    const char *target = realpath(path, real) ? real : path;

    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof(tmp), "%s.forge-tmp.%d", target, (int)getpid());

    FILE *f = fopen(tmp, "wb");
    if (f) {
        int bad = write_pieces(b, f);
        bad |= fflush(f);
        bad |= fsync(fileno(f));
        struct stat st;
        if (stat(target, &st) == 0) bad |= fchmod(fileno(f), st.st_mode & 07777);
        bad |= fclose(f);
        if (!bad && rename(tmp, target) == 0) return 0;
        remove(tmp);
        if (bad) return -1;
    }

    /* Fallback: direct write */
    f = fopen(target, "wb");
    if (!f) return -1;
    int bad = write_pieces(b, f);
    bad |= fclose(f);
    return bad ? -1 : 0;
}
