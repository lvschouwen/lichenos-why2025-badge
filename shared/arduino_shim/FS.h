// Arduino's fs::FS and fs::File, as much as MeshCore's DataStore and IdentityStore use (#65), over stdio below a root
// directory (/storage/meshcore on the badge). MeshCore paths start with '/'; directories are made as files need them.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <dirent.h>

#include "Stream.h"

namespace fs {

// A Stream, as Arduino's File: MeshCore reads and writes identities through Stream&
//
// A file opened "w" is held in a buffer (PSRAM on the badge) until flush() or close() (#97): DataStore writes contacts
// field by field, and every sector the file system rewrites costs a flash erase. Committing compares the buffer with
// the file in 4 KB chunks (the FatFs sector) and rewrites in place only the chunks that differ, then truncates, so
// unchanged contacts cost no flash write and the FAT is not freed and reallocated on every save. The data is on flash
// when close() returns.
class File : public Stream {
public:
    File() = default;
    File(FILE *f, char const *full_path); // takes the stream
    File(int fd, char const *full_path);  // takes the descriptor: a "w" file, buffered until close()
    File(DIR *d, char const *full_path);  // a directory, for openNextFile()
    File(File const &)            = delete;
    File &operator=(File const &) = delete;
    File(File &&other) noexcept : Stream() { *this = static_cast<File &&>(other); }
    File &operator=(File &&other) noexcept;
    ~File() { close(); }

    explicit operator bool() const { return f_ != nullptr || d_ != nullptr || fd_ >= 0; }
    bool        isDirectory() const { return d_ != nullptr; }
    char const *name() const; // the last path component
    File        openNextFile(); // a directory's next entry, false at the end
    int         available() override; // bytes left to read
    size_t   read(uint8_t *buf, size_t len);
    int      read() override; // one byte, -1 at the end
    size_t   write(uint8_t const *buf, size_t len) override;
    size_t   write(uint8_t b) override { return write(&b, 1); }
    bool     seek(uint32_t pos);
    size_t   size();
    void     flush() override; // a "w" file: commit what is written so far and sync it
    void     close();

private:
    bool commit(); // a "w" file's buffer onto the file, only the chunks that differ

    FILE    *f_ = nullptr;
    DIR     *d_ = nullptr;
    int      fd_ = -1;      // a "w" file
    uint8_t *buf_ = nullptr; // a "w" file's whole contents
    size_t   len_ = 0, cap_ = 0, pos_ = 0;
    uint32_t caps_ = 0;       // which heap buf_ came from (badge): later reallocs stay there
    bool     failed_ = false; // a write did not fit: the file is left as it was
    char     path_[256] = "";
};

class FS {
public:
    // mode: "r", "w" (truncate) or "a"; create is accepted for the ESP32 signature (writing always creates)
    File open(char const *path, char const *mode = "r", bool create = false);
    bool exists(char const *path);
    bool remove(char const *path);
    bool mkdir(char const *path);
};

class SPIFFSFS : public FS {
public:
    bool   begin(bool format_on_fail = false); // makes the root directory
    bool   format();                           // deletes everything below the root
    size_t totalBytes();
    size_t usedBytes();
};

} // namespace fs

using fs::File;

// The root directory below which MeshCore's paths live: the OS hands the app its directory (lichen_fs_app_dir) before
// begin(); host tests point it at a temp dir. Unset, begin() fails. Paths with a ".." component or "//" are refused.
void fs_shim_set_root(char const *dir);
// Sizes of the drive the root lives on (totalBytes/usedBytes); the host default measures the root's file system
void fs_shim_set_drive_bytes(void (*fn)(size_t *total, size_t *used));
// format() wipes only a directory of its own below a parent ("/storage/meshcore"): never a top-level directory or the
// drive itself, never the C6 image's /storage/c6, never through "..". Pure, host-tested.
bool fs_shim_format_allowed(char const *root);
// Microseconds spent in file calls (open, read, write, close, exists, remove) since the last call, and how many calls
// (ops may be NULL); always 0 off the badge
uint32_t fs_shim_take_busy_us(uint32_t *ops);
// Writes (and truncates) that reached the file system for buffered "w" files since the last call: host tests count them
uint32_t fs_shim_take_raw_writes(void);
// The chunk a "w" file is compared and rewritten in: the FatFs sector on the badge, one flash erase each
#define FS_SHIM_CHUNK 4096
// The largest "w" file the shim buffers (1 MB): MeshCore's biggest is contacts3, 152 KB at 1000 contacts. A write past
// it fails, and close() leaves the old file as it was.
#define FS_SHIM_MAX_FILE ((size_t)1 << 20)
// Host tests only: make buffer allocations fail, or make the reads that compare chunks fail
void fs_shim_test_faults(bool fail_alloc, bool fail_reads);
