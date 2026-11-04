// lichen_task_start's bookkeeping (#181), see task_book.h. Pure: no FreeRTOS here.
#include "task_book.h"

#include <string.h>

static struct {
    int8_t prio, core;
} const classes[TASK_CLASS_COUNT] = {
    [TASK_CLASS_BACKGROUND] = {2, 0},
    [TASK_CLASS_SERVICE]    = {3, 0},
    [TASK_CLASS_JOB]        = {4, 0},
    [TASK_CLASS_URGENT]     = {5, 0},
    [TASK_CLASS_APP_LOOP]   = {3, 1},
};

int task_class_priority(task_class_t cls) {
    return (unsigned)cls < TASK_CLASS_COUNT ? classes[cls].prio : -1;
}

int task_class_core(task_class_t cls) {
    return (unsigned)cls < TASK_CLASS_COUNT ? classes[cls].core : -1;
}

bool task_name_fit(char out[TASK_BOOK_NAME_CAP], char const *name) {
    if (!name) {
        name = "";
    }
    size_t n   = strlen(name);
    bool   cut = n >= TASK_BOOK_NAME_CAP;
    if (cut) {
        n = TASK_BOOK_NAME_CAP - 1;
    }
    memcpy(out, name, n);
    out[n] = 0;
    return cut;
}

static void owner_fit(char out[TASK_BOOK_OWNER_CAP], char const *owner) {
    if (!owner) {
        owner = TASK_BOOK_OS;
    }
    size_t n = strlen(owner);
    if (n >= TASK_BOOK_OWNER_CAP) {
        n = TASK_BOOK_OWNER_CAP - 1;
    }
    memcpy(out, owner, n);
    out[n] = 0;
}

int task_book_add(task_book_t *b, char const *owner, char const *name, uint32_t stack, task_class_t cls,
                  void (*fn)(void *), void *arg) {
    if ((unsigned)cls >= TASK_CLASS_COUNT) {
        return -1;
    }
    for (int i = 0; i < TASK_BOOK_SLOTS; i++) {
        task_slot_t *s = &b->slot[i];
        if (s->used) {
            continue;
        }
        memset(s, 0, sizeof(*s));
        s->used = true;
        owner_fit(s->owner, owner);
        task_name_fit(s->name, name);
        s->stack = stack;
        s->cls   = (uint8_t)cls;
        s->fn    = fn;
        s->arg   = arg;
        return i;
    }
    return -1;
}

void task_book_remove(task_book_t *b, int slot) {
    if (slot >= 0 && slot < TASK_BOOK_SLOTS) {
        memset(&b->slot[slot], 0, sizeof(b->slot[slot]));
    }
}

size_t task_book_count(task_book_t const *b, char const *owner, void const *skip) {
    char want[TASK_BOOK_OWNER_CAP];
    owner_fit(want, owner);
    size_t n = 0;
    for (int i = 0; i < TASK_BOOK_SLOTS; i++) {
        task_slot_t const *s = &b->slot[i];
        n += s->used && strcmp(s->owner, want) == 0 && (!skip || s->handle != skip);
    }
    return n;
}
