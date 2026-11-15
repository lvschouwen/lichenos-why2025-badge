#pragma once

#include "lvgl.h"

// MeshCore settings: the "Contacts on the card" row (#472, meshcore_card.c). LVGL context only.
void meshcore_card_row(lv_obj_t *page);
// The page is gone
void meshcore_card_row_forget(void);
