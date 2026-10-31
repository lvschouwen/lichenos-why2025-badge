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
#define LOG_RING_OUT_LINE (LOG_RING_LINE_MAX + 32) // the most one line takes in log_ring_read's output

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
} log_ring_t;

void log_ring_init(log_ring_t *r, uint8_t *buf, size_t cap);

// Appends text. A '\n' ends a line (empty lines are not stored), '\r' is ignored, text after the last '\n' waits for
// the rest of its line; ms is the uptime when the line started.
void log_ring_write(log_ring_t *r, uint32_t ms, char const *text, size_t len);

// Renders the lines with sequence number >= since as "<seq> <sec>.<ms> <text>\n" into out (whole lines only). A since
// of 0, or beyond the newest line (the badge restarted), starts at the oldest line. *next is the number to ask for
// next time; *missed counts lines the reader asked for that were already dropped. Returns the bytes written.
size_t log_ring_read(log_ring_t const *r, uint32_t since, char *out, size_t cap, uint32_t *next, uint32_t *missed);
