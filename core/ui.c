#include "ui.h"
#include "render.h"
#include <string.h>

/* Recompute slot geometry from terminal size.
   Layout (rows, 1-indexed):
     row 1            : TOPBAR   (height 0 = hidden by default)
     rows 2..H-1      : CONTENT  (full width; GUTTER is virtual inside it)
     row H            : STATUSBAR (height 1, handled by render_frame directly)
   RIGHT_PANEL and BOTTOMBAR default hidden.                           */
void ui_layout(UIRegistry *ui, bool topbar_visible, bool gutter_visible, bool right_panel_visible, int right_panel_width, bool bottombar_visible) {
    int W = ui->term_w, H = ui->term_h;

    int top_h = topbar_visible ? 1 : 0;
    int status_h = 1;
    int bottom_h = bottombar_visible ? 3 : 0;

    int right_w = right_panel_visible ? (right_panel_width > 0 ? right_panel_width : 35) : 0;
    int gutter_w = gutter_visible ? 5 : 0;

    /* TOPBAR */
    ui->slots[SLOT_TOPBAR].row     = 1;
    ui->slots[SLOT_TOPBAR].col     = 1;
    ui->slots[SLOT_TOPBAR].width   = W;
    ui->slots[SLOT_TOPBAR].height  = top_h;
    ui->slots[SLOT_TOPBAR].visible = topbar_visible;

    /* STATUSBAR */
    ui->slots[SLOT_STATUSBAR].row     = H - bottom_h;
    ui->slots[SLOT_STATUSBAR].col     = 1;
    ui->slots[SLOT_STATUSBAR].width   = W - right_w;
    ui->slots[SLOT_STATUSBAR].height  = status_h;
    ui->slots[SLOT_STATUSBAR].visible = true;

    /* BOTTOMBAR */
    ui->slots[SLOT_BOTTOMBAR].row     = H - bottom_h + 1;
    ui->slots[SLOT_BOTTOMBAR].col     = 1;
    ui->slots[SLOT_BOTTOMBAR].width   = W - right_w;
    ui->slots[SLOT_BOTTOMBAR].height  = bottom_h;
    ui->slots[SLOT_BOTTOMBAR].visible = bottombar_visible;

    /* RIGHT_PANEL */
    ui->slots[SLOT_RIGHT_PANEL].row     = 1;
    ui->slots[SLOT_RIGHT_PANEL].col     = W - right_w + 1;
    ui->slots[SLOT_RIGHT_PANEL].width   = right_w;
    ui->slots[SLOT_RIGHT_PANEL].height  = H;
    ui->slots[SLOT_RIGHT_PANEL].visible = right_panel_visible;

    /* CONTENT */
    ui->slots[SLOT_CONTENT].row     = top_h + 1;
    ui->slots[SLOT_CONTENT].col     = gutter_w + 1;
    ui->slots[SLOT_CONTENT].width   = W - right_w - gutter_w;
    ui->slots[SLOT_CONTENT].height  = H - top_h - status_h - bottom_h;
    ui->slots[SLOT_CONTENT].visible = true;

    /* GUTTER */
    ui->slots[SLOT_GUTTER].row     = top_h + 1;
    ui->slots[SLOT_GUTTER].col     = 1;
    ui->slots[SLOT_GUTTER].width   = gutter_w;
    ui->slots[SLOT_GUTTER].height  = H - top_h - status_h - bottom_h;
    ui->slots[SLOT_GUTTER].visible = gutter_visible;
}

void ui_init(UIRegistry *ui, int term_w, int term_h) {
    memset(ui, 0, sizeof(*ui));
    ui->term_w  = term_w;
    ui->term_h  = term_h;
    ui->focused = NULL;
    for (int i = 0; i < SLOT_COUNT; i++)
        ui->slots[i].widget_count = 0;
    ui_layout(ui, false, true, false, 35, false);
}

void ui_resize(UIRegistry *ui, int term_w, int term_h) {
    ui->term_w = term_w;
    ui->term_h = term_h;
    ui_layout(ui, false, true, false, 35, false);
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
