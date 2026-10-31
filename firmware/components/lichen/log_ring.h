#pragma once

#include <stddef.h>
#include <stdint.h>

// The pure part of the badge's log store (#38): a byte ring of text lines, each with a sequence number and an uptime
// stamp. Pure so it is host-tested (host_tests/log_ring_test.c); log_store.c puts it in PSRAM and feeds it.
//
// Sequence numbers start at 1, so "since 0" means everything the ring still holds. When the ring is full the oldest
// lines are dropped; a reader that asks for lines already gone is told how many it missed.

#define LOG_RING_LINE_MAX 200 // a longer line is split here
#define LOG_RING_HEADER   10  // seq (4), uptime ms (4), length (2) in front of each line's text
#define LOG_RING_ROW_EXTRA 24 // the most "<seq> <sec>.<ms> " and the '\n' add to a line's text in log_ring_format's output
#define LOG_RING_OUT_LINE (LOG_RING_LINE_MAX + LOG_RING_ROW_EXTRA) // the most one line takes in the output
#define LOG_RING_IDX_STEP 32  // every 32nd record is indexed, so finding a sequence number walks at most 31 records
#define LOG_RING_IDX_SLOTS 512 // covers 16384 records: more than a 96 KB ring can hold (11 bytes at least per line)

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   head; // offset of the oldest record
    size_t   used; // bytes of records
    uint32_t first_seq; // sequence number of the oldest record (== next_seq when empty)
    uint32_t next_seq;  // number the next line gets
    char     pend[LOG_RING_LINE_MAX]; // the line being assembled: text that has not seen its '\n' yet
    size_t   pend_len;
    uint32_t pend_ms;
    uint32_t idx_seq[LOG_RING_IDX_SLOTS]; // sparse index: sequence number (multiple of the step) and ...
    uint32_t idx_at[LOG_RING_IDX_SLOTS];  // ... offset of its record; stale once idx_seq < first_seq
} log_ring_t;

void log_ring_init(log_ring_t *r, uint8_t *buf, size_t cap);

// Appends text. A '\n' ends a line (empty lines are not stored), '\r' is ignored, text after the last '\n' waits for
// the rest of its line; ms is the uptime when the line started.
void log_ring_write(log_ring_t *r, uint32_t ms, char const *text, size_t len);

// Text is sanitised when a line is stored: control characters (except tab), 0x7f and bytes that are not valid UTF-8 (C1
// controls included) become '?', so node names and messages from the radio can't drive the reader's terminal.

// Step 1, the only part that must run under the store's lock: copies the records (header and text, back to back) with
// sequence number >= since into raw, as many as fit in raw_cap and whose formatted size fits in out_budget. A since of 0,
// or beyond the newest line (the badge restarted), starts at the oldest line. *next is the number to ask for next time;
// *missed counts lines the reader asked for that were already dropped. The work is bounded: at most 31 records to find
// `since`, then raw_cap bytes. Returns the bytes written to raw.
size_t log_ring_copy(log_ring_t const *r, uint32_t since, uint8_t *raw, size_t raw_cap, size_t out_budget,
                     uint32_t *next, uint32_t *missed);

// Step 2, outside the lock: renders copied records as "<seq> <sec>.<ms> <text>\n" into out (cap >= the out_budget given
// to log_ring_copy). Returns the bytes written.
size_t log_ring_format(uint8_t const *raw, size_t raw_len, char *out, size_t cap);
