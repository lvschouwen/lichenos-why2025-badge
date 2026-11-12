#pragma once
// The bench test card (#352): an original Lichen card for the 720x720 panel, drawn with LVGL primitives (test_card_layout.h
// says what is where). It answers panel, backlight and flicker bench steps on one screen. Shared by `fw ui card` (an
// overlay the shell puts on the sys layer) and the Test card app. LVGL context only (the caller holds the UI service's
// lock or is on the LVGL task). Memory: draw objects and one small state block.

#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "test_card_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the info strip reads. The gate never calls the OS itself (tools/layers.conf), so the shell sets these once at boot:
// fw_label "fw-<sha>" (must outlive the card), reg_get is lichen_reg_get, display_get the requested backlight levels
// (lichen_sys_display_get; NULL = leave "requested" out). Without them the strip reads "?". opened (may be NULL) is
// told +1 when a card is created and -1 when it is deleted, on the LVGL task, so the screen timeout can hold the panel
// bright while any card is up (#355), the Test card app's included.
typedef struct {
    char const *fw_label;
    size_t (*reg_get)(char const *name, char *buf, size_t cap);
    void (*display_get)(uint8_t *display, uint8_t *keyboard);
    void (*opened)(int delta);
} test_card_hooks_t;
void test_card_set_hooks(test_card_hooks_t const *hooks);

// A full-screen card on `parent` (the sys layer: it covers the status bar too), showing `mode` (not TCARD_OFF). The
// card owns its state and frees it when the object is deleted; the marker steps once per flushed frame, and stops with
// the object. NULL when out of memory.
lv_obj_t *test_card_create(lv_obj_t *parent, tcard_mode_t mode);
// Redraws the card as another mode (the app's Left/Right)
void test_card_set_mode(lv_obj_t *card, tcard_mode_t mode);

#ifdef __cplusplus
}
#endif
