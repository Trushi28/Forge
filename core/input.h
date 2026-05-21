#ifndef FORGE_INPUT_H
#define FORGE_INPUT_H

enum KeyCode {
  KEY_CTRL_A = 1,
  KEY_CTRL_C = 3,
  KEY_CTRL_G = 7,
  KEY_CTRL_P = 16,
  KEY_CTRL_Q = 17,
  KEY_CTRL_S = 19,
  KEY_ESC = 27,
  KEY_BACKSPACE = 127,
  KEY_ARROW_UP = 1000,
  KEY_ARROW_DOWN,
  KEY_ARROW_LEFT,
  KEY_ARROW_RIGHT,
  KEY_PAGE_UP,
  KEY_PAGE_DOWN,
  KEY_HOME,
  KEY_END
};

void input_enable_raw_mode(void);
void input_disable_raw_mode(void);
int input_read_key(void);
int input_get_term_size(int *rows, int *cols);

#endif
