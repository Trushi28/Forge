#include "arena.h"
#include "buffer.h"
#include "input.h"
#include "render.h"
#include "ui.h"
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Arena *session_arena = NULL;

/* ── Resize signal ──────────────────────────────────────────── */
static volatile int g_resized = 0;
static void sigwinch_handler(int sig) {
  (void)sig;
  g_resized = 1;
}

/* ── File loading ───────────────────────────────────────────── */
static Buffer *load_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return buffer_new("", 0);
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  if (sz <= 0) {
    fclose(f);
    return buffer_new("", 0);
  }
  char *data = malloc((size_t)sz + 1);
  size_t got = fread(data, 1, (size_t)sz, f);
  fclose(f);
  data[got] = '\0';
  Buffer *b = buffer_new(data, got);
  free(data);
  return b;
}

/* ── Cursor helpers ─────────────────────────────────────────── */
static int line_len(Buffer *b, int cy) {
  char *l = buffer_get_line(b, cy);
  int n = l ? (int)strlen(l) : 0;
  if (l)
    free(l);
  return n;
}

static void clamp_cx(Buffer *b, int *cx, int cy) {
  int max = line_len(b, cy);
  if (*cx > max)
    *cx = max;
  if (*cx < 0)
    *cx = 0;
}

/* ══════════════════════════════════════════════════════════════
   main
   ══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
  const char *filepath = (argc >= 2) ? argv[1] : NULL;

  session_arena = arena_new(1024 * 1024 * 16);

  signal(SIGWINCH, sigwinch_handler);
  input_enable_raw_mode();

  int term_rows, term_cols;
  if (input_get_term_size(&term_rows, &term_cols) == -1) {
    perror("forge: could not get terminal size");
    exit(1);
  }

  Buffer *buf = filepath ? load_file(filepath) : buffer_new("", 0);
  UIRegistry ui;
  RenderState render;

  ui_init(&ui, term_cols, term_rows);
  render_init(&render, term_cols, term_rows);

  if (filepath)
    render_set_status(&render, "%s", filepath);
  else
    render_set_status(&render, "[new file]   ^S save   ^Q quit");

  /* Initial full clear */
  (void)write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);

  int cx = 0, cy = 0;
  bool running = true;
  bool modified = false;

  while (running) {

    /* Handle terminal resize */
    if (g_resized) {
      g_resized = 0;
      if (input_get_term_size(&term_rows, &term_cols) == 0) {
        render_resize(&render, term_cols, term_rows);
        ui_resize(&ui, term_cols, term_rows);
      }
    }

    render_frame(&render, buf, &ui, cx, cy);

    int key = input_read_key();

    /* ── Quit ─────────────────────────────────────────── */
    if (key == KEY_CTRL_Q) {
      running = false;

      /* ── Save ─────────────────────────────────────────── */
    } else if (key == KEY_CTRL_S) {
      if (filepath) {
        if (buffer_save(buf, filepath) == 0) {
          render_set_status(&render, "Saved  %s", filepath);
          modified = false;
        } else {
          render_set_status(&render, "ERROR: could not save  %s", filepath);
        }
      } else {
        render_set_status(&render, "No filename – open as: forge <file>");
      }
      /* Flush immediately so status shows without waiting for next keypress */
      render.full_redraw = true;
      render_frame(&render, buf, &ui, cx, cy);
      continue;

      /* ── Navigation ───────────────────────────────────── */
    } else if (key == KEY_ARROW_UP) {
      if (cy > 0) {
        cy--;
        clamp_cx(buf, &cx, cy);
      }

    } else if (key == KEY_ARROW_DOWN) {
      size_t lc = buffer_line_count(buf);
      if (lc > 0 && (size_t)cy < lc - 1) {
        cy++;
        clamp_cx(buf, &cx, cy);
      }

    } else if (key == KEY_ARROW_LEFT) {
      if (cx > 0) {
        cx--;
      } else if (cy > 0) {
        cy--;
        cx = line_len(buf, cy);
      }

    } else if (key == KEY_ARROW_RIGHT) {
      int ll = line_len(buf, cy);
      if (cx < ll) {
        cx++;
      } else {
        size_t lc = buffer_line_count(buf);
        if ((size_t)cy < lc - 1) {
          cy++;
          cx = 0;
        }
      }

    } else if (key == KEY_PAGE_UP) {
      cy -= render.height - 2;
      if (cy < 0)
        cy = 0;
      clamp_cx(buf, &cx, cy);

    } else if (key == KEY_PAGE_DOWN) {
      size_t lc = buffer_line_count(buf);
      cy += render.height - 2;
      if (lc > 0 && (size_t)cy >= lc)
        cy = (int)lc - 1;
      clamp_cx(buf, &cx, cy);

    } else if (key == KEY_HOME) {
      cx = 0;

    } else if (key == KEY_END) {
      cx = line_len(buf, cy);

      /* ── Enter ────────────────────────────────────────── */
    } else if (key == '\r' || key == '\n') {
      size_t pos = buffer_get_offset(buf, cx, cy);
      buffer_insert(buf, pos, "\n", 1);
      cy++;
      cx = 0;
      modified = true;
      render_set_status(&render, "%s [+]", filepath ? filepath : "[new file]");

      /* ── Backspace ────────────────────────────────────── */
    } else if (key == KEY_BACKSPACE) {
      size_t pos = buffer_get_offset(buf, cx, cy);
      if (pos > 0) {
        buffer_delete(buf, pos - 1, 1);
        if (cx > 0) {
          cx--;
        } else if (cy > 0) {
          cy--;
          cx = line_len(buf, cy);
        }
        modified = true;
        render_set_status(&render, "%s [+]",
                          filepath ? filepath : "[new file]");
      }
    } else if (key == '\t') {
      int spaces = 4 - (cx % 4);
      size_t pos = buffer_get_offset(buf, cx, cy);
      for (int i = 0; i < spaces; i++)
        buffer_insert(buf, pos + i, " ", 1);
      cx += spaces;
      modified = true;
      render_set_status(&render, "%s [+]", filepath ? filepath : "[new file]");

    } else if (key >= 32 && key <= 126) {
      char c = (char)key;
      size_t pos = buffer_get_offset(buf, cx, cy);
      buffer_insert(buf, pos, &c, 1);
      cx++;
      modified = true;
      render_set_status(&render, "%s [+]", filepath ? filepath : "[new file]");
    }
  }

  (void)write(STDOUT_FILENO, "\x1b[?25h", 6);     /* show cursor  */
  (void)write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7); /* clear screen */

  render_free(&render);
  buffer_free(buf);
  arena_free(session_arena);

  if (modified && filepath)
    fprintf(stderr, "forge: warning: unsaved changes in %s\n", filepath);

  return 0;
}
