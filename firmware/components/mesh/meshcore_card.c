// MeshCore settings: "Contacts on the card" (#472). Says where the contact list lives; when the card holds a contact
// file this badge left alone (another badge's, or unreadable), Enter asks whether the card may be used: the old file is
// moved aside and a new list starts. Only at the badge: the dialog is a careful one.
#include "meshcore_card.h"

#include "lichen_ui.h"
#include "menu.h"
#include "mesh_ctab_run.h"

#include <stddef.h>

#define POLL_MS 1000 // the mesh task does the work: the row follows

static lv_obj_t   *row; // NULL once the page is gone
static lv_timer_t *timer;
static bool        asked; // a request is with the mesh task

static void refresh(lv_timer_t *t) {
    (void)t;
    if (!row) {
        return;
    }
    mesh_ctab_place_t place = mesh_ctab_run_place();
    char const       *error = mesh_ctab_run_use_card_error();
    if (place != MESH_CTAB_CARD_LEFT_ALONE) {
        asked = false;
        menu_set_value(row, place == MESH_CTAB_ON_CARD ? "In use" : "Not in use");
    } else if (asked && !error) {
        menu_set_value(row, "Working...");
    } else {
        asked = false;
        menu_set_value(row, error ? error : "Another list is on it");
    }
}

static void answer(bool confirmed) {
    if (confirmed) {
        asked = true;
        mesh_ctab_run_use_card();
        refresh(NULL);
    }
}

static void on_enter(void *arg) {
    (void)arg;
    if (mesh_ctab_run_place() != MESH_CTAB_CARD_LEFT_ALONE) {
        return; // nothing to decide: the row only says where the list is
    }
    shell_dialog_t const d = {
        .title         = "Use this card for contacts? The list on it now is kept as contacts.old; its contacts are not "
                         "taken over.",
        .seconds       = 20,
        .careful       = true,
        .confirm_label = "use card",
    };
    shell_dialog_countdown(&d, answer);
}

void meshcore_card_row(lv_obj_t *page) {
    row   = menu_add(page, "Contacts on the card", "", on_enter, NULL);
    timer = lichen_ui_timer_create(refresh, POLL_MS, NULL);
    refresh(NULL);
}

void meshcore_card_row_forget(void) {
    row = NULL;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
}
