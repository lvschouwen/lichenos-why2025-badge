// Host test: the gate's widgets allocate through the OS's lichen_alloc table, charged to the ctx that built them and
// freed under that same ctx wherever the free runs (#296)
#include "gate_mem.h"

#include "lichen_mem.h"
#include "lichen_sys_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define CTX_OS  ((lichen_ctx_t)0x01000001u)
#define CTX_APP ((lichen_ctx_t)0x02000005u)

// lichen_app.c's lichen_ctx_self, faked: who the calling code runs for
static lichen_ctx_t self = CTX_OS;
lichen_ctx_t lichen_ctx_self(void) {
    return self;
}

// A fake memory book: every live block with the ctx it was charged to
#define MAX_BLOCKS 32
static struct {
    void        *ptr;
    lichen_ctx_t ctx;
} blocks[MAX_BLOCKS];
static int  wrong_owner; // a free under another ctx than the alloc's (what lichen_free logs as MEM_RELEASE_OVER)
static int  bad_flags;
static int  fail_after = -1; // allocations left before one fails; -1: never
static char trace[64];       // 'a' per allocation, 'f' per free, in order

static void note(char c) {
    size_t n = strlen(trace);
    if (n + 1 < sizeof(trace)) {
        trace[n]     = c;
        trace[n + 1] = '\0';
    }
}

static void *fake_alloc(uint32_t ctx, size_t size, uint32_t flags) {
    if (flags != (LICHEN_MEM_BULK | LICHEN_MEM_ZERO)) {
        bad_flags++;
    }
    if (fail_after == 0) {
        return NULL;
    }
    if (fail_after > 0) {
        fail_after--;
    }
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (!blocks[i].ptr) {
            blocks[i].ptr = calloc(1, size);
            blocks[i].ctx = ctx;
            note('a');
            return blocks[i].ptr;
        }
    }
    return NULL;
}

static void fake_release(uint32_t ctx, void *ptr) {
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (blocks[i].ptr == ptr) {
            wrong_owner += blocks[i].ctx != ctx;
            free(ptr);
            blocks[i].ptr = NULL;
            note('f');
            return;
        }
    }
    wrong_owner++; // never allocated here
}

static lichen_sys_mem_ops_t const fake_ops = {.alloc = fake_alloc, .release = fake_release};

static int held(lichen_ctx_t ctx) {
    int n = 0;
    for (int i = 0; i < MAX_BLOCKS; i++) {
        n += blocks[i].ptr && blocks[i].ctx == ctx;
    }
    return n;
}

static void reset(void) {
    lichen_sys_mem_ops_set(&fake_ops);
    self        = CTX_OS;
    wrong_owner = bad_flags = 0;
    fail_after  = -1;
    trace[0]    = '\0';
}

// The shapes menu.c keeps: a menu's data and a row, each with the owner beside it
typedef struct {
    lichen_ctx_t owner;
    int          hook;
} fake_menu_t;
typedef struct {
    lichen_ctx_t    owner;
    gate_mem_pair_t danger;
} fake_row_t;

static void row_delete(fake_row_t *row) { // menu.c's on_delete
    gate_mem_pair_clear(&row->danger);
    gate_mem_free(row->owner, row);
}

// An app builds a menu with rows and a danger row; LVGL deletes it later, as the OS: the app's count is back to 0
static void test_app_menu_freed_as_os(void) {
    reset();
    self = CTX_APP;
    lichen_ctx_t owner = LICHEN_CTX_NONE;
    fake_menu_t *m     = gate_mem_new(sizeof(*m), &owner);
    CHECK(m && owner == CTX_APP);
    if (!m) {
        return;
    }
    m->owner = owner;
    fake_row_t *rows[3];
    for (int i = 0; i < 3; i++) {
        lichen_ctx_t ro = LICHEN_CTX_NONE;
        rows[i]         = gate_mem_new(sizeof(*rows[i]), &ro);
        CHECK(rows[i] && ro == CTX_APP && rows[i]->danger.a == NULL);
        if (!rows[i]) {
            return;
        }
        rows[i]->owner = ro;
    }
    CHECK(gate_mem_pair_set(&rows[1]->danger, "Erase every contact?", "erase"));
    CHECK(strcmp(rows[1]->danger.a, "Erase every contact?") == 0 && strcmp(rows[1]->danger.b, "erase") == 0);
    CHECK(gate_mem_pair_set(&rows[2]->danger, "Forget the channel?", NULL));
    CHECK(rows[2]->danger.b == NULL);
    CHECK(held(CTX_APP) == 7 && held(CTX_OS) == 0);

    self = CTX_OS; // LV_EVENT_DELETE handlers run in LVGL context
    for (int i = 0; i < 3; i++) {
        row_delete(rows[i]);
    }
    gate_mem_free(m->owner, m);
    CHECK(held(CTX_APP) == 0 && held(CTX_OS) == 0);
    CHECK(wrong_owner == 0 && bad_flags == 0);
}

// A menu built inside an LVGL event callback (a submenu opened on a click) is the OS's: shown, not hidden
static void test_lvgl_context_build_is_the_os(void) {
    reset();
    lichen_ctx_t owner = LICHEN_CTX_NONE;
    fake_row_t  *row   = gate_mem_new(sizeof(*row), &owner);
    CHECK(row && owner == CTX_OS && held(CTX_OS) == 1 && held(CTX_APP) == 0);
    if (!row) {
        return;
    }
    row->owner = owner;
    row_delete(row);
    CHECK(held(CTX_OS) == 0 && wrong_owner == 0);
}

// Before lichen_os hands its heap over: NULL, never a plain calloc, and owner untouched
static void test_ops_not_up(void) {
    reset();
    lichen_sys_mem_ops_set(NULL);
    lichen_ctx_t owner = 0x55;
    CHECK(gate_mem_new(16, &owner) == NULL && owner == 0x55);
    CHECK(gate_mem_strdup(CTX_OS, "x") == NULL);
    gate_mem_pair_t pair = {0};
    CHECK(!gate_mem_pair_set(&pair, "q", NULL) && pair.a == NULL);
    CHECK(gate_mem_pair_set(&pair, NULL, NULL)); // nothing to make
    gate_mem_free(CTX_OS, NULL);
    CHECK(trace[0] == '\0');
}

// A replace makes the new words before the old go, frees the old under their own ctx, and a failure keeps the pair
static void test_pair_replace(void) {
    reset();
    self                 = CTX_APP;
    gate_mem_pair_t pair = {0};
    CHECK(gate_mem_pair_set(&pair, "old question", "old"));
    char *old_a = pair.a, *old_b = pair.b;

    trace[0]   = '\0';
    fail_after = 1; // the confirm label's copy fails
    CHECK(!gate_mem_pair_set(&pair, "new question", "new"));
    CHECK(pair.a == old_a && pair.b == old_b && pair.owner == CTX_APP);
    CHECK(held(CTX_APP) == 2 && strcmp(trace, "af") == 0); // the one copy made went again, the old pair stayed

    fail_after = -1;
    trace[0]   = '\0';
    self       = CTX_OS; // set again from an LVGL callback: the new words are the OS's, the old go back to the app
    CHECK(gate_mem_pair_set(&pair, "new question", "new"));
    CHECK(strcmp(trace, "aaff") == 0);
    CHECK(pair.owner == CTX_OS && strcmp(pair.a, "new question") == 0 && strcmp(pair.b, "new") == 0);
    CHECK(held(CTX_APP) == 0 && held(CTX_OS) == 2);
    gate_mem_pair_clear(&pair);
    CHECK(pair.a == NULL && pair.b == NULL && held(CTX_OS) == 0 && wrong_owner == 0);
}

// The copy is bounded and never ends inside a UTF-8 character
static void test_strdup_bounded(void) {
    reset();
    char long_text[400];
    memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    char *c = gate_mem_strdup(CTX_OS, long_text);
    CHECK(c && strlen(c) == GATE_MEM_TEXT_MAX);
    gate_mem_free(CTX_OS, c);

    char utf[GATE_MEM_TEXT_MAX + 4];
    memset(utf, 'x', GATE_MEM_TEXT_MAX - 1);
    memcpy(utf + GATE_MEM_TEXT_MAX - 1, "\xC3\xA9yz", 5); // 'é' straddles the limit
    c = gate_mem_strdup(CTX_OS, utf);
    CHECK(c && strlen(c) == GATE_MEM_TEXT_MAX - 1);
    gate_mem_free(CTX_OS, c);

    c = gate_mem_strdup(CTX_OS, "short");
    CHECK(c && strcmp(c, "short") == 0);
    gate_mem_free(CTX_OS, c);
    CHECK(gate_mem_strdup(CTX_OS, NULL) == NULL);
    CHECK(held(CTX_OS) == 0 && wrong_owner == 0);
}

int main(void) {
    test_app_menu_freed_as_os();
    test_lvgl_context_build_is_the_os();
    test_ops_not_up();
    test_pair_replace();
    test_strdup_bounded();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("gate_mem_test: all passed\n");
    return 0;
}
