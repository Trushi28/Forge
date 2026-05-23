#ifndef FORGE_BUFFER_H
#define FORGE_BUFFER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { RB_RED, RB_BLACK } RBColor;

typedef struct PieceNode {
  bool        is_original;
  size_t      buf_start;
  size_t      buf_len;
  size_t      subtree_len;    /* total bytes in this subtree          */
  size_t      subtree_lines;  /* total '\n' chars in this subtree     */
  RBColor     color;
  struct PieceNode *left, *right, *parent;
} PieceNode;

typedef struct Buffer {
  char       *original;
  size_t      original_len;
  char       *add_buf;
  size_t      add_len;
  size_t      add_cap;
  PieceNode  *root;
  PieceNode  *nil;            /* sentinel – always BLACK, all fields 0 */
  size_t      piece_count;

  /* ── lazy line cache ─────────────────────────────────────── */
  char       *cached_text;    /* full flattened text (NULL = dirty)   */
  size_t     *line_starts;    /* line_starts[i] = byte offset of line i */
  size_t      line_count;
  bool        line_cache_dirty;
} Buffer;

Buffer *buffer_new   (const char *initial_content, size_t len);
void    buffer_free  (Buffer *b);

void    buffer_insert(Buffer *b, size_t char_pos, const char *text, size_t len);
void    buffer_delete(Buffer *b, size_t char_pos, size_t len);

char   *buffer_get_text  (Buffer *b);               /* caller frees */
char   *buffer_get_line  (Buffer *b, int line_idx); /* caller frees */
size_t  buffer_get_offset(Buffer *b, int cx, int cy);
size_t  buffer_line_count(Buffer *b);
int     buffer_save      (Buffer *b, const char *path);
size_t  buffer_total_len (Buffer *b);  /* total byte length, no allocation */

#endif
