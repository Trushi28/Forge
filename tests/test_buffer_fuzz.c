/* Differential fuzz + stress test for the piece tree.
   Build: gcc -std=c11 -D_GNU_SOURCE -g -fsanitize=address,undefined \
          tests/test_buffer_fuzz.c core/buffer.c core/arena.c core/globals.c -o fuzz */
#include "../core/buffer.h"
#include "../core/arena.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern Arena *session_arena;

typedef struct { char *p; size_t len, cap; } Model;
static void m_ins(Model *m, size_t pos, const char *t, size_t n) {
    if (pos > m->len) pos = m->len;
    if (m->len + n + 1 > m->cap) { m->cap = (m->len + n + 1) * 2; m->p = realloc(m->p, m->cap); }
    memmove(m->p + pos + n, m->p + pos, m->len - pos);
    memcpy(m->p + pos, t, n); m->len += n;
}
static void m_del(Model *m, size_t pos, size_t n) {
    if (pos >= m->len) return;
    if (pos + n > m->len) n = m->len - pos;
    memmove(m->p + pos, m->p + pos + n, m->len - pos - n); m->len -= n;
}
static size_t nl(const char *s, size_t n) { size_t c = 0; for (size_t i = 0; i < n; i++) c += s[i] == '\n'; return c; }

static int tree_depth(Buffer *b, PieceNode *n, int d) {
    if (n == b->nil) return d;
    int l = tree_depth(b, n->left, d + 1), r = tree_depth(b, n->right, d + 1);
    return l > r ? l : r;
}
static void check_node(Buffer *b, PieceNode *n, PieceNode *parent) {
    if (n == b->nil) return;
    assert(n->parent == parent);
    check_node(b, n->left, n); check_node(b, n->right, n);
    assert(n->subtree_len == n->buf_len + n->left->subtree_len + n->right->subtree_len);
    assert(n->subtree_lines == n->own_lines + n->left->subtree_lines + n->right->subtree_lines);
}
static size_t nodes(Buffer *b, PieceNode *n) { return n == b->nil ? 0 : 1 + nodes(b, n->left) + nodes(b, n->right); }

static void verify(Buffer *b, Model *m, int deep) {
    assert(buffer_total_len(b) == m->len);
    assert(b->root == b->nil ? m->len == 0 : b->root->subtree_lines == nl(m->p, m->len));
    assert(buffer_line_count(b) == nl(m->p, m->len) + 1);
    if (deep) {
        char *t = buffer_get_text(b);
        assert(strlen(t) == m->len && memcmp(t, m->p, m->len) == 0);
        free(t);
        check_node(b, b->root, b->nil);
        assert(tree_depth(b, b->root, 0) <= 2 * 20 + 16 + 40);
    }
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(void) {
    session_arena = arena_new(256u << 20);
    srand(12345);
    const char alpha[] = "abc\n xyz\n\n";

    /* 1. randomized differential test, many seeds / sizes */
    for (int round = 0; round < 40; round++) {
        size_t init = (size_t)(rand() % 3000);
        char *seed = malloc(init + 1);
        for (size_t i = 0; i < init; i++) seed[i] = alpha[rand() % (sizeof alpha - 1)];
        Model m = {0}; m_ins(&m, 0, seed, init);
        Buffer *b = buffer_new(seed, init);
        for (int op = 0; op < 4000; op++) {
            int kind = rand() % 10;
            if (kind < 6) {
                size_t n = 1 + rand() % (rand() % 5 == 0 ? 40 : 4);
                char t[64]; for (size_t i = 0; i < n; i++) t[i] = alpha[rand() % (sizeof alpha - 1)];
                size_t pos = m.len ? (size_t)rand() % (m.len + 3) : 0;   /* may exceed end */
                buffer_insert(b, pos, t, n); m_ins(&m, pos, t, n);
            } else {
                size_t n = 1 + rand() % (rand() % 7 == 0 ? 300 : 5);
                size_t pos = m.len ? (size_t)rand() % (m.len + 2) : 0;
                buffer_delete(b, pos, n); m_del(&m, pos, n);
            }
            verify(b, &m, op % 97 == 0);
            if (op % 211 == 0) {            /* line API spot check */
                size_t lc = buffer_line_count(b);
                int li = (int)(rand() % lc);
                char *ln = buffer_get_line(b, li); assert(ln);
                size_t cur = 0; int cl = 0; while (cl < li) { if (m.p[cur++] == '\n') cl++; }
                size_t e = cur; while (e < m.len && m.p[e] != '\n') e++;
                assert(strlen(ln) == e - cur && memcmp(ln, m.p + cur, e - cur) == 0);
                assert(buffer_get_offset(b, 0, li) == cur);
                free(ln);
            }
        }
        verify(b, &m, 1);
        buffer_free(b); free(m.p); free(seed);
    }
    puts("fuzz: ok");

    /* 2. 400k sequential appends — must stay ~1 node */
    { Buffer *b = buffer_new("", 0); double t = now();
      for (int i = 0; i < 400000; i++) buffer_insert(b, (size_t)i, "x", 1);
      assert(buffer_total_len(b) == 400000 && nodes(b, b->root) <= 2);
      printf("append 400k: %.3fs, nodes=%zu\n", now() - t, nodes(b, b->root)); buffer_free(b); }

    /* 3. 20k prepends (worst case for an unbalanced tree) */
    { Buffer *b = buffer_new("", 0); double t = now();
      for (int i = 0; i < 20000; i++) buffer_insert(b, 0, "y", 1);
      int d = tree_depth(b, b->root, 0);
      assert(buffer_total_len(b) == 20000 && d < 80);
      char *s = buffer_get_text(b); assert(strlen(s) == 20000); free(s);
      printf("prepend 20k: %.3fs, depth=%d\n", now() - t, d); buffer_free(b); }

    /* 4. random typing into a 20 MB original: must not be O(file) per edit */
    { size_t N = 20u << 20; char *big = malloc(N);
      for (size_t i = 0; i < N; i++) big[i] = (i % 61 == 60) ? '\n' : 'a';
      Buffer *b = buffer_new(big, N); double t = now();
      for (int i = 0; i < 20000; i++) buffer_insert(b, (size_t)rand() % buffer_total_len(b), "z", 1);
      printf("20k random inserts in 20MB doc: %.3fs, nodes=%zu depth=%d\n", now() - t,
             nodes(b, b->root), tree_depth(b, b->root, 0));
      assert(b->root->subtree_lines == nl(big, N));
      buffer_free(b); free(big); }

    /* 5. delete everything piecemeal, dead nodes get compacted */
    { Buffer *b = buffer_new("hello\nworld\n", 12);
      for (int i = 0; i < 5000; i++) buffer_insert(b, (size_t)(i % 7), "ab", 2);
      while (buffer_total_len(b) > 0) buffer_delete(b, buffer_total_len(b) - 1, 1);
      assert(buffer_line_count(b) == 1); buffer_free(b); }

    /* 6. save is atomic + round-trips */
    { Buffer *b = buffer_new("one\ntwo\n", 8); buffer_insert(b, 4, "ONE-AND-A-HALF\n", 15);
      assert(buffer_save(b, "/tmp/forge_fuzz_save.txt") == 0);
      FILE *f = fopen("/tmp/forge_fuzz_save.txt", "r"); char buf[128] = {0}; fread(buf, 1, 127, f); fclose(f);
      assert(strcmp(buf, "one\nONE-AND-A-HALF\ntwo\n") == 0); remove("/tmp/forge_fuzz_save.txt"); buffer_free(b); }

    /* 7. arena exhaustion must degrade (drop edits), never crash */
    { arena_free(session_arena); session_arena = arena_new(4096);
      Buffer *b = buffer_new("abc", 3);
      for (int i = 0; i < 5000; i++) buffer_insert(b, (size_t)(i % 3), "q", 1);
      char *s = buffer_get_text(b); free(s); buffer_free(b); puts("arena exhaustion: survived"); }

    puts("ALL OK");
    return 0;
}
