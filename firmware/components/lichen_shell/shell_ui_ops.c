// The shell's table for the gate's lichen_ui.h / lichen_keys.h calls (#221): see shell_ui_ops.h and lichen_ui_ops.h
#include "shell_ui_ops.h"

#include "lichen_sys_ui.h"

// The gate's widgets run on the LVGL task, which holds the lock: no wait (0 is "forever" only off that task)
static bool later(void (*fn)(void *), void *arg) {
    return lichen_sys_ui_call(fn, arg, 0);
}

static lichen_ui_ops_t const ops = {
    .later            = later,
    .later_cancel     = lichen_sys_ui_cancel,
    .home             = shell_home_impl,
    .back             = shell_back_impl,
    .open_keyed       = shell_open_keyed_impl,
    .open_reuse_count = shell_open_reuse_count_impl,
    .open_count_reuse = shell_open_count_reuse_impl,
    .screen_open      = shell_screen_open_impl,
    .on_esc           = shell_on_esc_impl,
    .on_search        = shell_on_search_impl,
    .scroll_by        = shell_scroll_by_impl,
    .page_create      = shell_page_create_impl,
    .scroll_tame      = shell_scroll_tame_impl,
    .no_arrow_scroll  = shell_no_arrow_scroll_impl,
    .back_then        = shell_back_then_impl,
    .back_to          = shell_back_to_impl,
    .stack_screen     = shell_stack_screen_impl,
    .style_screen     = shell_style_screen_impl,
    .dialog_countdown = shell_dialog_countdown_impl,
    .dialog_close     = shell_dialog_close_impl,
    .screen_locked    = lichen_screen_locked_impl,
    .station_clock    = lichen_station_clock_impl,
    .date_text        = lichen_date_text_impl,
    .keys_raw_take    = lichen_keys_raw_take_impl,
    .keys_raw_release = lichen_keys_raw_release_impl,
};

void shell_ui_ops_register(void) {
    lichen_ui_ops_set(&ops);
}
