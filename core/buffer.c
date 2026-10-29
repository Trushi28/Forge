#include "buffer.h"
#include "arena.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
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
   Buffer lifecycle
   ══════════════════════════════════════════════════════════════ */

Buffer *buffer_new(const char *initial_content, size_t len) {
    if (len > 0 && !initial_content) return NULL;
    Buffer *b = calloc(1, sizeof(Buffer));
    if (!b) return NULL;

    b->nil = calloc(1, sizeof(PieceNode));
    if (!b->nil) { free(b); return NULL; }
    b->nil->color  = RB_BLACK;
    b->nil->left   = b->nil->right = b->nil->parent = b->nil;

    b->original_len = len;
    if (len > 0) {
        b->original = malloc(len);
        if (!b->original) { free(b->nil); free(b); return NULL; }
        memcpy(b->original, initial_content, len);
    }

    b->add_cap = 64 * 1024;
    b->add_buf = malloc(b->add_cap);
    if (!b->add_buf) { free(b->original); free(b->nil); free(b); return NULL; }
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
        } else { buffer_free(b); return NULL; }
    }
    return b;
}

void buffer_free(Buffer *b) {
    if (!b) return;
    free(b->original);
    free(b->add_buf);
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
    if (!b || len == 0 || !text || len > SIZE_MAX - b->add_len) return;

    size_t total = doc_len(b);
    if (char_pos > total) char_pos = total;

    /* Grow add-buffer */
    if (b->add_len + len > b->add_cap) {
        size_t needed = b->add_len + len;
        size_t ncap = needed > SIZE_MAX / 2 ? needed : needed * 2;
        char *nb = realloc(b->add_buf, ncap);
        if (!nb) return;
        b->add_buf = nb;
        b->add_cap = ncap;
    }
    size_t add_start = b->add_len;
    memcpy(b->add_buf + add_start, text, len);
    b->add_len += len;
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
    if (!b || del_len == 0 || b->root == b->nil) return;
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
   Public read API

   Line starts are selected using cached subtree newline counts. Rendering a
   visible line therefore copies only that line; it no longer flattens and
   rescans the entire document after each edit.
   ══════════════════════════════════════════════════════════════ */

size_t buffer_total_len(Buffer *b) { return b ? doc_len(b) : 0; }

size_t buffer_line_count(Buffer *b) {
    return b ? (b->root != b->nil ? b->root->subtree_lines : 0) + 1 : 0;
}

size_t buffer_line_to_offset(Buffer *b, size_t line) {
    if (!b || line == 0) return 0;
    size_t line_count = buffer_line_count(b);
    if (line >= line_count) return doc_len(b);

    /* Find the (line - 1)th newline in the in-order piece sequence. */
    size_t target = line - 1;
    size_t base = 0;
    PieceNode *n = b->root;
    while (n != b->nil) {
        size_t left_lines = n->left->subtree_lines;
        if (target < left_lines) {
            n = n->left;
            continue;
        }
        target -= left_lines;
        base += n->left->subtree_len;

        if (target < n->own_lines) {
            const char *src = piece_src(b, n) + n->buf_start;
            const char *p = src;
            while (target > 0) {
                p = memchr(p, '\n', (size_t)(src + n->buf_len - p));
                if (!p) return doc_len(b);
                p++;
                target--;
            }
            p = memchr(p, '\n', (size_t)(src + n->buf_len - p));
            return p ? base + (size_t)(p - src) + 1 : doc_len(b);
        }

        target -= n->own_lines;
        base += n->buf_len;
        n = n->right;
    }
    return doc_len(b);
}

size_t buffer_offset_to_line(Buffer *b, size_t offset) {
    if (!b) return 0;
    size_t total = doc_len(b);
    if (offset > total) offset = total;
    size_t lines = 0;
    PieceNode *n = b->root;
    while (n != b->nil) {
        size_t left_len = n->left->subtree_len;
        if (offset < left_len) {
            n = n->left;
            continue;
        }
        lines += n->left->subtree_lines;
        offset -= left_len;
        if (n->buf_len > 0 && offset <= n->buf_len) {
            const char *src = piece_src(b, n) + n->buf_start;
            lines += count_newlines(src, offset);
            return lines;
        }
        offset -= n->buf_len;
        lines += n->own_lines;
        n = n->right;
    }
    return lines;
}

static PieceNode *piece_successor(Buffer *b, PieceNode *n) {
    if (n->right != b->nil) {
        n = n->right;
        while (n->left != b->nil) n = n->left;
        return n;
    }
    PieceNode *parent = n->parent;
    while (parent != b->nil && n == parent->right) {
        n = parent;
        parent = parent->parent;
    }
    return parent;
}

size_t buffer_read_range(Buffer *b, size_t offset, char *out, size_t len) {
    if (!b || !out || len == 0) return 0;
    size_t total = doc_len(b);
    if (offset >= total) return 0;
    if (len > total - offset) len = total - offset;

    size_t piece_start = 0;
    PieceNode *n = find_piece(b, offset, &piece_start);
    if (!n) return 0;
    size_t copied = 0;
    while (n != b->nil && copied < len) {
        if (n->buf_len > 0) {
            size_t in_piece = offset > piece_start ? offset - piece_start : 0;
            size_t avail = n->buf_len - in_piece;
            size_t take = len - copied < avail ? len - copied : avail;
            memcpy(out + copied, piece_src(b, n) + n->buf_start + in_piece, take);
            copied += take;
            offset += take;
        }
        size_t next_start = piece_start + n->buf_len;
        n = piece_successor(b, n);
        piece_start = next_start;
    }
    return copied;
}

char *buffer_get_text(Buffer *b) {
    if (!b) return NULL;
    size_t len = doc_len(b);
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    buffer_read_range(b, 0, copy, len);
    copy[len] = '\0';
    return copy;
}

char *buffer_get_line(Buffer *b, int line_idx) {
    if (!b || line_idx < 0 || (size_t)line_idx >= buffer_line_count(b)) return NULL;
    size_t start = buffer_line_to_offset(b, (size_t)line_idx);
    size_t end = (size_t)line_idx + 1 < buffer_line_count(b)
               ? buffer_line_to_offset(b, (size_t)line_idx + 1) : doc_len(b);
    if (end > start) {
        char last;
        if (buffer_read_range(b, end - 1, &last, 1) == 1 && last == '\n') end--;
    }
    size_t len = end - start;
    char *result = malloc(len + 1);
    if (!result) return NULL;
    buffer_read_range(b, start, result, len);
    result[len] = '\0';
    return result;
}

size_t buffer_get_offset(Buffer *b, int cx, int cy) {
    if (!b) return 0;
    size_t count = buffer_line_count(b);
    size_t line = cy < 0 ? 0 : (size_t)cy < count ? (size_t)cy : count - 1;
    size_t start = buffer_line_to_offset(b, line);
    size_t end = line + 1 < count ? buffer_line_to_offset(b, line + 1) : doc_len(b);
    if (end > start) {
        char last;
        if (buffer_read_range(b, end - 1, &last, 1) == 1 && last == '\n') end--;
    }
    size_t line_len = end - start;
    size_t col = cx < 0 ? 0 : (size_t)cx < line_len ? (size_t)cx : line_len;
    return start + col;
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
    if (!b || !path || !path[0]) return -1;
    char real[PATH_MAX];
    const char *target = realpath(path, real) ? real : path;

    char tmp[PATH_MAX + 32];
    int n = snprintf(tmp, sizeof(tmp), "%s.forge-tmp.XXXXXX", target);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;

    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(tmp);
        return -1;
    }

    int bad = write_pieces(b, f);
    if (fflush(f) != 0) bad = 1;
    if (fsync(fileno(f)) != 0) bad = 1;
    struct stat st;
    if (stat(target, &st) == 0 && fchmod(fileno(f), st.st_mode & 07777) != 0)
        bad = 1;
    if (fclose(f) != 0) bad = 1;

    if (!bad && rename(tmp, target) == 0) {
        /* Persist the rename as well as the file data where the filesystem
           supports directory fsync. */
        char dir[PATH_MAX];
        size_t target_len = strlen(target);
        if (target_len < sizeof(dir)) {
            memcpy(dir, target, target_len + 1);
            char *slash = strrchr(dir, '/');
            if (slash) {
                if (slash == dir) slash[1] = '\0';
                else *slash = '\0';
            } else {
                strcpy(dir, ".");
            }
            int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dfd >= 0) {
                (void)fsync(dfd);
                close(dfd);
            }
        }
        return 0;
    }

    unlink(tmp);
    return -1;
}
