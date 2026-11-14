// Host test: the atomic file replace (#463) on a real directory (LittleFS rules: rename replaces). A power cut leaves
// one of three states, which the test seeds directly: the old file with a half-written .new (cut in the write), the old
// file with a complete .new (cut before the rename), the new file (cut after). Whatever the state, the file reads as the
// old or the new content complete, and the next replace converges. Also the refusals and the legacy adoption.
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

static char dir[64];

static void put(char const *name, char const *text) {
    char  p[128];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static bool has(char const *name) {
    char p[128];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    return access(p, F_OK) == 0;
}

static bool is(char const *name, char const *text) {
    char  p[128], buf[64] = "";
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "r");
    if (!f) {
        return false;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

static void clean(void) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -f %s/*", dir);
    CHECK(system(cmd) == 0);
}

static char const *path_of(char const *name) {
    static char p[4][128]; // a call may name two paths
    static int  i;
    i = (i + 1) % 4;
    snprintf(p[i], sizeof(p[i]), "%s/%s", dir, name);
    return p[i];
}

static bool open_reads(char const *name, char const *text) {
    FILE *f = lichen_file_open_recovered(path_of(name));
    char  buf[64] = "";
    if (!f) {
        return false;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

static void test_basic(void) {
    clean();
    errno = 0;
    CHECK(lichen_file_open_recovered(path_of("f")) == NULL && errno == ENOENT);
    CHECK(lichen_file_replace(path_of("f"), "one", 3) && is("f", "one") && !has("f.new"));
    CHECK(lichen_file_replace(path_of("f"), "two!", 4) && is("f", "two!") && !has("f.new"));
    CHECK(lichen_file_replace(path_of("f"), "", 0) && is("f", "")); // an empty file is a file
}

// The cut states, then a recovered open and a replace after each
static void test_cuts(void) {
    for (int state = 0; state < 3; state++) {
        for (int twice = 0; twice < 2; twice++) { // a second cut before any recovery ran: same state again
            clean();
            put("f", "old");
            if (state == 0) {
                put("f.new", "ne"); // cut in the write: partial
            } else if (state == 1) {
                put("f.new", "new"); // cut before the rename: complete but never promoted
            } else {
                put("f", "new"); // cut after the rename
            }
            if (twice) {
                put("f.new", "n"); // the next replace was cut in its write too
            }
            CHECK(open_reads("f", state == 2 ? "new" : "old")); // never a partial, never neither
            CHECK(!has("f.new"));                              // the recovered open removed the stale one
        }
    }
    // A replace straight after a cut (no open in between) overwrites the stale .new and lands whole
    clean();
    put("f", "old");
    put("f.new", "a much longer stale partial");
    CHECK(lichen_file_replace(path_of("f"), "new", 3) && is("f", "new") && !has("f.new"));
    // First write cut in its write: nothing there, the partial .new is not a file
    clean();
    put("f.new", "pa");
    errno = 0;
    CHECK(lichen_file_open_recovered(path_of("f")) == NULL && errno == ENOENT && !has("f.new"));
}

static void test_refusals(void) {
    errno = 0;
    CHECK(!lichen_file_replace("/sd/x", "a", 1) && errno == EINVAL);
    errno = 0;
    CHECK(!lichen_file_replace("/sd", "a", 1) && errno == EINVAL);
    errno = 0;
    CHECK(lichen_file_open_recovered("/sd/x") == NULL && errno == EINVAL);
    errno = 0;
    CHECK(!lichen_file_replace("", "a", 1) && errno == EINVAL);
    errno = 0;
    CHECK(lichen_file_open_recovered("") == NULL && errno == EINVAL);
    char longp[FILE_REPLACE_PATH_MAX + 8];
    memset(longp, 'a', sizeof(longp));
    longp[0]                 = '/';
    longp[sizeof(longp) - 1] = '\0';
    errno = 0;
    CHECK(!lichen_file_replace(longp, "x", 1) && errno == ENAMETOOLONG);
    // "/sdx/..." is no card path: accepted (it fails only because the directory is not there)
    errno = 0;
    CHECK(!lichen_file_replace("/sdx_not_there/f", "a", 1) && errno == ENOENT);
    errno = 0;
    CHECK(lichen_file_open_recovered("/sdx_not_there/f") == NULL && errno == ENOENT);
    // A directory where the file goes: refused with errno set, no side file left
    clean();
    char blocked[128];
    snprintf(blocked, sizeof(blocked), "%s/d", dir);
    CHECK(mkdir(blocked, 0777) == 0);
    errno = 0;
    CHECK(!lichen_file_replace(blocked, "x", 1) && errno != 0 && !has("d.new"));
    // A stale .new that cannot be removed (a directory): the open reports it instead of ENOENT
    char dn[140];
    snprintf(dn, sizeof(dn), "%s/g.new", dir);
    CHECK(mkdir(dn, 0777) == 0);
    put("g", "x");
    errno = 0;
    CHECK(lichen_file_open_recovered(path_of("g")) == NULL && errno != ENOENT && errno != 0);
    CHECK(rmdir(dn) == 0 && rmdir(blocked) == 0);
}

static void test_legacy(void) {
    clean();
    put("log.tmp", "newer"); // the only copy: the main file is missing
    lichen_file_adopt_legacy(path_of("log.txt"), path_of("log.tmp"));
    CHECK(is("log.txt", "newer") && !has("log.tmp"));
    put("log.tmp", "stale"); // the main file is there: the leftover goes
    lichen_file_adopt_legacy(path_of("log.txt"), path_of("log.tmp"));
    CHECK(is("log.txt", "newer") && !has("log.tmp"));
    lichen_file_adopt_legacy(path_of("log.txt"), path_of("log.tmp")); // none: nothing happens
    CHECK(is("log.txt", "newer"));
    // notify's order: .tmp (newer) beats .old when both are left and the main file is gone
    clean();
    put("n.tmp", "newer");
    put("n.old", "older");
    lichen_file_adopt_legacy(path_of("n.bin"), path_of("n.tmp"));
    lichen_file_adopt_legacy(path_of("n.bin"), path_of("n.old"));
    CHECK(is("n.bin", "newer") && !has("n.tmp") && !has("n.old"));
    clean();
    put("n.old", "older"); // only the old one left: it is the file
    lichen_file_adopt_legacy(path_of("n.bin"), path_of("n.tmp"));
    lichen_file_adopt_legacy(path_of("n.bin"), path_of("n.old"));
    CHECK(is("n.bin", "older") && !has("n.old"));
}

int main(void) {
    snprintf(dir, sizeof(dir), "/tmp/file_replace_XXXXXX");
    CHECK(mkdtemp(dir) != NULL);
    test_basic();
    test_cuts();
    test_refusals();
    test_legacy();
    clean();
    rmdir(dir);
    printf(failures ? "file_replace_test: %d FAILED\n" : "file_replace_test: ok\n", failures);
    return failures != 0;
}
