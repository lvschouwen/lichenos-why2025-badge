#pragma once

// Heap inspection for the shell and main (#228, #216 decision 1): free and largest-block figures for the About and
// Benchmark screens and the boot log, and the diagnostic walk of the UI check. System surface: tools/layers.conf forbids
// lichen_sys_* in L6. Allocation is lichen_alloc (lichen_mem.h), never this.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LICHEN_SYS_MEM_DEFAULT,  // what malloc() draws from
    LICHEN_SYS_MEM_INTERNAL, // internal RAM, byte-addressable
    LICHEN_SYS_MEM_DMA,      // internal RAM a DMA engine can reach
    LICHEN_SYS_MEM_PSRAM,
} lichen_sys_mem_kind_t;

// The figures below: any task but not an interrupt.
// Free bytes of that heap. Any task.
size_t lichen_sys_mem_free(lichen_sys_mem_kind_t kind);
// Largest block that can still be allocated. Any task.
size_t lichen_sys_mem_largest(lichen_sys_mem_kind_t kind);
// Size of that heap; 0 when the badge has none (no PSRAM). Any task.
size_t lichen_sys_mem_total(lichen_sys_mem_kind_t kind);
// Free internal bytes now and the least there ever were since boot. Any task.
void lichen_sys_mem_internal(size_t *free_now, size_t *free_min);

// The least free bytes that heap ever had since boot. Any task.
size_t lichen_sys_mem_min(lichen_sys_mem_kind_t kind);

typedef struct {
    uint16_t in_use, peak; // blocks handed out now; the most there ever were since boot
    uint32_t waits, drops; // TX: the sender waited for a block; drops: TX still empty after the wait, RX dropped
} lichen_sys_frame_pool_t;
// One Wi-Fi frame pool of the C6 link (#260), for the stress run (#278): how deep it got and how often a frame was
// dropped. False (zeros) when the pools are off. Any task.
bool lichen_sys_mem_frame_pool(bool tx, lichen_sys_frame_pool_t *out);

typedef struct {
    char     name[16];
    uint32_t free, size;
} lichen_sys_stack_t;
// Every task's stack (#278): its name, the bytes it never used and its size (0 when the OS did not start it through
// lichen_task_start: drivers, the IDF, services). Fills up to cap entries and returns how many tasks there are (more
// than cap when out was short; 0 without memory for the snapshot). A diagnostic, not for a tight loop. Any task.
size_t lichen_sys_mem_stacks(lichen_sys_stack_t *out, size_t cap);

// A block of the walk: its address and size, whether it is in use
typedef bool (*lichen_sys_mem_walk_fn)(void const *ptr, size_t size, bool used, void *user);
// Calls fn for every block of the heap, until it returns false. fn runs with that heap locked: no allocation, no
// blocking. Diagnostic: any task but an interrupt.
void lichen_sys_mem_walk(lichen_sys_mem_kind_t kind, lichen_sys_mem_walk_fn fn, void *user);
// Heap integrity check (true = sound, details are logged); slow, a diagnostic. Any task but an interrupt.
bool lichen_sys_mem_check(lichen_sys_mem_kind_t kind);

// The OS's lichen_alloc and lichen_free for the gate's own widgets (#296, gate_mem.h): the gate may not call lichen_os,
// so lichen_os hands them over. ctx is a lichen_ctx_t (lichen_app.h).
typedef struct {
    void *(*alloc)(uint32_t ctx, size_t size, uint32_t flags);
    void (*release)(uint32_t ctx, void *ptr);
} lichen_sys_mem_ops_t;
// The OS's table, or NULL to take it away (host tests only: a block made before must not outlive the table). ops must
// outlive every call (a static const). Defined by the gate (gate_mem.c). Any task (an atomic store).
void lichen_sys_mem_ops_set(lichen_sys_mem_ops_t const *ops);
// main.c, the first boot step that can: hands lichen_alloc and lichen_free to the gate, before any screen exists. Until
// then the gate's widgets get no memory. Any task.
void lichen_sys_mem_start(void);
// main.c's apps boot step, once every app registered (#231): each app's declared PSRAM budget becomes its PSRAM quota in
// the memory book, accounted (a charge past it is granted and counted in "fw mem"), not enforced. Any task.
void lichen_sys_mem_quotas(void);

#ifdef __cplusplus
}
#endif
