// Host test: Files app file work (#71), run on a temp directory (files_ops.c prefixes paths with files_test_root)
#include "files_ops.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char files_test_root[256];

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void fpath(char const *p, char *out) {
    snprintf(out, 512, "%s%s", files_test_root, p);
}

static void write_file(char const *p, size_t n, char fill) {
    char rp[512];
    fpath(p, rp);
    FILE *f = fopen(rp, "wb");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        fputc(fill, f);
    }
    fclose(f);
}

static bool exists(char const *p) {
    char rp[512];
    struct stat st;
    fpath(p, rp);
    return stat(rp, &st) == 0;
}

static void mkdirp(char const *p) {
    char rp[512];
    fpath(p, rp);
    mkdir(rp, 0700);
}

static bool stop_at;
static uint32_t calls;
static bool progress(uint32_t done, uint32_t total, void *ctx) {
    (void)ctx;
    CHECK(done <= total);
    calls++;
    return !(stop_at && done >= 100);
}

static void test_list(void) {
    files_entry_t e[8];
    size_t n, skipped;
    write_file("/storage/b.txt", 5, 'x');
    write_file("/storage/A.txt", 7, 'x');
    mkdirp("/storage/zdir");
    CHECK(files_list("/storage", e, 8, &n, &skipped) == 0 && n == 3 && skipped == 0);
    CHECK(e[0].dir && !strcmp(e[0].name, "zdir") && !strcmp(e[1].name, "A.txt") && e[1].size == 7);
    CHECK(files_list("/storage", e, 2, &n, &skipped) == 0 && n == 2 && skipped == 1);
    CHECK(files_list("/storage/nope", e, 8, &n, &skipped) == ENOENT);
    CHECK(files_list("/etc", e, 8, &n, &skipped) == EINVAL);
}

static void test_delete(void) {
    mkdirp("/storage/meshcore");
    write_file("/storage/meshcore/id", 3, 'k');
    write_file("/storage/lora.toml", 3, 'k');
    mkdirp("/storage/userdir");
    CHECK(files_delete("/storage/userdir") == FILES_ERR_PROTECTED && exists("/storage/userdir")); // a root directory
    CHECK(files_delete("/storage/meshcore/id") == FILES_ERR_PROTECTED && exists("/storage/meshcore/id"));
    CHECK(files_delete("/storage/lora.toml") == FILES_ERR_PROTECTED && exists("/storage/lora.toml"));
    CHECK(files_delete("/storage") == FILES_ERR_PROTECTED && files_delete("/storage/../x") == FILES_ERR_ARG);
    CHECK(files_delete("/storage/b.txt") == FILES_OK && !exists("/storage/b.txt"));
    CHECK(files_delete("/storage/b.txt") == FILES_ERR_OPEN);
    mkdirp("/sd");
    mkdirp("/sd/full");
    write_file("/sd/full/f", 1, 'x');
    CHECK(files_delete("/sd/full") == FILES_ERR_NOT_EMPTY && exists("/sd/full/f"));
    CHECK(files_delete("/sd/full/f") == FILES_OK && files_delete("/sd/full") == FILES_OK);
    CHECK(files_delete("/storage/full/f") == FILES_ERR_PROTECTED);
}

static void test_read(void) {
    uint8_t buf[16];
    size_t got;
    uint32_t total;
    write_file("/storage/r.txt", 10, 'r');
    CHECK(files_read_at("/storage/r.txt", 4, buf, sizeof(buf), &got, &total) == FILES_OK && got == 6 && total == 10);
    CHECK(files_read_at("/storage/r.txt", 0, buf, 4, &got, &total) == FILES_OK && got == 4);
    CHECK(files_read_at("/storage/r.txt", 99, buf, 4, &got, &total) == FILES_OK && got == 0);
    CHECK(files_read_at("/storage/lora.toml", 0, buf, 4, &got, &total) == FILES_OK); // protected, not secret
    CHECK(files_read_at("/storage/meshcore/id", 0, buf, 4, &got, &total) == FILES_ERR_PROTECTED);
    CHECK(files_read_at("/storage/none", 0, buf, 4, &got, &total) == FILES_ERR_OPEN);
}

static void test_copy(void) {
    static uint8_t buf[64];
    mkdirp("/sd");
    write_file("/storage/big.bin", 1000, 'b');
    CHECK(files_copy("/storage/big.bin", "/sd/big.bin", buf, sizeof(buf), UINT64_MAX, progress, NULL) == FILES_OK);
    CHECK(exists("/sd/big.bin") && !exists("/sd/big.bin.lichen-part") && calls == 16);
    CHECK(files_copy("/storage/big.bin", "/sd/big.bin", buf, sizeof(buf), UINT64_MAX, NULL, NULL) == FILES_ERR_EXISTS);
    CHECK(files_copy("/storage/big.bin", "/sd/x.bin", buf, sizeof(buf), 999, NULL, NULL) == FILES_ERR_SPACE);
    CHECK(!exists("/sd/x.bin") && !exists("/sd/x.bin.lichen-part"));
    stop_at = true; // cancel part-way: nothing is left behind
    CHECK(files_copy("/storage/big.bin", "/sd/c.bin", buf, sizeof(buf), UINT64_MAX, progress, NULL) == FILES_CANCELLED);
    CHECK(!exists("/sd/c.bin") && !exists("/sd/c.bin.lichen-part"));
    stop_at = false;
    mkdirp("/storage/meshcore");
    write_file("/storage/meshcore/id", 3, 'k');
    CHECK(files_copy("/storage/meshcore/id", "/sd/l", buf, sizeof(buf), UINT64_MAX, NULL, NULL) == FILES_ERR_PROTECTED);
    CHECK(files_copy("/sd/big.bin", "/storage/c6/big.bin", buf, sizeof(buf), UINT64_MAX, NULL, NULL) == FILES_ERR_PROTECTED);
    CHECK(files_copy("/sd/big.bin", "/storage/lora.toml", buf, sizeof(buf), UINT64_MAX, NULL, NULL) == FILES_ERR_PROTECTED);
    CHECK(files_copy("/storage/big.bin", "/sd/lichen/messages/x", buf, sizeof(buf), UINT64_MAX, NULL, NULL) == FILES_ERR_PROTECTED);
    CHECK(files_copy("/storage/zdir", "/sd/zdir", buf, sizeof(buf), UINT64_MAX, NULL, NULL) == FILES_ERR_NOT_FILE);
    files_fail_t fail = {FILES_STEP_NONE, 0}; // #443: the step and errno say which of the failures it was
    CHECK(files_copy_ex("/storage/nothing", "/sd/n", buf, sizeof(buf), UINT64_MAX, NULL, NULL, &fail) == FILES_ERR_SRC);
    CHECK(fail.step == FILES_STEP_SRC_STAT && fail.err == ENOENT);
    fail.step = FILES_STEP_NONE;
    CHECK(files_copy_ex("/storage/big.bin", "/sd/nodir/x", buf, sizeof(buf), UINT64_MAX, NULL, NULL, &fail) == FILES_ERR_CREATE);
    CHECK(fail.step == FILES_STEP_DST_OPEN && fail.err == ENOENT);
    CHECK(!strcmp(files_step_name(fail.step), "dst_open") && !strcmp(files_step_name(FILES_STEP_SD_BEGIN), "sd_begin"));
    CHECK(!exists("/sd/nodir/x.lichen-part"));
    CHECK(files_copy("/storage/big.bin", "/sd/y", NULL, 0, UINT64_MAX, NULL, NULL) == FILES_ERR_NO_MEM);
    write_file("/sd/u.part", 3, 'u'); // a user's own .part file is never touched
    CHECK(files_copy("/storage/big.bin", "/sd/u", buf, sizeof(buf), UINT64_MAX, NULL, NULL) == FILES_OK && exists("/sd/u.part"));
    write_file("/sd/z.lichen-part", 3, 'p'); // our own leftover (power cut) is replaced
    CHECK(files_copy("/storage/big.bin", "/sd/z", buf, sizeof(buf), UINT64_MAX, NULL, NULL) == FILES_OK && !exists("/sd/z.lichen-part"));
}

// FatFs's file lock (#274) answers EACCES for a file another task has open. A read-only directory gives the host the
// same errno: the Files app says "in use", leaves the file as it was and does not blame the card (FILES_ERR_IO).
static void test_texts(void) {
    // the three copy failures and the name refusal each get their own sentence
    files_result_t const all[] = {FILES_ERR_OPEN, FILES_ERR_CARD, FILES_ERR_SRC, FILES_ERR_DEST, FILES_ERR_CREATE, FILES_ERR_NAME};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        for (size_t j = i + 1; j < sizeof(all) / sizeof(all[0]); j++) {
            CHECK(strcmp(files_result_text(all[i]), files_result_text(all[j])) != 0);
        }
        CHECK(strlen(files_result_text(all[i])) < 90); // action_done's buffer is 96
    }
    CHECK(strstr(files_result_text(FILES_ERR_CARD), "not ready") != NULL);
    CHECK(strstr(files_result_text(FILES_ERR_SRC), "source") != NULL);
    CHECK(strstr(files_result_text(FILES_ERR_DEST), "name not allowed") != NULL);
}

// The create failure is named by its errno, so the "name not allowed" hint appears only when it can be the name
static void test_dest_and_strikes(void) {
    CHECK(files_dest_result(EINVAL) == FILES_ERR_DEST && files_dest_result(ENOSPC) == FILES_ERR_SPACE);
    CHECK(files_dest_result(ENOENT) == FILES_ERR_CREATE && files_dest_result(EIO) == FILES_ERR_CREATE &&
          files_dest_result(ENOMEM) == FILES_ERR_CREATE && files_dest_result(EROFS) == FILES_ERR_CREATE);
    // the old rule: (IO || OPEN) and not ENFILE/EMFILE; SRC and DEST are the old OPEN, so they strike as OPEN did
    files_result_t const old[] = {FILES_ERR_IO, FILES_ERR_OPEN};
    files_result_t const now[] = {FILES_ERR_SRC, FILES_ERR_DEST, FILES_ERR_CREATE};
    int const            errs[] = {0, EIO, ENODEV, ENOENT, EINVAL, ENFILE, EMFILE};
    for (size_t e = 0; e < sizeof(errs) / sizeof(errs[0]); e++) {
        for (size_t i = 0; i < 2; i++) {
            CHECK(files_card_strike(old[i], errs[e]) == (errs[e] != ENFILE && errs[e] != EMFILE));
        }
        for (size_t i = 0; i < 3; i++) {
            CHECK(files_card_strike(now[i], errs[e]) == files_card_strike(FILES_ERR_OPEN, errs[e]));
        }
        CHECK(!files_card_strike(FILES_OK, errs[e]) && !files_card_strike(FILES_CANCELLED, errs[e]));
    }
    CHECK(!files_card_strike(FILES_ERR_BUSY, EIO) && !files_card_strike(FILES_ERR_CARD, EIO) &&
          !files_card_strike(FILES_ERR_NAME, EIO) && !files_card_strike(FILES_ERR_SPACE, EIO));
}

static void test_busy(void) {
    static uint8_t buf[64];
    char           rp[512];
    if (geteuid() == 0) {
        printf("files_ops_test: running as root, the EACCES cases are skipped\n");
        return;
    }
    mkdirp("/sd");
    mkdirp("/sd/held");
    write_file("/sd/held/f", 5, 'h');
    write_file("/sd/held/r", 5, 'h');
    fpath("/sd/held/r", rp);
    chmod(rp, 0000);
    fpath("/sd/held", rp);
    chmod(rp, 0500);
    CHECK(files_delete("/sd/held/f") == FILES_ERR_BUSY && exists("/sd/held/f"));
    CHECK(files_copy("/sd/held/f", "/sd/held/g", buf, sizeof(buf), UINT64_MAX, NULL, NULL) == FILES_ERR_BUSY);
    CHECK(!exists("/sd/held/g") && !exists("/sd/held/g.lichen-part"));
    size_t   got;
    uint32_t total;
    CHECK(files_read_at("/sd/held/r", 0, buf, 4, &got, &total) == FILES_ERR_BUSY);
    CHECK(strstr(files_result_text(FILES_ERR_BUSY), "In use") != NULL);
    // an unreadable source is "could not read the source", not the card's fault (#443)
    files_fail_t fail = {FILES_STEP_NONE, 0};
    CHECK(files_copy_ex("/sd/held/r", "/sd/held/h", buf, sizeof(buf), UINT64_MAX, NULL, NULL, &fail) == FILES_ERR_BUSY);
    CHECK(fail.step == FILES_STEP_SRC_OPEN && fail.err == EACCES);
    chmod(rp, 0700);
    fpath("/sd/held/r", rp);
    chmod(rp, 0600);
}

int main(void) {
    char tmpl[] = "/tmp/files_ops_XXXXXX";
    if (!mkdtemp(tmpl)) {
        return 2;
    }
    snprintf(files_test_root, sizeof(files_test_root), "%s", tmpl);
    files_add_app_root("meshcore", true); // MeshCore's record (#230 E1d)
    mkdirp("/storage");
    test_list();
    test_delete();
    test_read();
    test_copy();
    test_texts();
    test_dest_and_strikes();
    test_busy();
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -r %s", tmpl); // test data only, in our own mkdtemp directory
    if (system(cmd) != 0) {
        printf("cleanup of %s failed\n", tmpl);
    }
    if (failures) {
        printf("files_ops_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("files_ops_test: all passed\n");
    return 0;
}
