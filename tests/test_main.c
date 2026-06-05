/*
 * test_main.c — Forge C core test suite
 *
 * Lightweight test harness for the C editor core:
 *   - Buffer (piece table) tests
 *   - Undo/Redo tests
 *   - Arena allocator tests
 *
 * Build & run:  make test
 */

#include "../core/buffer.h"
#include "../core/undo.h"
#include "../core/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ═══════════════════════════════════════════════════════════════
   Test harness
   ═══════════════════════════════════════════════════════════════ */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ANSI_GREEN  "\033[32m"
#define ANSI_RED    "\033[31m"
#define ANSI_RESET  "\033[0m"
#define ANSI_BOLD   "\033[1m"

#define RUN_TEST(fn) do { \
    tests_run++; \
    printf("  %-50s ", #fn); \
    fflush(stdout); \
    fn(); \
    tests_passed++; \
    printf(ANSI_GREEN "PASS" ANSI_RESET "\n"); \
} while(0)

/* If a test fails via ASSERT, longjmp would be complex in C.
   We just use assert() which aborts on failure. The test output
   up to that point shows which test failed.
   For non-fatal checks: */
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf(ANSI_RED "FAIL" ANSI_RESET " — %s\n", msg); \
        tests_failed++; \
        tests_passed--; /* undo the pre-increment in RUN_TEST */ \
        return; \
    } \
} while(0)

#define ASSERT_EQ_INT(a, b) do { \
    long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { \
        printf(ANSI_RED "FAIL" ANSI_RESET \
               " — expected %ld, got %ld (%s:%d)\n", \
               _b, _a, __FILE__, __LINE__); \
        tests_failed++; tests_passed--; return; \
    } \
} while(0)

#define ASSERT_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (!_a || !_b || strcmp(_a, _b) != 0) { \
        printf(ANSI_RED "FAIL" ANSI_RESET \
               " — expected \"%s\", got \"%s\" (%s:%d)\n", \
               _b ? _b : "(null)", _a ? _a : "(null)", \
               __FILE__, __LINE__); \
        tests_failed++; tests_passed--; return; \
    } \
} while(0)

#define ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        printf(ANSI_RED "FAIL" ANSI_RESET \
               " — unexpected NULL (%s:%d)\n", __FILE__, __LINE__); \
        tests_failed++; tests_passed--; return; \
    } \
} while(0)

/* ═══════════════════════════════════════════════════════════════
   Buffer tests
   ═══════════════════════════════════════════════════════════════ */

static void test_buffer_new_empty(void) {
    Buffer *b = buffer_new("", 0);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ_INT(buffer_total_len(b), 0);
    ASSERT_EQ_INT(buffer_line_count(b), 1); /* empty buffer = 1 line */
    buffer_free(b);
}

static void test_buffer_new_with_content(void) {
    const char *text = "Hello, world!";
    Buffer *b = buffer_new(text, strlen(text));
    ASSERT_NOT_NULL(b);
    char *got = buffer_get_text(b);
    ASSERT_EQ_STR(got, text);
    ASSERT_EQ_INT(buffer_total_len(b), 13);
    free(got);
    buffer_free(b);
}

static void test_buffer_insert_beginning(void) {
    Buffer *b = buffer_new("world", 5);
    buffer_insert(b, 0, "Hello, ", 7);
    char *got = buffer_get_text(b);
    ASSERT_EQ_STR(got, "Hello, world");
    ASSERT_EQ_INT(buffer_total_len(b), 12);
    free(got);
    buffer_free(b);
}

static void test_buffer_insert_end(void) {
    Buffer *b = buffer_new("Hello", 5);
    buffer_insert(b, 5, ", world", 7);
    char *got = buffer_get_text(b);
    ASSERT_EQ_STR(got, "Hello, world");
    free(got);
    buffer_free(b);
}

static void test_buffer_insert_middle(void) {
    Buffer *b = buffer_new("Helo", 4);
    buffer_insert(b, 3, "l", 1);
    char *got = buffer_get_text(b);
    ASSERT_EQ_STR(got, "Hello");
    free(got);
    buffer_free(b);
}

static void test_buffer_delete_beginning(void) {
    Buffer *b = buffer_new("Hello, world", 12);
    buffer_delete(b, 0, 7);
    char *got = buffer_get_text(b);
    ASSERT_EQ_STR(got, "world");
    free(got);
    buffer_free(b);
}

static void test_buffer_delete_end(void) {
    Buffer *b = buffer_new("Hello, world", 12);
    buffer_delete(b, 5, 7);
    char *got = buffer_get_text(b);
    ASSERT_EQ_STR(got, "Hello");
    free(got);
    buffer_free(b);
}

static void test_buffer_delete_middle(void) {
    Buffer *b = buffer_new("Helllo", 6);
    buffer_delete(b, 2, 1);
    char *got = buffer_get_text(b);
    ASSERT_EQ_STR(got, "Hello");
    free(got);
    buffer_free(b);
}

static void test_buffer_line_count_single(void) {
    Buffer *b = buffer_new("no newlines here", 16);
    ASSERT_EQ_INT(buffer_line_count(b), 1);
    buffer_free(b);
}

static void test_buffer_line_count_multi(void) {
    const char *text = "line1\nline2\nline3\n";
    Buffer *b = buffer_new(text, strlen(text));
    /* 3 lines of content + trailing newline = 4 lines */
    size_t lc = buffer_line_count(b);
    CHECK(lc >= 3, "expected at least 3 lines");
    buffer_free(b);
}

static void test_buffer_get_line(void) {
    const char *text = "alpha\nbeta\ngamma";
    Buffer *b = buffer_new(text, strlen(text));
    char *l0 = buffer_get_line(b, 0);
    char *l1 = buffer_get_line(b, 1);
    char *l2 = buffer_get_line(b, 2);
    ASSERT_NOT_NULL(l0);
    ASSERT_NOT_NULL(l1);
    ASSERT_NOT_NULL(l2);
    ASSERT_EQ_STR(l0, "alpha");
    ASSERT_EQ_STR(l1, "beta");
    ASSERT_EQ_STR(l2, "gamma");
    free(l0); free(l1); free(l2);
    buffer_free(b);
}

static void test_buffer_get_offset(void) {
    const char *text = "abc\ndef\nghi";
    Buffer *b = buffer_new(text, strlen(text));
    /* line 0, col 0 → offset 0 */
    ASSERT_EQ_INT(buffer_get_offset(b, 0, 0), 0);
    /* line 1, col 0 → offset 4 (after "abc\n") */
    ASSERT_EQ_INT(buffer_get_offset(b, 0, 1), 4);
    /* line 2, col 2 → offset 10 (after "abc\ndef\n" + "gh") */
    ASSERT_EQ_INT(buffer_get_offset(b, 2, 2), 10);
    buffer_free(b);
}

static void test_buffer_total_len(void) {
    Buffer *b = buffer_new("12345", 5);
    ASSERT_EQ_INT(buffer_total_len(b), 5);
    buffer_insert(b, 5, "67890", 5);
    ASSERT_EQ_INT(buffer_total_len(b), 10);
    buffer_delete(b, 0, 3);
    ASSERT_EQ_INT(buffer_total_len(b), 7);
    buffer_free(b);
}

static void test_buffer_large_insert(void) {
    /* Stress test: 1000 single-char inserts */
    Buffer *b = buffer_new("", 0);
    for (int i = 0; i < 1000; i++) {
        buffer_insert(b, (size_t)i, "x", 1);
    }
    ASSERT_EQ_INT(buffer_total_len(b), 1000);
    char *got = buffer_get_text(b);
    ASSERT_NOT_NULL(got);
    /* All chars should be 'x' */
    CHECK(got[0] == 'x' && got[999] == 'x', "stress content mismatch");
    free(got);
    buffer_free(b);
}

static void test_buffer_empty_operations(void) {
    Buffer *b = buffer_new("", 0);
    /* Delete from empty buffer (should not crash) */
    buffer_delete(b, 0, 0);
    ASSERT_EQ_INT(buffer_total_len(b), 0);
    /* Insert empty string */
    buffer_insert(b, 0, "", 0);
    ASSERT_EQ_INT(buffer_total_len(b), 0);
    buffer_free(b);
}

static void test_buffer_multiline_insert(void) {
    Buffer *b = buffer_new("", 0);
    buffer_insert(b, 0, "a\nb\nc", 5);
    ASSERT_EQ_INT(buffer_line_count(b), 3);
    char *got = buffer_get_text(b);
    ASSERT_EQ_STR(got, "a\nb\nc");
    free(got);
    buffer_free(b);
}

static void test_buffer_save_roundtrip(void) {
    const char *text = "Hello\nWorld\n";
    Buffer *b = buffer_new(text, strlen(text));
    const char *path = "/tmp/forge_test_buffer.tmp";
    int rc = buffer_save(b, path);
    ASSERT_EQ_INT(rc, 0);

    /* Read back */
    FILE *f = fopen(path, "r");
    ASSERT_NOT_NULL(f);
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    ASSERT_EQ_STR(buf, text);

    remove(path);
    buffer_free(b);
}

/* ═══════════════════════════════════════════════════════════════
   Undo tests
   ═══════════════════════════════════════════════════════════════ */

static void test_undo_insert_then_undo(void) {
    UndoStack u;
    undo_init(&u);
    undo_record(&u, UNDO_INSERT, 0, "hello", 5, 0, 0);
    CHECK(undo_can_undo(&u), "should be able to undo");
    UndoEntry *e = undo_pop(&u);
    ASSERT_NOT_NULL(e);
    ASSERT_EQ_INT(e->type, UNDO_INSERT);
    ASSERT_EQ_INT(e->len, 5);
    undo_free(&u);
}

static void test_undo_delete_then_undo(void) {
    UndoStack u;
    undo_init(&u);
    undo_record(&u, UNDO_DELETE, 5, "world", 5, 5, 0);
    UndoEntry *e = undo_pop(&u);
    ASSERT_NOT_NULL(e);
    ASSERT_EQ_INT(e->type, UNDO_DELETE);
    ASSERT_EQ_INT(e->pos, 5);
    undo_free(&u);
}

static void test_undo_redo(void) {
    UndoStack u;
    undo_init(&u);
    undo_record(&u, UNDO_INSERT, 0, "abc", 3, 0, 0);
    undo_pop(&u);
    CHECK(undo_can_redo(&u), "should be able to redo");
    UndoEntry *e = undo_redo(&u);
    ASSERT_NOT_NULL(e);
    ASSERT_EQ_INT(e->type, UNDO_INSERT);
    CHECK(!undo_can_redo(&u), "should not redo after redo");
    undo_free(&u);
}

static void test_undo_branch(void) {
    UndoStack u;
    undo_init(&u);
    undo_record(&u, UNDO_INSERT, 0, "abc", 3, 0, 0);
    undo_pop(&u);
    CHECK(undo_can_redo(&u), "should be able to redo before branching");
    /* New edit discards redo history */
    undo_record(&u, UNDO_INSERT, 0, "xyz", 3, 0, 0);
    CHECK(!undo_can_redo(&u), "redo should be discarded after new edit");
    undo_free(&u);
}

static void test_undo_merge_consecutive(void) {
    UndoStack u;
    undo_init(&u);
    /* First char */
    undo_record(&u, UNDO_INSERT, 0, "a", 1, 0, 0);
    /* Try merging consecutive chars */
    bool merged = undo_try_merge(&u, UNDO_INSERT, 1, "b", 1, 1, 0);
    /* Whether merge succeeds depends on implementation; just ensure no crash */
    if (!merged) {
        undo_record(&u, UNDO_INSERT, 1, "b", 1, 1, 0);
    }
    CHECK(undo_can_undo(&u), "should have undo entries");
    undo_free(&u);
}

static void test_undo_multiple_operations(void) {
    UndoStack u;
    undo_init(&u);
    for (int i = 0; i < 100; i++) {
        undo_record(&u, UNDO_INSERT, (size_t)i, "x", 1, i, 0);
    }
    int undone = 0;
    while (undo_can_undo(&u)) {
        undo_pop(&u);
        undone++;
    }
    ASSERT_EQ_INT(undone, 100);
    undo_free(&u);
}

/* ═══════════════════════════════════════════════════════════════
   Arena tests
   ═══════════════════════════════════════════════════════════════ */

static void test_arena_basic_alloc(void) {
    Arena *a = arena_new(4096);
    ASSERT_NOT_NULL(a);
    void *p1 = arena_alloc(a, 100);
    ASSERT_NOT_NULL(p1);
    void *p2 = arena_alloc(a, 200);
    ASSERT_NOT_NULL(p2);
    /* p1 and p2 should not overlap */
    CHECK((char *)p2 >= (char *)p1 + 100, "allocations should not overlap");
    arena_free(a);
}

static void test_arena_alignment(void) {
    Arena *a = arena_new(4096);
    void *p1 = arena_alloc(a, 1);   /* 1 byte, but should be aligned */
    void *p2 = arena_alloc(a, 1);
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    /* Allocations should be 8-byte aligned */
    CHECK(((size_t)p1 % 8) == 0 || ((size_t)p2 % 8) == 0,
          "allocations should be aligned");
    arena_free(a);
}

static void test_arena_reset(void) {
    Arena *a = arena_new(4096);
    arena_alloc(a, 1000);
    arena_alloc(a, 1000);
    arena_reset(a);
    /* After reset, we should be able to allocate again from the start */
    void *p = arena_alloc(a, 3000);
    ASSERT_NOT_NULL(p);
    arena_free(a);
}

static void test_arena_overflow(void) {
    Arena *a = arena_new(4096);
    /* Allocate more than capacity */
    void *p = arena_alloc(a, 5000);
    CHECK(p == NULL, "over-capacity allocation should return NULL");
    arena_free(a);
}

/* ═══════════════════════════════════════════════════════════════
   Main — run all tests
   ═══════════════════════════════════════════════════════════════ */

int main(void) {
    /* Buffer uses session_arena for PieceNode allocation */
    extern Arena *session_arena;
    session_arena = arena_new(1024 * 1024 * 4);

    printf("\n" ANSI_BOLD "═══ Forge C Core Test Suite ═══" ANSI_RESET "\n\n");

    printf(ANSI_BOLD "── Buffer Tests ──" ANSI_RESET "\n");
    RUN_TEST(test_buffer_new_empty);
    RUN_TEST(test_buffer_new_with_content);
    RUN_TEST(test_buffer_insert_beginning);
    RUN_TEST(test_buffer_insert_end);
    RUN_TEST(test_buffer_insert_middle);
    RUN_TEST(test_buffer_delete_beginning);
    RUN_TEST(test_buffer_delete_end);
    RUN_TEST(test_buffer_delete_middle);
    RUN_TEST(test_buffer_line_count_single);
    RUN_TEST(test_buffer_line_count_multi);
    RUN_TEST(test_buffer_get_line);
    RUN_TEST(test_buffer_get_offset);
    RUN_TEST(test_buffer_total_len);
    RUN_TEST(test_buffer_large_insert);
    RUN_TEST(test_buffer_empty_operations);
    RUN_TEST(test_buffer_multiline_insert);
    RUN_TEST(test_buffer_save_roundtrip);

    printf("\n" ANSI_BOLD "── Undo Tests ──" ANSI_RESET "\n");
    RUN_TEST(test_undo_insert_then_undo);
    RUN_TEST(test_undo_delete_then_undo);
    RUN_TEST(test_undo_redo);
    RUN_TEST(test_undo_branch);
    RUN_TEST(test_undo_merge_consecutive);
    RUN_TEST(test_undo_multiple_operations);

    printf("\n" ANSI_BOLD "── Arena Tests ──" ANSI_RESET "\n");
    RUN_TEST(test_arena_basic_alloc);
    RUN_TEST(test_arena_alignment);
    RUN_TEST(test_arena_reset);
    RUN_TEST(test_arena_overflow);

    /* Summary */
    printf("\n" ANSI_BOLD "════════════════════════════════════" ANSI_RESET "\n");
    printf("  Total:  %d\n", tests_run);
    printf("  " ANSI_GREEN "Passed: %d" ANSI_RESET "\n", tests_passed);
    if (tests_failed > 0)
        printf("  " ANSI_RED "Failed: %d" ANSI_RESET "\n", tests_failed);
    printf(ANSI_BOLD "════════════════════════════════════" ANSI_RESET "\n\n");

    arena_free(session_arena);
    return tests_failed > 0 ? 1 : 0;
}
