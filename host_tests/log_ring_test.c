// Host tests for the log ring (#38): sequence numbers, wrap-around, since=, partial lines, overflow accounting
#include "log_ring.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static uint8_t    store[512];
static log_ring_t ring;
static char       out[4096];

static void put(uint32_t ms, char const *s) {
    log_ring_write(&ring, ms, s, strlen(s));
}

static uint8_t raw[4096];

static size_t read_cap(uint32_t since, char *dst, size_t cap, uint32_t *next, uint32_t *missed) {
    size_t n = log_ring_copy(&ring, since, raw, sizeof(raw), cap, next, missed);
    return log_ring_format(raw, n, dst, cap);
}

static size_t read_since(uint32_t since, uint32_t *next, uint32_t *missed) {
    size_t n = read_cap(since, out, sizeof(out) - 1, next, missed);
    out[n]   = '\0';
    return n;
}

static void test_basic(void) {
    uint32_t next, missed;
    log_ring_init(&ring, store, sizeof(store));
    CHECK(read_since(0, &next, &missed) == 0 && next == 1 && missed == 0);
    put(1500, "first\n");
    put(2042, "second\r\n");
    put(3000, "\n"); // empty lines are not stored
    CHECK(read_since(0, &next, &missed) > 0);
    CHECK(strcmp(out, "1 1.500 first\n2 2.042 second\n") == 0);
    CHECK(next == 3 && missed == 0);
    CHECK(read_since(2, &next, &missed) > 0 && strcmp(out, "2 2.042 second\n") == 0 && next == 3);
    CHECK(read_since(3, &next, &missed) == 0 && next == 3 && missed == 0); // caught up
}

static void test_partial(void) {
    uint32_t next, missed;
    log_ring_init(&ring, store, sizeof(store));
    put(10, "half a ");
    CHECK(read_since(0, &next, &missed) == 0); // not a line yet
    put(20, "line\nnext");
    CHECK(read_since(0, &next, &missed) > 0 && strcmp(out, "1 0.010 half a line\n") == 0);
    put(30, " one\n");
    CHECK(read_since(next, &next, &missed) > 0 && strcmp(out, "2 0.020 next one\n") == 0);
}

static void test_long_line_split(void) {
    uint32_t next, missed;
    char     big[LOG_RING_LINE_MAX + 50];
    memset(big, 'x', sizeof(big));
    log_ring_init(&ring, store, sizeof(store));
    log_ring_write(&ring, 5, big, sizeof(big));
    log_ring_write(&ring, 5, "\n", 1);
    read_since(0, &next, &missed);
    CHECK(next == 3); // two lines: 200 and 50 characters
}

static void test_wrap_and_overflow(void) {
    uint32_t next, missed;
    char     line[32];
    log_ring_init(&ring, store, sizeof(store));
    // 512 bytes hold about 15 of these (10 + 20 each): 100 lines wrap many times
    for (int i = 1; i <= 100; i++) {
        snprintf(line, sizeof(line), "line number %05d\n", i);
        put((uint32_t)i, line);
    }
    CHECK(read_since(0, &next, &missed) > 0);
    CHECK(next == 101 && missed == 0);
    CHECK(strstr(out, "line number 00100\n") != NULL);
    CHECK(strstr(out, "line number 00001\n") == NULL);
    uint32_t oldest = ring.first_seq;
    CHECK(oldest > 80 && oldest < 100);

    // Asking from a point that is gone reports what was missed and starts at the oldest line
    CHECK(read_since(5, &next, &missed) > 0);
    CHECK(missed == oldest - 5 && next == 101);
    char first[32];
    snprintf(first, sizeof(first), "%u 0.", (unsigned)oldest);
    CHECK(strncmp(out, first, strlen(first)) == 0);

    // Asking exactly from the oldest line misses nothing
    CHECK(read_since(oldest, &next, &missed) > 0 && missed == 0);
    // A sequence number beyond the newest (the badge restarted) starts over
    CHECK(read_since(5000, &next, &missed) > 0 && missed == 0 && next == 101);
}

static void test_small_output(void) {
    uint32_t next, missed;
    log_ring_init(&ring, store, sizeof(store));
    for (int i = 0; i < 12; i++) {
        put(1, "some text of a line\n");
    }
    // Room for under a worst-case line: nothing is cut in half, the next call continues
    char tiny[LOG_RING_OUT_LINE + 40];
    size_t n = read_cap(0, tiny, sizeof(tiny), &next, &missed);
    CHECK(n > 0 && tiny[n - 1] == '\n' && next > 1 && next <= 12);
    uint32_t from = next;
    n = read_cap(from, tiny, sizeof(tiny), &next, &missed);
    CHECK(n > 0 && next > from && missed == 0);
}

// Every sequence number is found, whatever the wrap, through the sparse index; asking at the next number is empty
static uint8_t big_store[16384];

static void test_seek(void) {
    uint32_t next, missed;
    char     line[40];
    log_ring_init(&ring, big_store, sizeof(big_store));
    for (int i = 1; i <= 3000; i++) { // wraps the ring many times and passes many index steps
        snprintf(line, sizeof(line), "L%05d\n", i);
        put((uint32_t)i, line);
    }
    uint32_t first = ring.first_seq;
    CHECK(first > 1000 && ring.next_seq == 3001);
    for (uint32_t s = first; s <= ring.next_seq; s++) {
        size_t n = log_ring_copy(&ring, s, raw, sizeof(raw), sizeof(out), &next, &missed);
        if (s == ring.next_seq) {
            CHECK(n == 0 && next == s && missed == 0);
            continue;
        }
        n = log_ring_format(raw, n, out, sizeof(out));
        out[n] = '\0';
        char want[32];
        snprintf(want, sizeof(want), "%u %u.%03u L%05u\n", (unsigned)s, (unsigned)(s / 1000), (unsigned)(s % 1000),
                 (unsigned)s);
        if (strncmp(out, want, strlen(want)) != 0 || missed != 0) {
            CHECK(!"seek landed on the wrong record");
            break;
        }
    }
    // Older than the oldest line: starts at the oldest and says how many were missed
    log_ring_copy(&ring, 3, raw, sizeof(raw), sizeof(out), &next, &missed);
    CHECK(missed == first - 3);
    // The work per call is bounded by the buffers, not by the ring: a tiny raw buffer takes a few records only
    size_t n = log_ring_copy(&ring, first + 100, raw, 3 * (LOG_RING_HEADER + 6), sizeof(out), &next, &missed);
    CHECK(n == 3 * (LOG_RING_HEADER + 6) && next == first + 103);
    // A budget below one worst-case row yields nothing and leaves next at the request
    n = log_ring_copy(&ring, first + 100, raw, sizeof(raw), 20, &next, &missed);
    CHECK(n == 0 && next == first + 100);
}

// Terminal control bytes and invalid UTF-8 from the radio never reach the reader
static void test_sanitise(void) {
    uint32_t next, missed;
    log_ring_init(&ring, store, sizeof(store));
    put(1, "a\x1b[31mred\x07\tb\x7f\x01\n");
    put(2, "caf\xc3\xa9 \xf0\x9f\x8d\x84 ok\n");   // valid: e-acute, mushroom
    put(3, "x\xc2\x9b" "31m\n");                      // C1 CSI encoded in UTF-8
    put(4, "x\x9b" "31m \x80 \xc3\n");               // lone C1 byte, stray continuation, truncated sequence
    put(5, "\xed\xa0\x80 \xc0\xaf \xf5\x80\x80\x80 \xe0\x80\x80\n"); // surrogate, overlong, > U+10FFFF
    read_since(0, &next, &missed);
    CHECK(strstr(out, "1 0.001 a?[31mred?\tb??\n") != NULL);
    CHECK(strstr(out, "caf\xc3\xa9 \xf0\x9f\x8d\x84 ok\n") != NULL);
    CHECK(strstr(out, "3 0.003 x??31m\n") != NULL);
    CHECK(strstr(out, "4 0.004 x?31m ? ??\n") != NULL || strstr(out, "4 0.004 x?31m ? ?\n") != NULL);
    CHECK(strstr(out, "5 0.005 ??? ?? ???? ???\n") != NULL);
    for (char const *p = out; *p; p++) {
        CHECK((unsigned char)*p >= 0x20 || *p == '\n' || *p == '\t');
        CHECK((unsigned char)*p != 0x7f);
    }
}

int main(void) {
    test_basic();
    test_partial();
    test_long_line_split();
    test_wrap_and_overflow();
    test_small_output();
    test_seek();
    test_sanitise();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("All log_ring tests passed\n");
    return 0;
}
