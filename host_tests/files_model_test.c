// Host test: Files app rules (#71): paths, protected list, sort, text paging
#include "files_model.h"

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

static void test_paths(void) {
    CHECK(files_path_ok("/storage") && files_path_ok("/sd") && files_path_ok("/sd/a/b.txt"));
    CHECK(!files_path_ok("") && !files_path_ok("/") && !files_path_ok("/etc/passwd") && !files_path_ok("/sdx"));
    CHECK(!files_path_ok("/storage/../sd") && !files_path_ok("/storage/./a") && !files_path_ok("/storage//a"));
    CHECK(!files_path_ok("/storage/a/") && !files_path_ok("/storage/a\nb") && !files_path_ok("storage/a"));
    CHECK(!files_path_ok(NULL));
    CHECK(files_name_ok("a b.txt") && !files_name_ok("") && !files_name_ok("..") && !files_name_ok(".") &&
          !files_name_ok("a/b") && !files_name_ok("a\tb"));
    char out[FILES_PATH_MAX];
    CHECK(files_path_join("/sd", "x.txt", out, sizeof(out)) && !strcmp(out, "/sd/x.txt"));
    CHECK(!files_path_join("/sd", "../x", out, sizeof(out)));
    CHECK(!files_path_join("/sd", "x.txt", out, 8));
    CHECK(!files_path_join("/sd/..", "x", out, sizeof(out)));
    CHECK(files_path_parent("/sd/a/b", out, sizeof(out)) && !strcmp(out, "/sd/a"));
    CHECK(files_path_parent("/sd/a", out, sizeof(out)) && !strcmp(out, "/sd"));
    CHECK(files_path_parent("/sd", out, sizeof(out)) && !out[0]);
    CHECK(!files_path_parent("", out, sizeof(out)) && !files_path_parent("/etc", out, sizeof(out)));
    CHECK(!strcmp(files_volume_of("/sd/x"), "/sd") && !strcmp(files_volume_of("/storage"), "/storage"));
    CHECK(!files_volume_of("/sdcard") && !strcmp(files_other_volume("/sd"), "/storage") &&
          !strcmp(files_other_volume("/storage"), "/sd"));
}

static void test_protected(void) {
    CHECK(files_protected("/storage") && files_protected("/sd"));
    CHECK(files_protected("/storage/meshcore") && files_protected("/storage/meshcore/_main.id"));
    CHECK(files_protected("/storage/c6") && files_protected("/storage/c6/firmware.bin"));
    CHECK(files_protected("/storage/crash/x") && files_protected("/storage/lichen/notify.bin"));
    CHECK(files_protected("/storage/lora.toml") && files_protected("/storage/ota_token.txt"));
    CHECK(files_protected("/storage/MeshCore/ID") && files_protected("/storage/C6"));  // FAT ignores case
    CHECK(files_protected("/storage/meshcore./x") && files_protected("/storage/meshcore /x"));
    CHECK(files_protected("/storage/MESHCO~1/x"));                                 // 8.3 alias
    CHECK(files_protected("/sd/lichen") && files_protected("/sd/lichen/apps"));
    CHECK(files_protected("/sd/lichen/messages") && files_protected("/sd/lichen/messages/a/b"));
    CHECK(files_protected("/sd/LICHEN/Messages/x"));
    CHECK(files_protected("/sd/../storage"));                                       // not a usable path
    CHECK(!files_protected("/storage/notes.txt") && !files_protected("/storage/meshcore2/x"));
    CHECK(!files_protected("/storage/files/a") && !files_protected("/storage/lora.toml.bak"));
    CHECK(!files_protected("/sd/photo.jpg") && !files_protected("/sd/lichen/apps/files/a.txt"));
    CHECK(!files_protected("/sd/meshcore") && !files_protected("/sd/dir/meshcore"));
    CHECK(!files_protected("/storage/sub/meshcore/x"));                             // only top-level names
}

static void put(files_entry_t *e, char const *name, bool dir) {
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->dir = dir;
}

static void test_sort(void) {
    files_entry_t e[6];
    put(&e[0], "zeta.txt", false);
    put(&e[1], "Alpha.txt", false);
    put(&e[2], "beta", true);
    put(&e[3], "alpha.txt", false);
    put(&e[4], "Zoo", true);
    put(&e[5], "b.txt", false);
    files_sort(e, 6);
    CHECK(!strcmp(e[0].name, "beta") && !strcmp(e[1].name, "Zoo"));
    CHECK(!strcmp(e[2].name, "Alpha.txt") && !strcmp(e[3].name, "alpha.txt"));
    CHECK(!strcmp(e[4].name, "b.txt") && !strcmp(e[5].name, "zeta.txt"));
    files_sort(e, 0);
    files_sort(NULL, 3);
}

static size_t page(char const *in, size_t len, bool eof, int cols, int lines, char *out, size_t cap, bool *bin) {
    size_t used = 0;
    files_text_page((uint8_t const *)in, len, eof, cols, lines, out, cap, &used, bin);
    return used;
}

static void test_text(void) {
    char out[512];
    bool bin;
    size_t used = page("one\r\ntwo\nthree\n", 15, true, 20, 2, out, sizeof(out), &bin);
    CHECK(!strcmp(out, "one\ntwo\n") && used == 9 && !bin); // CR dropped; stops after 2 lines
    used = page("abcdefghij", 10, true, 4, 10, out, sizeof(out), &bin);
    CHECK(!strcmp(out, "abcd\nefgh\nij") && used == 10);    // hard wrap
    used = page("abcdefgh", 8, true, 4, 2, out, sizeof(out), &bin);
    CHECK(!strcmp(out, "abcd\nefgh") && used == 8);
    used = page("abcdefghijkl", 12, true, 4, 2, out, sizeof(out), &bin);
    CHECK(!strcmp(out, "abcd\nefgh") && used == 8);         // the 3rd line is the next page
    used = page("a\tb", 3, true, 10, 2, out, sizeof(out), &bin);
    CHECK(!strcmp(out, "a b") && used == 3);
    used = page("h\xC3\xA9llo", 6, true, 10, 2, out, sizeof(out), &bin);
    CHECK(!strcmp(out, "h\xC3\xA9llo") && used == 6);       // valid UTF-8 passes through
    used = page("h\xC3", 2, false, 10, 2, out, sizeof(out), &bin);
    CHECK(!strcmp(out, "h") && used == 1);                  // cut off: left for the next read
    used = page("h\xC3", 2, true, 10, 2, out, sizeof(out), &bin);
    CHECK(!strcmp(out, "h?") && used == 2);                 // at the end of the file it is just broken
    used = page("a\xFF" "b", 3, true, 10, 2, out, sizeof(out), &bin);
    CHECK(!strcmp(out, "a?b"));
    used = page("a\x01" "b", 3, true, 10, 2, out, sizeof(out), &bin);
    CHECK(!strcmp(out, "a.b"));
    used = page("\x01\x02\x03\x04", 4, true, 10, 2, out, sizeof(out), &bin);
    CHECK(bin);
    used = page("", 0, true, 10, 2, out, sizeof(out), &bin);
    CHECK(used == 0 && !out[0]);
    used = page("abcdefghij", 10, true, 10, 2, out, 5, &bin); // output buffer too small: stops, no overflow
    CHECK(strlen(out) <= 4 && used == strlen(out));
    // paging walks a whole text without losing or repeating a byte
    char text[400];
    for (size_t i = 0; i < sizeof(text) - 1; i++) {
        text[i] = "abcdefghij\n"[i % 11];
    }
    text[sizeof(text) - 1] = '\0';
    size_t at = 0, total = strlen(text), pages = 0;
    while (at < total && pages < 100) {
        size_t n = page(text + at, total - at, true, 7, 3, out, sizeof(out), &bin);
        CHECK(n > 0);
        at += n;
        pages++;
    }
    CHECK(at == total && pages > 5 && pages < 100);
}

int main(void) {
    test_paths();
    test_protected();
    test_sort();
    test_text();
    if (failures) {
        printf("files_model_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("files_model_test: all passed\n");
    return 0;
}
