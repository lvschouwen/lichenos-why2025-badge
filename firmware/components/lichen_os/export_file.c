// The file calls an SD app resolves under their libc names (#533): fopen fclose fread fwrite fseek ftell fflush remove
// rename mkdir, mapped in tools/export_allow.txt. Sandboxed to the app's own SD directory (lichen_fs_sd_dir, rule in
// lichen_api/fs_path.c), with the SD batch lock taken per call and never held between calls.
//
// The app never gets a real FILE: the handle is a slot of a small book {ctx, fd, card generation}, so a handle the loader
// closed (unload, card removal) or one that belongs to another app is an error on the next call, never a use after free.
// Underneath are plain descriptors (no stdio buffers; a call is one read or write). An unmount closes nothing: the card's
// mount frees its descriptors and the numbers may be handed out again, so a slot of an older card generation is only
// dropped, never closed. At most FILES_PER_APP open files per app, FILES_TOTAL together.
//
// Not supported: the getc/putc/feof/ferror family and stdio's buffering (fflush only syncs the file); exit is in
// sd_app_loader.c. A call inside the app's own lichen_fs_sd_begin batch waits SD_WAIT_MS for the lock it holds itself and
// then fails: an app uses either the batch or these calls, not both at once.
#include "export_file.h"

#include "fs_path.h"
#include "lichen_fs.h"
#include "sdcard.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILES_PER_APP 4
#define FILES_TOTAL   16 // internal .bss: 16 x 12 B
#define SD_WAIT_MS    1000
#define CLOSE_WAIT_MS 2000
#define PATH_CAP      160

static char const TAG[] = "sdfile";

typedef struct {
    lichen_ctx_t ctx; // 0: free
    int          fd;  // -1 while the open is under way
    uint32_t     gen; // the card generation it was opened on
} slot_t;

static slot_t      book[FILES_TOTAL];
static portMUX_TYPE book_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t card_generation(void) {
    sd_info_t info;
    sd_info(&info);
    return info.state == SD_STATE_MOUNTED ? info.generation : 0; // 0: no card; a mount's generation is never 0
}

// ---- the book (critical sections only; nothing slow inside) ------------------------------------------------------------

static slot_t *slot_reserve(lichen_ctx_t ctx) {
    slot_t *got = NULL;
    int     own = 0;
    portENTER_CRITICAL(&book_mux);
    for (size_t i = 0; i < FILES_TOTAL; i++) {
        if (book[i].ctx == ctx) {
            own++;
        } else if (!book[i].ctx && !got) {
            got = &book[i];
        }
    }
    if (got && own < FILES_PER_APP) {
        got->ctx = ctx;
        got->fd  = -1;
        got->gen = 0;
    } else {
        got = NULL;
    }
    portEXIT_CRITICAL(&book_mux);
    return got;
}

static void slot_free(slot_t *s) {
    portENTER_CRITICAL(&book_mux);
    s->ctx = 0;
    s->fd  = -1;
    portEXIT_CRITICAL(&book_mux);
}

// The slot a handle names, when it is a slot of the book and ctx's own with an open descriptor. Copies it out.
static bool slot_get(lichen_ctx_t ctx, void const *h, slot_t *out) {
    uintptr_t base = (uintptr_t)book, at = (uintptr_t)h;
    if (!ctx || at < base || at - base >= sizeof(book) || (at - base) % sizeof(slot_t)) {
        return false;
    }
    bool ok;
    portENTER_CRITICAL(&book_mux);
    slot_t const *s = (slot_t const *)h;
    ok              = s->ctx == ctx && s->fd >= 0;
    if (ok) {
        *out = *s;
    }
    portEXIT_CRITICAL(&book_mux);
    return ok;
}

// ---- one call ----------------------------------------------------------------------------------------------------------

typedef struct {
    lichen_ctx_t ctx;
    slot_t       slot; // the open file of a FILE call
} call_t;

// The SD batch, and for a FILE call (file) the app's open file on this card; a NULL handle is a handle that is none. False: errno says why and no batch is held.
static bool call_begin(call_t *c, void *h, bool file) {
    c->ctx  = lichen_ctx_self();
    c->slot = (slot_t){0, -1, 0}; // never a descriptor 0 by accident
    if (!lichen_fs_sd_begin(c->ctx, SD_WAIT_MS)) {
        errno = sd_mounted() ? EBUSY : ENODEV;
        return false;
    }
    if (file && (!slot_get(c->ctx, h, &c->slot) || c->slot.gen != card_generation())) {
        lichen_fs_sd_end(c->ctx, 0);
        errno = EBADF; // closed by the loader, not this app's, or the card went or was swapped
        return false;
    }
    return true;
}

static int call_end(call_t *c, int rc) {
    int err = rc < 0 ? errno : 0;
    lichen_fs_sd_end(c->ctx, err);
    errno = err;
    return rc;
}

// A path of the app, inside its SD directory. The directory is asked before any batch (its first use makes it, under the
// lock). Refusals are logged, a few: an app that probes the sandbox is worth a line, not a flood.
static bool app_path(char const *path, bool with_root, char *out, size_t cap) {
    static uint8_t logged;
    char const    *dir = lichen_fs_sd_dir(lichen_ctx_self());
    if (!dir) {
        errno = ENODEV;
        return false;
    }
    if (!fs_sandbox_resolve(dir, path, with_root, out, cap)) {
        if (logged < 8) {
            logged++;
            ESP_LOGW(TAG, "%s: path refused (outside its directory, or bad): %.60s", dir, path ? path : "(null)");
        }
        errno = EACCES;
        return false;
    }
    return true;
}

// "r", "w", "a" with an optional "+" and "b" in either order; anything else is refused
static int mode_flags(char const *m) {
    if (!m || !m[0]) {
        return -1;
    }
    int  base = m[0] == 'r' ? O_RDONLY : m[0] == 'w' ? (O_WRONLY | O_CREAT | O_TRUNC) : m[0] == 'a' ? (O_WRONLY | O_CREAT | O_APPEND) : -1;
    bool plus = false;
    int  n    = 1;
    for (; base >= 0 && m[n] && n < 3; n++) {
        if (m[n] == '+' && !plus) {
            plus = true;
        } else if (m[n] != 'b') {
            return -1;
        }
    }
    if (base < 0 || m[n]) {
        return -1;
    }
    return plus ? (base & ~(O_RDONLY | O_WRONLY)) | O_RDWR : base;
}

// ---- the calls ---------------------------------------------------------------------------------------------------------

void *lichen_exp_fopen(char const *path, char const *mode) {
    int  flags = mode_flags(mode);
    char full[PATH_CAP];
    if (flags < 0) {
        errno = EINVAL;
        return NULL;
    }
    if (!app_path(path, false, full, sizeof(full))) {
        return NULL;
    }
    call_t c;
    if (!call_begin(&c, NULL, false)) {
        return NULL;
    }
    slot_t *s   = slot_reserve(c.ctx);
    int     rc  = -1;
    void   *out = NULL;
    if (!s) {
        errno = EMFILE;
    } else {
        int fd = open(full, flags, 0666);
        if (fd < 0) {
            slot_free(s);
        } else {
            uint32_t gen = card_generation();
            portENTER_CRITICAL(&book_mux);
            s->gen = gen;
            s->fd  = fd;
            portEXIT_CRITICAL(&book_mux);
            out = s;
            rc  = 0;
        }
    }
    call_end(&c, rc);
    return out;
}

int lichen_exp_fclose(void *h) {
    lichen_ctx_t ctx   = lichen_ctx_self();
    bool         batch = lichen_fs_sd_begin(ctx, SD_WAIT_MS);
    if (!batch && sd_mounted()) {
        errno = EBUSY; // the card is there but busy: the handle stays, the app may ask again
        return -1;
    }
    slot_t s;
    if (!slot_get(ctx, h, &s)) {
        if (batch) {
            lichen_fs_sd_end(ctx, 0);
        }
        errno = EBADF;
        return -1;
    }
    slot_free((slot_t *)h);
    int rc = 0;
    if (batch && s.gen == card_generation()) {
        rc = close(s.fd) == 0 ? 0 : -1;
    } else {
        rc    = -1; // the card is gone: the descriptor died with the mount (and must not be closed: the number may be reused)
        errno = EIO;
    }
    int err = rc < 0 ? errno : 0;
    if (batch) {
        lichen_fs_sd_end(ctx, err == EIO ? 0 : err);
    }
    errno = err;
    return rc;
}

// A read or write of every byte asked for, or what came before the end or the error
typedef ssize_t (*xfer_fn)(int fd, void *buf, size_t n);
static ssize_t xfer_read(int fd, void *buf, size_t n) {
    return read(fd, buf, n);
}
static ssize_t xfer_write(int fd, void *buf, size_t n) {
    return write(fd, buf, n);
}

static size_t xfer(void *buf, size_t size, size_t count, void *h, xfer_fn fn) {
    if (size == 0 || count == 0) {
        return 0;
    }
    if (count > SIZE_MAX / size) {
        errno = EINVAL;
        return 0;
    }
    call_t c;
    if (!call_begin(&c, h, true)) {
        return 0;
    }
    size_t total = size * count, done = 0;
    int    rc    = 0;
    while (done < total) {
        ssize_t r = fn(c.slot.fd, (uint8_t *)buf + done, total - done);
        if (r < 0) {
            rc = -1;
            break;
        }
        if (r == 0) {
            break; // the end of the file (a write of nothing: the card is full)
        }
        done += (size_t)r;
    }
    call_end(&c, done == 0 ? rc : 0); // an error after some data is told by the short count
    return done / size;
}

size_t lichen_exp_fread(void *ptr, size_t size, size_t count, void *h) {
    return xfer(ptr, size, count, h, xfer_read);
}

size_t lichen_exp_fwrite(void const *ptr, size_t size, size_t count, void *h) {
    return xfer((void *)ptr, size, count, h, xfer_write);
}

int lichen_exp_fseek(void *h, long off, int whence) {
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        errno = EINVAL;
        return -1;
    }
    call_t c;
    if (!call_begin(&c, h, true)) {
        return -1;
    }
    return call_end(&c, lseek(c.slot.fd, off, whence) < 0 ? -1 : 0);
}

long lichen_exp_ftell(void *h) {
    call_t c;
    if (!call_begin(&c, h, true)) {
        return -1;
    }
    off_t at = lseek(c.slot.fd, 0, SEEK_CUR);
    call_end(&c, at < 0 ? -1 : 0);
    return (long)at;
}

int lichen_exp_fflush(void *h) {
    if (!h) {
        return 0; // nothing is buffered here
    }
    call_t c;
    if (!call_begin(&c, h, true)) {
        return -1;
    }
    return call_end(&c, fsync(c.slot.fd) == 0 ? 0 : -1);
}

int lichen_exp_remove(char const *path) {
    char full[PATH_CAP];
    if (!app_path(path, false, full, sizeof(full))) {
        return -1;
    }
    call_t c;
    if (!call_begin(&c, NULL, false)) {
        return -1;
    }
    int rc = unlink(full);
    if (rc < 0 && (errno == EISDIR || errno == EPERM)) {
        rc = rmdir(full); // remove() takes an empty folder too
    }
    return call_end(&c, rc);
}

int lichen_exp_rename(char const *from, char const *to) {
    char a[PATH_CAP], b[PATH_CAP];
    if (!app_path(from, false, a, sizeof(a)) || !app_path(to, false, b, sizeof(b))) {
        return -1;
    }
    if (strcmp(a, b) != 0 && fs_sandbox_within(a, b)) {
        errno = EINVAL; // a folder into itself
        return -1;
    }
    call_t c;
    if (!call_begin(&c, NULL, false)) {
        return -1;
    }
    // A folder only changes its name, in its own parent: FAT folds case and answers to 8.3 aliases, so "within" cannot be
    // decided from the names, and a folder moved into its own subtree must never reach FatFs. Files move anywhere inside.
    struct stat st;
    if (stat(a, &st) == 0 && S_ISDIR(st.st_mode) && !fs_sandbox_same_parent(a, b)) {
        errno = EINVAL;
        return call_end(&c, -1);
    }
    return call_end(&c, rename(a, b));
}

// Only below the app's directory (or the directory itself, which exists: EEXIST like any other)
int lichen_exp_mkdir(char const *path, int mode) {
    (void)mode; // FAT has none
    char full[PATH_CAP];
    if (!app_path(path, true, full, sizeof(full))) {
        return -1;
    }
    call_t c;
    if (!call_begin(&c, NULL, false)) {
        return -1;
    }
    return call_end(&c, mkdir(full, 0777));
}

// ---- the loader's side -------------------------------------------------------------------------------------------------

size_t export_file_close_all(lichen_ctx_t ctx) {
    size_t open = 0;
    portENTER_CRITICAL(&book_mux);
    for (size_t i = 0; i < FILES_TOTAL; i++) {
        open += book[i].ctx == ctx;
    }
    portEXIT_CRITICAL(&book_mux);
    if (!open || !ctx) {
        return 0;
    }
    bool batch = sd_begin(CLOSE_WAIT_MS);
    for (size_t i = 0; i < FILES_TOTAL; i++) {
        slot_t s;
        bool   mine;
        portENTER_CRITICAL(&book_mux);
        s    = book[i];
        mine = s.ctx == ctx;
        if (mine) {
            book[i].ctx = 0;
            book[i].fd  = -1;
        }
        portEXIT_CRITICAL(&book_mux);
        if (!mine || s.fd < 0) {
            continue;
        }
        if (batch && s.gen == card_generation()) {
            close(s.fd);
        } else if (sd_mounted() && s.gen == card_generation()) {
            ESP_LOGE(TAG, "descriptor %d of a leaving app not closed: the card stayed busy", s.fd);
        } // else: its card is gone, the descriptor with it
    }
    if (batch) {
        sd_end(SD_IO_NEUTRAL);
    }
    ESP_LOGI(TAG, "%u open file(s) of a leaving app dropped", (unsigned)open);
    return open;
}
