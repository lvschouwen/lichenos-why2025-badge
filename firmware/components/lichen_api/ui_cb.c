#include "ui_cb.h"

static ui_cb_rec_t *rec_new(ui_cb_env_t const *env) {
    ui_cb_rec_t *rec = env->alloc(sizeof(*rec));
    if (rec) {
        rec->ctx = env->self(); // the registration's context: an app inside its hooks, its callbacks and its UI calls
    }
    return rec;
}

ui_cb_rec_t *ui_cb_new_event(ui_cb_env_t const *env, void (*fn)(struct _lv_event_t *e)) {
    ui_cb_rec_t *rec = fn ? rec_new(env) : NULL;
    if (rec) {
        rec->fn.event = fn;
    }
    return rec;
}

ui_cb_rec_t *ui_cb_new_timer(ui_cb_env_t const *env, void (*fn)(struct _lv_timer_t *t)) {
    ui_cb_rec_t *rec = fn ? rec_new(env) : NULL;
    if (rec) {
        rec->fn.timer = fn;
    }
    return rec;
}

void ui_cb_free(ui_cb_env_t const *env, void *rec) {
    if (rec) {
        env->free(rec);
    }
}

// Both copy what they need first: the callback may delete its object or timer, and LVGL then frees the record
void ui_cb_run_event(ui_cb_env_t const *env, ui_cb_rec_t const *rec, struct _lv_event_t *e) {
    if (!rec) {
        return;
    }
    void (*fn)(struct _lv_event_t *) = rec->fn.event;
    uint32_t previous                = env->enter(rec->ctx);
    fn(e);
    env->leave(previous);
}

void ui_cb_run_timer(ui_cb_env_t const *env, ui_cb_rec_t const *rec, struct _lv_timer_t *t) {
    if (!rec) {
        return;
    }
    void (*fn)(struct _lv_timer_t *) = rec->fn.timer;
    uint32_t previous                = env->enter(rec->ctx);
    fn(t);
    env->leave(previous);
}
