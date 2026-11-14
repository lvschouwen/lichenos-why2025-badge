// The fs::FS shim (#65): MeshCore's file paths mapped below a root directory, plain stdio underneath
#include "SPIFFS.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#else
#include <sys/statvfs.h>
#endif

#define PATH_MAX_LEN 256

// The slow-call notes (#425) are a bench diagnostic: in the developer build only (the host tests build them too).
// CONFIG_ comes through esp_log.h (the layer rule keeps sdkconfig.h itself out); mesh_task.cpp links against these
// functions in the developer build, so a config that did not reach this file fails the link, not silently.
#if !defined(ESP_PLATFORM) || defined(CONFIG_LICHEN_DEV_COMMANDS)
#define FS_SHIM_SLOW_NOTES 1
#else
#define FS_SHIM_SLOW_NOTES 0
#endif

fs::SPIFFSFS SPIFFS;

static char root[PATH_MAX_LEN]; // empty until the OS sets it (#84 T6)
static void (*drive_fn)(size_t *total, size_t *used);

// Time spent in the file system (#67: where does a slow MeshCore loop go?). Only the badge measures; the mesh task reads
// and resets it once per loop. Atomic: since #257 the mesh task also writes advert blobs after its unlock, while another
// task may be in a file under the mesh lock.
static uint32_t busy_us, busy_ops;
static bool     fail_alloc, fail_reads, fat_rename;

// Counters only the host tests read: kept out of the badge build, where several tasks use files at once and plain
// globals would race
#ifndef ESP_PLATFORM
static uint32_t raw_writes;   // writes and truncates of buffered "w" files
static size_t   largest_read; // the largest single read() the shim asked for (the compare reads)
#define COUNT_RAW_WRITE() (raw_writes++)
#define NOTE_READ(n)      (largest_read = (n) > largest_read ? (n) : largest_read)
#else
#define COUNT_RAW_WRITE() ((void)0)
#define NOTE_READ(n)      ((void)(n))
#endif

void fs_shim_test_faults(bool alloc, bool reads) {
    fail_alloc = alloc;
    fail_reads = reads;
}

void fs_shim_test_fat_rename(bool on) {
    fat_rename = on;
}

#ifdef ESP_PLATFORM
#define SHIM_ERROR(...) ESP_LOGE("fs_shim", __VA_ARGS__)
// The badge always sets the buffer hooks (lichen_alloc): no plain heap fallback, so the app uses no malloc (#283)
#define DEFAULT_MALLOC(n) NULL
#define DEFAULT_FREE(p)   ((void)(p))
#else
#define SHIM_ERROR(...) ((void)0)
#define DEFAULT_MALLOC(n) malloc(n)
#define DEFAULT_FREE(p)   free(p)
#endif

#ifndef ESP_PLATFORM
size_t fs_shim_take_largest_read(void) {
    size_t n     = largest_read;
    largest_read = 0;
    return n;
}

uint32_t fs_shim_take_raw_writes(void) {
    uint32_t n = raw_writes;
    raw_writes = 0;
    return n;
}
#endif

// A "w" file's buffer comes from the owner's hooks (the badge: lichen_alloc, PSRAM first and internal RAM when PSRAM is
// full, so a small prefs or identity save still lands; contacts3 alone is 152 bytes a contact, up to 1000 contacts).
// Without hooks it is plain malloc in the host tests and no buffer on the badge (it sets them before SPIFFS.begin).
static void *(*buf_alloc_hook)(size_t);
static void (*buf_free_hook)(void *);

void fs_shim_set_buffer_hooks(void *(*alloc)(size_t bytes), void (*release)(void *ptr)) {
    buf_alloc_hook = alloc;
    buf_free_hook  = release;
}

static void buf_free(void *p) {
    if (buf_free_hook) {
        buf_free_hook(p);
    } else {
        DEFAULT_FREE(p);
    }
}

// Grows p to n bytes (p NULL: a fresh buffer): a new block, the old bytes copied, the old block released. old is the
// size of what p holds.
static void *buf_grow(void *p, size_t old, size_t n) {
    if (fail_alloc) {
        return NULL;
    }
    void *q = buf_alloc_hook ? buf_alloc_hook(n) : DEFAULT_MALLOC(n);
    if (q && p) {
        memcpy(q, p, old < n ? old : n);
        buf_free(p);
    }
    return q;
}

static bool read_full(int fd, uint8_t *dst, size_t n) {
    if (fail_reads) {
        return false;
    }
    NOTE_READ(n);
    while (n > 0) {
        ssize_t r = ::read(fd, dst, n);
        if (r <= 0) {
            return false;
        }
        dst += r;
        n -= (size_t)r;
    }
    return true;
}

static bool write_full(int fd, uint8_t const *src, size_t n) {
    while (n > 0) {
        ssize_t w = ::write(fd, src, n);
        if (w <= 0) {
            return false;
        }
        src += w;
        n -= (size_t)w;
    }
    return true;
}

#if FS_SHIM_SLOW_NOTES
// The slow calls (#425). A slot is claimed with one atomic add, so two tasks never write the same one; a reader may see
// a slot a writer is still filling, which only blurs one diagnostic line.
static fs_shim_call_t slow_calls[FS_SHIM_SLOW_CALLS];
static uint32_t       slow_count;
#define FORGET_SLOW_CALLS() __atomic_store_n(&slow_count, 0, __ATOMIC_RELAXED)
#else
#define FORGET_SLOW_CALLS() ((void)0)
#endif

uint32_t fs_shim_take_busy_us(uint32_t *ops) {
    uint32_t n = __atomic_exchange_n(&busy_ops, 0, __ATOMIC_RELAXED);
    if (ops) {
        *ops = n;
    }
    FORGET_SLOW_CALLS();
    return __atomic_exchange_n(&busy_us, 0, __ATOMIC_RELAXED);
}

#if FS_SHIM_SLOW_NOTES
size_t fs_shim_slow_calls(fs_shim_call_t *out, size_t max) {
    size_t n = __atomic_load_n(&slow_count, __ATOMIC_RELAXED);
    for (size_t i = 0; i < n && i < max && i < FS_SHIM_SLOW_CALLS; i++) {
        out[i] = slow_calls[i];
    }
    return n;
}

char const *fs_shim_op_name(uint8_t op) {
    static char const *const names[] = {"open", "read", "write", "flush", "close", "exists", "remove", "rename", "dir"};
    return op < sizeof(names) / sizeof(names[0]) ? names[op] : "?";
}
#endif

namespace {
struct Timed;
}
static int64_t                       timed_now(void);
__attribute__((noinline)) static void timed_end(Timed &t);

namespace {
// One file call's time. In the developer build the call also fills in what it did, which only a slow call keeps
// (TIMED_SET). The end is out of line: a dozen call sites share one copy.
struct Timed {
#if FS_SHIM_SLOW_NOTES
    struct {
        uint8_t  op;
        uint32_t bytes = 0, rewritten = 0, sync_us = 0;
    } c;
    char const *path;
#endif
    int64_t start;
    explicit Timed(fs_shim_op_t op, char const *p = NULL) : start(timed_now()) {
#if FS_SHIM_SLOW_NOTES
        c.op = (uint8_t)op;
        path = p;
#else
        (void)op;
        (void)p;
#endif
    }
    ~Timed() { timed_end(*this); }
    static int64_t now() { return timed_now(); }
};
} // namespace

#if FS_SHIM_SLOW_NOTES
#define TIMED_SET(t, field, value) ((t).c.field = (uint32_t)(value))
#else
#define TIMED_SET(t, field, value) ((void)0)
#endif

#ifdef ESP_PLATFORM
static int64_t timed_now(void) {
    return esp_timer_get_time();
}

#if FS_SHIM_SLOW_NOTES
static void note_slow(Timed const &t, uint32_t us) {
    uint32_t i = __atomic_fetch_add(&slow_count, 1, __ATOMIC_RELAXED);
    if (i >= FS_SHIM_SLOW_CALLS) {
        return;
    }
    fs_shim_call_t *s = &slow_calls[i];
    s->op             = t.c.op;
    s->us             = us;
    s->bytes          = t.c.bytes;
    s->rewritten      = t.c.rewritten;
    s->sync_us        = t.c.sync_us;
    char const *base  = t.path ? strrchr(t.path, '/') : NULL;
    char const *name  = base ? base + 1 : t.path ? t.path : "";
    size_t      n     = strnlen(name, sizeof(s->name) - 1); // cut short on purpose
    memcpy(s->name, name, n);
    s->name[n] = '\0';
}
#endif

static void timed_end(Timed &t) {
    uint32_t us = (uint32_t)(timed_now() - t.start);
    __atomic_fetch_add(&busy_us, us, __ATOMIC_RELAXED);
    __atomic_fetch_add(&busy_ops, 1, __ATOMIC_RELAXED);
#if FS_SHIM_SLOW_NOTES
    if (us >= FS_SHIM_SLOW_CALL_US) {
        note_slow(t, us);
    }
#endif
}
#else
static int64_t timed_now(void) {
    return 0;
}

static void timed_end(Timed &t) {
    (void)t;
}
#endif

// The held path's saves (#425): the path (the owner's string) and the newest save not yet taken
static char const    *hold_path;
static fs_shim_held_t held_slot;

// Atomic: other tasks open their own files without the owner's lock and read it then
static char const *held_path(void) {
    return __atomic_load_n(&hold_path, __ATOMIC_ACQUIRE);
}

void fs_shim_drop_held(void) {
    buf_free(held_slot.buf);
    held_slot = fs_shim_held_t{};
}

// Whether path (MeshCore's form, "/contacts3") is the held one
static bool is_held(char const *path) {
    char const *held = held_path();
    return held && path && strcmp(path, held) == 0;
}

void fs_shim_hold(char const *path) {
    __atomic_store_n(&hold_path, path, __ATOMIC_RELEASE);
    if (!path) {
        fs_shim_drop_held(); // a direct save follows: a parked one written later would be older than it
    }
}

void fs_shim_take_held(fs_shim_held_t *out) {
    *out      = held_slot;
    held_slot = fs_shim_held_t{};
}


void fs_shim_set_root(char const *dir) {
    snprintf(root, sizeof(root), "%s", dir ? dir : "");
}

void fs_shim_set_drive_bytes(void (*fn)(size_t *total, size_t *used)) {
    drive_fn = fn;
}

// No "." or ".." component (the app stays below its root) and no empty component ("//")
static bool components_ok(char const *path) {
    for (char const *p = path; *p; ) {
        while (*p == '/') {
            p++;
            if (*p == '/') {
                return false;
            }
        }
        char const *end = p;
        while (*end && *end != '/') {
            end++;
        }
        size_t n = (size_t)(end - p);
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) {
            return false;
        }
        p = end;
    }
    return true;
}

bool fs_shim_format_allowed(char const *r) {
    if (!r || r[0] != '/' || !components_ok(r)) {
        return false;
    }
    char const *last = strrchr(r, '/');
    bool        nested = last != r && last[1] != '\0'; // "/parent/name", not "/name", "/" or "/parent/"
    return nested && strcmp(r, "/storage/c6") != 0; // defence in depth: lichen_fs never hands it out (fs_path.c)
}

static bool full_path(char const *path, char *out) {
    if (!root[0] || !path || !components_ok(path)) {
        return false;
    }
    int n = snprintf(out, PATH_MAX_LEN, "%s%s%s", root, path[0] == '/' ? "" : "/", path);
    return n > 0 && n < PATH_MAX_LEN;
}

// mkdir -p for every directory in full (the last component too when last is true)
static bool make_dirs(char *full, bool last) {
    size_t root_len = strlen(root);
    for (char *p = full + root_len + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            bool ok = ::mkdir(full, 0755) == 0 || errno == EEXIST;
            *p      = '/';
            if (!ok) {
                return false;
            }
        }
    }
    return !last || ::mkdir(full, 0755) == 0 || errno == EEXIST;
}

namespace fs {

File::File(FILE *f, char const *full_path) : f_(f) {
    snprintf(path_, sizeof(path_), "%s", full_path);
}

File::File(char const *full_path, bool held) : held_(held) {
    snprintf(path_, sizeof(path_), "%s", full_path);
}

File::File(char const *full_path, uint8_t *view, size_t len) : buf_(view), len_(len), cap_(len), view_(true) {
    snprintf(path_, sizeof(path_), "%s", full_path);
}

File::File(DIR *d, char const *full_path) : d_(d) {
    snprintf(path_, sizeof(path_), "%s", full_path);
}

File::File(int fd, char const *full_path) : fd_(fd) {
    snprintf(path_, sizeof(path_), "%s", full_path);
}

File &File::operator=(File &&other) noexcept {
    if (this != &other) {
        close();
        f_      = other.f_;
        d_      = other.d_;
        fd_     = other.fd_;
        buf_    = other.buf_;
        len_    = other.len_;
        cap_    = other.cap_;
        pos_    = other.pos_;
        failed_ = other.failed_;
        held_   = other.held_;
        view_   = other.view_;
        rewritten_ = other.rewritten_;
        memcpy(path_, other.path_, sizeof(path_));
        other.f_   = nullptr;
        other.d_   = nullptr;
        other.fd_  = -1;
        other.buf_ = nullptr;
        other.len_ = other.cap_ = other.pos_ = 0;
        other.failed_ = false;
        other.held_   = false;
        other.view_   = false;
    }
    return *this;
}

char const *File::name() const {
    char const *slash = strrchr(path_, '/');
    return slash ? slash + 1 : path_;
}

File File::openNextFile() {
    [[maybe_unused]] Timed timed(FS_OP_DIR, path_);
    struct dirent *e;
    while (d_ && (e = readdir(d_)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char        child[PATH_MAX_LEN];
        struct stat st;
        if (snprintf(child, sizeof(child), "%s/%s", path_, e->d_name) >= (int)sizeof(child) || stat(child, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            return File(opendir(child), child);
        }
        return File(fopen(child, "rb"), child);
    }
    return File();
}

int File::available() {
    if (buffered()) {
        return pos_ < len_ ? (int)(len_ - pos_) : 0;
    }
    long pos = f_ ? ftell(f_) : -1;
    return pos < 0 ? 0 : (int)size() - (int)pos;
}

size_t File::read(uint8_t *buf, size_t len) {
    if (buffered()) { // what this File wrote: the buffer is the whole file
        size_t n = pos_ < len_ ? len_ - pos_ : 0;
        n        = n < len ? n : len;
        if (n > 0) {
            memcpy(buf, buf_ + pos_, n);
            pos_ += n;
        }
        return n;
    }
    [[maybe_unused]] Timed timed(FS_OP_READ, path_);
    size_t n = f_ ? fread(buf, 1, len, f_) : 0;
    TIMED_SET(timed, bytes, n);
    return n;
}

int File::read() {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
}

size_t File::write(uint8_t const *buf, size_t len) {
    if (buffered()) { // into the buffer, no file system call: committed by flush() or close()
        if (failed_ || view_ || len == 0) { // a view of a parked save is read-only
            return 0;
        }
        size_t end = pos_ + len;
        if (end < pos_ || end > FS_SHIM_MAX_FILE) { // also keeps the doubling below from overflowing
            failed_ = true;
            return 0;
        }
        if (end > cap_) {
            size_t cap = cap_ ? cap_ : FS_SHIM_CHUNK;
            while (cap < end) {
                cap *= 2;
            }
            uint8_t *grown = (uint8_t *)buf_grow(buf_, cap_, cap);
            if (!grown) {
                failed_ = true; // a half-written file must not replace the old one
                return 0;
            }
            buf_ = grown;
            cap_ = cap;
        }
        if (pos_ > len_) { // a seek past the end leaves zeroes, as a file does
            memset(buf_ + len_, 0, pos_ - len_);
        }
        memcpy(buf_ + pos_, buf, len);
        pos_ = end;
        len_ = end > len_ ? end : len_;
        return len;
    }
    [[maybe_unused]] Timed timed(FS_OP_WRITE, path_);
    size_t n = f_ ? fwrite(buf, 1, len, f_) : 0;
    TIMED_SET(timed, bytes, n);
    return n;
}

bool File::seek(uint32_t pos) {
    if (buffered()) {
        pos_ = pos;
        return true;
    }
    return f_ && fseek(f_, (long)pos, SEEK_SET) == 0;
}

size_t File::size() {
    struct stat st;
    if (buffered()) {
        return len_;
    }
    if (!f_) {
        return 0;
    }
    fflush(f_);
    return fstat(fileno(f_), &st) == 0 ? (size_t)st.st_size : 0;
}

// Whether the n bytes at the file's position equal expect, read FS_SHIM_COMPARE_READ bytes at a time into a stack
// buffer. LittleFS reads a request of a whole block (4 KB) straight into the caller's buffer: with a PSRAM buffer that
// made esp_flash_read malloc a 4 KB internal temp buffer per chunk (#364). Below the block size LittleFS reads through
// its internal file cache (#362), and the stack buffer is internal and per caller.
static bool same_bytes(int fd, uint8_t const *expect, size_t n) {
    uint8_t got[FS_SHIM_COMPARE_READ];
    for (size_t at = 0; at < n; at += sizeof(got)) {
        size_t k = n - at < sizeof(got) ? n - at : sizeof(got);
        if (!read_full(fd, got, k) || memcmp(got, expect + at, k) != 0) {
            return false;
        }
    }
    return true;
}

// Rewrites only the chunks that differ from what is on the file system, then cuts off what is left over. An unchanged
// chunk costs a read, not a flash erase. On the badge's LittleFS (#300) the rewrite is copy-on-write and becomes the
// file only when it is synced or closed: a cut before that leaves the old file whole.
bool File::commit() {
    struct stat st;
    if (fstat(fd_, &st) != 0) {
        return false;
    }
    size_t old = (size_t)st.st_size;
    bool   ok  = true;
    rewritten_ = 0;
    for (size_t off = 0; ok && off < len_; off += FS_SHIM_CHUNK) {
        size_t n    = len_ - off < FS_SHIM_CHUNK ? len_ - off : FS_SHIM_CHUNK;
        bool   same = off + n <= old && lseek(fd_, (off_t)off, SEEK_SET) == (off_t)off && same_bytes(fd_, buf_ + off, n);
        if (!same) {
            ok = lseek(fd_, (off_t)off, SEEK_SET) == (off_t)off && write_full(fd_, buf_ + off, n);
            rewritten_ += n;
            COUNT_RAW_WRITE();
        }
    }
    if (ok && old > len_) {
        ok = ftruncate(fd_, (off_t)len_) == 0;
        COUNT_RAW_WRITE();
    }
    if (!ok) {
        SHIM_ERROR("writing %s failed: errno %d", path_, errno);
    }
    return ok;
}

void File::flush() {
    if (fd_ < 0) {
        return; // a held file is written whole, later (#425)
    }
    if (failed_) {
        SHIM_ERROR("%s not saved: a write did not fit in memory (%u bytes)", path_, (unsigned)len_);
        return;
    }
    [[maybe_unused]] Timed timed(FS_OP_FLUSH, path_);
    if (commit() && fsync(fd_) != 0) {
        SHIM_ERROR("syncing %s failed: errno %d", path_, errno);
    }
    TIMED_SET(timed, bytes, len_);
    TIMED_SET(timed, rewritten, rewritten_);
}

void File::hold() {
    char const *path = held_path();
    if (failed_) {
        SHIM_ERROR("%s not saved, the old file is kept: a write did not fit in memory (%u bytes)", path_,
                   (unsigned)len_);
        buf_free(buf_);
    } else if (!path) {
        // Opened while held, closed after the hold ended: the direct save made since has the newer state, so this
        // one is dropped rather than written over it
        SHIM_ERROR("%s: a save begun while held is dropped, the hold ended before its close", path_);
        buf_free(buf_);
    } else {
        buf_free(held_slot.buf); // an older save of the same file: this one is newer
        held_slot = fs_shim_held_t{buf_, len_, cap_, true, path};
    }
    held_   = false;
    buf_    = nullptr;
    len_    = cap_ = pos_ = 0;
    failed_ = false;
}

void File::close() {
    if (held_) {
        hold(); // no file system call: written after the owner's unlock (#425)
        return;
    }
    if (view_) { // the parked save's bytes stay the shim's
        view_ = false;
        buf_  = nullptr;
        len_  = cap_ = pos_ = 0;
        return;
    }
    if (fd_ < 0 && !f_ && !d_) {
        return; // nothing open (the destructor after an explicit close): no file system call, not counted
    }
    [[maybe_unused]] Timed timed(FS_OP_CLOSE, path_);
    if (fd_ >= 0) {
        if (failed_) {
            SHIM_ERROR(
                "%s not saved, the old file is kept: a write did not fit in memory (%u bytes)",
                path_,
                (unsigned)len_
            );
        } else {
            saved_ = commit();
        }
        TIMED_SET(timed, bytes, len_);
        TIMED_SET(timed, rewritten, rewritten_);
        [[maybe_unused]] int64_t sync = Timed::now();
        // LittleFS commits the file here (data, then its metadata in one step): on flash when close() returns
        if (::close(fd_) != 0) {
            SHIM_ERROR("closing %s failed: errno %d", path_, errno);
            saved_ = false;
        }
        TIMED_SET(timed, sync_us, Timed::now() - sync);
        fd_ = -1;
        buf_free(buf_);
        buf_    = nullptr;
        len_    = cap_ = pos_ = 0;
        failed_ = false;
    }
    if (f_) {
        fclose(f_);
        f_ = nullptr;
    }
    if (d_) {
        closedir(d_);
        d_ = nullptr;
    }
}

File FS::open(char const *path, char const *mode, bool create) {
    (void)create;
    char full[PATH_MAX_LEN];
    if (!full_path(path, full)) {
        return File();
    }
    if ((mode[0] == 'w' || (mode[0] == 'r' && held_slot.set)) && is_held(path)) {
        // "w": a buffer only, File::writeHeld opens the file after the owner's unlock; "r": the parked save is the
        // file's newest state
        return mode[0] == 'w' ? File(full, true) : File(full, held_slot.buf, held_slot.len);
    }
    [[maybe_unused]] Timed timed(FS_OP_OPEN, full);
    char        m[4] = {mode[0], 'b', '\0', '\0'};
    struct stat st;
    if (mode[0] == 'r' && stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        return File(opendir(full), full);
    }
    if (mode[0] != 'r' && !make_dirs(full, false)) {
        return File();
    }
    if (mode[0] == 'w') { // not truncated here: close() rewrites what changed (#97)
        int fd = ::open(full, O_RDWR | O_CREAT, 0644);
        return fd >= 0 ? File(fd, full) : File();
    }
    FILE *f = fopen(full, m);
    return f ? File(f, full) : File();
}

bool FS::exists(char const *path) {
    if (is_held(path) && held_slot.set) {
        return true; // parked, not yet written (a first save, or one after a remove)
    }
    [[maybe_unused]] Timed timed(FS_OP_EXISTS, path);
    char        full[PATH_MAX_LEN];
    struct stat st;
    return full_path(path, full) && stat(full, &st) == 0;
}

bool FS::remove(char const *path) {
    [[maybe_unused]] Timed timed(FS_OP_REMOVE, path);
    if (is_held(path)) {
        fs_shim_drop_held(); // a held save written after this would bring the file back
    }
    char full[PATH_MAX_LEN];
    return full_path(path, full) && unlink(full) == 0;
}

static int rename_raw(char const *from, char const *to) {
    struct stat st;
    if (fat_rename && stat(to, &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    return ::rename(from, to);
}

bool FS::rename(char const *from, char const *to) {
    [[maybe_unused]] Timed timed(FS_OP_RENAME, to);
    char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
    if (!full_path(from, a) || !full_path(to, b)) {
        return false;
    }
    if (rename_raw(a, b) == 0) {
        return true;
    }
    struct stat st;
    if (errno != EEXIST || stat(a, &st) != 0) { // never remove the target for a source that is not there
        return false;
    }
    return unlink(b) == 0 && rename_raw(a, b) == 0; // FatFs: no rename over an existing file (LittleFS replaces)
}

// Opens the held file as FS::open does for "w" (directories made, not truncated) and closes it the usual way: only the
// chunks that differ are written, and LittleFS makes them the file in one step at the close, so a cut leaves the old
// file or the new one
fs_shim_wrote_t File::writeHeld(fs_shim_held_t *h) {
    fs_shim_wrote_t r = {};
    if (!h->set) {
        return r;
    }
    int64_t start = Timed::now();
    char    full[PATH_MAX_LEN];
    int     fd = -1;
    char const *path = h->path; // the path it was parked for: a hold ended since does not lose it
    if (path && full_path(path, full) && make_dirs(full, false)) {
        fd = ::open(full, O_RDWR | O_CREAT, 0644);
    }
    if (fd < 0) {
        SHIM_ERROR("%s not saved, the old file is kept: errno %d", path ? path : "(no held path)", errno);
        buf_free(h->buf);
    } else {
        File f(fd, full);
        f.buf_ = h->buf; // close() commits it, closes and frees it, as for any "w" file
        f.len_ = h->len;
        f.cap_ = h->cap;
        f.close();
        r.ok        = f.saved_;
        r.len       = h->len;
        r.rewritten = f.rewritten_;
    }
    *h   = fs_shim_held_t{};
    r.us = (uint32_t)(Timed::now() - start);
    return r;
}

bool FS::mkdir(char const *path) {
    char full[PATH_MAX_LEN];
    return full_path(path, full) && make_dirs(full, true);
}

bool SPIFFSFS::begin(bool format_on_fail) {
    (void)format_on_fail;
    return root[0] && (::mkdir(root, 0755) == 0 || errno == EEXIST);
}

// Deletes the contents of dir (not dir itself)
static bool remove_tree(char const *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }
    bool           ok = true;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char        child[PATH_MAX_LEN];
        struct stat st;
        if (snprintf(child, sizeof(child), "%s/%s", dir, e->d_name) >= (int)sizeof(child) || stat(child, &st) != 0) {
            ok = false;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            ok = remove_tree(child) && rmdir(child) == 0 && ok;
        } else {
            ok = unlink(child) == 0 && ok;
        }
    }
    closedir(d);
    return ok;
}

bool SPIFFSFS::format() {
    fs_shim_drop_held(); // MeshCore's factory reset: nothing saved before it is written after it
    return fs_shim_format_allowed(root) && remove_tree(root);
}

// Sizes of the whole drive the root lives on
static void drive_bytes(size_t *total, size_t *used) {
    if (drive_fn) {
        drive_fn(total, used);
        return;
    }
#ifdef ESP_PLATFORM
    *total = *used = 0; // the OS did not say
#else
    struct statvfs st;
    if (statvfs(root, &st) != 0) {
        *total = *used = 0;
        return;
    }
    *total = (size_t)(st.f_blocks * st.f_frsize);
    *used  = (size_t)((st.f_blocks - st.f_bfree) * st.f_frsize);
#endif
}

size_t SPIFFSFS::totalBytes() {
    size_t total, used;
    drive_bytes(&total, &used);
    return total;
}

size_t SPIFFSFS::usedBytes() {
    size_t total, used;
    drive_bytes(&total, &used);
    return used;
}

} // namespace fs
