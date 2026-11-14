// Arduino's fs::FS and fs::File, as much as MeshCore's DataStore and IdentityStore use (#65), over stdio below a root
// directory (/storage/meshcore on the badge). MeshCore paths start with '/'; directories are made as files need them.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <dirent.h>

#include "Stream.h"

// A "w" file's contents held back from the file system (#425): taken under the owner's lock, written after it
typedef struct {
    uint8_t *buf;      // the file's whole contents (the File's own buffer, PSRAM on the badge); NULL when empty
    size_t   len, cap; // bytes in it, and the block's size
    bool     set;      // something is held (an empty file too: it truncates)
} fs_shim_held_t;

// What writing a held file did, for the owner's log
typedef struct {
    size_t   len;       // the file's size
    size_t   rewritten; // bytes of the chunks that differed and were written
    uint32_t us;        // the whole write, open to close (0 off the badge)
    bool     ok;
} fs_shim_wrote_t;

namespace fs {

// A Stream, as Arduino's File: MeshCore reads and writes identities through Stream&
//
// A file opened "w" is held in a buffer (PSRAM on the badge) until flush() or close() (#97): DataStore writes contacts
// field by field, and every block the file system rewrites costs a flash erase. Committing compares the buffer with
// the file in 4 KB chunks (one flash block) and rewrites only the chunks that differ, then truncates, so an unchanged
// file costs no flash write. On the badge's LittleFS (#300) a rewritten chunk is copy-on-write (it and the blocks after
// it are written anew) and the whole commit lands at once: the data is on flash when close() returns, and a cut before
// that leaves the old file.
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

    explicit operator bool() const { return f_ != nullptr || d_ != nullptr || buffered(); }
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
    // A held file's contents onto the file system: opened, committed as close() does, closed; h is emptied
    static fs_shim_wrote_t writeHeld(fs_shim_held_t *h);

private:
    friend class FS;
    File(char const *full_path, bool held); // the held path's "w" file: a buffer and no descriptor (#425)
    bool buffered() const { return fd_ >= 0 || held_; }
    bool commit(); // a "w" file's buffer onto the file, only the chunks that differ
    void hold();   // close() of a held file: the buffer goes to the held slot

    FILE    *f_ = nullptr;
    DIR     *d_ = nullptr;
    int      fd_ = -1;      // a "w" file
    uint8_t *buf_ = nullptr; // a "w" file's whole contents
    size_t   len_ = 0, cap_ = 0, pos_ = 0;
    size_t   rewritten_ = 0;  // what the last commit wrote
    bool     failed_ = false; // a write did not fit: the file is left as it was
    bool     held_ = false;   // a "w" file of the held path: no descriptor, close() keeps the buffer (#425)
    char     path_[256] = "";
};

class FS {
public:
    // mode: "r", "w" (truncate) or "a"; create is accepted for the ESP32 signature (writing always creates)
    File open(char const *path, char const *mode = "r", bool create = false);
    bool exists(char const *path);
    bool remove(char const *path);
    bool mkdir(char const *path);
    // Replaces to with from (#110). POSIX and the badge's LittleFS (#300) do that in one atomic step; FatFs refuses an
    // existing target, so there the target is removed first: a power cut in between leaves only from, which
    // IdentityStore's load takes back.
    bool rename(char const *from, char const *to);
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
// drive itself, never the C6 image's /storage/c6, never through "..". The root is lichen_fs_app_dir's, which never
// hands out an OS directory (fs_path.h, #283): the c6 check here is a second line. Pure, host-tested.
bool fs_shim_format_allowed(char const *root);
// Microseconds spent in file calls (open, read, write, close, exists, remove) since the last call, and how many calls
// (ops may be NULL); always 0 off the badge. Also forgets the slow calls below.
uint32_t fs_shim_take_busy_us(uint32_t *ops);

// The file calls of at least FS_SHIM_SLOW_CALL_US since the last fs_shim_take_busy_us (#425: which calls make a slow
// MeshCore loop). Only a slow call pays for the note, the others a compare; the first FS_SHIM_SLOW_CALLS are kept.
#define FS_SHIM_SLOW_CALL_US 10000
#define FS_SHIM_SLOW_CALLS   4
typedef enum { FS_OP_OPEN, FS_OP_READ, FS_OP_WRITE, FS_OP_FLUSH, FS_OP_CLOSE, FS_OP_EXISTS, FS_OP_REMOVE,
               FS_OP_RENAME, FS_OP_DIR } fs_shim_op_t;
typedef struct {
    uint8_t  op;        // fs_shim_op_t
    uint32_t us;
    uint32_t bytes;     // read or written by the call; a close or flush: the file's size
    uint32_t rewritten; // a close or flush of a "w" file: bytes of the chunks it wrote
    uint32_t sync_us;   // a close: the file system's own close (LittleFS commits the file there)
    char     name[16];  // the last path component, cut short
} fs_shim_call_t;
// Copies up to max slow calls into out and returns how many there were (may exceed max); all 0 off the badge
size_t      fs_shim_slow_calls(fs_shim_call_t *out, size_t max);
char const *fs_shim_op_name(uint8_t op);

// Holding one file's saves (#425): while path is held, a "w" open of it touches no file system and its close() keeps
// the buffer in the held slot (a newer save replaces an older one) instead of writing. The owner takes the slot under
// its lock and writes it after the unlock with File::writeHeld, so it stays the one writer of that file. The owner
// serializes hold, take, drop and every open of the path (the badge: the mesh lock). path must outlive the hold
// (MeshCore's "/contacts3"); NULL stops holding. Stopping does not drop what is held; remove() of the path and
// format() do, so an older save never brings back a file deleted after it.
void fs_shim_hold(char const *path);
// Moves the held save into out (out->set false when nothing was held)
void fs_shim_take_held(fs_shim_held_t *out);
// Forgets what is held, unwritten (a save of the newest state follows)
void fs_shim_drop_held(void);
#ifndef ESP_PLATFORM
// Host tests only. Writes (and truncates) that reached the file system for buffered "w" files since the last call
uint32_t fs_shim_take_raw_writes(void);
// Host tests only. The largest single read the shim asked the file system for since the last call (the compare reads)
size_t fs_shim_take_largest_read(void);
#endif
// The chunk a "w" file is compared and rewritten in: the LittleFS block on the badge, one flash erase each
#define FS_SHIM_CHUNK 4096
// The largest read a chunk compare makes, below the block size LittleFS reads through its internal file cache: the
// flash read never targets the caller's (PSRAM) buffer, so it needs no temp buffer of its own (#364)
#define FS_SHIM_COMPARE_READ 256
// The largest "w" file the shim buffers (1 MB): MeshCore's biggest is contacts3, 152 KB at 1000 contacts. A write past
// it fails, and close() leaves the old file as it was.
#define FS_SHIM_MAX_FILE ((size_t)1 << 20)
// Where a "w" file's buffer comes from (the badge hands in lichen_alloc/lichen_free); NULL hooks: malloc and free in
// the host tests, no buffer on the badge (#283).
// alloc returns n bytes or NULL; release takes what alloc gave.
void fs_shim_set_buffer_hooks(void *(*alloc)(size_t bytes), void (*release)(void *ptr));
// Host tests only: make buffer allocations fail, or make the reads that compare chunks fail
void fs_shim_test_faults(bool fail_alloc, bool fail_reads);
// Host tests only: rename refuses an existing target as FatFs does (EEXIST), so the remove-first path is run
void fs_shim_test_fat_rename(bool on);
