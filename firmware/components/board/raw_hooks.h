#pragma once

#include "key_raw.h"

#include <stdatomic.h>
#include <stdbool.h>

// The raw key hook table behind keyboard_raw_add/remove (#81, #251). Pure (host_tests/raw_hooks_test.c): no lock, only
// C11 atomics, so the keyboard task that runs the hooks never blocks on a task that adds or removes one.
//
// The rule: once raw_hooks_remove(fn, arg, wait) returns with a wait function, no call of that hook is running and none
// will start, so the hook's owner may free what arg or the hook touches (e.g. a task handle). Each slot counts the calls
// in flight; remove takes the hook out of the table first, then waits for that count to reach zero.
//
// A hook that removes itself (or another hook) runs on the calling task, and that call is still in flight: it passes
// wait = NULL, which removes without waiting (waiting there would wait for itself forever).
#define RAW_HOOKS_MAX 4

typedef struct {
    atomic_int          state;    // raw_hook_state_t
    _Atomic(key_raw_fn) fn;       // written only while claiming: stable while live or removing
    void *_Atomic       arg;
    atomic_uint         inflight; // raw_hooks_run is between looking at this slot and leaving it
} raw_hook_slot_t;

typedef struct {
    raw_hook_slot_t slot[RAW_HOOKS_MAX];
} raw_hooks_t; // all zero (static storage) is an empty table

// False when every slot is taken. Any task.
bool raw_hooks_add(raw_hooks_t *h, key_raw_fn fn, void *arg);
// Removes every slot holding (fn, arg). wait (e.g. a one-tick sleep) is called while a removed hook is still running;
// NULL only from inside a hook (the task that runs them), where it does not wait.
void raw_hooks_remove(raw_hooks_t *h, key_raw_fn fn, void *arg, void (*wait)(void));
// Offers ev to every live hook; true = one of them swallowed it. One task calls this (the keyboard task); it never
// blocks.
bool raw_hooks_run(raw_hooks_t *h, key_raw_event_t const *ev);
