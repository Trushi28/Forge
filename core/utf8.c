#include "utf8.h"
#include <wchar.h>

size_t utf8_decode(const char *s, size_t len, uint32_t *codepoint) {
  if (!s || len == 0) {
    if (codepoint) *codepoint = 0xfffd;
    return 0;
  }

  const unsigned char *p = (const unsigned char *)s;
  uint32_t cp;
  size_t n;
  if (p[0] < 0x80) {
    cp = p[0]; n = 1;
  } else if (p[0] >= 0xc2 && p[0] <= 0xdf) {
    cp = p[0] & 0x1f; n = 2;
  } else if (p[0] >= 0xe0 && p[0] <= 0xef) {
    cp = p[0] & 0x0f; n = 3;
  } else if (p[0] >= 0xf0 && p[0] <= 0xf4) {
    cp = p[0] & 0x07; n = 4;
  } else {
    if (codepoint) *codepoint = 0xfffd;
    return 1;
  }

  if (n > len) {
    if (codepoint) *codepoint = 0xfffd;
    return 1;
  }
  for (size_t i = 1; i < n; i++) {
    if ((p[i] & 0xc0) != 0x80) {
      if (codepoint) *codepoint = 0xfffd;
      return 1;
    }
    cp = (cp << 6) | (p[i] & 0x3f);
  }
  if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
      cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
    if (codepoint) *codepoint = 0xfffd;
    return 1;
  }
  if (codepoint) *codepoint = cp;
  return n;
}

size_t utf8_encode(uint32_t cp, char out[4]) {
  if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
  if (cp < 0x80) {
    out[0] = (char)cp; return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xc0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3f)); return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xe0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f)); return 3;
  }
  out[0] = (char)(0xf0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  out[3] = (char)(0x80 | (cp & 0x3f)); return 4;
}

size_t utf8_next(const char *s, size_t len, size_t offset) {
  if (offset >= len) return len;
  size_t n = utf8_decode(s + offset, len - offset, NULL);
  return offset + (n ? n : 1);
}

size_t utf8_prev(const char *s, size_t len, size_t offset) {
  if (!s || offset == 0) return 0;
  if (offset > len) offset = len;
  size_t candidate = offset - 1;
  while (candidate > 0 && (((unsigned char)s[candidate] & 0xc0) == 0x80))
    candidate--;
  if (utf8_next(s, len, candidate) == offset) return candidate;
  return offset - 1;
}

int utf8_codepoint_width(uint32_t cp) {
  if (cp == 0) return 0;
  if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 1;
  int width = wcwidth((wchar_t)cp);
  if (width >= 0) return width;
  /* C locale fallback: combining marks are zero cells; East Asian and
     emoji ranges are typically two cells. Ambiguous-width glyphs stay one. */
  if ((cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x1ab0 && cp <= 0x1aff) ||
      (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0x20d0 && cp <= 0x20ff) ||
      (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xfe20 && cp <= 0xfe2f) ||
      (cp >= 0xe0100 && cp <= 0xe01ef)) return 0;
  if ((cp >= 0x1100 && cp <= 0x115f) || cp == 0x2329 || cp == 0x232a ||
      (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) ||
      (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe10 && cp <= 0xfe6f) ||
      (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) ||
      (cp >= 0x1f300 && cp <= 0x1faff) || (cp >= 0x20000 && cp <= 0x3fffd))
    return 2;
  return 1;
}

size_t utf8_display_width(const char *s, size_t len) {
  size_t cells = 0;
  for (size_t i = 0; i < len;) {
    uint32_t cp;
    size_t n = utf8_decode(s + i, len - i, &cp);
    if (!n) break;
    int width = utf8_codepoint_width(cp);
    if (width > 0) cells += (size_t)width;
    i += n;
  }
  return cells;
}

size_t utf8_display_col(const char *s, size_t len, size_t byte_offset) {
  if (byte_offset > len) byte_offset = len;
  size_t cells = 0;
  for (size_t i = 0; i < byte_offset;) {
    uint32_t cp;
    size_t n = utf8_decode(s + i, len - i, &cp);
    if (!n) break;
    if (i + n > byte_offset) break;
    int width = utf8_codepoint_width(cp);
    if (width > 0) cells += (size_t)width;
    i += n;
  }
  return cells;
}

size_t utf8_byte_col(const char *s, size_t len, size_t display_col) {
  size_t cells = 0;
  for (size_t i = 0; i < len;) {
    uint32_t cp;
    size_t n = utf8_decode(s + i, len - i, &cp);
    if (!n) break;
    int width = utf8_codepoint_width(cp);
    if (width > 0 && cells + (size_t)width > display_col) return i;
    if (width > 0) cells += (size_t)width;
    i += n;
  }
  return len;
}

size_t utf8_utf16_col(const char *s, size_t len, size_t byte_offset) {
  if (byte_offset > len) byte_offset = len;
  size_t units = 0;
  for (size_t i = 0; i < byte_offset;) {
    uint32_t cp;
    size_t n = utf8_decode(s + i, len - i, &cp);
    if (!n || i + n > byte_offset) break;
    units += cp > 0xffff ? 2 : 1;
    i += n;
  }
  return units;
}

size_t utf8_byte_from_utf16_col(const char *s, size_t len, size_t utf16_col) {
  size_t units = 0;
  for (size_t i = 0; i < len;) {
    uint32_t cp;
    size_t n = utf8_decode(s + i, len - i, &cp);
    if (!n) break;
    size_t add = cp > 0xffff ? 2 : 1;
    if (units + add > utf16_col) return i;
    units += add;
    i += n;
  }
  return len;
}
