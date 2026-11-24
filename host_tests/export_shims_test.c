// The malloc family an SD app gets (#519, export_shims.c): bulk memory through the OS's book under the caller's context.
// lichen_alloc and friends are recorded fakes here; the book itself is tested elsewhere.
#include "lichen_app.h"
#include "export_blocks.h"
#include "lichen_mem.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *lichen_exp_malloc(size_t size);
void *lichen_exp_calloc(size_t n, size_t size);
void *lichen_exp_realloc(void *ptr, size_t size);
void  lichen_exp_free(void *ptr);

static lichen_ctx_t const CTX = (lichen_ctx_t)0x1234;
static uint32_t           last_flags;
static lichen_ctx_t       last_ctx, free_ctx;
static int                frees, fail_alloc;

static lichen_ctx_t const OS_CTX = (lichen_ctx_t)0x1;
static lichen_ctx_t       self_ctx = CTX;
lichen_ctx_t lichen_ctx_self(void) {
    return self_ctx;
}
lichen_ctx_t lichen_sys_ctx_os(void) {
    return OS_CTX;
}

// 8 bytes in front hold the size, as heap_caps_get_allocated_size would answer
void *lichen_alloc(lichen_ctx_t ctx, size_t size, uint32_t flags) {
    last_ctx   = ctx;
    last_flags = flags;
    if (fail_alloc) {
        return NULL;
    }
    uint64_t *p = malloc(size + 8);
    if (!p) {
        abort();
    }
    p[0] = size;
    if (flags & LICHEN_MEM_ZERO) {
        memset(p + 1, 0, size);
    } else {
        memset(p + 1, 0xA5, size);
    }
    return p + 1;
}

void lichen_free(lichen_ctx_t ctx, void *ptr) {
    free_ctx = ctx;
    if (ptr) {
        frees++;
        free((uint64_t *)ptr - 1);
    }
}

size_t heap_caps_get_allocated_size(void *ptr) {
    return (size_t)((uint64_t *)ptr)[-1];
}

// The loader's view of the blocks (#536): what the shims tell it
static void *noted[16], *forgotten[16];
static int   n_noted, n_forgotten;
void sd_app_block_note(lichen_ctx_t ctx, void *ptr) {
    (void)ctx;
    if (n_noted < 16) {
        noted[n_noted++] = ptr;
    }
}
void sd_app_block_forget(void *ptr) {
    if (n_forgotten < 16) {
        forgotten[n_forgotten++] = ptr;
    }
}

static int fails;

static void check(int ok, char const *what) {
    if (!ok) {
        fails++;
        printf("FAIL: %s\n", what);
    }
}

int main(void) {
    unsigned char *p = lichen_exp_malloc(16);
    check(p && last_ctx == CTX && last_flags == LICHEN_MEM_BULK, "malloc: bulk, under the caller's context");
    check(n_noted == 1 && noted[0] == p, "malloc: the loader is told of the block");
    lichen_exp_free(p);
    check(free_ctx == CTX && frees == 1, "free: the caller's context");
    check(n_forgotten == 1 && forgotten[0] == p, "free: the loader forgets the block");
    lichen_exp_free(NULL);
    check(n_forgotten == 1, "free(NULL) forgets nothing");
    fail_alloc = 1;
    check(lichen_exp_malloc(8) == NULL && n_noted == 1, "a failed malloc notes nothing");
    fail_alloc = 0;
    self_ctx = OS_CTX; // a malloc as the OS (a delete callback after a card pull): the block is the app's, nothing is noted
    p = lichen_exp_malloc(8);
    check(p && n_noted == 1, "malloc as the OS context notes nothing");
    lichen_exp_free(p);
    self_ctx = CTX;
    n_forgotten--; // the forget above is the free's, kept out of the balance below

    p = lichen_exp_calloc(4, 8);
    check(p && (last_flags & LICHEN_MEM_ZERO) && p[31] == 0, "calloc: zeroed");
    lichen_exp_free(p);
    check(lichen_exp_calloc(SIZE_MAX / 2, 4) == NULL, "calloc: n * size overflow is refused");

    p = lichen_exp_realloc(NULL, 8);
    check(p != NULL, "realloc(NULL) is malloc");
    memcpy(p, "ABCDEFGH", 8);
    p = lichen_exp_realloc(p, 32);
    check(p && !memcmp(p, "ABCDEFGH", 8), "realloc grow keeps the bytes");
    p = lichen_exp_realloc(p, 4);
    check(p && !memcmp(p, "ABCD", 4), "realloc shrink keeps the head");
    fail_alloc = 1;
    unsigned char *q = lichen_exp_realloc(p, 64);
    check(q == NULL && !memcmp(p, "ABCD", 4), "failed realloc leaves the old block valid");
    fail_alloc = 0;
    int before = frees;
    check(lichen_exp_realloc(p, 0) == NULL && frees == before + 1, "realloc(p, 0) frees");

    check(n_noted == n_forgotten, "every block noted is forgotten by the end (realloc swaps old for new)");

    printf("export_shims_test: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
