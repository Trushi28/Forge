#ifndef FORGE_UTF8_H
#define FORGE_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Decode one UTF-8 scalar. Invalid input consumes one byte and returns U+FFFD. */
size_t utf8_decode(const char *s, size_t len, uint32_t *codepoint);
size_t utf8_encode(uint32_t codepoint, char out[4]);
size_t utf8_next(const char *s, size_t len, size_t offset);
size_t utf8_prev(const char *s, size_t len, size_t offset);

/* Terminal-cell width and conversions between byte and display columns. */
int    utf8_codepoint_width(uint32_t codepoint);
size_t utf8_display_width(const char *s, size_t len);
size_t utf8_display_col(const char *s, size_t len, size_t byte_offset);
size_t utf8_byte_col(const char *s, size_t len, size_t display_col);
size_t utf8_utf16_col(const char *s, size_t len, size_t byte_offset);
size_t utf8_byte_from_utf16_col(const char *s, size_t len, size_t utf16_col);

#endif
