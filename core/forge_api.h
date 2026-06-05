/*
 * forge_api.h — Plugin API for Forge Editor
 *
 * C plugins (.so) include this header to access the editor API.
 * Plugins run in a child process (forge-plugin-host) and communicate
 * with the editor via pipes. The API functions below are implemented
 * as IPC calls that cross the process boundary.
 *
 * Usage:
 *   #include "forge_api.h"
 *
 *   void handle_save(const char *path) {
 *       forge_run("prettier --write $FILE");
 *       forge_notify("formatted!");
 *   }
 *
 *   FORGE_PLUGIN_INIT {
 *       forge_on_save(handle_save);
 *   }
 */

#ifndef FORGE_API_H
#define FORGE_API_H

#include <stddef.h>
#include <stdbool.h>

/* ── Plugin version ────────────────────────────────────────── */

#define FORGE_API_VERSION  1

/* ── Buffer handle (opaque to plugins) ─────────────────────── */

typedef struct ForgeBuffer ForgeBuffer;

/* ── Event callback types ──────────────────────────────────── */

typedef void (*forge_save_cb)(const char *filepath);
typedef void (*forge_open_cb)(const char *filepath);
typedef void (*forge_close_cb)(const char *filepath);
typedef void (*forge_keypress_cb)(int key, ForgeBuffer *buf);

/* ── Plugin entry point macro ──────────────────────────────── */

#define FORGE_PLUGIN_INIT \
    void forge_plugin_init(void); \
    __attribute__((constructor)) void forge_plugin_init(void)

/* Alternate explicit registration */
#define FORGE_PLUGIN_EXPORT __attribute__((visibility("default")))

/* ── Event registration ────────────────────────────────────── */

/* Register a callback for when a file is saved */
void forge_on_save(forge_save_cb cb);

/* Register a callback for when a file is opened */
void forge_on_open(forge_open_cb cb);

/* Register a callback for when a file is closed */
void forge_on_close(forge_close_cb cb);

/* Register a callback for keypress events */
void forge_on_keypress(forge_keypress_cb cb);

/* ── Editor actions ────────────────────────────────────────── */

/* Insert text at the current cursor position */
void forge_insert(const char *text);

/* Delete n characters at the current cursor position */
void forge_delete(int n);

/* Show a notification in the status bar */
void forge_notify(const char *message);

/* Run a shell command. $FILE is expanded to the current file path.
   Non-blocking: runs in the background. */
void forge_run(const char *cmd);

/* Get the current file path (NULL if no file open) */
const char *forge_get_filepath(void);

/* Move the cursor by a relative offset (e.g., -1 to move left one char) */
void forge_cursor_move(int col_delta, int row_delta);

/* ── Buffer query ──────────────────────────────────────────── */

/* Get the total number of lines in the buffer */
size_t forge_buffer_line_count(ForgeBuffer *buf);

/* Get a line's content (caller must free the returned string) */
char *forge_buffer_get_line(ForgeBuffer *buf, int line);

/* Get the current cursor position */
void forge_buffer_get_cursor(ForgeBuffer *buf, int *line, int *col);

/* Get the total character count */
size_t forge_buffer_char_count(ForgeBuffer *buf);

/* Get the word count */
size_t forge_buffer_word_count(ForgeBuffer *buf);

/* ── Widget API ────────────────────────────────────────────── */

/* Slot IDs for widget registration */
#define FORGE_SLOT_TOPBAR      0
#define FORGE_SLOT_GUTTER      1
#define FORGE_SLOT_CONTENT     2
#define FORGE_SLOT_RIGHT_PANEL 3
#define FORGE_SLOT_STATUSBAR   4
#define FORGE_SLOT_BOTTOMBAR   5

typedef struct ForgeWidget ForgeWidget;
typedef void (*forge_widget_render_cb)(ForgeWidget *w, ForgeBuffer *buf);
typedef void (*forge_widget_key_cb)(ForgeWidget *w, int key);

/* Create a new widget */
ForgeWidget *forge_widget_new(const char *name, int slot, int priority);

/* Set render callback */
void forge_widget_set_render_cb(ForgeWidget *w, forge_widget_render_cb cb);

/* Set key callback */
void forge_widget_set_key_cb(ForgeWidget *w, forge_widget_key_cb cb);

/* Mark widget as needing redraw */
void forge_widget_mark_dirty(ForgeWidget *w);

/* Register widget with the editor */
void forge_widget_register(ForgeWidget *w);

/* Write text at a position in the widget's render area */
void forge_widget_write(ForgeWidget *w, int row, int col,
                        const char *text, unsigned int fg_color,
                        unsigned int bg_color);

#endif /* FORGE_API_H */
