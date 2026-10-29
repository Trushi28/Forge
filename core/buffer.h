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
  size_t      own_lines;      /* '\n' count in this node's own piece   */
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
  size_t      piece_count;    /* live nodes in tree (incl. dead ones)  */
  size_t      dead_count;     /* zero-length nodes awaiting compaction */
  PieceNode  *free_list;      /* recycled nodes (linked via ->right)   */

} Buffer;

Buffer *buffer_new   (const char *initial_content, size_t len);
void    buffer_free  (Buffer *b);

void    buffer_insert(Buffer *b, size_t char_pos, const char *text, size_t len);
void    buffer_delete(Buffer *b, size_t char_pos, size_t len);

char   *buffer_get_text  (Buffer *b);               /* caller frees */
char   *buffer_get_line  (Buffer *b, int line_idx); /* caller frees */
size_t  buffer_get_offset(Buffer *b, int cx, int cy);
/* Line starts are found from subtree newline counts; offsets and columns are bytes. */
size_t  buffer_line_to_offset(Buffer *b, size_t line);
size_t  buffer_offset_to_line(Buffer *b, size_t offset);
/* Copy up to len bytes beginning at offset. Returns bytes copied. */
size_t  buffer_read_range(Buffer *b, size_t offset, char *out, size_t len);
size_t  buffer_line_count(Buffer *b);
int     buffer_save      (Buffer *b, const char *path);
size_t  buffer_total_len (Buffer *b);  /* total byte length, no allocation */

#endif
