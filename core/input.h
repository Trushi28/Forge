#ifndef FORGE_INPUT_H
#define FORGE_INPUT_H

/* ── Control keys (ASCII) ────────────────────────────────────── */
enum KeyCode {
  KEY_CTRL_A = 1,  /* line start (Emacs)         */
  KEY_CTRL_B = 2,  /* git blame / timeline        */
  KEY_CTRL_C = 3,  /* quit hint                   */
  KEY_CTRL_D = 4,  /* duplicate line               */
  KEY_CTRL_E = 5,  /* line end (Emacs)             */
  KEY_CTRL_F = 6,  /* find / search                */
  KEY_CTRL_G = 7,  /* guild panel                  */
  KEY_CTRL_H = 8,  /* backspace                    */
  KEY_CTRL_K = 11, /* hover / kill line            */
  KEY_CTRL_N = 14, /* next (completion)            */
  KEY_CTRL_P = 16, /* command palette / prev       */
  KEY_CTRL_Q = 17, /* quit                         */
  KEY_CTRL_R = 18, /* find previous                */
  KEY_CTRL_S = 19, /* save                         */
  KEY_CTRL_T = 20, /* git timeline                 */
  KEY_CTRL_U = 21, /* delete to line start         */
  KEY_CTRL_W = 23, /* delete word back             */
  KEY_CTRL_Y = 25, /* redo                         */
  KEY_CTRL_Z = 26, /* undo                         */
  KEY_ESC = 27,
  KEY_BACKSPACE = 127,

  /* ── Extended keys ──────────────────────────────────────── */
  KEY_ARROW_UP = 1000,
  KEY_ARROW_DOWN,
  KEY_ARROW_LEFT,
  KEY_ARROW_RIGHT,
  KEY_PAGE_UP,
  KEY_PAGE_DOWN,
  KEY_HOME,
  KEY_END,
  KEY_DELETE,

  /* ── Word jump (Ctrl+Arrow) ──────────────────────────────── */
  KEY_WORD_LEFT = 1100,
  KEY_WORD_RIGHT,

  /* ── Tab switching (Ctrl+PageUp/Down) ─────────────────── */
  KEY_CTRL_PAGE_UP = 1150,
  KEY_CTRL_PAGE_DOWN,

  KEY_CTRL_ALT_UP = 1160,
  KEY_CTRL_ALT_DOWN,

  /* ── Mouse events (coordinates in g_mouse_x / g_mouse_y) ── */
  KEY_MOUSE_LEFT = 1200,
  KEY_MOUSE_RIGHT,
  KEY_MOUSE_SCROLL_UP,
  KEY_MOUSE_SCROLL_DOWN,
  KEY_ALT_MOUSE_LEFT = 1205,
};

/* Set by input_read_key() when a mouse key is returned.
   Both are 0-indexed terminal cell coordinates.               */
extern int g_mouse_x;
extern int g_mouse_y;

/* Raw mode */
void input_enable_raw_mode(void);
void input_disable_raw_mode(void);

/* Mouse reporting — call after entering raw mode */
void input_enable_mouse(void);
void input_disable_mouse(void);

/* Returns a KeyCode or ASCII value. Returns 0 on timeout (100 ms). */
int input_read_key(void);

/* Terminal size */
int input_get_term_size(int *rows, int *cols);

#endif
