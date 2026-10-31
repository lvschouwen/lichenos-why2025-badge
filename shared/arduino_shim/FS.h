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
class File : public Stream {
public:
    File() = default;
    File(FILE *f, char const *full_path); // takes the stream
    File(DIR *d, char const *full_path);  // a directory, for openNextFile()
    File(File const &)            = delete;
    File &operator=(File const &) = delete;
    File(File &&other) noexcept : Stream() { *this = static_cast<File &&>(other); }
    File &operator=(File &&other) noexcept;
    ~File() { close(); }

    explicit operator bool() const { return f_ != nullptr || d_ != nullptr; }
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
    void     close();

private:
    FILE *f_ = nullptr;
    DIR  *d_ = nullptr;
    char  path_[256] = "";
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
