#pragma once
// The notification history behind lichen_notify.h (#62): 48 entries, newest first by sequence number, with coalescing,
// the eviction order and the file codec. Pure (no ESP headers), host-tested in host_tests/notify_ring_test.c;
// lichen_notify.c adds the mutex, PSRAM and the file.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_SLOTS       48
#define NR_SOURCE_CAP  14 // an app id is 1..13 of a-z 0-9 _ (app_ns.h), or "lichen"
#define NR_TARGET_CAP  32
#define NR_TITLE_CAP   40
#define NR_BODY_CAP    120
#define NR_COALESCE_S  600 // an unread entry with the same (source, target) this recent takes a new notice in

// Levels as lichen_notify.h's ln_level_t
#define NR_INFO  0
#define NR_WARN  1
#define NR_ERROR 2

typedef struct {
    uint32_t id;     // 0: a free slot
    uint32_t seq;    // order: higher is newer (a coalesced entry gets a new one, so it moves to the top)
    int64_t  epoch;  // wall clock (s) of the last change, 0 while the clock was not set
    uint32_t mono_s; // uptime (s) of the last change, in boot `boot`
    uint16_t boot;   // the store's boot counter when it last changed
    uint16_t count;  // notices merged into it (1 = just this one)
    uint8_t  level;
    uint8_t  flags;  // the lichen_notify flags it was raised with
    uint8_t  read;
    char     source[NR_SOURCE_CAP];
    char     target[NR_TARGET_CAP]; // "" for none
    char     title[NR_TITLE_CAP];
    char     body[NR_BODY_CAP];
} nr_entry_t;

typedef struct {
    nr_entry_t e[NR_SLOTS];
    uint32_t   next_id;  // ids are never 0 and never reused within a store (they survive in the file)
    uint32_t   next_seq;
    uint16_t   boot;
    uint32_t   dropped;    // notices that found no room (this boot)
    uint32_t   generation; // moves on every change
} nr_ring_t;

// One new notice. Strings may be NULL (= ""); they are cut to their fields.
typedef struct {
    char const *source;
    char const *target;
    char const *title;
    char const *body;
    uint8_t     level;
    uint8_t     flags;
    int64_t     epoch;
    uint32_t    mono_s;
    bool        coalesce;   // may merge into an unread entry with the same (source, target) of the last NR_COALESCE_S
    bool        restricted; // over its rate limit: may only push out read entries or its own
} nr_new_t;

typedef enum { NR_ADDED, NR_COALESCED, NR_DROPPED } nr_outcome_t;

void nr_init(nr_ring_t *r, uint16_t boot);

// Adds n; *id gets the entry's id (0 when dropped). With the ring full, one entry goes first: the oldest read one,
// else the unread one of the lowest level (oldest first) that is not above n's level. So an unread error only ever
// gives way to another error, and a notice that finds only higher unread levels is dropped (counted).
nr_outcome_t nr_add(nr_ring_t *r, nr_new_t const *n, uint32_t *id);

nr_entry_t const *nr_get(nr_ring_t const *r, uint32_t id);
// Ids, newest first; returns how many (at most max)
size_t nr_ids(nr_ring_t const *r, uint32_t *ids, size_t max);
size_t nr_count(nr_ring_t const *r);

// Unread entries of source (NULL: all) and the highest unread level (-1 when none)
size_t nr_unread(nr_ring_t const *r, char const *source);
int    nr_unread_top(nr_ring_t const *r);

// Marks read; each returns how many entries changed. source NULL = every source, target NULL = every target.
size_t nr_mark_read(nr_ring_t *r, char const *source, char const *target);
size_t nr_mark_read_id(nr_ring_t *r, uint32_t id);
size_t nr_mark_read_upto(nr_ring_t *r, uint8_t level); // unread entries of this level or lower
bool   nr_remove(nr_ring_t *r, uint32_t id);
void   nr_clear(nr_ring_t *r);

// The file: header (magic "LNTF", version, record size, count, boot, next_id, next_seq), the entries oldest first,
// CRC-32 over all of it. NR_FILE_MAX bytes at most.
#define NR_FILE_MAGIC   0x46544E4Cu // "LNTF" little-endian
#define NR_FILE_VERSION 1
#define NR_HEADER_SIZE  24
#define NR_RECORD_SIZE  234
#define NR_FILE_MAX     (NR_HEADER_SIZE + NR_SLOTS * NR_RECORD_SIZE + 4)
size_t nr_encode(nr_ring_t const *r, uint8_t *buf, size_t cap);
// False (and r left empty with boot 0) for anything but a whole, valid file: wrong magic, version or size, a bad CRC,
// a torn write. dropped and generation start at 0.
bool   nr_decode(nr_ring_t *r, uint8_t const *buf, size_t len);

uint32_t nr_crc32(uint8_t const *p, size_t n);

#ifdef __cplusplus
}
#endif
