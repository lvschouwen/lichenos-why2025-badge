#pragma once
// The gate's own heap (#296): what the gate's widgets allocate (the menu's rows and its danger dialog's text, a menu
// filter) is counted in the memory book under whoever built it, through lichen_alloc. lichen_alloc is lichen_os's (L3)
// and the gate (L4) may not call it, so lichen_os hands its alloc and free over at boot (lichen_sys_mem_ops_set,
// lichen_sys_mem.h) and these calls forward to them. Before that, every allocation here is NULL: never a plain calloc,
// whose memory "fw mem" cannot see. Pure (no LVGL, no heap of its own): host_tests/gate_mem_test.c.
//
// The owner is lichen_ctx_self() when the block is made: the app inside its hooks, its tasks and (#349) the event
// callbacks it registered (a submenu opened on a click). The caller keeps it next to the block and frees with that
// one, never with lichen_ctx_self() at the free: the delete may run in another context (a raw OS handler, a screen
// closed from another app's callback), and the owner's count would stay up.

#include "lichen_app.h"

#include <stdbool.h>
#include <stddef.h>

// Longest text gate_mem_strdup keeps, in bytes without the terminator: a dialog line, never a document
#define GATE_MEM_TEXT_MAX 255

// Two strings made and freed together (a danger row's question and confirm label), with the ctx they are charged to
typedef struct {
    lichen_ctx_t owner; // lichen_ctx_self() when they were set
    char        *a, *b; // NULL for one that was not given
} gate_mem_pair_t;

// A zeroed block in PSRAM (LICHEN_MEM_BULK | LICHEN_MEM_ZERO) charged to lichen_ctx_self(), which *owner gets for the
// free. NULL (and *owner untouched) when the OS has not handed over its heap yet or there is no room. Any task.
void *gate_mem_new(size_t size, lichen_ctx_t *owner);
// Frees what gate_mem_new or gate_mem_strdup gave owner (NULL is fine). Any task.
void gate_mem_free(lichen_ctx_t owner, void *ptr);
// A copy of s charged to owner, cut at GATE_MEM_TEXT_MAX bytes (never inside a UTF-8 character). NULL for a NULL s,
// before the OS's heap is up, or without room. Any task.
char *gate_mem_strdup(lichen_ctx_t owner, char const *s);
// Replaces the pair's strings with copies of a and b (either may be NULL), charged to lichen_ctx_self(). The new
// copies are made before the old ones go: false when one could not be made, and then the pair is as it was and
// nothing leaked. Any task.
bool gate_mem_pair_set(gate_mem_pair_t *pair, char const *a, char const *b);
// Frees both strings with the ctx they were charged to and leaves the pair empty. Any task.
void gate_mem_pair_clear(gate_mem_pair_t *pair);
