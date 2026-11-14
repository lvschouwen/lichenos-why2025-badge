// Host test: the Arduino-style file system shim that MeshCore's DataStore uses, over a plain directory (#65)
#include "SPIFFS.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void fake_drive(size_t *total, size_t *used) {
    *total = 1000;
    *used  = 250;
}

static char root_dir[64];

// Buffer hooks that count: every block the shim takes is given back (the badge books them as the app's memory)
static int live_blocks, total_blocks, alloc_calls, fail_from; // fail_from: the Nth call on returns NULL, 0 = never
static void *count_alloc(size_t n) {
    alloc_calls++;
    if (fail_from && alloc_calls >= fail_from) {
        return NULL;
    }
    live_blocks++;
    total_blocks++;
    return malloc(n);
}
static void count_free(void *p) {
    if (p) {
        live_blocks--;
    }
    free(p);
}

// The file's bytes as the file system has them, read past the shim
static size_t disk_read(char const *name, uint8_t *out, size_t cap) {
    char path[300];
    snprintf(path, sizeof(path), "%s%s", root_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    size_t n = fread(out, 1, cap, f);
    fclose(f);
    return n;
}

// One contact record as DataStore::saveContacts writes it: 12 writes, 152 bytes
#define REC_SIZE 152
static void fill_record(uint8_t *rec, uint32_t i, uint32_t stamp) {
    for (size_t j = 0; j < REC_SIZE; j++) {
        rec[j] = (uint8_t)(i * 7 + j);
    }
    memcpy(rec + 72, &stamp, 4); // last_advert_timestamp sits at 72
}

static void save_contacts(uint8_t *expect, uint32_t count, uint32_t changed, uint32_t stamp) {
    static size_t const fields[12] = {32, 32, 1, 1, 1, 4, 1, 4, 64, 4, 4, 4};
    File f = SPIFFS.open("/contacts3", "w", true);
    CHECK(f);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *rec = expect + i * REC_SIZE;
        fill_record(rec, i, i == changed ? stamp : 0);
        size_t off = 0;
        for (size_t k = 0; k < 12; k++) {
            CHECK(f.write(rec + off, fields[k]) == fields[k]);
            off += fields[k];
        }
    }
    f.close();
}

// #97: "w" files are buffered and committed by flush()/close(), rewriting only the 4 KB chunks that changed
static void buffered_writes(void) {
    static uint8_t expect[400 * REC_SIZE], disk[400 * REC_SIZE];
    fs_shim_take_raw_writes();

    // A contacts save of 184 contacts: 2208 File writes, 7 chunk writes into a new file
    save_contacts(expect, 184, 999, 0);
    CHECK(fs_shim_take_raw_writes() == (184 * REC_SIZE + FS_SHIM_CHUNK - 1) / FS_SHIM_CHUNK);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 184 * REC_SIZE && memcmp(disk, expect, 184 * REC_SIZE) == 0);
    // The same save again writes nothing; one contact's new timestamp rewrites one chunk
    save_contacts(expect, 184, 999, 0);
    CHECK(fs_shim_take_raw_writes() == 0);
    save_contacts(expect, 184, 100, 12345); // record 100 sits in bytes 15200..15351: chunk 3 only
    CHECK(fs_shim_take_raw_writes() == 1);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 184 * REC_SIZE && memcmp(disk, expect, 184 * REC_SIZE) == 0);
    // A new contact at the end: only the last chunk; fewer contacts: a truncate, nothing rewritten
    save_contacts(expect, 185, 100, 12345);
    CHECK(fs_shim_take_raw_writes() == 1);
    save_contacts(expect, 150, 100, 12345);
    CHECK(fs_shim_take_raw_writes() == 1);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 150 * REC_SIZE && memcmp(disk, expect, 150 * REC_SIZE) == 0);
    // Fewer contacts and a changed one in the new last chunk: the write and the truncate (in that order) both land
    save_contacts(expect, 150, 999, 0); // record 100 back to its old timestamp: chunk 3
    CHECK(fs_shim_take_raw_writes() == 1);
    save_contacts(expect, 140, 135, 777); // 21280 bytes; record 135 sits in bytes 20520..20671: chunk 5, the last one
    CHECK(fs_shim_take_raw_writes() == 2);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 140 * REC_SIZE && memcmp(disk, expect, 140 * REC_SIZE) == 0);
    save_contacts(expect, 0, 999, 0); // none left: an empty file, one truncate
    CHECK(fs_shim_take_raw_writes() == 1);
    CHECK(SPIFFS.exists("/contacts3") && disk_read("/contacts3", disk, sizeof(disk)) == 0);

    // The compare reads stay small (#364): on the badge they land in a stack buffer, so a PSRAM file buffer never makes
    // the flash read malloc an internal temp buffer of a whole chunk
    save_contacts(expect, 140, 999, 0);
    fs_shim_take_raw_writes();
    fs_shim_take_largest_read();
    save_contacts(expect, 140, 999, 0); // all six chunks compared, none rewritten
    CHECK(fs_shim_take_raw_writes() == 0);
    size_t largest = fs_shim_take_largest_read();
    CHECK(largest > 0 && largest <= FS_SHIM_COMPARE_READ);

    // A chunk that cannot be read back for comparing is rewritten, not skipped
    save_contacts(expect, 30, 999, 0);
    fs_shim_take_raw_writes();
    fs_shim_test_faults(false, true);
    save_contacts(expect, 30, 999, 0);
    fs_shim_test_faults(false, false);
    CHECK(fs_shim_take_raw_writes() == 2);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 30 * REC_SIZE && memcmp(disk, expect, 30 * REC_SIZE) == 0);

    // No memory for the buffer: the write fails and close() leaves the old file as it was
    fs_shim_test_faults(true, false);
    File nomem = SPIFFS.open("/contacts3", "w", true);
    CHECK(nomem && nomem.write(expect, 10) == 0);
    nomem.close();
    fs_shim_test_faults(false, false);
    CHECK(fs_shim_take_raw_writes() == 0);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 30 * REC_SIZE && memcmp(disk, expect, 30 * REC_SIZE) == 0);

    // Through the hooks: a save that grows the buffer takes blocks and gives all of them back (the chunk compare reads
    // into a stack buffer, no block of its own: #364)
    fs_shim_set_buffer_hooks(count_alloc, count_free);
    save_contacts(expect, 30, 999, 0);
    CHECK(total_blocks >= 2 && live_blocks == 0);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 30 * REC_SIZE && memcmp(disk, expect, 30 * REC_SIZE) == 0);
    // A grow that fails (the 2nd block, past the first chunk) fails the write, releases the block it had, and the old
    // file stays as it was
    alloc_calls = 0;
    fail_from   = 2;
    File nomem2 = SPIFFS.open("/contacts3", "w", true);
    CHECK(nomem2 && nomem2.write(expect, 100) == 100 && live_blocks == 1);
    CHECK(nomem2.write(expect, FS_SHIM_CHUNK) == 0); // 100 + 4096 needs a second chunk: the hook says no
    nomem2.close();
    fail_from = 0;
    CHECK(live_blocks == 0);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 30 * REC_SIZE && memcmp(disk, expect, 30 * REC_SIZE) == 0);
    fs_shim_set_buffer_hooks(NULL, NULL);

    // A write past FS_SHIM_MAX_FILE (or one that would wrap) fails instead of growing the buffer: old file kept
    File huge = SPIFFS.open("/contacts3", "w", true);
    CHECK(huge.write(expect, 0) == 0); // nothing to write, nothing allocated
    CHECK(huge.seek(0xFFFFFFFFu) && huge.write(expect, 2) == 0);
    CHECK(huge.seek(0) && huge.write(expect, 2) == 0); // failed stays failed
    huge.close();
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 30 * REC_SIZE && memcmp(disk, expect, 30 * REC_SIZE) == 0);
    huge = SPIFFS.open("/contacts3", "w", true);
    CHECK(huge.seek((uint32_t)FS_SHIM_MAX_FILE) && huge.write(expect, 1) == 0);
    huge.close();
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 30 * REC_SIZE);
    fs_shim_take_raw_writes();

    // Nothing reaches the file before flush(); flush() commits, close() commits the rest
    uint8_t data[3 * FS_SHIM_CHUNK + 5];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 13 + 1);
    }
    File f = SPIFFS.open("/b", "w");
    CHECK(f && f.write(data, 100) == 100 && f.size() == 100);
    CHECK(disk_read("/b", disk, sizeof(disk)) == 0 && fs_shim_take_raw_writes() == 0);
    f.flush();
    CHECK(disk_read("/b", disk, sizeof(disk)) == 100 && memcmp(disk, data, 100) == 0);
    CHECK(f.write(data + 100, 50) == 50);
    f.close();
    CHECK(disk_read("/b", disk, sizeof(disk)) == 150 && memcmp(disk, data, 150) == 0);

    // Exactly one chunk, one write; more than a chunk in one call; many one-byte writes
    f = SPIFFS.open("/b", "w");
    CHECK(f.write(data, FS_SHIM_CHUNK) == FS_SHIM_CHUNK);
    f.close();
    CHECK(disk_read("/b", disk, sizeof(disk)) == FS_SHIM_CHUNK && memcmp(disk, data, FS_SHIM_CHUNK) == 0);
    fs_shim_take_raw_writes();
    f = SPIFFS.open("/c", "w");
    CHECK(f.write(data, sizeof(data)) == sizeof(data));
    f.close();
    CHECK(fs_shim_take_raw_writes() == 4);
    CHECK(disk_read("/c", disk, sizeof(disk)) == sizeof(data) && memcmp(disk, data, sizeof(data)) == 0);
    f = SPIFFS.open("/c", "w");
    for (size_t i = 0; i < sizeof(data); i++) {
        CHECK(f.write(data[i]) == 1);
    }
    f.close();
    CHECK(fs_shim_take_raw_writes() == 0); // the same bytes: nothing rewritten
    CHECK(disk_read("/c", disk, sizeof(disk)) == sizeof(data) && memcmp(disk, data, sizeof(data)) == 0);

    // Seek and read back in the same File; a seek past the end leaves zeroes; a moved File keeps its buffer
    f = SPIFFS.open("/d", "w");
    CHECK(f.write(data, 10) == 10 && f.seek(2) && f.write(data + 50, 2) == 2 && f.size() == 10);
    CHECK(f.seek(20) && f.write(data, 1) == 1 && f.size() == 21);
    uint8_t back[21];
    CHECK(f.seek(0) && f.available() == 21 && f.read(back, sizeof(back)) == 21 && f.read() == -1);
    CHECK(back[0] == data[0] && back[2] == data[50] && back[3] == data[51] && back[4] == data[4]);
    CHECK(back[10] == 0 && back[19] == 0 && back[20] == data[0]);
    File moved;
    moved = static_cast<File &&>(f);
    CHECK(!f && moved);
    moved.close();
    CHECK(disk_read("/d", disk, sizeof(disk)) == 21 && memcmp(disk, back, 21) == 0);
    File in = SPIFFS.open("/d", "r");
    CHECK(in && in.size() == 21);
    in.close();

    // DataStore's NRF52/STM32 putBlobByKey pattern (not built for ESP32, but the File serves it): read the records,
    // seek back, overwrite one in place
    struct Blob {
        uint8_t key[7];
        uint8_t len;
        uint8_t data[24];
    } blobs[3], tmp;
    for (int i = 0; i < 3; i++) {
        memset(&blobs[i], 0x10 * (i + 1), sizeof(blobs[i]));
    }
    f = SPIFFS.open("/blobs", "w");
    CHECK(f.write(reinterpret_cast<uint8_t *>(blobs), sizeof(blobs)) == sizeof(blobs));
    f.close();
    f = SPIFFS.open("/blobs", "w");
    CHECK(f.seek(0));
    uint32_t pos = 0, found = 0;
    while (f.read(reinterpret_cast<uint8_t *>(&tmp), sizeof(tmp)) == sizeof(tmp)) { // a fresh "w" File has nothing to read yet
        pos += sizeof(tmp);
    }
    CHECK(pos == 0);
    CHECK(f.write(reinterpret_cast<uint8_t *>(blobs), sizeof(blobs)) == sizeof(blobs) && f.seek(0));
    while (f.read(reinterpret_cast<uint8_t *>(&tmp), sizeof(tmp)) == sizeof(tmp)) {
        if (tmp.key[0] == 0x20) {
            found = pos;
        }
        pos += sizeof(tmp);
    }
    CHECK(pos == sizeof(blobs) && found == sizeof(Blob));
    memset(&tmp, 0x77, sizeof(tmp));
    CHECK(f.seek(found) && f.write(reinterpret_cast<uint8_t *>(&tmp), sizeof(tmp)) == sizeof(tmp));
    f.close();
    blobs[1] = tmp;
    CHECK(disk_read("/blobs", disk, sizeof(disk)) == sizeof(blobs) && memcmp(disk, blobs, sizeof(blobs)) == 0);
    CHECK(SPIFFS.remove("/blobs"));

    // "a" still appends
    f = SPIFFS.open("/d", "a");
    CHECK(f && f.write(data, 3) == 3);
    f.close();
    CHECK(disk_read("/d", disk, sizeof(disk)) == 24 && memcmp(disk + 21, data, 3) == 0);

    CHECK(SPIFFS.remove("/b") && SPIFFS.remove("/c") && SPIFFS.remove("/d") && SPIFFS.remove("/contacts3"));
}

// #425 review: while a save is parked, reading the file gives the parked bytes (a reload under the lock sees the newest
// state); a hold that ends drops what is parked, and a held File closed after it is not written over a direct save
static void held_reads(uint8_t *expect, uint8_t *older, uint8_t *disk) {
    size_t const n = 40 * REC_SIZE;
    save_contacts(older, 40, 999, 0); // on the file system: the old state
    fs_shim_hold("/contacts3");
    save_contacts(expect, 40, 7, 99); // parked: the new state
    File in = SPIFFS.open("/contacts3", "r");
    static uint8_t got[40 * REC_SIZE];
    CHECK(in && in.size() == n && in.available() == (int)n && in.read(got, n) == n && memcmp(got, expect, n) == 0);
    CHECK(in.seek(0) && in.write(older, 10) == 0); // read-only: the parked bytes stay as saved
    in.close();
    CHECK(live_blocks == 1 && SPIFFS.exists("/contacts3"));
    fs_shim_held_t taken;
    fs_shim_take_held(&taken);
    fs_shim_hold(NULL);                         // the hold ends between the take and the write: the save still lands
    CHECK(File::writeHeld(&taken).ok);
    CHECK(disk_read("/contacts3", disk, n + 1) == n && memcmp(disk, expect, n) == 0); // what the read gave
    in = SPIFFS.open("/contacts3", "r");        // from the file system again
    CHECK(in && in.read(got, n) == n && memcmp(got, expect, n) == 0);
    in.close();

    // Parked and never taken: ending the hold drops it (a direct save follows)
    fs_shim_hold("/contacts3");
    save_contacts(older, 40, 999, 0);
    CHECK(live_blocks == 1);
    fs_shim_hold(NULL);
    CHECK(live_blocks == 0);
    fs_shim_take_held(&taken);
    CHECK(!taken.set && disk_read("/contacts3", disk, n + 1) == n && memcmp(disk, expect, n) == 0);

    // Opened while held, closed after the hold ended and a direct save: dropped, the direct save stays
    fs_shim_hold("/contacts3");
    File late = SPIFFS.open("/contacts3", "w", true);
    CHECK(late.write(older, n) == n);
    fs_shim_hold(NULL);
    save_contacts(expect, 40, 3, 5);            // the direct save, newest state
    static uint8_t direct[40 * REC_SIZE];
    memcpy(direct, expect, n);
    late.close();
    fs_shim_take_held(&taken);
    CHECK(!taken.set && live_blocks == 0);
    CHECK(disk_read("/contacts3", disk, n + 1) == n && memcmp(disk, direct, n) == 0);
}

// #425: a held file's saves reach the file system only when the owner writes them, after its unlock: the save under
// the lock makes no file system call, and what is written is the newest save, whole
static void held_saves(void) {
    static uint8_t expect[300 * REC_SIZE], older[300 * REC_SIZE], disk[300 * REC_SIZE];
    save_contacts(older, 200, 999, 0); // what is on the file system before
    fs_shim_take_raw_writes();
    fs_shim_set_buffer_hooks(count_alloc, count_free);

    fs_shim_hold("/contacts3");
    save_contacts(expect, 200, 10, 4242); // under the lock: buffered and held, nothing opened or written
    CHECK(fs_shim_take_raw_writes() == 0 && live_blocks == 1);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 200 * REC_SIZE && memcmp(disk, older, 200 * REC_SIZE) == 0);
    // A newer save before the take replaces the held one (its block given back): the newest state is written
    save_contacts(expect, 210, 20, 777);
    CHECK(live_blocks == 1 && fs_shim_take_raw_writes() == 0);

    fs_shim_held_t taken;
    fs_shim_take_held(&taken); // still under the lock
    CHECK(taken.set && taken.len == 210 * REC_SIZE);
    fs_shim_held_t none;
    fs_shim_take_held(&none);
    CHECK(!none.set); // taken once
    // After the unlock: one writer, only the chunks that differ, and the block given back
    fs_shim_wrote_t w = File::writeHeld(&taken);
    CHECK(w.ok && w.len == 210 * REC_SIZE && w.rewritten > 0 && w.rewritten < w.len && !taken.set);
    CHECK(live_blocks == 0 && fs_shim_take_raw_writes() > 0);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 210 * REC_SIZE && memcmp(disk, expect, 210 * REC_SIZE) == 0);
    w = File::writeHeld(&taken); // nothing held: nothing written
    CHECK(!w.ok && w.len == 0 && fs_shim_take_raw_writes() == 0);

    // Other files are not held; an empty save is held too and truncates when written
    File other = SPIFFS.open("/prefs", "w");
    CHECK(other.write(expect, 10) == 10);
    other.close();
    CHECK(disk_read("/prefs", disk, sizeof(disk)) == 10 && SPIFFS.remove("/prefs"));
    save_contacts(expect, 0, 999, 0);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 210 * REC_SIZE);
    fs_shim_take_held(&taken);
    CHECK(taken.set && taken.len == 0 && File::writeHeld(&taken).ok);
    CHECK(SPIFFS.exists("/contacts3") && disk_read("/contacts3", disk, sizeof(disk)) == 0);

    // A stop drops what is held and saves directly (no hold): the dropped save is never written over it
    save_contacts(older, 50, 999, 0);
    fs_shim_drop_held();
    CHECK(live_blocks == 0);
    fs_shim_hold(NULL);
    save_contacts(expect, 60, 999, 0);
    fs_shim_take_held(&taken);
    CHECK(!taken.set);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 60 * REC_SIZE && memcmp(disk, expect, 60 * REC_SIZE) == 0);

    // A remove of the held file, or a format, forgets a save held before it: the file is not brought back
    fs_shim_hold("/contacts3");
    save_contacts(older, 5, 999, 0);
    CHECK(live_blocks == 1 && SPIFFS.remove("/contacts3") && live_blocks == 0);
    fs_shim_take_held(&taken);
    CHECK(!taken.set && !SPIFFS.exists("/contacts3"));
    save_contacts(expect, 60, 999, 0); // held again, and written: the file the checks below expect
    char on_disk[300];
    snprintf(on_disk, sizeof(on_disk), "%s/contacts3", root_dir);
    CHECK(access(on_disk, F_OK) != 0);  // the held open made no file system call: nothing created
    CHECK(SPIFFS.exists("/contacts3")); // ... but the parked save counts as the file
    fs_shim_take_held(&taken);
    CHECK(File::writeHeld(&taken).ok);
    fs_shim_hold(NULL);

    // A held save that did not fit in memory is not held: the old file stays
    fs_shim_hold("/contacts3");
    alloc_calls = 0;
    fail_from   = 1;
    File nomem = SPIFFS.open("/contacts3", "w", true);
    CHECK(nomem && nomem.write(older, 10) == 0);
    nomem.close();
    fail_from = 0;
    fs_shim_take_held(&taken);
    CHECK(!taken.set && live_blocks == 0);
    fs_shim_hold(NULL);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 60 * REC_SIZE && memcmp(disk, expect, 60 * REC_SIZE) == 0);

    held_reads(expect, older, disk);
    fs_shim_set_buffer_hooks(NULL, NULL);
    // No slow calls off the badge (nothing is timed there)
    fs_shim_call_t calls[FS_SHIM_SLOW_CALLS];
    CHECK(fs_shim_slow_calls(calls, FS_SHIM_SLOW_CALLS) == 0);
    CHECK(strcmp(fs_shim_op_name(FS_OP_CLOSE), "close") == 0 && strcmp(fs_shim_op_name(200), "?") == 0);
    CHECK(SPIFFS.remove("/contacts3"));
}

// #425 round two: a held save reserves its buffer once, at the file's last size plus a chunk, instead of doubling up
// from 4 KB (at 552 contacts: six PSRAM allocations, five frees and 124 KB copied under the mesh lock)
static void held_reserve(void) {
    static uint8_t expect[260 * REC_SIZE], disk[260 * REC_SIZE];
    CHECK(fs_shim_reserve(0) == 0 && fs_shim_reserve(100) == 100 + FS_SHIM_CHUNK);
    CHECK(fs_shim_reserve(FS_SHIM_MAX_FILE - 1) == FS_SHIM_MAX_FILE);
    CHECK(fs_shim_reserve(FS_SHIM_MAX_FILE) == FS_SHIM_MAX_FILE);
    CHECK(fs_shim_grow_cap(0, 10, 0) == FS_SHIM_CHUNK && fs_shim_grow_cap(0, 10, 9000) == 9000);
    CHECK(fs_shim_grow_cap(0, 9001, 9000) == 4 * FS_SHIM_CHUNK && fs_shim_grow_cap(9000, 9001, 0) == 18000);
    CHECK(fs_shim_grow_cap(9000, 9001, 50000) == 18000); // a reserve only for an empty buffer

    save_contacts(expect, 200, 999, 0); // not held: on the file system, 30400 B
    fs_shim_set_buffer_hooks(count_alloc, count_free);
    fs_shim_hold("/contacts3"); // the reserve starts at the file's size
    fs_shim_held_stats_t st;
    fs_shim_take_held_stats(NULL);
    alloc_calls = 0;
    save_contacts(expect, 200, 5, 1);
    fs_shim_take_held_stats(&st);
    CHECK(st.saves == 1 && st.writes == 200 * 12 && st.allocs == 1 && st.copied == 0 && alloc_calls == 1);
    fs_shim_held_t taken;
    fs_shim_take_held(&taken);
    CHECK(taken.set && taken.len == 200 * REC_SIZE && taken.cap == 200 * REC_SIZE + FS_SHIM_CHUNK);
    CHECK(File::writeHeld(&taken).ok && live_blocks == 0);

    // Thirty contacts more than the headroom holds: one growth past the reserve, then the next save fits at once
    save_contacts(expect, 230, 6, 2);
    fs_shim_take_held_stats(&st);
    CHECK(st.saves == 1 && st.allocs == 2 && st.copied > 200 * REC_SIZE && st.copied < 230 * REC_SIZE);
    fs_shim_take_held(&taken);
    CHECK(File::writeHeld(&taken).ok);
    save_contacts(expect, 230, 7, 3);
    fs_shim_take_held_stats(&st);
    CHECK(st.saves == 1 && st.allocs == 1 && st.copied == 0);
    fs_shim_take_held(&taken);
    CHECK(File::writeHeld(&taken).ok && live_blocks == 0);
    CHECK(disk_read("/contacts3", disk, sizeof(disk)) == 230 * REC_SIZE && memcmp(disk, expect, 230 * REC_SIZE) == 0);

    // A file that is not held never reserves, and its writes are not counted
    File other = SPIFFS.open("/prefs", "w");
    CHECK(other.write(expect, 10) == 10);
    other.close();
    fs_shim_take_held_stats(&st);
    CHECK(st.saves == 0 && st.writes == 0 && st.allocs == 0);
    fs_shim_hold(NULL);
    fs_shim_set_buffer_hooks(NULL, NULL);
    CHECK(SPIFFS.remove("/prefs") && SPIFFS.remove("/contacts3"));
}

int main(void) {
    char root[] = "/tmp/fs_shim_test.XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    CHECK(!SPIFFS.begin(true)); // no root yet: the OS hands it over first (#84 T6)
    fs_shim_set_root(root);
    CHECK(SPIFFS.begin(true));
    snprintf(root_dir, sizeof(root_dir), "%s", root);

    // The app stays below its root: no "..", ".", or "//" anywhere in a path
    CHECK(!SPIFFS.exists("/../x") && !SPIFFS.exists("/a/../b") && !SPIFFS.exists("a//b") && !SPIFFS.exists("/./a"));
    File escape = SPIFFS.open("/../escape", "w", true);
    CHECK(!escape);
    char outside[300];
    snprintf(outside, sizeof(outside), "%s/../escape", root);
    CHECK(access(outside, F_OK) != 0);
    CHECK(!SPIFFS.mkdir("/a/../up") && !SPIFFS.remove("/.."));

    // Which roots format() may wipe
    CHECK(fs_shim_format_allowed("/storage/meshcore") && fs_shim_format_allowed(root));
    CHECK(!fs_shim_format_allowed("/storage") && !fs_shim_format_allowed("/") && !fs_shim_format_allowed(""));
    CHECK(!fs_shim_format_allowed(NULL) && !fs_shim_format_allowed("/storage/") && !fs_shim_format_allowed("storage/x"));
    CHECK(!fs_shim_format_allowed("/storage/c6") && !fs_shim_format_allowed("/storage/meshcore/.."));

    // Missing files: not there, and opening one for reading gives a false File
    CHECK(!SPIFFS.exists("/a/b"));
    File missing = SPIFFS.open("/a/b", "r");
    CHECK(!missing);

    // Write 300 bytes into a directory that doesn't exist yet, as DataStore's "w", create = true
    uint8_t data[300];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)i;
    }
    File out = SPIFFS.open("/a/b", "w", true);
    CHECK(out);
    CHECK(out.write(data, sizeof(data)) == sizeof(data));
    out.close();
    CHECK(SPIFFS.exists("/a/b"));

    // Read back, seek, single bytes, size
    File in = SPIFFS.open("/a/b", "r");
    uint8_t back[300] = {0};
    CHECK(in && in.size() == 300);
    CHECK(in.read(back, sizeof(back)) == sizeof(back) && memcmp(back, data, sizeof(data)) == 0);
    CHECK(in.read() == -1); // at the end
    CHECK(in.seek(10) && in.read() == 10);
    in.close();

    // "w" truncates
    out = SPIFFS.open("/a/b", "w");
    CHECK(out.write(data, 5) == 5);
    out.close();
    in = SPIFFS.open("/a/b");
    CHECK(in.size() == 5 && in.read(back, sizeof(back)) == 5); // a short file reads short, no error
    in.close();

    buffered_writes();
    held_saves();
    held_reserve();

    // Remove; mkdir of a nested directory
    CHECK(SPIFFS.remove("/a/b") && !SPIFFS.exists("/a/b"));
    CHECK(!SPIFFS.remove("/a/b"));
    CHECK(SPIFFS.mkdir("/x/y") && SPIFFS.exists("/x/y"));

    // Sizes come from the platform hook; the host's numbers just have to make sense, and the OS hook is used when set
    CHECK(SPIFFS.totalBytes() > 0 && SPIFFS.usedBytes() <= SPIFFS.totalBytes());
    fs_shim_set_drive_bytes(fake_drive);
    CHECK(SPIFFS.totalBytes() == 1000 && SPIFFS.usedBytes() == 250);
    fs_shim_set_drive_bytes(NULL);

    // rename (#110) replaces the target, also where the file system refuses an existing one (FatFs)
    for (int fat = 0; fat < 2; fat++) {
        fs_shim_test_fat_rename(fat == 1);
        out = SPIFFS.open("/r/old", "w");
        CHECK(out.write((uint8_t const *)"old", 3) == 3);
        out.close();
        out = SPIFFS.open("/r/new", "w");
        CHECK(out.write((uint8_t const *)"newer", 5) == 5);
        out.close();
        CHECK(SPIFFS.rename("/r/new", "/r/old"));
        CHECK(!SPIFFS.exists("/r/new"));
        File got = SPIFFS.open("/r/old", "r");
        char text[8] = {0};
        CHECK(got && got.read((uint8_t *)text, sizeof(text)) == 5 && memcmp(text, "newer", 5) == 0);
        got.close();
        CHECK(!SPIFFS.rename("/r/none", "/r/old") && SPIFFS.exists("/r/old")); // no source: the target stays
        CHECK(!SPIFFS.rename("/r/old", "/../old"));
        CHECK(SPIFFS.rename("/r/old", "/r/moved") && SPIFFS.exists("/r/moved") && SPIFFS.remove("/r/moved"));
    }
    fs_shim_test_fat_rename(false);

    // format() empties the root
    out = SPIFFS.open("/c", "w");
    out.close();
    CHECK(SPIFFS.format() && !SPIFFS.exists("/c") && !SPIFFS.exists("/x/y"));
    // ... but never a root the rule refuses: the file survives
    out = SPIFFS.open("/d", "w");
    out.close();
    fs_shim_set_root("/");
    CHECK(!SPIFFS.format());
    fs_shim_set_root(root);
    CHECK(SPIFFS.exists("/d") && SPIFFS.remove("/d"));
    rmdir(root);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("fs_shim_test: all checks passed\n");
    return 0;
}
