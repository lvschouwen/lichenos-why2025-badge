#include "nvs_image.h"
#include "nvs_image_fixtures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void page_from(uint8_t *page, uint8_t const *head, size_t n) {
    memset(page, 0xFF, NVS_IMG_PAGE_SIZE);
    memcpy(page, head, n);
}

static nvs_image_kind_t kind_of_page(uint8_t const *page) {
    nvs_image_tally_t t = {0};
    nvs_image_tally_page(&t, page);
    return nvs_image_tally_kind(&t);
}

static void real_pages_are_told_apart(void) {
    uint8_t page[NVS_IMG_PAGE_SIZE];
    page_from(page, nvs_plain_page0, sizeof(nvs_plain_page0));
    CHECK(kind_of_page(page) == NVS_IMG_PLAIN);
    page_from(page, nvs_enc_page0, sizeof(nvs_enc_page0));
    CHECK(kind_of_page(page) == NVS_IMG_ENCRYPTED);
}

static void erased_and_corrupt_pages_count_for_nothing(void) {
    uint8_t page[NVS_IMG_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    CHECK(kind_of_page(page) == NVS_IMG_EMPTY); // uninitialised
    page_from(page, nvs_plain_page0, sizeof(nvs_plain_page0));
    memset(page + 32, 0xFF, 32); // an active page whose entries are all empty
    CHECK(kind_of_page(page) == NVS_IMG_EMPTY);
    page_from(page, nvs_plain_page0, sizeof(nvs_plain_page0));
    memset(page, 0, 4); // page state CORRUPT
    CHECK(kind_of_page(page) == NVS_IMG_EMPTY);
    memset(page + 32, 0, 32); // every entry erased
    memcpy(page, nvs_plain_page0, 4);
    CHECK(kind_of_page(page) == NVS_IMG_EMPTY);
}

static void a_flipped_bit_in_the_first_entry_is_not_plaintext(void) {
    uint8_t page[NVS_IMG_PAGE_SIZE];
    page_from(page, nvs_plain_page0, sizeof(nvs_plain_page0));
    page[64 + 20] ^= 1; // a key byte of entry 0
    CHECK(kind_of_page(page) == NVS_IMG_ENCRYPTED);
}

static void the_majority_decides(void) {
    uint8_t good[NVS_IMG_PAGE_SIZE], bad[NVS_IMG_PAGE_SIZE];
    page_from(good, nvs_plain_page0, sizeof(nvs_plain_page0));
    page_from(bad, nvs_enc_page0, sizeof(nvs_enc_page0));
    nvs_image_tally_t t = {0};
    nvs_image_tally_page(&t, good);
    nvs_image_tally_page(&t, good);
    nvs_image_tally_page(&t, bad);
    CHECK(t.plain == 2 && t.bad == 1);
    CHECK(nvs_image_tally_kind(&t) == NVS_IMG_PLAIN); // one damaged page: still plaintext, still migrated
    nvs_image_tally_page(&t, bad);
    CHECK(nvs_image_tally_kind(&t) == NVS_IMG_ENCRYPTED); // a tie is not trusted as plain
}

// The safety property of #116: no combination asks for a key to be made, and nothing but "enabled + key + plaintext"
// rewrites the partition
static void policy_table(void) {
    nvs_image_kind_t kinds[] = {NVS_IMG_EMPTY, NVS_IMG_PLAIN, NVS_IMG_ENCRYPTED, NVS_IMG_UNKNOWN};
    for (int k = 0; k < 4; k++) {
        CHECK(nvs_image_decide(false, false, kinds[k]) == NVS_ACT_PLAIN_INIT);
        CHECK(nvs_image_decide(false, true, kinds[k]) == NVS_ACT_PLAIN_INIT);  // option off: the old behaviour
        CHECK(nvs_image_decide(true, false, kinds[k]) == NVS_ACT_PLAIN_INIT);  // no key burned: never encrypt
    }
    CHECK(nvs_image_decide(true, true, NVS_IMG_EMPTY) == NVS_ACT_SECURE_INIT);
    CHECK(nvs_image_decide(true, true, NVS_IMG_ENCRYPTED) == NVS_ACT_SECURE_INIT);
    CHECK(nvs_image_decide(true, true, NVS_IMG_PLAIN) == NVS_ACT_MIGRATE);
    CHECK(nvs_image_decide(true, true, NVS_IMG_UNKNOWN) == NVS_ACT_REFUSE); // not classified: no init that could end in an erase
}

static void key_pair_checks(void) {
    uint8_t a[32], b[32], z[32] = {0};
    memset(a, 1, 32);
    memset(b, 2, 32);
    CHECK(nvs_image_keys_ok(a, b));
    CHECK(!nvs_image_keys_ok(z, b)); // a failed HMAC leaves zeros
    CHECK(!nvs_image_keys_ok(a, z));
    CHECK(!nvs_image_keys_ok(a, a));
}

static void snapshot_keeps_and_wipes(void) {
    nvs_snap_t s = {0};
    char const pw[] = "hunter2";
    CHECK(nvs_snap_add(&s, "badge_fw", "wifi_pass", 0x21, 0, pw, sizeof(pw)));
    CHECK(nvs_snap_add(&s, "badge_fw", "tz", 0x01, 3, NULL, 0));
    uint8_t blob[5] = {1, 2, 3, 4, 5};
    CHECK(nvs_snap_add(&s, "a.meshcore", "pw0123456789ab", 0x42, 0, blob, sizeof(blob)));
    CHECK(nvs_snap_add(&s, "a.meshcore", "empty", 0x42, 0, blob, 0)); // an empty blob is still an entry
    CHECK(s.n == 4);
    for (int i = 0; i < 100; i++) { // growth past the first allocation keeps the earlier entries
        char k[16];
        snprintf(k, sizeof(k), "k%d", i);
        CHECK(nvs_snap_add(&s, "ns", k, 0x04, (uint64_t)i, NULL, 0));
    }
    CHECK(s.n == 104 && strcmp(s.e[0].key, "wifi_pass") == 0 && memcmp(s.e[0].data, "hunter2", 8) == 0);
    CHECK(s.e[2].len == 5 && ((uint8_t *)s.e[2].data)[4] == 5);

    char const *secret = s.e[0].data;
    nvs_snap_zero(&s);
    CHECK(secret[0] == 0 && secret[6] == 0 && ((uint8_t *)s.e[2].data)[0] == 0); // before the free: the bytes are gone
    nvs_snap_wipe(&s);
    CHECK(s.e == NULL && s.n == 0);
}

static void snapshot_refuses_long_names(void) {
    nvs_snap_t s = {0};
    CHECK(!nvs_snap_add(&s, "0123456789abcdef", "k", 1, 0, NULL, 0)); // 16 chars: no room for the NUL
    CHECK(!nvs_snap_add(&s, "ns", "0123456789abcdef", 1, 0, NULL, 0));
    CHECK(nvs_snap_add(&s, "0123456789abcde", "pw0123456789abc", 1, 0, NULL, 0));
    nvs_snap_wipe(&s);
}

static void entries_compare(void) {
    nvs_snap_t a = {0}, b = {0};
    char const v1[] = "abc", v2[] = "abd";
    nvs_snap_add(&a, "n", "k", 0x21, 0, v1, 4);
    nvs_snap_add(&b, "n", "k", 0x21, 0, v1, 4);
    nvs_snap_add(&b, "n", "k", 0x21, 0, v2, 4);
    nvs_snap_add(&b, "n", "k", 0x42, 0, v1, 4);
    nvs_snap_add(&b, "n", "k", 0x01, 1, NULL, 0);
    nvs_snap_add(&a, "n", "k", 0x01, 2, NULL, 0);
    CHECK(nvs_snap_entry_same(&a.e[0], &b.e[0]));
    CHECK(!nvs_snap_entry_same(&a.e[0], &b.e[1])); // value differs
    CHECK(!nvs_snap_entry_same(&a.e[0], &b.e[2])); // type differs
    CHECK(!nvs_snap_entry_same(&a.e[1], &b.e[3])); // number differs
    nvs_snap_wipe(&a);
    nvs_snap_wipe(&b);
}

int main(void) {
    real_pages_are_told_apart();
    erased_and_corrupt_pages_count_for_nothing();
    a_flipped_bit_in_the_first_entry_is_not_plaintext();
    the_majority_decides();
    policy_table();
    key_pair_checks();
    snapshot_keeps_and_wipes();
    snapshot_refuses_long_names();
    entries_compare();
    if (fails) {
        printf("nvs_image_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("nvs_image_test: OK\n");
    return 0;
}
