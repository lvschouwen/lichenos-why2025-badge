// Host test: the settings backup swap (#453). A fake FAT card (rename refuses an existing target) with a failure injected
// at every file operation of a save, in two modes: a power cut (the operation and everything after it never happen; a
// write leaves a partial file) and a failed call (the operation does nothing, the code carries on). After each, a valid
// copy must still be readable and the choice must prefer the right one; a clean save afterwards must converge.
#include "settings_backup_swap.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define V0 "v0"
#define V1 "v1"
#define V2 "v2"
#define PARTIAL "v" // what a cut write leaves: not a valid copy

typedef struct {
    char file[SBS_COUNT][8]; // "" = absent
    bool present[SBS_COUNT];
    int  op;      // operations attempted so far
    int  fail_at; // 1-based, 0 = never
    bool cut;     // a cut at fail_at (else a failed call)
    bool dead;    // power is gone
} fake_t;

static bool gate(fake_t *f) { // false: this operation does not happen
    if (f->dead) {
        return false;
    }
    f->op++;
    if (f->op == f->fail_at) {
        f->dead = f->cut;
        return false;
    }
    return true;
}

static bool fk_write(void *c, sbs_name_t n, char const *buf, size_t len) {
    fake_t *f = c;
    bool    ok = gate(f);
    if (!ok && f->dead && f->fail_at == f->op) { // the cut left a partial file
        f->present[n] = true;
        snprintf(f->file[n], sizeof(f->file[n]), "%s", PARTIAL);
        return false;
    }
    f->present[n] = true;
    if (!ok) { // a failed write: the file exists, truncated
        snprintf(f->file[n], sizeof(f->file[n]), "%s", PARTIAL);
        return false;
    }
    snprintf(f->file[n], sizeof(f->file[n]), "%.*s", (int)len, buf);
    return true;
}

static bool fk_rename(void *c, sbs_name_t from, sbs_name_t to) {
    fake_t *f = c;
    if (!gate(f) || !f->present[from] || f->present[to]) {
        return false;
    }
    memcpy(f->file[to], f->file[from], sizeof(f->file[to]));
    f->present[to]   = true;
    f->present[from] = false;
    return true;
}

static bool fk_remove(void *c, sbs_name_t n) {
    fake_t *f = c;
    if (!gate(f)) {
        return false;
    }
    f->present[n] = false;
    return true;
}

static sbs_ops_t const OPS = {fk_write, fk_rename, fk_remove};

static sbs_state_t fk_probe(void *c, sbs_name_t n) {
    fake_t *f = c;
    if (!f->present[n]) {
        return SBS_ABSENT;
    }
    bool valid = !strcmp(f->file[n], V0) || !strcmp(f->file[n], V1) || !strcmp(f->file[n], V2);
    return valid ? SBS_VALID : SBS_INVALID;
}

static void put(fake_t *f, sbs_name_t n, char const *text) {
    f->present[n] = true;
    snprintf(f->file[n], sizeof(f->file[n]), "%s", text);
}

// The three starting states of a save: a backup, no backup, and what a cut between the swap's steps leaves
static void start(fake_t *f, int variant) {
    memset(f, 0, sizeof(*f));
    if (variant == 0) {
        put(f, SBS_MAIN, V0);
    } else if (variant == 2) {
        put(f, SBS_NEW, V1);
        put(f, SBS_OLD, V0);
    }
}

static char const *chosen_text(fake_t *f, bool *any) {
    bool       saw;
    sbs_name_t n = sbs_choose(fk_probe, f, &saw);
    *any         = n != SBS_COUNT;
    return *any ? f->file[n] : "";
}

static int count_ops(int variant) {
    fake_t f;
    start(&f, variant);
    CHECK(sbs_save(&OPS, &f, V2, 2));
    return f.op;
}

static void check_case(int variant, int step, bool cut) {
    fake_t f;
    start(&f, variant);
    f.fail_at = step;
    f.cut     = cut;
    bool saved = sbs_save(&OPS, &f, V2, 2);
    bool any;
    char got[8];
    snprintf(got, sizeof(got), "%s", chosen_text(&f, &any));
    if (variant == 1 && step == 1) { // nothing existed and the only write failed: there is no copy to keep
        CHECK(!any);
    } else {
        CHECK(any); // a readable valid copy remains
    }
    if (saved) {
        CHECK(strcmp(got, V2) == 0);
    } else if (variant == 0) {
        CHECK(strcmp(got, V0) == 0 || strcmp(got, V2) == 0);
    } else if (variant == 2) {
        // the newest; a failed first write truncates the leftover NEW, so only the older OLD remains (still valid)
        CHECK(strcmp(got, V2) == 0 || strcmp(got, V1) == 0 || (step == 1 && strcmp(got, V0) == 0));
    }
    // after a cut the next save (power is back) must converge: MAIN holds the new text, no leftovers
    f.dead = false; f.fail_at = 0; f.op = 0;
    CHECK(sbs_save(&OPS, &f, V2, 2));
    CHECK(f.present[SBS_MAIN] && strcmp(f.file[SBS_MAIN], V2) == 0);
    CHECK(!f.present[SBS_NEW] && !f.present[SBS_OLD]);
}

static void test_every_step(void) {
    for (int variant = 0; variant < 3; variant++) {
        int ops = count_ops(variant);
        CHECK(ops >= 3);
        for (int step = 1; step <= ops; step++) {
            check_case(variant, step, true);
            check_case(variant, step, false);
        }
    }
}

static void test_choose_order(void) {
    fake_t f;
    bool   saw;
    memset(&f, 0, sizeof(f));
    CHECK(sbs_choose(fk_probe, &f, &saw) == SBS_COUNT && !saw); // nothing
    put(&f, SBS_OLD, V0);
    CHECK(sbs_choose(fk_probe, &f, &saw) == SBS_OLD);
    put(&f, SBS_NEW, V1);
    CHECK(sbs_choose(fk_probe, &f, &saw) == SBS_NEW);
    put(&f, SBS_MAIN, V2);
    CHECK(sbs_choose(fk_probe, &f, &saw) == SBS_MAIN);
    put(&f, SBS_MAIN, "junk"); // an invalid MAIN is refused, not papered over with an older copy
    CHECK(sbs_choose(fk_probe, &f, &saw) == SBS_COUNT && saw);
    f.present[SBS_MAIN] = false;
    put(&f, SBS_NEW, PARTIAL); // a partial NEW with MAIN absent falls through to OLD
    CHECK(sbs_choose(fk_probe, &f, &saw) == SBS_OLD);
    put(&f, SBS_OLD, "junk");
    CHECK(sbs_choose(fk_probe, &f, &saw) == SBS_COUNT && saw); // refused, not missing
}

// The sequence this replaces: write NEW, remove MAIN, rename NEW over it, delete NEW on failure. The test above would
// have failed on it: a cut or a failed rename after the remove leaves no valid copy a read looks at (it read MAIN only).
static bool old_save(fake_t *f, char const *buf, size_t len) {
    if (!fk_write(f, SBS_NEW, buf, len)) {
        return false;
    }
    fk_remove(f, SBS_MAIN);
    if (!fk_rename(f, SBS_NEW, SBS_MAIN)) {
        fk_remove(f, SBS_NEW);
        return false;
    }
    return true;
}

static void test_old_sequence_lost_the_backup(void) {
    int lost = 0;
    for (int step = 1; step <= 3; step++) {
        for (int cut = 0; cut < 2; cut++) {
            fake_t f;
            start(&f, 0);
            f.fail_at = step;
            f.cut     = cut;
            old_save(&f, V2, 2);
            lost += !(f.present[SBS_MAIN] && fk_probe(&f, SBS_MAIN) == SBS_VALID); // the old read looked at MAIN only
        }
    }
    CHECK(lost > 0);
}

int main(void) {
    test_every_step();
    test_choose_order();
    test_old_sequence_lost_the_backup();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("settings_backup_swap_test: ok\n");
    return 0;
}
