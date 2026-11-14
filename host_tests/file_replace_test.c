// Host test: the atomic file replace (#463). A fake file system (FAT mode: rename refuses an existing target) with a
// power cut injected at every file operation of a replace: the operation and everything after it never happen, a
// write leaves a partial file. After the cut, and the recovery a boot runs, the file must hold the old content or the
// new one complete, never neither (when there was an old one), and the next replace must converge.
#include "file_replace.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define FILES_MAX 6
typedef struct {
    char name[40];
    char data[16];
    bool used;
} ffile_t;

typedef struct {
    ffile_t files[FILES_MAX];
    bool    fat;
    int     op, cut_at; // 1-based, 0 = never
    bool    dead;
} fake_t;

static ffile_t *find(fake_t *f, char const *name) {
    for (int i = 0; i < FILES_MAX; i++) {
        if (f->files[i].used && strcmp(f->files[i].name, name) == 0) {
            return &f->files[i];
        }
    }
    return NULL;
}

static ffile_t *make(fake_t *f, char const *name) {
    ffile_t *e = find(f, name);
    for (int i = 0; !e && i < FILES_MAX; i++) {
        e = f->files[i].used ? NULL : &f->files[i];
    }
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->used = true;
    return e;
}

// True when this operation may take effect; a cut kills it and every later one
static bool alive(fake_t *f) {
    if (f->dead) {
        return false;
    }
    if (++f->op == f->cut_at) {
        f->dead = true;
        return false;
    }
    return true;
}

static bool f_write(void *c, char const *path, void const *buf, size_t len) {
    fake_t *f = c;
    bool    ok = alive(f);
    ffile_t *e = ok || f->dead ? make(f, path) : NULL;
    snprintf(e->data, sizeof(e->data), "%.*s", ok ? (int)len : 1, (char const *)buf); // a cut leaves a partial file
    return ok;
}

static bool f_rename(void *c, char const *from, char const *to) {
    fake_t *f = c;
    if (!alive(f)) {
        return false;
    }
    ffile_t *s = find(f, from), *d = find(f, to);
    if (!s || (d && f->fat)) {
        errno = s ? EEXIST : ENOENT;
        return false;
    }
    if (d) {
        d->used = false; // LittleFS replaces
    }
    snprintf(s->name, sizeof(s->name), "%s", to);
    return true;
}

static bool f_remove(void *c, char const *path) {
    fake_t *f = c;
    if (!alive(f)) {
        return false;
    }
    ffile_t *e = find(f, path);
    if (e) {
        e->used = false;
    }
    return true;
}

static bool f_exists(void *c, char const *path) {
    return find(c, path) != NULL;
}

static file_ops_t const OPS = {f_write, f_rename, f_remove, f_exists};

#define P "/d/file"

static void seed(fake_t *f, bool fat, char const *old) {
    memset(f, 0, sizeof(*f));
    f->fat = fat;
    if (old) {
        snprintf(make(f, P)->data, sizeof(f->files[0].data), "%s", old);
    }
}

// One replace of "new" with a cut at op `cut`; then the boot-time recovery and the checks
static void run_cut(bool fat, char const *old, int cut, int *total_ops) {
    fake_t f;
    seed(&f, fat, old);
    f.cut_at = cut;
    bool ok  = file_replace_ops(&OPS, &f, P, "new", 3, fat);
    if (total_ops) {
        *total_ops = f.op;
    }
    CHECK(cut != 0 || ok); // with a cut the call may still report true: the last step is a best-effort cleanup
    f.dead = false;
    f.cut_at = 0;
    CHECK(file_recover_ops(&OPS, &f, P, fat));
    ffile_t *e = find(&f, P);
    if (old) {
        CHECK(e && (strcmp(e->data, old) == 0 || strcmp(e->data, "new") == 0));
    } else {
        CHECK(!e || strcmp(e->data, "new") == 0); // first write: nothing, or complete, never a part
    }
    if (cut == 0 || cut > f.op) {
        CHECK(e && strcmp(e->data, "new") == 0);
    }
    // The next replace works on whatever the cut left
    CHECK(file_replace_ops(&OPS, &f, P, "again", 5, fat));
    e = find(&f, P);
    CHECK(e && strcmp(e->data, "again") == 0);
    CHECK(!find(&f, P ".new") && (!fat || !find(&f, P ".old")));
}

static void test_cuts(void) {
    for (int fat = 0; fat < 2; fat++) {
        for (int has_old = 0; has_old < 2; has_old++) {
            int total = 0;
            run_cut(fat, has_old ? "old" : NULL, 0, &total);
            CHECK(total >= 2);
            for (int cut = 1; cut <= total + 1; cut++) {
                run_cut(fat, has_old ? "old" : NULL, cut, NULL);
            }
        }
    }
}

// The window FAT has: the old file is aside, the new one complete; the open must find it
static void test_fat_window(void) {
    fake_t f;
    seed(&f, true, NULL);
    snprintf(make(&f, P ".old")->data, 16, "old");
    snprintf(make(&f, P ".new")->data, 16, "new");
    CHECK(file_recover_ops(&OPS, &f, P, true));
    CHECK(find(&f, P) && strcmp(find(&f, P)->data, "new") == 0 && !find(&f, P ".old") && !find(&f, P ".new"));
    // Only the .old left: the previous file comes back
    seed(&f, true, NULL);
    snprintf(make(&f, P ".old")->data, 16, "old");
    CHECK(file_recover_ops(&OPS, &f, P, true) && strcmp(find(&f, P)->data, "old") == 0);
    // An unfinished first write (.new alone) is never promoted
    seed(&f, true, NULL);
    snprintf(make(&f, P ".new")->data, 16, "n");
    CHECK(file_recover_ops(&OPS, &f, P, true) && !find(&f, P));
    // A write that was cut mid-way on top of an intact file leaves it alone
    seed(&f, true, "old");
    snprintf(make(&f, P ".new")->data, 16, "n");
    snprintf(make(&f, P ".old")->data, 16, "older");
    CHECK(file_recover_ops(&OPS, &f, P, true) && strcmp(find(&f, P)->data, "old") == 0);
}

static void test_names(void) {
    fake_t f;
    seed(&f, false, "old");
    char longp[FILE_REPLACE_PATH_MAX + 8];
    memset(longp, 'a', sizeof(longp));
    longp[sizeof(longp) - 1] = '\0';
    errno = 0;
    CHECK(!file_replace_ops(&OPS, &f, longp, "x", 1, false) && errno == ENAMETOOLONG && f.op == 0);
    CHECK(!file_replace_ops(&OPS, &f, "", "x", 1, false));
}

// The stdio binding on a real directory (LittleFS rules: rename replaces)
static void test_posix(void) {
    char dir[] = "/tmp/file_replace_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[128], side[140];
    snprintf(path, sizeof(path), "%s/f.txt", dir);
    snprintf(side, sizeof(side), "%s.new", path);
    CHECK(lichen_file_open_recovered(path) == NULL && errno == ENOENT);
    CHECK(lichen_file_replace(path, "one\n", 4));
    CHECK(lichen_file_replace(path, "two!\n", 5));
    char  buf[16] = "";
    FILE *f       = lichen_file_open_recovered(path);
    CHECK(f && fread(buf, 1, sizeof(buf) - 1, f) == 5 && strcmp(buf, "two!\n") == 0);
    if (f) {
        fclose(f);
    }
    CHECK(access(side, F_OK) != 0);
    CHECK(lichen_file_replace(path, "", 0)); // an empty file is a file
    f = lichen_file_open_recovered(path);
    CHECK(f && fgetc(f) == EOF);
    if (f) {
        fclose(f);
    }
    // A directory in the way of the target: the call fails with errno set and no .new is left
    char blocked[128];
    snprintf(blocked, sizeof(blocked), "%s/d", dir);
    CHECK(mkdir(blocked, 0777) == 0);
    errno = 0;
    CHECK(!lichen_file_replace(blocked, "x", 1) && errno != 0);
    snprintf(side, sizeof(side), "%s.new", blocked);
    CHECK(access(side, F_OK) != 0);
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "rm -r %s", dir);
    CHECK(system(cmd) == 0);
}

int main(void) {
    test_cuts();
    test_fat_window();
    test_names();
    test_posix();
    printf(failures ? "file_replace_test: %d FAILED\n" : "file_replace_test: ok\n", failures);
    return failures != 0;
}
