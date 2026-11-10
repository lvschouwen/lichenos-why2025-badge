// Host test: the Notes app's file work on a temp directory (#154)
#include "notes_store.h"

#include <dirent.h>
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

static void put(char const *name, char const *content) {
    char path[400];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    fputs(content, f);
    fclose(f);
}

static bool has(char const *name) {
    char        path[400];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return stat(path, &st) == 0;
}

static void clean_dir(void) {
    DIR *d = opendir(dir);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            char path[400];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            remove(path);
        }
    }
    closedir(d);
}

static void test_add_and_list(void) {
    notes_item_t items[8];
    size_t       count, skipped;
    char         name[NOTES_NAME_MAX];
    CHECK(notes_store_list(dir, items, 8, &count, &skipped) == NOTES_OK && count == 0 && skipped == 0);
    CHECK(notes_store_add(dir, "[ ] Buy milk", name, sizeof(name)) == NOTES_OK && strcmp(name, "buy-milk.txt") == 0);
    CHECK(notes_store_add(dir, "Buy milk again", name, sizeof(name)) == NOTES_OK && strcmp(name, "buy-milk-again.txt") == 0);
    // The same words get the next free number, never an overwrite
    CHECK(notes_store_add(dir, "Buy milk", name, sizeof(name)) == NOTES_OK && strcmp(name, "buy-milk-1.txt") == 0);
    CHECK(notes_store_add(dir, "Buy milk", name, sizeof(name)) == NOTES_OK && strcmp(name, "buy-milk-2.txt") == 0);
    CHECK(notes_store_list(dir, items, 8, &count, &skipped) == NOTES_OK && count == 4 && skipped == 0);
    CHECK(strcmp(items[0].name, "buy-milk-1.txt") == 0 && strcmp(items[0].line, "Buy milk") == 0);
    CHECK(strcmp(items[3].name, "buy-milk.txt") == 0 && strcmp(items[3].line, "[ ] Buy milk") == 0);
    CHECK(!has("buy-milk.txt.new") && !has("buy-milk.new"));
}

static void test_write(void) {
    notes_item_t items[8];
    size_t       count, skipped;
    CHECK(notes_store_write(dir, "buy-milk.txt", "[x] Buy milk") == NOTES_OK);
    CHECK(notes_store_list(dir, items, 8, &count, &skipped) == NOTES_OK);
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(items[i].name, "buy-milk.txt") == 0) {
            found = strcmp(items[i].line, "[x] Buy milk") == 0;
        }
    }
    CHECK(found);
    CHECK(!has("buy-milk.txt.new"));
    // A note that was deleted meanwhile does not come back
    CHECK(notes_store_write(dir, "nothing.txt", "x") == NOTES_ERR_GONE && !has("nothing.txt"));
    CHECK(notes_store_write(dir, "../x.txt", "x") == NOTES_ERR_ARG);
    CHECK(notes_store_write(dir, "buy-milk.txt", "") == NOTES_ERR_ARG);
}

static void test_delete(void) {
    CHECK(notes_store_delete(dir, "buy-milk.txt") == NOTES_OK && !has("buy-milk.txt"));
    CHECK(notes_store_delete(dir, "buy-milk.txt") == NOTES_ERR_GONE);
    CHECK(notes_store_delete(dir, "../etc.txt") == NOTES_ERR_ARG);
    CHECK(notes_store_delete(dir, "readme") == NOTES_ERR_ARG);
    // EACCES (FatFs's file lock, #274: the note is open elsewhere; a read-only directory on the host): the note stays,
    // and the app says why instead of blaming the card (NOTES_ERR_IO)
    if (geteuid() != 0) {
        put("held.txt", "held\n");
        chmod(dir, 0500);
        CHECK(notes_store_delete(dir, "held.txt") == NOTES_ERR_BUSY && has("held.txt"));
        // the .new cannot be created either: EACCES on fopen is BUSY too
        CHECK(notes_store_write(dir, "held.txt", "changed") == NOTES_ERR_BUSY && has("held.txt"));
        chmod(dir, 0700);
        CHECK(strstr(notes_result_text(NOTES_ERR_BUSY), "open elsewhere") != NULL);
        CHECK(notes_store_delete(dir, "held.txt") == NOTES_OK);
    }
}

extern int (*notes_test_remove)(char const *path);
extern int (*notes_test_rename)(char const *from, char const *to);

static bool ends_new(char const *path) {
    size_t n = strlen(path);
    return n > 4 && strcmp(path + n - 4, ".new") == 0;
}

static int remove_held(char const *path) { // the old note is open elsewhere (FatFs's file lock): EACCES
    if (!ends_new(path)) {
        errno = EACCES;
        return -1;
    }
    return remove(path);
}

static int rename_fails(char const *from, char const *to) {
    (void)from, (void)to;
    errno = EIO;
    return -1;
}

static bool line_is(char const *name, char const *want) {
    char  path[400], got[64] = {0};
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    size_t n = fread(got, 1, sizeof(got) - 1, f);
    fclose(f);
    return n > 0 && strncmp(got, want, strlen(want)) == 0;
}

// An edit with the old note open elsewhere: the old note stays, the .new goes, and the app hears BUSY (#274). An edit
// whose rename fails after the old note went: the .new is the only copy and is kept; the next listing takes it back.
static void test_edit_locked(void) {
    clean_dir();
    put("held.txt", "old\n");
    notes_test_remove = remove_held;
    CHECK(notes_store_write(dir, "held.txt", "new") == NOTES_ERR_BUSY);
    notes_test_remove = remove;
    CHECK(line_is("held.txt", "old") && !has("held.txt.new"));

    notes_test_rename = rename_fails;
    CHECK(notes_store_write(dir, "held.txt", "new") == NOTES_ERR_IO);
    notes_test_rename = rename;
    CHECK(!has("held.txt") && line_is("held.txt.new", "new"));
    notes_item_t items[4];
    size_t       count, skipped;
    CHECK(notes_store_list(dir, items, 4, &count, &skipped) == NOTES_OK && count == 1);
    CHECK(line_is("held.txt", "new") && !has("held.txt.new"));
    clean_dir();
}

static void test_list_edges(void) {
    clean_dir();
    notes_item_t items[2];
    size_t       count, skipped;
    put("a.txt", "one\nsecond line is ignored\n");
    put("b.txt", "\n");            // empty: left out
    put("c.txt", "three");         // no newline
    put("d.txt", "four");
    put("e.md", "not a note");     // other files are not counted at all
    put("f.new", "leftover");
    put(".hidden.txt", "hidden");
    CHECK(notes_store_list(dir, items, 2, &count, &skipped) == NOTES_OK);
    CHECK(count == 2 && skipped == 2); // b.txt empty, d.txt no room
    notes_item_t all[8];
    CHECK(notes_store_list(dir, all, 8, &count, &skipped) == NOTES_OK && count == 3 && skipped == 1);
    CHECK(strcmp(all[0].line, "one") == 0 && strcmp(all[1].line, "three") == 0 && strcmp(all[2].line, "four") == 0);
    CHECK(notes_store_list("/nonexistent-notes-dir", items, 2, &count, &skipped) == NOTES_ERR_OPEN && count == 0);
    // A long line is cut to what the list holds
    char big[400];
    memset(big, 'z', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    put("g.txt", big);
    notes_item_t many[8];
    CHECK(notes_store_list(dir, many, 8, &count, &skipped) == NOTES_OK);
    bool cut = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(many[i].name, "g.txt") == 0) {
            cut = strlen(many[i].line) == NOTES_LINE_MAX - 1;
        }
    }
    CHECK(cut);
}

// A save cut off between "remove the old note" and "rename .new over it" (power, card pulled) leaves only
// <name>.new: the next listing brings it back instead of the note vanishing. A .new next to its note is an
// unfinished write and stays untouched (review #154).
static void test_recover_orphan(void) {
    clean_dir();
    notes_item_t items[4];
    size_t       count, skipped;
    put("lost.txt.new", "saved just before the power went");
    put("kept.txt", "old text");
    put("kept.txt.new", "half written");
    CHECK(notes_store_list(dir, items, 4, &count, &skipped) == NOTES_OK && count == 2);
    CHECK(has("lost.txt") && !has("lost.txt.new"));
    CHECK(has("kept.txt") && has("kept.txt.new"));
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        found |= strcmp(items[i].name, "lost.txt") == 0 && strcmp(items[i].line, "saved just before the power went") == 0;
    }
    CHECK(found);
}

int main(void) {
    snprintf(dir, sizeof(dir), "/tmp/notes_store_test_XXXXXX");
    if (!mkdtemp(dir)) {
        printf("no temp dir\n");
        return 1;
    }
    test_add_and_list();
    test_write();
    test_delete();
    test_list_edges();
    test_edit_locked();
    test_recover_orphan();
    clean_dir();
    rmdir(dir);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("notes_store_test: ok\n");
    return 0;
}
