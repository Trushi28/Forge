#include "ui.h"
#include "render.h"
#include <string.h>

/* Recompute slot geometry from terminal size.
   Layout (rows, 1-indexed):
     row 1            : TOPBAR   (height 0 = hidden by default)
     rows 2..H-1      : CONTENT  (full width; GUTTER is virtual inside it)
     row H            : STATUSBAR (height 1, handled by render_frame directly)
   RIGHT_PANEL and BOTTOMBAR default hidden.                           */
static void layout(UIRegistry *ui) {
    int W = ui->term_w, H = ui->term_h;

    /* TOPBAR – hidden for now */
    ui->slots[SLOT_TOPBAR].row    = 1;
    ui->slots[SLOT_TOPBAR].col    = 1;
    ui->slots[SLOT_TOPBAR].width  = W;
    ui->slots[SLOT_TOPBAR].height = 0;
    ui->slots[SLOT_TOPBAR].visible = false;

    /* CONTENT – fills everything except the status row */
    ui->slots[SLOT_CONTENT].row    = 1;
    ui->slots[SLOT_CONTENT].col    = 1;
    ui->slots[SLOT_CONTENT].width  = W;
    ui->slots[SLOT_CONTENT].height = H - 1;
    ui->slots[SLOT_CONTENT].visible = true;

    /* GUTTER – virtual column inside CONTENT, managed by render */
    ui->slots[SLOT_GUTTER].row    = 1;
    ui->slots[SLOT_GUTTER].col    = 1;
    ui->slots[SLOT_GUTTER].width  = 5;
    ui->slots[SLOT_GUTTER].height = H - 1;
    ui->slots[SLOT_GUTTER].visible = true;

    /* STATUSBAR – bottom row, handled directly by render_frame */
    ui->slots[SLOT_STATUSBAR].row    = H;
    ui->slots[SLOT_STATUSBAR].col    = 1;
    ui->slots[SLOT_STATUSBAR].width  = W;
    ui->slots[SLOT_STATUSBAR].height = 1;
    ui->slots[SLOT_STATUSBAR].visible = true;

    /* RIGHT_PANEL, BOTTOMBAR – hidden */
    ui->slots[SLOT_RIGHT_PANEL].visible = false;
    ui->slots[SLOT_BOTTOMBAR].visible   = false;
}

void ui_init(UIRegistry *ui, int term_w, int term_h) {
    memset(ui, 0, sizeof(*ui));
    ui->term_w  = term_w;
    ui->term_h  = term_h;
    ui->focused = NULL;
    for (int i = 0; i < SLOT_COUNT; i++)
        ui->slots[i].widget_count = 0;
    layout(ui);
}

void ui_resize(UIRegistry *ui, int term_w, int term_h) {
    ui->term_w = term_w;
    ui->term_h = term_h;
    layout(ui);
}

void ui_register_widget(UIRegistry *ui, SlotId slot_id, Widget *w) {
    Slot *slot = &ui->slots[slot_id];
    if (slot->widget_count >= UI_MAX_WIDGETS) return;
    /* Priority-ordered insertion */
    int i = slot->widget_count - 1;
    while (i >= 0 && slot->widgets[i]->priority > w->priority) {
        slot->widgets[i + 1] = slot->widgets[i];
        i--;
    }
    slot->widgets[i + 1] = w;
    slot->widget_count++;
}

void ui_dispatch_key(UIRegistry *ui, int key) {
    if (ui->focused && ui->focused->on_key)
        ui->focused->on_key(ui->focused, key);
}

void ui_render_slots(UIRegistry *ui, RenderState *r, Buffer *b) {
    for (int s = 0; s < SLOT_COUNT; s++) {
        Slot *slot = &ui->slots[s];
        if (!slot->visible) continue;
        for (int w = 0; w < slot->widget_count; w++) {
            Widget *wg = slot->widgets[w];
            if (wg->visible && wg->render)
                wg->render(wg, r, b);
        }
    }
}
