// The cells MeshCore binds for the shell's remote key checks (#377): one function pointer each, atomic so the binding
// task and the reader need no lock
#include "bench_scope.h"

static bench_scope_fn bound_scope;
static bench_chat_fn  bound_chat;
static bench_starts_fn bound_starts;

void bench_scope_bind(bench_scope_fn fn) {
    __atomic_store_n(&bound_scope, fn, __ATOMIC_RELEASE);
}

bool bench_scope_set(void) {
    bench_scope_fn fn = __atomic_load_n(&bound_scope, __ATOMIC_ACQUIRE);
    return fn != NULL && fn();
}

void bench_chat_bind(bench_chat_fn fn) {
    __atomic_store_n(&bound_chat, fn, __ATOMIC_RELEASE);
}

void bench_starts_bind(bench_starts_fn fn) {
    __atomic_store_n(&bound_starts, fn, __ATOMIC_RELEASE);
}

bool bench_enter_starts(void) {
    bench_starts_fn fn = __atomic_load_n(&bound_starts, __ATOMIC_ACQUIRE);
    return fn != NULL && fn();
}

bool bench_chat_on_top(char *name, size_t cap, bool *dm) {
    bench_chat_fn fn = __atomic_load_n(&bound_chat, __ATOMIC_ACQUIRE);
    if (cap) {
        name[0] = '\0';
    }
    *dm = false;
    if (!fn || !cap || !fn(name, cap, dm)) {
        if (cap) {
            name[0] = '\0';
        }
        *dm = false;
        return false;
    }
    name[cap - 1] = '\0';
    return true;
}
