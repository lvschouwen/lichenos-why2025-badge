// Settings > Apps (#233): an app's data directory paths, its size and Clear data, on a temp directory. Clear data must
// only ever touch the selected app's own directory: hostile ids never make a path, a link is never followed, a sibling
// app's directory and the OS's files stay.
#define _XOPEN_SOURCE 700 // nftw
#include "lichen_sys_data_fs.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char root[128];

static void path_of(char *out, size_t cap, char const *rel) {
    snprintf(out, cap, "%s/%s", root, rel);
}

static void make_dir(char const *rel) {
    char p[256];
    path_of(p, sizeof(p), rel);
    CHECK(mkdir(p, 0700) == 0 || errno == EEXIST);
}

static void make_file(char const *rel, size_t bytes) {
    char p[256];
    path_of(p, sizeof(p), rel);
    FILE *f = fopen(p, "wb");
    CHECK(f != NULL);
    if (f) {
        for (size_t i = 0; i < bytes; i++) {
            fputc('x', f);
        }
        fclose(f);
    }
}

static bool exists(char const *rel) {
    char        p[256];
    struct stat st;
    path_of(p, sizeof(p), rel);
    return lstat(p, &st) == 0;
}

static void paths(void) {
    char out[APP_DATA_PATH_MAX];
    CHECK(lichen_sys_data_dir_path("/storage", "notes", out, sizeof(out)) && strcmp(out, "/storage/notes") == 0);
    CHECK(lichen_sys_data_dir_path("/sd/lichen/apps", "weather", out, sizeof(out)) && strcmp(out, "/sd/lichen/apps/weather") == 0);
    CHECK(lichen_sys_data_dir_path("/storage", "a_1", out, sizeof(out)) && strcmp(out, "/storage/a_1") == 0);
    // hostile ids: nothing but an empty path comes out, so nothing can be measured or cleared with them
    char const *bad[] = {"", "..", ".", "../x", "../../etc", "a/b", "/abs", "a\\b", "notes/..", "UP", "a b", "a.b", "a-b",
                         "c6", "lichen", "fourteen_chars", "meshcore\n", "a\x01", "\xc3\xa9", "%2e%2e", "*", "x;rm"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(out, 'Z', sizeof(out));
        CHECK(!lichen_sys_data_dir_path("/storage", bad[i], out, sizeof(out)));
        CHECK(out[0] == '\0');
    }
    CHECK(!lichen_sys_data_dir_path("/storage", NULL, out, sizeof(out)));
    CHECK(!lichen_sys_data_dir_path(NULL, "notes", out, sizeof(out)));
    CHECK(!lichen_sys_data_dir_path("storage", "notes", out, sizeof(out))); // a relative base
    CHECK(!lichen_sys_data_dir_path("/storage", "notes", NULL, 0));
    CHECK(!lichen_sys_data_dir_path("/storage", "notes", out, 8)); // too small
    char longbase[APP_DATA_PATH_MAX + 8];
    memset(longbase, 'b', sizeof(longbase));
    longbase[0]                   = '/';
    longbase[sizeof(longbase) - 1] = '\0';
    CHECK(!lichen_sys_data_dir_path(longbase, "notes", out, sizeof(out)) && out[0] == '\0');
}

static void measure_and_clear(void) {
    app_data_fs_t r;
    make_dir("notes");
    make_dir("notes/sub");
    make_dir("notes/sub/deeper");
    make_file("notes/a.txt", 100);
    make_file("notes/sub/b.txt", 2000);
    make_file("notes/sub/deeper/c.txt", 5);
    make_file("other_a.txt", 7); // outside
    make_dir("notes2"); // a sibling whose name starts with the app's own id
    make_dir("notes2/sub");
    make_file("notes2/n.txt", 11);
    make_file("notes2/sub/m.txt", 13);
    make_dir("weather");
    make_file("weather/w.csv", 300); // a sibling app
    char dir[256];
    path_of(dir, sizeof(dir), "notes");

    CHECK(lichen_sys_data_dir_measure(dir, &r));
    CHECK(r.bytes == 2105 && r.files == 3 && r.dirs == 2 && r.skipped == 0);
    CHECK(lichen_sys_data_dir_clear(dir, &r));
    CHECK(r.files == 3 && r.dirs == 2 && r.bytes == 2105 && r.skipped == 0);
    CHECK(exists("notes") && !exists("notes/a.txt") && !exists("notes/sub")); // the directory itself stays
    CHECK(exists("other_a.txt") && exists("weather/w.csv"));                    // nothing else was touched
    CHECK(exists("notes2/n.txt") && exists("notes2/sub/m.txt")); // not even a directory with the id as its prefix
    app_data_fs_t two;
    char          dir2[256];
    path_of(dir2, sizeof(dir2), "notes2");
    CHECK(lichen_sys_data_dir_measure(dir2, &two) && two.bytes == 24 && two.files == 2 && two.dirs == 1);
    CHECK(lichen_sys_data_dir_measure(dir, &r) && r.bytes == 0 && r.files == 0);
    CHECK(lichen_sys_data_dir_clear(dir, &r) && r.files == 0); // clearing twice is fine
}

static void missing_and_bad_dirs(void) {
    app_data_fs_t r;
    char          p[256];
    path_of(p, sizeof(p), "nothing_here");
    CHECK(lichen_sys_data_dir_measure(p, &r) && r.bytes == 0 && r.files == 0); // no directory: no data
    CHECK(lichen_sys_data_dir_clear(p, &r) && r.files == 0);
    CHECK(!lichen_sys_data_dir_measure("", &r) && !lichen_sys_data_dir_measure(NULL, &r) && !lichen_sys_data_dir_clear(NULL, &r));
    make_file("a_file", 4);
    path_of(p, sizeof(p), "a_file");
    CHECK(!lichen_sys_data_dir_clear(p, &r) && exists("a_file")); // a plain file is not an app directory
    CHECK(lichen_sys_data_dir_measure(p, NULL) == false);
    char longdir[APP_DATA_PATH_MAX + 4];
    memset(longdir, 'd', sizeof(longdir));
    longdir[0]                   = '/';
    longdir[sizeof(longdir) - 1] = '\0';
    CHECK(!lichen_sys_data_dir_clear(longdir, &r));
}

static void links_are_not_followed(void) {
    app_data_fs_t r;
    char          p[256], q[256];
    make_dir("victim");
    make_file("victim/keep.txt", 50);
    make_dir("linked");
    make_file("linked/own.txt", 10);
    path_of(p, sizeof(p), "victim");
    path_of(q, sizeof(q), "linked/out");
    CHECK(symlink(p, q) == 0); // a link inside the app's directory that points at another app's
    path_of(p, sizeof(p), "victim/keep.txt");
    path_of(q, sizeof(q), "linked/out_file");
    CHECK(symlink(p, q) == 0);
    path_of(p, sizeof(p), "linked");
    CHECK(lichen_sys_data_dir_measure(p, &r) && r.files == 1 && r.bytes == 10 && r.skipped == 2); // links are not counted
    CHECK(lichen_sys_data_dir_clear(p, &r) && r.files == 3 && r.skipped == 0); // the links themselves go, as entries
    CHECK(!exists("linked/out") && !exists("linked/out_file"));
    CHECK(exists("victim/keep.txt") && exists("victim"));                                  // the target survived
    CHECK(!exists("linked/own.txt"));
    // a link as the directory itself is refused outright
    path_of(p, sizeof(p), "victim");
    path_of(q, sizeof(q), "evil_link");
    CHECK(symlink(p, q) == 0);
    CHECK(!lichen_sys_data_dir_clear(q, &r) && exists("victim/keep.txt"));
}

static void deep_trees_stop(void) {
    app_data_fs_t r;
    char          rel[200] = "deep";
    make_dir("deep");
    for (int i = 0; i < APP_DATA_DEPTH_MAX + 2; i++) {
        strcat(rel, "/d");
        make_dir(rel);
    }
    char p[256];
    path_of(p, sizeof(p), "deep");
    CHECK(lichen_sys_data_dir_clear(p, &r) == false && r.skipped >= 1); // left alone, not claimed cleared
}

static int remove_one(char const *path, struct stat const *st, int flag, struct FTW *ftw) {
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path); // a link is removed itself, never followed (FTW_PHYS)
}

int main(void) {
    snprintf(root, sizeof(root), "/tmp/app_data_fs_test_%d", (int)getpid());
    CHECK(mkdir(root, 0700) == 0);
    paths();
    measure_and_clear();
    missing_and_bad_dirs();
    links_are_not_followed();
    deep_trees_stop();
    if (nftw(root, remove_one, 16, FTW_DEPTH | FTW_PHYS) != 0) {
        printf("note: %s left behind\n", root);
    }
    if (fails) {
        printf("app_data_fs_test: %d failed\n", fails);
        return 1;
    }
    printf("app_data_fs_test: ok\n");
    return 0;
}
