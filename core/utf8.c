#include "utf8.h"
#include <stdbool.h>
#include <wchar.h>
#ifdef HAS_UTF8PROC
#include <utf8proc.h>
#endif

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

static bool in_range(uint32_t cp, uint32_t lo, uint32_t hi) {
  return cp >= lo && cp <= hi;
}

#ifndef HAS_UTF8PROC
static bool grapheme_extend(uint32_t cp) {
  return in_range(cp, 0x0300, 0x036f) || in_range(cp, 0x0483, 0x0489) ||
         in_range(cp, 0x0591, 0x05bd) || cp == 0x05bf ||
         in_range(cp, 0x05c1, 0x05c2) || in_range(cp, 0x0610, 0x061a) ||
         in_range(cp, 0x064b, 0x065f) || cp == 0x0670 ||
         in_range(cp, 0x06d6, 0x06ed) || in_range(cp, 0x0711, 0x0711) ||
         in_range(cp, 0x0730, 0x074a) || in_range(cp, 0x07a6, 0x07b0) ||
         in_range(cp, 0x0816, 0x082d) || in_range(cp, 0x0859, 0x085b) ||
         in_range(cp, 0x08d3, 0x0902) || in_range(cp, 0x093a, 0x093c) ||
         in_range(cp, 0x0941, 0x0948) || cp == 0x094d ||
         in_range(cp, 0x0951, 0x0957) || in_range(cp, 0x0962, 0x0963) ||
         in_range(cp, 0x1ab0, 0x1aff) || in_range(cp, 0x1dc0, 0x1dff) ||
         in_range(cp, 0x20d0, 0x20ff) || in_range(cp, 0xfe00, 0xfe0f) ||
         in_range(cp, 0xfe20, 0xfe2f) || in_range(cp, 0xe0100, 0xe01ef) ||
         in_range(cp, 0x1f3fb, 0x1f3ff) || cp == 0x200c;
}

static bool grapheme_spacing_mark(uint32_t cp) {
  return cp == 0x0903 || in_range(cp, 0x093b, 0x0940) ||
         in_range(cp, 0x0949, 0x094c) || in_range(cp, 0x0982, 0x0983) ||
         in_range(cp, 0x09be, 0x09c0) || in_range(cp, 0x09c7, 0x09c8) ||
         in_range(cp, 0x09cb, 0x09cc) || in_range(cp, 0x0a3e, 0x0a40) ||
         in_range(cp, 0x0abe, 0x0ac0) || in_range(cp, 0x0ac9, 0x0ac9) ||
         in_range(cp, 0x0acb, 0x0acc) || in_range(cp, 0x0b3e, 0x0b40) ||
         in_range(cp, 0x0b47, 0x0b48) || in_range(cp, 0x0b4b, 0x0b4c) ||
         in_range(cp, 0x0bbe, 0x0bc2) || in_range(cp, 0x0bc6, 0x0bc8) ||
         in_range(cp, 0x0bca, 0x0bcc) || in_range(cp, 0x0c01, 0x0c03) ||
         in_range(cp, 0x0c41, 0x0c44) || in_range(cp, 0x0d3e, 0x0d40) ||
         in_range(cp, 0x0d46, 0x0d48) || in_range(cp, 0x0d4a, 0x0d4c);
}

static bool grapheme_prepend(uint32_t cp) {
  return in_range(cp, 0x0600, 0x0605) || cp == 0x06dd || cp == 0x070f ||
         cp == 0x0890 || cp == 0x0891 || cp == 0x08e2 || cp == 0x0d4e ||
         cp == 0x110bd || cp == 0x110cd || in_range(cp, 0x111c2, 0x111c3) ||
         in_range(cp, 0x13430, 0x1343f);
}

static bool grapheme_control(uint32_t cp) {
  return cp == '\r' || cp == '\n' || cp < 0x20 ||
         in_range(cp, 0x7f, 0x9f) || cp == 0x2028 || cp == 0x2029;
}
#endif

static bool extended_pictographic(uint32_t cp) {
  return in_range(cp, 0x1f000, 0x1faff) || in_range(cp, 0x2300, 0x23ff) ||
         in_range(cp, 0x2600, 0x27bf) || in_range(cp, 0x2b00, 0x2bff);
}

#ifndef HAS_UTF8PROC
static int hangul_type(uint32_t cp) {
  if (in_range(cp, 0x1100, 0x115f) || in_range(cp, 0xa960, 0xa97c)) return 1; /* L */
  if (in_range(cp, 0x1160, 0x11a7) || in_range(cp, 0xd7b0, 0xd7c6)) return 2; /* V */
  if (in_range(cp, 0x11a8, 0x11ff) || in_range(cp, 0xd7cb, 0xd7fb)) return 4; /* T */
  if (in_range(cp, 0xac00, 0xd7a3)) return ((cp - 0xac00) % 28) ? 5 : 3; /* LVT/LV */
  return 0;
}

static bool no_grapheme_break(const char *s, size_t start, size_t current,
                              uint32_t previous, uint32_t next,
                              size_t regional_count) {
  if (previous == '\r' && next == '\n') return true;
  if (grapheme_control(previous) || grapheme_control(next)) return false;
  int a = hangul_type(previous), b = hangul_type(next);
  if (a == 1 && (b == 1 || b == 2 || b == 3 || b == 5)) return true;
  if ((a == 2 || a == 3) && (b == 2 || b == 4)) return true;
  if ((a == 4 || a == 5) && b == 4) return true;
  if (grapheme_extend(next) || next == 0x200d || grapheme_spacing_mark(next)) return true;
  if (grapheme_prepend(previous)) return true;
  if (in_range(previous, 0x1f1e6, 0x1f1ff) && in_range(next, 0x1f1e6, 0x1f1ff))
    return (regional_count % 2) == 1;
  if (extended_pictographic(next) && previous == 0x200d) {
    size_t p = current;
    while (p > start) {
      size_t before = utf8_prev(s, current, p);
      uint32_t cp = 0;
      utf8_decode(s + before, p - before, &cp);
      if (cp != 0x200d && !grapheme_extend(cp))
        return extended_pictographic(cp);
      p = before;
    }
  }
  return false;
}
#endif

size_t utf8_grapheme_next(const char *s, size_t len, size_t offset) {
  if (!s || offset >= len) return len;
  if (offset > 0 && (((unsigned char)s[offset] & 0xc0) == 0x80))
    offset = utf8_prev(s, len, offset);
  size_t first_len = utf8_decode(s + offset, len - offset, NULL);
  size_t pos = offset + (first_len ? first_len : 1);
  uint32_t previous = 0;
  utf8_decode(s + offset, first_len, &previous);
#ifdef HAS_UTF8PROC
  utf8proc_int32_t state = 0;
  while (pos < len) {
    uint32_t next = 0;
    size_t n = utf8_decode(s + pos, len - pos, &next);
    if (!n || utf8proc_grapheme_break_stateful((utf8proc_int32_t)previous,
                                                (utf8proc_int32_t)next,
                                                &state))
      break;
    previous = next;
    pos += n;
  }
#else
  size_t regional_count = in_range(previous, 0x1f1e6, 0x1f1ff) ? 1 : 0;
  while (pos < len) {
    uint32_t next = 0;
    size_t n = utf8_decode(s + pos, len - pos, &next);
    if (!n || !no_grapheme_break(s, offset, pos, previous, next, regional_count)) break;
    if (in_range(next, 0x1f1e6, 0x1f1ff)) regional_count++;
    else if (!grapheme_extend(next)) regional_count = 0;
    previous = next;
    pos += n;
  }
#endif
  return pos;
}

size_t utf8_grapheme_prev(const char *s, size_t len, size_t offset) {
  if (!s || offset == 0) return 0;
  if (offset > len) offset = len;
  size_t prev = 0;
  for (size_t pos = 0; pos < offset;) {
    size_t next = utf8_grapheme_next(s, len, pos);
    if (next >= offset) return pos;
    prev = pos;
    pos = next;
  }
  return prev;
}

int utf8_codepoint_width(uint32_t cp) {
  if (cp == 0) return 0;
  if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 1;
#ifdef HAS_UTF8PROC
  int width = utf8proc_charwidth((utf8proc_int32_t)cp);
#elif defined(_WIN32)
  /* MinGW does not provide wcwidth; use the range-based fallback below. */
  int width = -1;
#else
  int width = wcwidth((wchar_t)cp);
#endif
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

int utf8_grapheme_width(const char *s, size_t len, size_t offset) {
  if (!s || offset >= len) return 0;
  size_t end = utf8_grapheme_next(s, len, offset);
  int width = 0;
  bool emoji = false;
  for (size_t i = offset; i < end;) {
    uint32_t cp = 0;
    size_t n = utf8_decode(s + i, end - i, &cp);
    if (!n) break;
    int cp_width = utf8_codepoint_width(cp);
    if (cp_width > width) width = cp_width;
    if (extended_pictographic(cp) || cp == 0xfe0f || cp == 0x200d) emoji = true;
    i += n;
  }
  if (emoji && width > 0) return 2;
  return width;
}

size_t utf8_display_width(const char *s, size_t len) {
  size_t cells = 0;
  for (size_t i = 0; i < len;) {
    size_t next = utf8_grapheme_next(s, len, i);
    if (next <= i) break;
    int width = utf8_grapheme_width(s, len, i);
    if (width > 0) cells += (size_t)width;
    i = next;
  }
  return cells;
}

size_t utf8_display_col(const char *s, size_t len, size_t byte_offset) {
  if (byte_offset > len) byte_offset = len;
  size_t cells = 0;
  for (size_t i = 0; i < byte_offset;) {
    size_t next = utf8_grapheme_next(s, len, i);
    if (next <= i || next > byte_offset) break;
    int width = utf8_grapheme_width(s, len, i);
    if (width > 0) cells += (size_t)width;
    i = next;
  }
  return cells;
}

size_t utf8_byte_col(const char *s, size_t len, size_t display_col) {
  size_t cells = 0;
  for (size_t i = 0; i < len;) {
    size_t next = utf8_grapheme_next(s, len, i);
    if (next <= i) break;
    int width = utf8_grapheme_width(s, len, i);
    if (width > 0 && cells + (size_t)width > display_col) return i;
    if (width > 0) cells += (size_t)width;
    i = next;
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
