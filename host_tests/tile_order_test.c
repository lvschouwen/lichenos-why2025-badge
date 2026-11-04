// Host test: a grid's saved display order (#189): merge, parse, encode, move
#include "tile_order.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static char const *const IDS[] = {"settings", "about", "files", "clock", "meshcore"};
#define N 5

static bool same(int const *order, int const *want, int n) {
    return memcmp(order, want, (size_t)n * sizeof(*order)) == 0;
}

static bool permutation(int const *order, int n) {
    int seen[64] = {0};
    for (int i = 0; i < n; i++) {
        if (order[i] < 0 || order[i] >= n || seen[order[i]]++) {
            return false;
        }
    }
    return true;
}

static void identity_when_nothing_saved(void) {
    int       order[N];
    int const want[N] = {0, 1, 2, 3, 4};
    CHECK(!tile_order_merge(NULL, IDS, N, order) && same(order, want, N));
    CHECK(!tile_order_merge("", IDS, N, order) && same(order, want, N));
    CHECK(tile_order_is_default(order, N));
    CHECK(tile_order_merge("1:", IDS, N, order) && same(order, want, N)); // valid, names nothing
}

static void saved_order_new_and_removed(void) {
    int order[N];
    // clock and files swapped; meshcore is new (not in the string): it goes to the end
    int const want1[N] = {0, 1, 3, 2, 4};
    CHECK(tile_order_merge("1:settings,about,clock,files", IDS, N, order) && same(order, want1, N));
    // a removed app ("weather") is skipped; the rest that are missing follow in registration order
    int const want2[N] = {4, 3, 0, 1, 2};
    CHECK(tile_order_merge("1:meshcore,weather,clock", IDS, N, order) && same(order, want2, N));
    CHECK(!tile_order_is_default(order, N));
}

static void duplicates_and_junk(void) {
    int       order[N];
    int const want[N] = {2, 0, 1, 3, 4};
    CHECK(tile_order_merge("1:files,files,settings,,files", IDS, N, order) && same(order, want, N));
    int const plain[N] = {0, 1, 2, 3, 4};
    CHECK(!tile_order_merge("2:files,settings", IDS, N, order) && same(order, plain, N)); // another version
    CHECK(!tile_order_merge("files,settings", IDS, N, order) && same(order, plain, N));   // no version
    CHECK(!tile_order_merge("1", IDS, N, order) && same(order, plain, N));
    CHECK(!tile_order_merge("1:files\x01,about", IDS, N, order) && same(order, plain, N)); // corrupt bytes
    CHECK(!tile_order_merge("\xff\xfe:", IDS, N, order) && same(order, plain, N));
    // ids match exactly: a prefix or a longer name is not the app
    int const want3[N] = {1, 0, 2, 3, 4};
    CHECK(tile_order_merge("1:file,filesx,about,Settings", IDS, N, order) && same(order, want3, N));
}

static void truncated_and_overlong(void) {
    int order[N];
    // Cut off mid-id: the half id is unknown, what came before it counts
    int const want[N] = {3, 0, 1, 2, 4};
    CHECK(tile_order_merge("1:clock,meshc", IDS, N, order) && same(order, want, N));
    // Overlong: a very long string of junk ids still ends in a permutation
    char long_text[2048] = "1:";
    for (int i = 0; i < 200; i++) {
        strcat(long_text, "zzzzzzz,");
    }
    strcat(long_text, "about");
    int const want2[N] = {1, 0, 2, 3, 4};
    CHECK(tile_order_merge(long_text, IDS, N, order) && same(order, want2, N));
    int found[N], count = 99;
    CHECK(!tile_order_parse(NULL, IDS, N, found, &count) && count == 0);
    CHECK(tile_order_parse("1:about,about,clock", IDS, N, found, &count) && count == 2 && found[0] == 1 &&
          found[1] == 3);
}

static void moves(void) {
    int order[N] = {0, 1, 2, 3, 4};
    CHECK(!tile_order_move(order, N, 2, 2));
    CHECK(!tile_order_move(order, N, -1, 2) && !tile_order_move(order, N, 0, N));
    int const plain[N] = {0, 1, 2, 3, 4};
    CHECK(same(order, plain, N));
    int const to_first[N] = {3, 0, 1, 2, 4};
    CHECK(tile_order_move(order, N, 3, 0) && same(order, to_first, N));
    int const to_last[N] = {0, 1, 2, 4, 3};
    CHECK(tile_order_move(order, N, 0, 4) && same(order, to_last, N));
    int const one_down[N] = {1, 0, 2, 4, 3};
    CHECK(tile_order_move(order, N, 0, 1) && same(order, one_down, N));
    int slot_of[N];
    tile_order_slots(order, N, slot_of);
    for (int s = 0; s < N; s++) {
        CHECK(slot_of[order[s]] == s);
    }
}

static void round_trip(void) {
    int  order[N] = {4, 2, 0, 3, 1};
    char text[64];
    CHECK(tile_order_encode(IDS, order, N, text, sizeof(text)) == strlen("1:meshcore,files,settings,clock,about"));
    CHECK(strcmp(text, "1:meshcore,files,settings,clock,about") == 0);
    int back[N];
    CHECK(tile_order_merge(text, IDS, N, back) && same(back, order, N));
    // Too small a buffer: nothing half-written
    char small[12] = "x";
    CHECK(tile_order_encode(IDS, order, N, small, sizeof(small)) == 0 && small[0] == '\0');
    // An id that would break the format is refused
    char const *const bad[] = {"a,b", "c"};
    int const         two[] = {0, 1};
    CHECK(tile_order_encode(bad, two, 2, text, sizeof(text)) == 0 && text[0] == '\0');
    CHECK(tile_order_encode(IDS, order, 0, text, sizeof(text)) == 2 && strcmp(text, "1:") == 0);
}

// Whatever the string, the output is a permutation, and a move keeps it one
static void always_a_permutation(void) {
    static char const alphabet[] = "1:,abcdefilmnorstuxz2";
    srand(189);
    for (int round = 0; round < 20000; round++) {
        char text[48];
        int  len = rand() % (int)(sizeof(text) - 1);
        for (int i = 0; i < len; i++) {
            text[i] = alphabet[rand() % (int)(sizeof(alphabet) - 1)];
        }
        text[len] = '\0';
        if (round % 3 == 0 && len > 2) {
            text[0] = '1';
            text[1] = ':';
        }
        int order[N];
        tile_order_merge(text, IDS, N, order);
        CHECK(permutation(order, N));
        tile_order_move(order, N, rand() % N, rand() % N);
        CHECK(permutation(order, N));
        char enc[64];
        int  back[N];
        CHECK(tile_order_encode(IDS, order, N, enc, sizeof(enc)) > 0);
        CHECK(tile_order_merge(enc, IDS, N, back) && same(back, order, N));
    }
}

int main(void) {
    identity_when_nothing_saved();
    saved_order_new_and_removed();
    duplicates_and_junk();
    truncated_and_overlong();
    moves();
    round_trip();
    always_a_permutation();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("tile_order_test: all passed\n");
    return 0;
}
