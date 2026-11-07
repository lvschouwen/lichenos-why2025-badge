#pragma once
// The shell's implementations behind the gate's lichen_ui.h / lichen_keys.h calls (#221). The gate forwards to them
// through the table shell_ui_ops.c hands it (lichen_ui_ops.h); everyone else, the shell included, calls the gate's
// names. Each keeps the thread rule its lichen_ui.h / lichen_keys.h comment states.
#include "lichen_ui_ops.h"

// Hands the table to the gate. shell_boot_screen and shell_start call it first thing; a second call changes nothing.
void shell_ui_ops_register(void);

// shell.c
void      shell_home_impl(void);
void      shell_back_impl(void);
void      shell_open_keyed_impl(lv_obj_t *(*build)(void), int screen_id, uint32_t key);
unsigned  shell_open_reuse_count_impl(void);
void      shell_open_count_reuse_impl(void);
bool      shell_screen_open_impl(int screen_id, uint32_t key);
void      shell_on_esc_impl(bool (*fn)(void *arg), void *arg);
void      shell_on_search_impl(void (*fn)(void *arg), void *arg);
void      shell_scroll_by_impl(lv_obj_t *obj, int32_t dy);
void      shell_scroll_tame_impl(lv_obj_t *obj);
void      shell_no_arrow_scroll_impl(lv_obj_t *obj);
bool      shell_back_then_impl(void (*then)(void *), void *arg);
bool      shell_back_to_impl(lv_obj_t *screen, void (*then)(void *), void *arg);
lv_obj_t *shell_stack_screen_impl(int from_top);
void      shell_style_screen_impl(lv_obj_t *screen);
// app_settings.c
lv_obj_t *shell_page_create_impl(void);
// dialog.c
bool      shell_dialog_countdown_impl(shell_dialog_t const *d, shell_dialog_fn done);
void      shell_dialog_close_impl(void);
// screen_saver.c
bool      lichen_screen_locked_impl(void);
// station_view.c
bool      lichen_station_clock_impl(lv_obj_t *screen, void (*update_line2)(lv_obj_t *line2));
bool      lichen_date_text_impl(char *out, size_t cap);
// raw_keys.c
bool      lichen_keys_raw_take_impl(lichen_raw_key_fn fn, void *arg);
void      lichen_keys_raw_release_impl(lichen_raw_key_fn fn, void *arg);
