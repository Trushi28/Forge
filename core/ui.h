#ifndef FORGE_UI_H
#define FORGE_UI_H

#include <stdbool.h>

#define UI_MAX_WIDGETS 32

typedef enum {
    SLOT_TOPBAR,
    SLOT_GUTTER,
    SLOT_CONTENT,
    SLOT_RIGHT_PANEL,
    SLOT_STATUSBAR,
    SLOT_BOTTOMBAR,
    SLOT_COUNT
} SlotId;

typedef struct Buffer      Buffer;
typedef struct RenderState RenderState;

typedef struct Widget {
    char  name[64];
    int   priority;
    int   min_width;
    int   fixed_height;
    bool  visible;

    void (*render)(struct Widget *self, RenderState *r, Buffer *b);
    void (*on_key)(struct Widget *self, int key);

    void *userdata;
} Widget;

typedef struct {
    int  row, col;
    int  width, height;
    bool visible;
    Widget *widgets[UI_MAX_WIDGETS];
    int  widget_count;
} Slot;

typedef struct {
    Slot     slots[SLOT_COUNT];
    Widget  *focused;
    int      term_w, term_h;
} UIRegistry;

void ui_init           (UIRegistry *ui, int term_w, int term_h);
void ui_resize         (UIRegistry *ui, int term_w, int term_h);
void ui_layout         (UIRegistry *ui, bool topbar_visible, bool gutter_visible, bool right_panel_visible, int right_panel_width, bool bottombar_visible);
void ui_register_widget(UIRegistry *ui, SlotId slot, Widget *w);
void ui_dispatch_key   (UIRegistry *ui, int key);
void ui_render_slots   (UIRegistry *ui, RenderState *r, Buffer *b);

#endif
