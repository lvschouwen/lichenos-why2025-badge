// "fw mem diff" (#547): the internal heap's allocated blocks at "mark" against now, so the bench can see what an action
// (the first SD0: stream, an unmount) left behind: the blocks that appeared with their size and address, and the totals.
// The two lists live in PSRAM: they must not move the internal figures they measure.
#include "mem_diff_cmd.h"

#include "sdkconfig.h"

#ifdef CONFIG_LICHEN_DEV_COMMANDS

#include "esp_heap_caps.h"
#include "fw_console.h"
#include "mem_diff.h"

#include <stdlib.h>
#include <string.h>

#define SNAP_MAX   3072 // blocks; 24 KB each list
#define LINES_MAX  24   // blocks named per diff, in address order (sorting by size is not worth the code)
#define MIN_DEFAULT 64

typedef struct {
    mem_blk_t *v;
    size_t     n;
    bool       full;
} snap_t;

static snap_t mark;

static bool snap_cb(walker_heap_into_t heap, walker_block_info_t b, void *user) {
    (void)heap;
    snap_t *s = user;
    if (b.used) {
        if (s->n >= SNAP_MAX) {
            s->full = true;
            return false;
        }
        s->v[s->n++] = (mem_blk_t){.addr = (uint32_t)(uintptr_t)b.ptr, .size = (uint32_t)b.size};
    }
    return true;
}

// The walk holds the heap's lock and allocates nothing; the buffer is made before it
static bool take(snap_t *s) {
    s->v = heap_caps_malloc(SNAP_MAX * sizeof(mem_blk_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->v) {
        return false;
    }
    s->n    = 0;
    s->full = false;
    heap_caps_walk(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, snap_cb, s);
    mem_blk_sort(s->v, s->n);
    return true;
}

typedef struct {
    uint32_t min;
    unsigned listed, hidden;
} print_t;

static void print_one(mem_diff_kind_t kind, mem_blk_t const *was, mem_blk_t const *now, void *user) {
    print_t *p    = user;
    uint32_t size = kind == MEM_DIFF_REMOVED ? was->size : now->size;
    if (size < p->min && !(kind == MEM_DIFF_RESIZED && was->size >= p->min)) {
        return;
    }
    if (p->listed >= LINES_MAX) {
        p->hidden++;
        return;
    }
    p->listed++;
    if (kind == MEM_DIFF_RESIZED) {
        console_reply("mem diff resized %08x %u -> %u B", (unsigned)was->addr, (unsigned)was->size, (unsigned)now->size);
    } else {
        console_reply("mem diff %s %08x %u B", kind == MEM_DIFF_ADDED ? "added" : "removed",
                      (unsigned)(kind == MEM_DIFF_ADDED ? now->addr : was->addr), (unsigned)size);
    }
}

// Parses a plain decimal minimum; false for a sign, junk or an empty string
static bool parse_min(char const *s, uint32_t *out) {
    if (*s < '0' || *s > '9') {
        return false;
    }
    char         *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0' || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static void do_mark(void) {
    snap_t fresh = {0};
    if (!take(&fresh)) {
        heap_caps_free(fresh.v);
        console_reply("mem diff: no PSRAM for the list (old mark kept)");
        return;
    }
    heap_caps_free(mark.v); // only now: a failed take leaves the old mark
    mark = fresh;
    console_reply("mem diff: marked %u internal blocks%s", (unsigned)mark.n,
                  mark.full ? " (list full, the rest is not compared)" : "");
}

static void do_compare(char const *args) {
    uint32_t min = MIN_DEFAULT;
    if (*args && !parse_min(args, &min)) {
        console_reply("mem diff: usage: mem diff mark | mem diff [min bytes]");
        return;
    }
    if (!mark.v) {
        console_reply("mem diff: no mark yet (fw mem diff mark)");
        return;
    }
    snap_t now = {0};
    if (!take(&now)) {
        heap_caps_free(now.v);
        console_reply("mem diff: no PSRAM for the list");
        return;
    }
    print_t        p = {.min = min};
    mem_diff_sum_t sum;
    mem_diff(mark.v, mark.n, now.v, now.n, &sum, print_one, &p);
    bool partial = mark.full || now.full;
    console_reply("mem diff: added %u = %llu B, removed %u = %llu B, resized %u, net %lld B%s; %u of them >= %u B listed%s",
                  sum.added_n, (unsigned long long)sum.added_bytes, sum.removed_n, (unsigned long long)sum.removed_bytes,
                  sum.resized_n, (long long)sum.net, partial ? " (UNRELIABLE)" : "", p.listed, (unsigned)min,
                  p.hidden ? " (more not listed)" : "");
    if (partial) {
        console_reply("mem diff: a list was full (%u blocks): blocks past it are missing, so adds and removes may be false",
                      SNAP_MAX);
    }
    heap_caps_free(now.v);
}

void mem_diff_command(char const *args) {
    if (strncmp(args, "mark", 4) == 0 && (args[4] == '\0' || args[4] == ' ')) {
        do_mark();
    } else {
        do_compare(args);
    }
}

#endif
