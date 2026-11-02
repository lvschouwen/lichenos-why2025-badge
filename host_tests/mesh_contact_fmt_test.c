// Host test: a contact's texts (#85 hop count, #49 details)
#include "mesh_contact_fmt.h"

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

static char const *hops(uint8_t path_len) {
    static char out[16];
    mesh_format_hops(path_len, out, sizeof(out));
    return out;
}

// #85: the hash size rides in bits 6-7; the raw byte showed "65 hops" for one hop with 2-byte hashes
static void test_hops(void) {
    CHECK(strcmp(hops(0xFF), "flood") == 0);
    CHECK(strcmp(hops(0x00), "direct") == 0);
    CHECK(strcmp(hops(0x01), "1 hop") == 0);
    CHECK(strcmp(hops(0x03), "3 hops") == 0);
    CHECK(strcmp(hops(0x41), "1 hop") == 0);  // 2-byte hashes
    CHECK(strcmp(hops(0x82), "2 hops") == 0); // 3-byte hashes
    CHECK(strcmp(hops(0x40), "direct") == 0);
    mesh_path_t p = mesh_path_decode(0x41);
    CHECK(!p.flood && p.hops == 1 && p.hash_size == 2);
    p = mesh_path_decode(0xC5);
    CHECK(!p.flood && p.hops == 5 && p.hash_size == 4);
    p = mesh_path_decode(0x3F);
    CHECK(!p.flood && p.hops == 63 && p.hash_size == 1);
    p = mesh_path_decode(0xFF);
    CHECK(p.flood && p.hops == 0);
}

static void test_hex_and_route(void) {
    uint8_t key[32];
    for (int i = 0; i < 32; i++) {
        key[i] = (uint8_t)(0xA0 + i);
    }
    char out[160];
    mesh_hex(key, 6, 0, out, sizeof(out));
    CHECK(strcmp(out, "a0a1a2a3a4a5") == 0);
    mesh_hex(key, 8, 4, out, sizeof(out));
    CHECK(strcmp(out, "a0a1a2a3 a4a5a6a7") == 0);
    mesh_hex(key, 32, 4, out, sizeof(out));
    CHECK(strlen(out) == 64 + 7);
    mesh_hex(key, 8, 0, out, 7); // cut on a whole byte
    CHECK(strcmp(out, "a0a1a2") == 0);

    uint8_t path[64] = {0x2a, 0xe9, 0x6c, 0x65, 0x7d, 0x03};
    mesh_format_route(0xFF, path, out, sizeof(out));
    CHECK(strcmp(out, "flood") == 0);
    mesh_format_route(0x00, path, out, sizeof(out));
    CHECK(strcmp(out, "direct") == 0);
    mesh_format_route(0x02, path, out, sizeof(out));
    CHECK(strcmp(out, "2 hops via 2a,e9") == 0);
    mesh_format_route(0x42, path, out, sizeof(out)); // #85: two hops, not 66
    CHECK(strcmp(out, "2 hops via 2ae9,6c65 (2-byte hashes)") == 0);
    mesh_format_route(0x81, path, out, sizeof(out));
    CHECK(strcmp(out, "1 hop via 2ae96c (3-byte hashes)") == 0);
    mesh_format_route(0xFE, path, out, sizeof(out)); // 62 hops of 4 bytes: past the 64-byte path, never read past it
    CHECK(strncmp(out, "62 hops via 2ae96c65,7d030000", 29) == 0);
    mesh_format_route(0x3F, path, out, 20); // cut short, still a string
    CHECK(strlen(out) == 19);
}

static void test_uri(void) {
    uint8_t key[32];
    for (int i = 0; i < 32; i++) {
        key[i] = (uint8_t)i;
    }
    char out[MESH_URI_MAX];
    CHECK(mesh_contact_uri("Example Contact", key, 1, out, sizeof(out)));
    CHECK(
        strcmp(
            out,
            "meshcore://contact/add?name=Example+Contact&public_key="
            "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f&type=1"
        ) == 0
    );
    CHECK(mesh_contact_uri("a&b=c/\xC3\xA9", key, 2, out, sizeof(out)));
    char const *want = "meshcore://contact/add?name=a%26b%3Dc%2F%C3%A9&public_key=";
    CHECK(strncmp(out, want, strlen(want)) == 0);
    CHECK(strcmp(out + strlen(out) - 7, "&type=2") == 0);
    // The longest name (32 bytes, all escaped) still fits
    char name[33];
    memset(name, '/', 32);
    name[32] = '\0';
    CHECK(mesh_contact_uri(name, key, 3, out, sizeof(out)) && strlen(out) == 207);
    CHECK(!mesh_contact_uri("x", key, 1, out, 40) && out[0] == '\0');
}

static void test_order(void) {
    CHECK(mesh_contact_before(MESH_CONTACT_FAV, 900, 0, 5)); // a favourite beats a newer contact
    CHECK(!mesh_contact_before(0, 5, MESH_CONTACT_FAV, 900));
    CHECK(mesh_contact_before(0, 5, 0, 900));
    CHECK(mesh_contact_before(MESH_CONTACT_FAV | 0x02, 5, MESH_CONTACT_FAV, 900)); // other flag bits don't count
    CHECK(!mesh_contact_before(0, 5, 0, 5));
}

// The grid's contact lists (#88)
static void test_views(void) {
    CHECK(mesh_contact_view_keeps(MESH_VIEW_ALL, 1) && mesh_contact_view_keeps(MESH_VIEW_HEARD, 4));
    CHECK(mesh_contact_view_keeps(MESH_VIEW_REPEATERS, 2) && mesh_contact_view_keeps(MESH_VIEW_REPEATERS, 3));
    CHECK(!mesh_contact_view_keeps(MESH_VIEW_REPEATERS, 1) && !mesh_contact_view_keeps(MESH_VIEW_REPEATERS, 4));
    CHECK(!mesh_contact_view_keeps(MESH_VIEW_REPEATERS, 0));
    // Contacts are the people (#101): repeaters and rooms only on Repeaters; sensors stay (no other list has them)
    CHECK(mesh_contact_view_keeps(MESH_VIEW_CONTACTS, 1) && mesh_contact_view_keeps(MESH_VIEW_CONTACTS, 4));
    CHECK(!mesh_contact_view_keeps(MESH_VIEW_CONTACTS, 2) && !mesh_contact_view_keeps(MESH_VIEW_CONTACTS, 3));
    CHECK(mesh_contact_view_before(MESH_VIEW_CONTACTS, MESH_CONTACT_FAV, 900, 0, 5)); // favourites first
    CHECK(mesh_contact_view_before(MESH_VIEW_ALL, MESH_CONTACT_FAV, 900, 0, 5));
    CHECK(mesh_contact_view_before(MESH_VIEW_REPEATERS, MESH_CONTACT_FAV, 900, 0, 5));
    CHECK(!mesh_contact_view_before(MESH_VIEW_HEARD, MESH_CONTACT_FAV, 900, 0, 5)); // newest first, favourite or not
    CHECK(mesh_contact_view_before(MESH_VIEW_HEARD, 0, 5, MESH_CONTACT_FAV, 900));
    CHECK(!mesh_contact_view_before(MESH_VIEW_HEARD, 0, 5, 0, 5));
}

static void test_lines(void) {
    mesh_contact_detail_t d = {.type = 1, .path_len = 0x41, .age_s = 180, .lat_e6 = 52012345, .lon_e6 = -4500000};
    for (int i = 0; i < 32; i++) {
        d.pub[i] = (uint8_t)(0x10 + i);
    }
    d.path[0] = 0xab;
    d.path[1] = 0xcd;
    mesh_contact_line_t l[MESH_CONTACT_LINES];
    CHECK(mesh_contact_lines(&d, l, MESH_CONTACT_LINES) == MESH_CONTACT_LINES);
    CHECK(strcmp(l[MESH_LINE_TYPE].text, "Type: companion") == 0);
    CHECK(strcmp(l[MESH_LINE_ID].text, "ID: 101112131415") == 0);
    CHECK(strncmp(l[MESH_LINE_KEY].text, "Key: 10111213 14151617 ", 23) == 0 && strlen(l[MESH_LINE_KEY].text) == 76);
    CHECK(strcmp(l[MESH_LINE_HEARD].text, "Last heard: 3 min ago") == 0);
    CHECK(strcmp(l[MESH_LINE_ROUTE].text, "Route: 1 hop via abcd (2-byte hashes)") == 0);
    CHECK(strcmp(l[MESH_LINE_LOCATION].text, "Location: 52.012345, -4.500000") == 0);
    d.lat_e6 = d.lon_e6 = 0;
    d.type              = 2;
    d.path_len          = 0xFF;
    CHECK(mesh_contact_lines(&d, l, 2) == 2);
    CHECK(strcmp(l[MESH_LINE_TYPE].text, "Type: repeater") == 0);
    mesh_contact_lines(&d, l, MESH_CONTACT_LINES);
    CHECK(strcmp(l[MESH_LINE_ROUTE].text, "Route: flood") == 0);
    CHECK(strcmp(l[MESH_LINE_LOCATION].text, "Location: not shared") == 0);
    CHECK(strcmp(mesh_contact_type_name(3), "room") == 0 && strcmp(mesh_contact_type_name(9), "unknown") == 0);
}

// #130: fw mesh path <id> set <hops>
static void test_path_parse(void) {
    uint8_t len = 0xEE, path[64];
    CHECK(mesh_path_parse("854e", 2, &len, path) == MESH_PATH_OK && len == 0x41 && path[0] == 0x85 && path[1] == 0x4e);
    CHECK(path[2] == 0 && path[63] == 0);
    CHECK(mesh_path_parse("854E,a1b2,0001", 2, &len, path) == MESH_PATH_OK && len == (0x40 | 3) && path[2] == 0xa1 &&
          path[5] == 0x01);
    CHECK(mesh_path_parse("ab", 1, &len, path) == MESH_PATH_OK && len == 1 && path[0] == 0xab);
    CHECK(mesh_path_parse("aabbcc,010203", 3, &len, path) == MESH_PATH_OK && len == (0x80 | 2));
    len = 0xEE;
    CHECK(mesh_path_parse("854", 2, &len, path) == MESH_PATH_ERR_LEN);
    CHECK(mesh_path_parse("85", 2, &len, path) == MESH_PATH_ERR_LEN);
    CHECK(mesh_path_parse("854e,12", 2, &len, path) == MESH_PATH_ERR_LEN);
    CHECK(mesh_path_parse("854e5", 2, &len, path) == MESH_PATH_ERR_LEN);
    CHECK(mesh_path_parse("", 2, &len, path) == MESH_PATH_ERR_EMPTY);
    CHECK(mesh_path_parse("854e,", 2, &len, path) == MESH_PATH_ERR_EMPTY);
    CHECK(mesh_path_parse(",854e", 2, &len, path) == MESH_PATH_ERR_EMPTY);
    CHECK(mesh_path_parse("854e,,854e", 2, &len, path) == MESH_PATH_ERR_EMPTY);
    CHECK(mesh_path_parse("85zz", 2, &len, path) == MESH_PATH_ERR_HEX);
    CHECK(mesh_path_parse("854e 854e", 2, &len, path) == MESH_PATH_ERR_HEX);
    CHECK(mesh_path_parse("854e", 4, &len, path) == MESH_PATH_ERR_HASH);
    CHECK(len == 0xEE); // nothing written on an error
    // 32 hops of 2 B = 64 B fit, a 33rd does not; 1 B hops stop at 63
    char text[400];
    size_t n = 0;
    for (int i = 0; i < 33; i++) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "%s0a0b", i ? "," : "");
        CHECK(mesh_path_parse(text, 2, &len, path) == (i < 32 ? MESH_PATH_OK : MESH_PATH_ERR_TOO_LONG));
    }
    CHECK(mesh_path_parse(text, 2, &len, path) == MESH_PATH_ERR_TOO_LONG);
    n = 0;
    for (int i = 0; i < 64; i++) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "%s0a", i ? "," : "");
        CHECK(mesh_path_parse(text, 1, &len, path) == (i < 63 ? MESH_PATH_OK : MESH_PATH_ERR_TOO_LONG));
    }
    CHECK(mesh_path_err_text(MESH_PATH_ERR_LEN)[0] != 0);
}

// #130: routes of another hash size are forgotten when the size changes
static void test_stale(void) {
    CHECK(!mesh_path_stale(MESH_PATH_FLOOD, 2));
    CHECK(mesh_path_stale(3, 2));            // 3 hops of 1 B
    CHECK(mesh_path_stale(0, 2));            // direct, 1 B packing
    CHECK(!mesh_path_stale(0x40 | 2, 2));    // 2 hops of 2 B
    CHECK(!mesh_path_stale(0x40, 2));        // direct packed as 2 B
    CHECK(mesh_path_stale(0x40 | 2, 1) && mesh_path_stale(0x40 | 2, 3));
    CHECK(!mesh_path_stale(0x80 | 1, 3));
}

int main(void) {
    test_stale();
    test_path_parse();
    test_hops();
    test_hex_and_route();
    test_uri();
    test_order();
    test_views();
    test_lines();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_contact_fmt_test: all passed\n");
    return 0;
}
