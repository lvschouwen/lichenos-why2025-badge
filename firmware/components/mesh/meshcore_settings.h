#pragma once

#include "lvgl.h"

// Settings > MeshCore (#67): between meshcore_app.c and meshcore_settings.c, LVGL context only

// The app's settings callback: Name, Region & radio, Channels, Send advert on the page (a menu.h menu)
void meshcore_settings(lv_obj_t *page);
// The about rows (meshcore_app.c) show the radio again after a change; nothing when the page is gone
void meshcore_about_refresh(void);

// The first-open questions (#67) reuse the Name entry and the Region & radio list. Both return false when the screen
// did not open (the caller goes home). ask_name: submitted() once Enter started applying a name, then done(true) when it
// is applied or done(false) when it could not be (empty, busy); none of them on Esc. ask_region: chosen() once a preset
// or Custom radio is applied (the screens are still open: the caller pops them), never on Esc. ask_cancel forgets both.
bool meshcore_ask_name(void (*submitted)(void), void (*done)(bool ok));
bool meshcore_ask_region(void (*chosen)(void));
void meshcore_ask_cancel(void);
