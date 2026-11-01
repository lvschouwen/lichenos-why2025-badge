// #89 diagnostic: once a second, walk every LVGL object and verify the metadata the crashes died on, so damage is
// seen (and its heap neighbours named) while the object is still alive, instead of in a panic during a later delete.
//
// Three crashes (2026-10-31 22:40, 23:07; 2026-11-01 ~09:00) all hit objects in the PSRAM LVGL heap near 0x48c1c000:
// a spec_attr->children pointer outside every heap, then twice a NULL event descriptor in an event list. Whole words,
// not single bits, in boot-time-stable addresses. This walker checks exactly those two fields on every object of every
// screen, logs the first bad one with the 128-byte cache line around the bad slot and the heap blocks around it, and
// writes a restart-log note so "fw crash" shows the finding after the reboot that usually follows.
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_memory_utils.h"
#include "fw_console.h"
#include "lvgl.h"
#include "lvgl_private.h"
#include "shell_parts.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define PERIOD_MS     1000
#define MAX_DEPTH     48
#define MAX_REPORTS   3   // per boot: the first finding is the one that matters, the rest would flood the log
#define LINE_BYTES    128 // the P4's L2 cache line
#define NEAR_BYTES    512 // heap blocks within this distance of the bad slot are listed

static char const TAG[] = "ui_check";

static uint32_t walks, objects_last, reports;
static int64_t  last_walk_us;
static char     last_finding[160];

typedef struct {
    uintptr_t at;
    int       shown;
} near_ctx_t;

static bool in_ram(void const *p) {
    return p && ((uintptr_t)p & 3) == 0 && (esp_ptr_internal(p) || esp_ptr_external_ram(p));
}

// Heap blocks around the bad slot: the neighbour that overran or whose cache line was clipped shows up here
static bool near_cb(walker_heap_into_t heap, walker_block_info_t b, void *user) {
    (void)heap;
    near_ctx_t *ctx   = user;
    uintptr_t   start = (uintptr_t)b.ptr;
    uintptr_t   end   = start + b.size;
    if (end + NEAR_BYTES < ctx->at || start > ctx->at + NEAR_BYTES) {
        return true;
    }
    if (ctx->shown < 12) {
        ESP_LOGE(TAG, "  heap block %p..%p %u B %s%s", (void *)start, (void *)end, (unsigned)b.size,
                 b.used ? "used" : "free", start <= ctx->at && ctx->at < end ? "  <- holds the bad slot" : "");
        ctx->shown++;
    }
    return true;
}

static void dump_line(void const *slot) {
    uint8_t const *line = (uint8_t const *)((uintptr_t)slot & ~(uintptr_t)(LINE_BYTES - 1));
    char           hex[LINE_BYTES * 2 + 1];
    for (int i = 0; i < LINE_BYTES; i++) {
        snprintf(hex + 2 * i, 3, "%02x", line[i]);
    }
    ESP_LOGE(TAG, "  cache line %p (bad slot at +%u): %.128s", (void const *)line, (unsigned)((uintptr_t)slot - (uintptr_t)line), hex);
    ESP_LOGE(TAG, "                                      %s", hex + 128);
}

static bool group_exists(lv_group_t const *g) {
    lv_ll_t *ll = &LV_GLOBAL_DEFAULT()->group_ll; // no public iterator in LVGL 9; lv_group.c walks this list too
    for (lv_group_t *it = lv_ll_get_head(ll); it; it = lv_ll_get_next(ll, it)) {
        if (it == g) {
            return true;
        }
    }
    return false;
}

static void dump_spec_attr(lv_obj_spec_attr_t const *sa) {
    uint8_t const *b = (uint8_t const *)sa;
    char           hex[sizeof(*sa) * 2 + 1];
    for (size_t i = 0; i < sizeof(*sa); i++) {
        snprintf(hex + 2 * i, 3, "%02x", b[i]);
    }
    ESP_LOGE(TAG, "  spec_attr %p (%u B): %s", (void const *)sa, (unsigned)sizeof(*sa), hex);
}

static void report(lv_obj_t *obj, unsigned depth, char const *what, void const *slot, void const *value) {
    if (reports >= MAX_REPORTS) {
        return;
    }
    reports++;
    if (in_ram(obj->spec_attr)) {
        dump_spec_attr(obj->spec_attr);
    }
    lv_obj_t *parent = obj->parent;
    ESP_LOGE(TAG, "FOUND: obj %p (class %p, parent %p, depth %u, %u children): %s: slot %p holds %p, walk %" PRIu32 ", uptime %" PRId64 " ms",
             (void *)obj, (void const *)obj->class_p, (void *)parent, depth, obj->spec_attr ? obj->spec_attr->child_cnt : 0, what,
             slot, value, walks, esp_timer_get_time() / 1000);
    // Who is around it: the parent chain tells which screen, the heap blocks tell which allocation sits next to it
    lv_obj_t *p = parent;
    for (int i = 0; p && i < 6; i++) {
        ESP_LOGE(TAG, "  parent %d: %p class %p", i, (void *)p, (void const *)p->class_p);
        p = p->parent;
    }
    if (in_ram(slot)) {
        dump_line(slot);
    }
    near_ctx_t ctx = {.at = (uintptr_t)slot};
    heap_caps_walk(MALLOC_CAP_SPIRAM, near_cb, &ctx);
    if (ctx.shown == 0) {
        heap_caps_walk(MALLOC_CAP_INTERNAL, near_cb, &ctx);
    }
    snprintf(last_finding, sizeof(last_finding), "ui check: obj %p %s slot %p holds %p (walk %" PRIu32 ")", (void *)obj, what,
             slot, value, walks);
    system_restart_note(last_finding);
}

static void check_tree(lv_obj_t *obj, unsigned depth) {
    if (!in_ram(obj)) {
        return; // reported by the caller
    }
    objects_last++;
    lv_obj_spec_attr_t *sa = obj->spec_attr;
    if (sa) {
        if (!in_ram(sa)) {
            report(obj, depth, "spec_attr pointer not in RAM", &obj->spec_attr, sa);
            return;
        }
        if (sa->child_cnt && !in_ram(sa->children)) {
            report(obj, depth, "children pointer not in RAM", &sa->children, sa->children);
            return;
        }
        if (sa->group_p && !group_exists(sa->group_p)) { // crash 4: group_p held 0x200
            report(obj, depth, "group pointer is not a live group", &sa->group_p, sa->group_p);
            return;
        }
        lv_array_t *ev = &sa->event_list.array;
        uint32_t    n  = lv_array_size(ev);
        if (n > ev->capacity || ev->capacity > 64 || (n && ev->element_size != sizeof(void *))) {
            report(obj, depth, "event list size/capacity/element size are not sane", &ev->size, (void *)(uintptr_t)ev->size);
            return;
        }
        if (n) {
            if (!in_ram(ev->data)) {
                report(obj, depth, "event list array not in RAM", &ev->data, ev->data);
                return;
            }
            for (uint32_t i = 0; i < n; i++) {
                lv_event_dsc_t **slot = lv_array_at(ev, i);
                lv_event_dsc_t  *dsc  = slot ? *slot : NULL;
                if (!in_ram(dsc)) {
                    report(obj, depth, dsc ? "event descriptor not in RAM" : "NULL event descriptor", slot, dsc);
                    return;
                }
                if (!dsc->cb && !(dsc->filter & LV_EVENT_MARKED_DELETING)) {
                    report(obj, depth, "event descriptor without callback", &dsc->cb, NULL);
                    return;
                }
            }
        }
        if (depth < MAX_DEPTH) {
            for (uint32_t i = 0; i < sa->child_cnt; i++) {
                lv_obj_t *child = sa->children[i];
                if (!in_ram(child)) {
                    report(obj, depth, "child pointer not in RAM", &sa->children[i], child);
                    return;
                }
                check_tree(child, depth + 1);
            }
        }
    }
}

static void walk_all(void) {
    int64_t t0  = esp_timer_get_time();
    objects_last = 0;
    for (lv_display_t *disp = lv_display_get_next(NULL); disp; disp = lv_display_get_next(disp)) {
        for (uint32_t i = 0; i < disp->screen_cnt; i++) { // layers are screens too
            lv_obj_t *scr = disp->screens[i];
            if (!in_ram(scr)) {
                ESP_LOGE(TAG, "FOUND: display %p screen %" PRIu32 " pointer %p not in RAM", (void *)disp, i, (void *)scr);
                continue;
            }
            check_tree(scr, 0);
        }
    }
    walks++;
    last_walk_us = esp_timer_get_time() - t0;
}

// With CONFIG_HEAP_POISONING_LIGHT every block carries head and tail canaries: an overrun from a neighbour is seen
// here, at the victim, before anything reads the damaged words (a double free aborts in heap_caps_free itself)
static void heap_tick(void) {
    static uint32_t n;
    if (++n % 10) {
        return;
    }
    if (!heap_caps_check_integrity(MALLOC_CAP_SPIRAM, true)) {
        ESP_LOGE(TAG, "FOUND: PSRAM heap integrity check failed (details above), walk %" PRIu32, walks);
        if (reports < MAX_REPORTS) {
            reports++;
            snprintf(last_finding, sizeof(last_finding), "ui check: PSRAM heap integrity failed (walk %" PRIu32 ")", walks);
            system_restart_note(last_finding);
        }
    }
}

static void tick(lv_timer_t *t) {
    (void)t;
    walk_all();
    heap_tick();
}

static void cmd_uicheck(char const *args) {
    (void)args;
    lvgl_port_lock(0); // console commands run on the console task, outside LVGL's lock
    walk_all();
    lvgl_port_unlock();
    console_reply("ui check: %" PRIu32 " walks, %" PRIu32 " objects in %" PRId64 " us, %" PRIu32 " finding(s)%s%s", walks, objects_last,
                  last_walk_us, reports, reports ? ": " : "", reports ? last_finding : "");
}

void ui_check_start(void) {
    lv_timer_create(tick, PERIOD_MS, NULL);
    console_register("uicheck", "uicheck (walk every LVGL object now: children and event lists, #89)", cmd_uicheck);
}
