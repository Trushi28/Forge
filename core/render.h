#ifndef FORGE_RENDER_H
#define FORGE_RENDER_H

#include "buffer.h"
#include "ui.h"
#include <stdbool.h>
#include <stdarg.h>

#define GUTTER_WIDTH 5   /* 4 digits + 1 separator space */

typedef struct RenderState {
    int   width, height;
    int   scroll_row;
    int   scroll_col;
    bool  full_redraw;       /* force full repaint on next frame  */
    char **front_buffer;     /* what is currently on screen       */
    char **back_buffer;      /* what we want to draw this frame   */
    char  *output_stream;
    int    out_len, out_cap;
    char   status_msg[256];
} RenderState;

void render_init      (RenderState *r, int width, int height);
void render_free      (RenderState *r);
void render_resize    (RenderState *r, int width, int height);
void render_set_status(RenderState *r, const char *fmt, ...);

void render_frame     (RenderState *r, Buffer *b, UIRegistry *ui, int cx, int cy);

#endif
