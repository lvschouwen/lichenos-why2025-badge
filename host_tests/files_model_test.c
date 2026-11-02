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

typedef struct {
    char const *path;
    bool        protected_file; // files_protected, and for a root entry as a plain file
    bool        secret;
} prot_row_t;

static void test_protected(void) {
    static prot_row_t const rows[] = {
        // the volume roots and unusable paths
        {"/storage", true, true}, {"/sd", true, true}, {"/etc/passwd", true, true}, {"", true, true},
        {"/storage/../sd/x", true, true}, {"/sd/../storage/meshcore", true, true}, {"/storage/./x", true, true},
        {"/storage//x", true, true}, {"/storage/x/", true, true},
        // everything the firmware keeps on /storage
        {"/storage/meshcore", true, true}, {"/storage/meshcore/_main.id", true, true},
        {"/storage/meshcore/contacts3", true, true}, {"/storage/c6", true, false},
        {"/storage/c6/firmware.bin", true, false}, {"/storage/crash/log.txt", true, false},
        {"/storage/lichen/notify.bin", true, false}, {"/storage/lora.toml", true, false},
        {"/storage/ota_token.txt", true, true}, {"/storage/ota_other", true, false},
        {"/storage/loraold.cfg", true, false},
        // other apps' directories and anything below any directory
        {"/storage/files/a.txt", true, false}, {"/storage/anyapp/settings.bin", true, false},
        {"/storage/sub/meshcore/x", true, false}, {"/storage/dir/deep/er/x", true, false},
        // case, dots, spaces and aliases
        {"/storage/MeshCore/ID", true, true}, {"/storage/LORA.TOML", true, false}, {"/storage/C6", true, false},
        {"/storage/meshcore./x", true, true}, {"/storage/meshcore /x", true, true}, {"/storage/MESHCO~1/x", true, true},
        {"/storage/lora.toml.", true, false}, {"/storage/OTA_TOKEN.TXT", true, true}, {"/storage/LORA~1.TOM", true, true},
        // /sd: the whole /lichen tree is protected, the rest is the user's
        {"/sd/lichen", true, false}, {"/sd/lichen/apps", true, true}, {"/sd/lichen/apps/files/a.txt", true, true},
        {"/sd/lichen/messages/a/b", true, true}, {"/sd/LICHEN/x", true, true}, {"/sd/Lichen./x", true, true},
        {"/sd/LICHEN~1/x", true, true},
        {"/sd/photo.jpg", false, false}, {"/sd/meshcore", false, false}, {"/sd/dir/lichen/x", false, false},
        {"/sd/lichen2/x", false, false}, {"/sd/a/b/c.txt", false, false},
        // plain files at the /storage root that are the user's
        {"/storage/notes.txt", false, false}, {"/storage/lora.toml2", true, false},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        bool p = files_protected(rows[i].path), sec = files_secret(rows[i].path);
        if (p != rows[i].protected_file || sec != rows[i].secret) {
            printf("FAIL protected table: %s protected=%d (want %d) secret=%d (want %d)\n", rows[i].path, p,
                   rows[i].protected_file, sec, rows[i].secret);
            failures++;
        }
    }
    // a directory straight under /storage is protected whatever its name; a file of that name is not
    CHECK(!files_protected_entry("/storage/notes.txt", false) && files_protected_entry("/storage/notes.txt", true));
    CHECK(files_protected_entry("/storage/anything", true) && !files_protected_entry("/sd/photos", true));
    CHECK(files_protected_entry("/storage/lora.toml", false));
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
