// The bench console scope cell (#377): one function pointer, atomic so the binding task and the reader need no lock
#include "bench_scope.h"

#include <stddef.h>

static bench_scope_fn bound;

void bench_scope_bind(bench_scope_fn fn) {
    __atomic_store_n(&bound, fn, __ATOMIC_RELEASE);
}

bool bench_scope_set(void) {
    bench_scope_fn fn = __atomic_load_n(&bound, __ATOMIC_ACQUIRE);
    return fn != NULL && fn();
}
