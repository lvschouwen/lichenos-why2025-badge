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

static size_t read_since(uint32_t since, uint32_t *next, uint32_t *missed) {
    size_t n = log_ring_read(&ring, since, out, sizeof(out), next, missed);
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
    for (int i = 0; i < 6; i++) {
        put(1, "some text of a line\n");
    }
    // Room for under a worst-case line: nothing is cut in half, the next call continues
    char tiny[LOG_RING_OUT_LINE + 40];
    size_t n = log_ring_read(&ring, 0, tiny, sizeof(tiny), &next, &missed);
    CHECK(n > 0 && tiny[n - 1] == '\n' && next > 1 && next <= 7);
    uint32_t from = next;
    n = log_ring_read(&ring, from, tiny, sizeof(tiny), &next, &missed);
    CHECK(n > 0 && next > from && missed == 0);
}

int main(void) {
    test_basic();
    test_partial();
    test_long_line_split();
    test_wrap_and_overflow();
    test_small_output();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("All log_ring tests passed\n");
    return 0;
}
