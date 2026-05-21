#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

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
  raw.c_cflag |= (CS8);
  raw.c_oflag &= ~(OPOST);
  raw.c_cc[VMIN]  = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    exit(1);
}

int input_read_key(void) {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1) exit(1);
  }

  if (c != '\x1b') return (unsigned char)c;

  /* Escape sequence */
  char seq[4];
  if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
  if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

  if (seq[0] == '[') {
    /* Digit sequences: ESC [ <digit> ~ */
    if (seq[1] >= '0' && seq[1] <= '9') {
      if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
      if (seq[2] == '~') {
        switch (seq[1]) {
          case '1': return KEY_HOME;
          case '4': return KEY_END;
          case '5': return KEY_PAGE_UP;
          case '6': return KEY_PAGE_DOWN;
          case '7': return KEY_HOME;
          case '8': return KEY_END;
        }
      }
    } else {
      switch (seq[1]) {
        case 'A': return KEY_ARROW_UP;
        case 'B': return KEY_ARROW_DOWN;
        case 'C': return KEY_ARROW_RIGHT;
        case 'D': return KEY_ARROW_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
      }
    }
  }

  /* SS3 sequences: ESC O <letter> */
  if (seq[0] == 'O') {
    switch (seq[1]) {
      case 'H': return KEY_HOME;
      case 'F': return KEY_END;
    }
  }

  return '\x1b';
}

int input_get_term_size(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    return -1;
  *cols = ws.ws_col;
  *rows = ws.ws_row;
  return 0;
}
