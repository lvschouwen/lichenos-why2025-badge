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

fs::SPIFFSFS SPIFFS;

static char root[PATH_MAX_LEN]; // empty until the OS sets it (#84 T6)
static void (*drive_fn)(size_t *total, size_t *used);

// Time spent in the file system (#67: where does a slow MeshCore loop go?). Only the badge measures; the mesh task reads
// and resets it once per loop. Atomic: since #257 the mesh task also writes advert blobs after its unlock, while another
// task may be in a file under the mesh lock.
static uint32_t busy_us, busy_ops;
static uint32_t raw_writes; // writes and truncates of buffered "w" files (host tests count them)
static bool     fail_alloc, fail_reads, fat_rename;

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

uint32_t fs_shim_take_raw_writes(void) {
    uint32_t n = raw_writes;
    raw_writes = 0;
    return n;
}

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

uint32_t fs_shim_take_busy_us(uint32_t *ops) {
    uint32_t n = __atomic_exchange_n(&busy_ops, 0, __ATOMIC_RELAXED);
    if (ops) {
        *ops = n;
    }
    return __atomic_exchange_n(&busy_us, 0, __ATOMIC_RELAXED);
}

namespace {
struct Timed {
#ifdef ESP_PLATFORM
    int64_t start = esp_timer_get_time();
    ~Timed() {
        __atomic_fetch_add(&busy_us, (uint32_t)(esp_timer_get_time() - start), __ATOMIC_RELAXED);
        __atomic_fetch_add(&busy_ops, 1, __ATOMIC_RELAXED);
    }
#endif
};
} // namespace

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
        memcpy(path_, other.path_, sizeof(path_));
        other.f_   = nullptr;
        other.d_   = nullptr;
        other.fd_  = -1;
        other.buf_ = nullptr;
        other.len_ = other.cap_ = other.pos_ = 0;
        other.failed_ = false;
    }
    return *this;
}

char const *File::name() const {
    char const *slash = strrchr(path_, '/');
    return slash ? slash + 1 : path_;
}

File File::openNextFile() {
    [[maybe_unused]] Timed timed;
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
    if (fd_ >= 0) {
        return pos_ < len_ ? (int)(len_ - pos_) : 0;
    }
    long pos = f_ ? ftell(f_) : -1;
    return pos < 0 ? 0 : (int)size() - (int)pos;
}

size_t File::read(uint8_t *buf, size_t len) {
    if (fd_ >= 0) { // what this File wrote: the buffer is the whole file
        size_t n = pos_ < len_ ? len_ - pos_ : 0;
        n        = n < len ? n : len;
        if (n > 0) {
            memcpy(buf, buf_ + pos_, n);
            pos_ += n;
        }
        return n;
    }
    [[maybe_unused]] Timed timed;
    return f_ ? fread(buf, 1, len, f_) : 0;
}

int File::read() {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
}

size_t File::write(uint8_t const *buf, size_t len) {
    if (fd_ >= 0) { // into the buffer, no file system call: committed by flush() or close()
        if (failed_ || len == 0) {
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
    [[maybe_unused]] Timed timed;
    return f_ ? fwrite(buf, 1, len, f_) : 0;
}

bool File::seek(uint32_t pos) {
    if (fd_ >= 0) {
        pos_ = pos;
        return true;
    }
    return f_ && fseek(f_, (long)pos, SEEK_SET) == 0;
}

size_t File::size() {
    struct stat st;
    if (fd_ >= 0) {
        return len_;
    }
    if (!f_) {
        return 0;
    }
    fflush(f_);
    return fstat(fileno(f_), &st) == 0 ? (size_t)st.st_size : 0;
}

// Rewrites in place only the chunks that differ from what is on the file system, then cuts off what is left over. An
// unchanged chunk costs a read, not a flash erase; nothing is freed and reallocated in the FAT unless the size changes.
bool File::commit() {
    struct stat st;
    if (fstat(fd_, &st) != 0) {
        return false;
    }
    size_t   old     = (size_t)st.st_size;
    uint8_t *scratch = old ? (uint8_t *)buf_grow(NULL, 0, FS_SHIM_CHUNK) : NULL; // none: all are rewritten
    bool     ok      = true;
    for (size_t off = 0; ok && off < len_; off += FS_SHIM_CHUNK) {
        size_t n    = len_ - off < FS_SHIM_CHUNK ? len_ - off : FS_SHIM_CHUNK;
        bool   same = scratch && off + n <= old && lseek(fd_, (off_t)off, SEEK_SET) == (off_t)off &&
                    read_full(fd_, scratch, n) && memcmp(scratch, buf_ + off, n) == 0;
        if (!same) {
            ok = lseek(fd_, (off_t)off, SEEK_SET) == (off_t)off && write_full(fd_, buf_ + off, n);
            raw_writes++;
        }
    }
    if (ok && old > len_) {
        ok = ftruncate(fd_, (off_t)len_) == 0;
        raw_writes++;
    }
    if (!ok) {
        SHIM_ERROR("writing %s failed: errno %d", path_, errno);
    }
    buf_free(scratch);
    return ok;
}

void File::flush() {
    if (fd_ < 0) {
        return;
    }
    if (failed_) {
        SHIM_ERROR("%s not saved: a write did not fit in memory (%u bytes)", path_, (unsigned)len_);
        return;
    }
    [[maybe_unused]] Timed timed;
    if (commit() && fsync(fd_) != 0) {
        SHIM_ERROR("syncing %s failed: errno %d", path_, errno);
    }
}

void File::close() {
    if (fd_ < 0 && !f_ && !d_) {
        return; // nothing open (the destructor after an explicit close): no file system call, not counted
    }
    [[maybe_unused]] Timed timed;
    if (fd_ >= 0) {
        if (failed_) {
            SHIM_ERROR(
                "%s not saved, the old file is kept: a write did not fit in memory (%u bytes)",
                path_,
                (unsigned)len_
            );
        } else {
            commit();
        }
        // FatFs syncs the file (data, FAT, directory entry) here: on flash when close() returns
        if (::close(fd_) != 0) {
            SHIM_ERROR("closing %s failed: errno %d", path_, errno);
        }
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
    [[maybe_unused]] Timed timed;
    (void)create;
    char full[PATH_MAX_LEN];
    if (!full_path(path, full)) {
        return File();
    }
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
    [[maybe_unused]] Timed timed;
    char        full[PATH_MAX_LEN];
    struct stat st;
    return full_path(path, full) && stat(full, &st) == 0;
}

bool FS::remove(char const *path) {
    [[maybe_unused]] Timed timed;
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
    [[maybe_unused]] Timed timed;
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
    return unlink(b) == 0 && rename_raw(a, b) == 0; // FatFs: no rename over an existing file
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
