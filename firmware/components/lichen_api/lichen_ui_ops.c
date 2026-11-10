// The gate's side of lichen_ui.h / lichen_keys.h (#221): each call is forwarded to the shell's table, see lichen_ui_ops.h.
// Pure (no LVGL call, no lock): host-tested by host_tests/ui_ops_test.c.
#include "lichen_ui_ops.h"

#include <stdatomic.h>

// The shell sets it on the boot task while other tasks may already call in (a c6link dialog, a console command), so
// the pointer is published with release and read with acquire: a reader that sees it sees the table's members too
static _Atomic(lichen_ui_ops_t const *) s_ops;

void lichen_ui_ops_set(lichen_ui_ops_t const *ops) {
    atomic_store_explicit(&s_ops, ops, memory_order_release);
}

// No table yet: every member NULL, so each call below gives its "not up" answer
static lichen_ui_ops_t const none;

// One load per call, so a call never mixes two tables
static lichen_ui_ops_t const *ops(void) {
    lichen_ui_ops_t const *o = atomic_load_explicit(&s_ops, memory_order_acquire);
    return o ? o : &none;
}

// The member of the shell's table, NULL when there is no table yet or the shell left it out
#define OP(member) (ops()->member)

bool lichen_ui_ops_later(void (*fn)(void *), void *arg) {
    bool (*later)(void (*)(void *), void *) = OP(later);
    return later && later(fn, arg);
}

void lichen_ui_ops_later_cancel(void (*fn)(void *), void *arg) {
    void (*cancel)(void (*)(void *), void *) = OP(later_cancel);
    if (cancel) {
        cancel(fn, arg);
    }
}

void shell_home(void) {
    void (*fn)(void) = OP(home);
    if (fn) {
        fn();
    }
}

void shell_back(void) {
    void (*fn)(void) = OP(back);
    if (fn) {
        fn();
    }
}

void shell_open(lv_obj_t *(*build)(void), int screen_id) {
    shell_open_keyed(build, screen_id, 0);
}

void shell_open_keyed(lv_obj_t *(*build)(void), int screen_id, uint32_t key) {
    void (*fn)(lv_obj_t *(*)(void), int, uint32_t) = OP(open_keyed);
    if (fn) {
        fn(build, screen_id, key);
    }
}

// FNV-1a, never SHELL_KEY_UNIQUE: pure, so it answers the same before the shell is up
uint32_t shell_key_hash(void const *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ ((uint8_t const *)data)[i]) * 16777619u;
    }
    return h == SHELL_KEY_UNIQUE ? 0 : h;
}

unsigned shell_open_reuse_count(void) {
    unsigned (*fn)(void) = OP(open_reuse_count);
    return fn ? fn() : 0;
}

void shell_open_count_reuse(void) {
    void (*fn)(void) = OP(open_count_reuse);
    if (fn) {
        fn();
    }
}

bool shell_screen_open(int screen_id, uint32_t key) {
    bool (*fn)(int, uint32_t) = OP(screen_open);
    return fn && fn(screen_id, key);
}

void shell_on_esc(bool (*esc)(void *arg), void *arg) {
    void (*fn)(bool (*)(void *), void *) = OP(on_esc);
    if (fn) {
        fn(esc, arg);
    }
}

void shell_on_search(void (*search)(void *arg), void *arg) {
    void (*fn)(void (*)(void *), void *) = OP(on_search);
    if (fn) {
        fn(search, arg);
    }
}

void shell_scroll_by(lv_obj_t *obj, int32_t dy) {
    void (*fn)(lv_obj_t *, int32_t) = OP(scroll_by);
    if (fn) {
        fn(obj, dy);
    }
}

lv_obj_t *shell_page_create(void) {
    lv_obj_t *(*fn)(void) = OP(page_create);
    return fn ? fn() : NULL;
}

void shell_scroll_tame(lv_obj_t *obj) {
    void (*fn)(lv_obj_t *) = OP(scroll_tame);
    if (fn) {
        fn(obj);
    }
}

void shell_no_arrow_scroll(lv_obj_t *obj) {
    void (*fn)(lv_obj_t *) = OP(no_arrow_scroll);
    if (fn) {
        fn(obj);
    }
}

bool shell_back_then(void (*then)(void *), void *arg) {
    bool (*fn)(void (*)(void *), void *) = OP(back_then);
    return fn && fn(then, arg);
}

bool shell_back_to(lv_obj_t *screen, void (*then)(void *), void *arg) {
    bool (*fn)(lv_obj_t *, void (*)(void *), void *) = OP(back_to);
    return fn && fn(screen, then, arg);
}

lv_obj_t *shell_stack_screen(int from_top) {
    lv_obj_t *(*fn)(int) = OP(stack_screen);
    return fn ? fn(from_top) : NULL;
}

void shell_style_screen(lv_obj_t *screen) {
    void (*fn)(lv_obj_t *) = OP(style_screen);
    if (fn) {
        fn(screen);
    }
}

bool shell_dialog_countdown(shell_dialog_t const *d, shell_dialog_fn done) {
    bool (*fn)(shell_dialog_t const *, shell_dialog_fn) = OP(dialog_countdown);
    return fn && fn(d, done);
}

void shell_dialog_close(void) {
    void (*fn)(void) = OP(dialog_close);
    if (fn) {
        fn();
    }
}

bool lichen_screen_locked(void) {
    bool (*fn)(void) = OP(screen_locked);
    return fn && fn();
}

bool lichen_station_clock(lv_obj_t *screen, void (*update_line2)(lv_obj_t *line2)) {
    bool (*fn)(lv_obj_t *, void (*)(lv_obj_t *)) = OP(station_clock);
    return fn && fn(screen, update_line2);
}

bool lichen_date_text(char *out, size_t cap) {
    bool (*fn)(char *, size_t) = OP(date_text);
    if (fn) {
        return fn(out, cap);
    }
    if (out && cap) {
        out[0] = '\0';
    }
    return false;
}

bool lichen_keys_raw_take(lichen_raw_key_fn take, void *arg) {
    bool (*fn)(lichen_raw_key_fn, void *) = OP(keys_raw_take);
    return fn && fn(take, arg);
}

void lichen_keys_raw_release(lichen_raw_key_fn release, void *arg) {
    void (*fn)(lichen_raw_key_fn, void *) = OP(keys_raw_release);
    if (fn) {
        fn(release, arg);
    }
}
