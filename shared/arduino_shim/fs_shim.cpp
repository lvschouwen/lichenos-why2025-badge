// The fs::FS shim (#65): MeshCore's file paths mapped below a root directory, plain stdio underneath
#include "SPIFFS.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#define FS_SHIM_MOUNT "/storage"
#else
#include <sys/statvfs.h>
#endif

#ifndef FS_SHIM_ROOT
#define FS_SHIM_ROOT "/storage/meshcore"
#endif

#define PATH_MAX_LEN 256

fs::SPIFFSFS SPIFFS;

static char root[PATH_MAX_LEN] = FS_SHIM_ROOT;

// Time spent in the file system (#67: where does a slow MeshCore loop go?). Only the badge measures; the mesh task reads
// and resets it once per loop, under the mesh lock that every MeshCore file access already runs under.
static uint32_t busy_us, busy_ops;

uint32_t fs_shim_take_busy_us(uint32_t *ops) {
    uint32_t us = busy_us;
    if (ops) {
        *ops = busy_ops;
    }
    busy_us = busy_ops = 0;
    return us;
}

namespace {
struct Timed {
#ifdef ESP_PLATFORM
    int64_t start = esp_timer_get_time();
    ~Timed() {
        busy_us += (uint32_t)(esp_timer_get_time() - start);
        busy_ops++;
    }
#endif
};
} // namespace

void fs_shim_set_root(char const *dir) {
    snprintf(root, sizeof(root), "%s", dir);
}

static bool full_path(char const *path, char *out) {
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

File &File::operator=(File &&other) noexcept {
    if (this != &other) {
        close();
        f_ = other.f_;
        d_ = other.d_;
        memcpy(path_, other.path_, sizeof(path_));
        other.f_ = nullptr;
        other.d_ = nullptr;
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
    long pos = f_ ? ftell(f_) : -1;
    return pos < 0 ? 0 : (int)size() - (int)pos;
}

size_t File::read(uint8_t *buf, size_t len) {
    [[maybe_unused]] Timed timed;
    return f_ ? fread(buf, 1, len, f_) : 0;
}

int File::read() {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
}

size_t File::write(uint8_t const *buf, size_t len) {
    [[maybe_unused]] Timed timed;
    return f_ ? fwrite(buf, 1, len, f_) : 0;
}

bool File::seek(uint32_t pos) {
    return f_ && fseek(f_, (long)pos, SEEK_SET) == 0;
}

size_t File::size() {
    struct stat st;
    if (!f_) {
        return 0;
    }
    fflush(f_);
    return fstat(fileno(f_), &st) == 0 ? (size_t)st.st_size : 0;
}

void File::close() {
    [[maybe_unused]] Timed timed;
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

bool FS::mkdir(char const *path) {
    char full[PATH_MAX_LEN];
    return full_path(path, full) && make_dirs(full, true);
}

bool SPIFFSFS::begin(bool format_on_fail) {
    (void)format_on_fail;
    return ::mkdir(root, 0755) == 0 || errno == EEXIST;
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
    return remove_tree(root);
}

// Sizes of the whole drive the root lives on
static void drive_bytes(size_t *total, size_t *used) {
#ifdef ESP_PLATFORM
    uint64_t t = 0, f = 0;
    esp_vfs_fat_info(FS_SHIM_MOUNT, &t, &f);
    *total = (size_t)t;
    *used  = (size_t)(t - f);
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
