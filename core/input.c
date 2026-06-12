#include "input.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* ── Mouse coordinate globals ───────────────────────────────── */
int g_mouse_x = 0;
int g_mouse_y = 0;

/* ── Terminal state ─────────────────────────────────────────── */
static struct termios orig_termios;

void input_disable_raw_mode(void) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void input_enable_raw_mode(void) {
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    exit(1);
  atexit(input_disable_raw_mode);

  struct termios raw = orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  raw.c_cflag |= CS8;
  raw.c_oflag &= ~OPOST;
  /* VMIN=0, VTIME=1: non-blocking read with 100 ms timeout.
     This lets the main loop poll LSP/IPC between keypresses
     and re-render when async results arrive.                 */
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    exit(1);
}

/* ── Mouse support ──────────────────────────────────────────── */
void input_enable_mouse(void) {
  /* Enable X10 mouse (click) + SGR extended mouse (>220 columns) */
  (void)write(STDOUT_FILENO, "\x1b[?1000h", 8);
  (void)write(STDOUT_FILENO, "\x1b[?1006h", 8);
}

void input_disable_mouse(void) {
  (void)write(STDOUT_FILENO, "\x1b[?1006l", 8);
  (void)write(STDOUT_FILENO, "\x1b[?1000l", 8);
}

/* ── Escape sequence parser ─────────────────────────────────── */

/* Read bytes into seq[] until we hit a CSI terminator character
   (letter or '~') or run out of buffer / time.
   Returns number of bytes read.                                */
static int read_escape_seq(char *seq, int maxlen) {
  int n = 0;
  while (n < maxlen - 1) {
    int r = read(STDIN_FILENO, &seq[n], 1);
    if (r != 1)
      break; /* timeout / error         */
    char c = seq[n++];
    /* CSI terminators: letters and '~'. Stop after we see one. */
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~')
      break;
  }
  seq[n] = '\0';
  return n;
}

/* Read exactly `need` bytes (used for X10 mouse coordinates). */
static int read_exact(char *buf, int need) {
  int got = 0;
  while (got < need) {
    int r = read(STDIN_FILENO, buf + got, need - got);
    if (r <= 0)
      break;
    got += r;
  }
  return got;
}

/* Parse an integer from *pp, advancing the pointer. */
static int parse_int(const char **pp) {
  int v = 0;
  while (**pp >= '0' && **pp <= '9')
    v = v * 10 + (*(*pp)++ - '0');
  return v;
}

static int handle_csi(const char *seq, int slen) {
  if (slen == 0)
    return KEY_ESC;

  /* ── X10 mouse: ESC [ M b x y ──────────────────────────── */
  if (seq[0] == 'M' && slen == 1) {
    char mb[3];
    if (read_exact(mb, 3) == 3) {
      int btn = (unsigned char)mb[0] - 32;
      g_mouse_x = (int)((unsigned char)mb[1]) - 33;
      g_mouse_y = (int)((unsigned char)mb[2]) - 33;
      if (g_mouse_x < 0)
        g_mouse_x = 0;
      if (g_mouse_y < 0)
        g_mouse_y = 0;
      if (btn == 64)
        return KEY_MOUSE_SCROLL_UP;
      if (btn == 65)
        return KEY_MOUSE_SCROLL_DOWN;
      if (btn == 0)
        return KEY_MOUSE_LEFT;
      if (btn == 2)
        return KEY_MOUSE_RIGHT;
    }
    return 0; /* release or unknown — ignore */
  }

  /* ── SGR mouse: ESC [ < btn ; x ; y M/m ────────────────── */
  if (seq[0] == '<') {
    const char *p = seq + 1;
    int btn = parse_int(&p);
    if (*p == ';')
      p++;
    int mx = parse_int(&p);
    if (*p == ';')
      p++;
    int my = parse_int(&p);
    char fin = *p; /* 'M' = press, 'm' = release */
    if (fin == 'M') {
      g_mouse_x = mx - 1;
      g_mouse_y = my - 1;
      if (btn == 64)
        return KEY_MOUSE_SCROLL_UP;
      if (btn == 65)
        return KEY_MOUSE_SCROLL_DOWN;
      if (btn == 0)
        return KEY_MOUSE_LEFT;
      if (btn == 8 || btn == 16 || btn == 24)
        return KEY_ALT_MOUSE_LEFT;
      if (btn == 2)
        return KEY_MOUSE_RIGHT;
    }
    return 0; /* release — ignore */
  }

  /* ── Simple single-letter sequences: ESC [ A/B/C/D/H/F ─── */
  if (slen == 1 && seq[0] >= 'A') {
    switch (seq[0]) {
    case 'A':
      return KEY_ARROW_UP;
    case 'B':
      return KEY_ARROW_DOWN;
    case 'C':
      return KEY_ARROW_RIGHT;
    case 'D':
      return KEY_ARROW_LEFT;
    case 'H':
      return KEY_HOME;
    case 'F':
      return KEY_END;
    }
    return KEY_ESC;
  }

  /* ── Parse numeric / modifier sequences ─────────────────── */
  const char *p = seq;
  int n1 = parse_int(&p);

  /* ESC [ n ~ */
  if (*p == '~') {
    switch (n1) {
    case 1:
      return KEY_HOME;
    case 3:
      return KEY_DELETE;
    case 4:
      return KEY_END;
    case 5:
      return KEY_PAGE_UP;
    case 6:
      return KEY_PAGE_DOWN;
    case 7:
      return KEY_HOME;
    case 8:
      return KEY_END;
    }
    return KEY_ESC;
  }

  /* ESC [ n ; mod x  — modifier sequences */
  if (*p == ';') {
    p++;
    int mod = parse_int(&p);
    char letter = *p;
    /* mod=5 Ctrl, mod=6 Ctrl+Shift, mod=2 Shift, mod=3 Alt */

    /* Ctrl+PageUp/Down: ESC [ 5;5~ / ESC [ 6;5~ */
    if (letter == '~' && (mod == 5 || mod == 6)) {
      if (n1 == 5) return KEY_CTRL_PAGE_UP;
      if (n1 == 6) return KEY_CTRL_PAGE_DOWN;
    }

    if (mod == 7) {
      if (letter == 'A') return KEY_CTRL_ALT_UP;
      if (letter == 'B') return KEY_CTRL_ALT_DOWN;
    }

    if (mod == 5 || mod == 6) {
      switch (letter) {
      case 'C':
        return KEY_WORD_RIGHT;
      case 'D':
        return KEY_WORD_LEFT;
      case 'A':
        return KEY_ARROW_UP;
      case 'B':
        return KEY_ARROW_DOWN;
      }
    }
    /* Shift+Arrow: treat as regular arrow */
    switch (letter) {
    case 'A':
      return KEY_ARROW_UP;
    case 'B':
      return KEY_ARROW_DOWN;
    case 'C':
      return KEY_ARROW_RIGHT;
    case 'D':
      return KEY_ARROW_LEFT;
    }
  }

  return KEY_ESC;
}

/* ── Main read function ─────────────────────────────────────── */
int input_read_key(void) {
  char c;
  int nread = read(STDIN_FILENO, &c, 1);

  if (nread == 0)
    return 0; /* 100 ms timeout — no key */
  if (nread == -1) {
    if (errno == EAGAIN || errno == EINTR)
      return 0;
    exit(1);
  }

  if (c != '\x1b')
    return (unsigned char)c;

  /* ── Escape sequence ──────────────────────────────────────── */
  char intro;
  if (read(STDIN_FILENO, &intro, 1) != 1)
    return KEY_ESC; /* lone ESC — no follow-up byte */

  if (intro == '[') {
    /* CSI sequence */
    char seq[32];
    int slen = read_escape_seq(seq, sizeof(seq));
    return handle_csi(seq, slen);
  }

  if (intro == 'O') {
    /* SS3 sequence */
    char ch;
    if (read(STDIN_FILENO, &ch, 1) != 1)
      return KEY_ESC;
    switch (ch) {
    case 'H':
      return KEY_HOME;
    case 'F':
      return KEY_END;
    case 'c':
      return KEY_WORD_RIGHT; /* rxvt Ctrl+Right */
    case 'd':
      return KEY_WORD_LEFT; /* rxvt Ctrl+Left  */
    }
    return KEY_ESC;
  }

  /* ESC followed by something unrecognised — return ESC */
  return KEY_ESC;
}

/* ── Terminal size ──────────────────────────────────────────── */
int input_get_term_size(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    return -1;
  *cols = ws.ws_col;
  *rows = ws.ws_row;
  return 0;
}
