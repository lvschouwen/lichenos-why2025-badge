#define _XOPEN_SOURCE 700
// The file calls an SD app gets (#533, export_file.c) over a real temporary directory standing in for the app's SD
// directory: open, read, seek, write and remove inside it, and refused outside it. The SD lock, the context and the card
// are recorded fakes. Run under ASan: a handle used after close-all or by another app must be an error, never a read.
#include "export_file.h"
#include "lichen_fs.h"
#include "sdcard.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void  *lichen_exp_fopen(char const *path, char const *mode);
int    lichen_exp_fclose(void *h);
size_t lichen_exp_fread(void *ptr, size_t size, size_t count, void *h);
size_t lichen_exp_fwrite(void const *ptr, size_t size, size_t count, void *h);
int    lichen_exp_fseek(void *h, long off, int whence);
long   lichen_exp_ftell(void *h);
int    lichen_exp_fflush(void *h);
int    lichen_exp_remove(char const *path);
int    lichen_exp_rename(char const *from, char const *to);
int    lichen_exp_mkdir(char const *path, int mode);

int host_log_count;

static lichen_ctx_t ctx_now = (lichen_ctx_t)0x101;
static char         dir[200], outside[260];
static bool         mounted = true, lock_free = true, batch_open;
static uint32_t     generation = 1;
static int          batches, ends;

lichen_ctx_t lichen_ctx_self(void) {
    return ctx_now;
}
char const *lichen_fs_sd_dir(lichen_ctx_t ctx) {
    return ctx && mounted ? dir : NULL;
}
bool lichen_fs_sd_begin(lichen_ctx_t ctx, uint32_t wait_ms) {
    (void)wait_ms;
    if (!ctx || !mounted || !lock_free || batch_open) {
        return false;
    }
    batch_open = true;
    batches++;
    return true;
}
void lichen_fs_sd_end(lichen_ctx_t ctx, int err) {
    (void)ctx;
    (void)err;
    batch_open = false;
    ends++;
}
bool sd_mounted(void) {
    return mounted;
}
bool sd_begin(uint32_t wait_ms) {
    (void)wait_ms;
    return lichen_fs_sd_begin((lichen_ctx_t)1, 0);
}
void sd_end(sd_io_t r) {
    (void)r;
    lichen_fs_sd_end(0, 0);
}
void sd_info(sd_info_t *out) {
    memset(out, 0, sizeof(*out));
    out->state      = mounted ? SD_STATE_MOUNTED : SD_STATE_REMOVED;
    out->generation = generation;
}

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static int remove_entry(char const *path, struct stat const *st, int flag, struct FTW *ftw) {
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static bool exists(char const *rel) {
    char p[300];
    snprintf(p, sizeof(p), "%s/%s", dir, rel);
    struct stat st;
    return stat(p, &st) == 0;
}

static void round_trip(void) {
    void *f = lichen_exp_fopen("save.dat", "wb");
    CHECK(f && exists("save.dat"));
    CHECK(lichen_exp_fwrite("hello world", 1, 11, f) == 11);
    CHECK(lichen_exp_ftell(f) == 11);
    CHECK(lichen_exp_fflush(f) == 0);
    CHECK(lichen_exp_fclose(f) == 0);
    CHECK(!batch_open);

    f = lichen_exp_fopen("save.dat", "r+");
    char b[16] = {0};
    CHECK(f && lichen_exp_fread(b, 1, 5, f) == 5 && memcmp(b, "hello", 5) == 0);
    CHECK(lichen_exp_fseek(f, 6, SEEK_SET) == 0 && lichen_exp_fread(b, 1, 5, f) == 5 && memcmp(b, "world", 5) == 0);
    CHECK(lichen_exp_fread(b, 1, 5, f) == 0); // the end
    CHECK(lichen_exp_fseek(f, -5, SEEK_END) == 0 && lichen_exp_ftell(f) == 6);
    CHECK(lichen_exp_fseek(f, 0, 99) == -1);
    CHECK(lichen_exp_fread(b, 4, 1, f) == 1); // items, not bytes
    CHECK(lichen_exp_fclose(f) == 0);

    f = lichen_exp_fopen("save.dat", "ab");
    CHECK(f && lichen_exp_fwrite("!", 1, 1, f) == 1 && lichen_exp_fclose(f) == 0);
    CHECK(lichen_exp_mkdir("sub", 0777) == 0 && exists("sub"));
    CHECK(lichen_exp_rename("save.dat", "sub/moved.dat") == 0 && exists("sub/moved.dat") && !exists("save.dat"));
    CHECK(lichen_exp_remove("sub/moved.dat") == 0 && !exists("sub/moved.dat"));
    CHECK(lichen_exp_remove("sub") == 0 && !exists("sub")); // an empty folder too
    CHECK(lichen_exp_remove("sub") == -1);
    CHECK(lichen_exp_fopen("missing.dat", "r") == NULL && errno == ENOENT);
    CHECK(lichen_exp_fread(b, 1, 1, NULL) == 0 && lichen_exp_fclose(NULL) == -1);
    CHECK(lichen_exp_fseek(NULL, 0, SEEK_SET) == -1 && lichen_exp_ftell(NULL) == -1 && lichen_exp_fwrite("x", 1, 1, NULL) == 0);
    CHECK(lichen_exp_fflush(NULL) == 0 && lichen_exp_fflush((void *)8) == -1); // a null handle never falls through to descriptor 0
}

static void refused_outside(void) {
    int logs = host_log_count;
    CHECK(lichen_exp_fopen("../secret.txt", "r") == NULL && errno == EACCES);
    CHECK(lichen_exp_fopen(outside, "r") == NULL && errno == EACCES); // absolute, outside the app's directory
    CHECK(lichen_exp_fopen("a/../../secret.txt", "w") == NULL);
    CHECK(lichen_exp_remove(outside) == -1 && access(outside, F_OK) == 0); // the file next to the dir survives
    CHECK(lichen_exp_rename("x", outside) == -1 && lichen_exp_rename(outside, "x") == -1);
    CHECK(lichen_exp_mkdir("../evil", 0777) == -1);
    CHECK(lichen_exp_fopen("", "r") == NULL && lichen_exp_fopen(NULL, "r") == NULL);
    CHECK(lichen_exp_fopen("ok.txt", "rw") == NULL && errno == EINVAL);
    CHECK(lichen_exp_fopen("ok.txt", "") == NULL && lichen_exp_fopen("ok.txt", "z") == NULL);
    CHECK(host_log_count > logs);
    CHECK(batches == ends && !batch_open); // no batch left held by a refusal
}

static void book_limits(void) {
    void *f[6];
    char  n[16];
    for (int i = 0; i < 4; i++) {
        snprintf(n, sizeof(n), "f%d", i);
        f[i] = lichen_exp_fopen(n, "w");
        CHECK(f[i]);
    }
    CHECK(lichen_exp_fopen("f4", "w") == NULL && errno == EMFILE && !exists("f4")); // the fifth
    ctx_now = (lichen_ctx_t)0x202; // another app: its own four, and no use of the first one's handles
    char b[4];
    CHECK(lichen_exp_fread(b, 1, 1, f[0]) == 0 && lichen_exp_fwrite("x", 1, 1, f[0]) == 0);
    CHECK(lichen_exp_fseek(f[0], 0, SEEK_SET) == -1 && lichen_exp_ftell(f[0]) == -1 && lichen_exp_fclose(f[0]) == -1);
    CHECK(errno == EBADF);
    void *other = lichen_exp_fopen("g0", "w");
    CHECK(other && export_file_close_all((lichen_ctx_t)0x101) == 4);
    ctx_now = (lichen_ctx_t)0x101;
    CHECK(lichen_exp_fwrite("x", 1, 1, f[0]) == 0 && lichen_exp_fclose(f[1]) == -1); // closed by the loader: an error
    CHECK(lichen_exp_fopen("f5", "w") != NULL); // the slots are free again
    ctx_now = (lichen_ctx_t)0x202;
    CHECK(lichen_exp_fclose(other) == 0); // untouched by the other app's close-all
    ctx_now = (lichen_ctx_t)0x101;
    CHECK(export_file_close_all((lichen_ctx_t)0x101) == 1 && export_file_close_all((lichen_ctx_t)0x101) == 0);
    CHECK(lichen_exp_fread(b, 1, 1, (void *)&b[0]) == 0 && lichen_exp_fread(b, 1, 1, (void *)1) == 0); // not slots at all
}

static void card_goes(void) {
    void *f = lichen_exp_fopen("keep.dat", "w");
    CHECK(f);
    mounted = false; // "fw sd unmount" with a file open
    char b[2];
    CHECK(lichen_exp_fread(b, 1, 1, f) == 0 && lichen_exp_fwrite("x", 1, 1, f) == 0); // an error, not a crash
    mounted = true;
    generation++; // and back, another mount: the old handle stays dead
    CHECK(lichen_exp_fwrite("x", 1, 1, f) == 0 && errno == EBADF);
    CHECK(lichen_exp_fclose(f) == -1); // dropped, not closed (its descriptor died with the mount)
    CHECK(lichen_exp_fclose(f) == -1 && errno == EBADF);
    void *g = lichen_exp_fopen("new.dat", "w");
    CHECK(g && lichen_exp_fclose(g) == 0);
    void *h = lichen_exp_fopen("late.dat", "w");
    mounted = false;
    CHECK(export_file_close_all((lichen_ctx_t)0x101) == 1); // the loader at card removal: slots dropped, no batch needed
    mounted = true;
    CHECK(lichen_exp_fclose(h) == -1);
    h = lichen_exp_fopen("busy.dat", "w");
    lock_free = false; // card busy for another holder: the handle stays
    CHECK(lichen_exp_fclose(h) == -1 && errno == EBUSY);
    lock_free = true;
    CHECK(lichen_exp_fclose(h) == 0);
    CHECK(lichen_exp_fopen("x", "w") != NULL && export_file_close_all((lichen_ctx_t)0x101) == 1);
    ctx_now = 0; // the OS's own task: refused
    CHECK(lichen_exp_fopen("os.dat", "w") == NULL);
    ctx_now = (lichen_ctx_t)0x101;
}

int main(void) {
    char base[] = "/tmp/export_file_testXXXXXX";
    if (!mkdtemp(base)) {
        return 1;
    }
    snprintf(dir, sizeof(dir), "%s/hello", base);
    snprintf(outside, sizeof(outside), "%s/secret.txt", base);
    mkdir(dir, 0777);
    FILE *s = fopen(outside, "w");
    if (!s) {
        return 1;
    }
    fputs("x", s);
    fclose(s);

    round_trip();
    refused_outside();
    book_limits();
    card_goes();

    nftw(base, remove_entry, 8, FTW_DEPTH | FTW_PHYS); // not system(): no child process in a host test
    if (fails) {
        printf("%d failures\n", fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
