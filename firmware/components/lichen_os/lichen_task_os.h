#pragma once
// lichen_os's own task start (#523): lichen_task_start without the BACKGROUND check, for the tasks the OS starts for an app
// (the SD apps' main() and stop hook). Not part of the gate; apps never see it.

#include "lichen_task.h"

bool lichen_os_task_start_for(lichen_ctx_t ctx, char const *name, void (*fn)(void *arg), void *arg,
                              uint32_t stack_bytes, lichen_prio_t prio, lichen_stack_t mem, TaskHandle_t *handle);

// The calling task ends like a returning one (an SD app's exit(), #533, lichen_task.c). Does not return for a booked task;
// false for one that is not in the book.
bool lichen_os_task_end_self(void);

// One observer of "a booked task ended and its context has no task left" (#541: the SD apps' late unload). Called on the
// ending task, after it left the book and outside the book's lock, right before it is deleted: it may take a leaf mutex or post
// to a queue, never wait. Set once at boot.
void lichen_os_task_end_notify(void (*cb)(lichen_ctx_t ctx));
