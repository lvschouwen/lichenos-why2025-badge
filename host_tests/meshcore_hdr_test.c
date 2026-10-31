// Host tests for the MeshCore header summary in the LoRa log
#include "meshcore_hdr.h"

#include <stdio.h>

#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// header, path_len, path, then an advert payload: 100 bytes of key/timestamp/signature, flags, name
static size_t make_advert(uint8_t *pkt, uint8_t flags, char const *name, size_t name_len) {
    size_t n = 0;
    pkt[n++] = 0x11; // flood, advert, version 0
    pkt[n++] = 0x02; // 2 hashes of 1 byte
    pkt[n++] = 0xAA;
    pkt[n++] = 0xBB;
    memset(pkt + n, 0x5A, 100);
    n        += 100;
    pkt[n++]  = flags;
    memcpy(pkt + n, name, name_len);
    return n + name_len;
}

static void flood_advert_with_name(void) {
    uint8_t  pkt[255];
    size_t   n = make_advert(pkt, 0x82, "Rep1", 4);
    mc_hdr_t h;
    CHECK(mc_hdr_parse(pkt, n, &h));
    CHECK(h.route == 1);
    CHECK(h.type == 4);
    CHECK(h.ver == 0);
    CHECK(h.path_hash_count == 2);
    CHECK(h.path_hash_size == 1);
    CHECK(h.is_advert);
    CHECK(h.adv_type == 2);
    CHECK(strcmp(h.name, "Rep1") == 0);

    char line[128];
    CHECK(mc_hdr_format(&h, line, sizeof(line)) == strlen(line));
    CHECK(strstr(line, "flood advert"));
    CHECK(strstr(line, "path 2x1"));
    CHECK(strstr(line, "'Rep1' (repeater)"));
    // The last repeater appended its hash last
    CHECK(strstr(line, "path 2x1 via bb "));

    // Location and feature fields come before the name
    uint8_t with_loc[255];
    n = make_advert(with_loc, 0x91, "12345678Chat", 12);
    CHECK(mc_hdr_parse(with_loc, n, &h));
    CHECK(h.adv_type == 1);
    CHECK(strcmp(h.name, "Chat") == 0);
}

static void transport_direct_txt(void) {
    uint8_t pkt[32] = {0x0B, 1, 2, 3, 4, 0x41, 0xC1, 0xC2};
    size_t  n       = 8 + 10;
    memset(pkt + 8, 0x77, 10);
    mc_hdr_t h;
    CHECK(mc_hdr_parse(pkt, n, &h));
    CHECK(h.route == 3);
    CHECK(h.type == 2);
    CHECK(h.path_hash_size == 2);
    CHECK(h.path_hash_count == 1);
    CHECK(h.payload_len == 10);
    CHECK(!h.is_advert);

    char line[128];
    mc_hdr_format(&h, line, sizeof(line));
    CHECK(strncmp(line, "tdirect txt", 11) == 0);
    CHECK(strstr(line, "path 1x2"));
    // A direct packet's path is the route ahead, not who relayed it
    CHECK(!strstr(line, "via"));
}

static void last_hop_of_flood(void) {
    // Transport flood, request, 2 hashes of 2 bytes
    uint8_t  pkt[16] = {0x00, 1, 2, 3, 4, 0x42, 0x11, 0x22, 0x3F, 0xA1, 0x55, 0x66};
    mc_hdr_t h;
    char     line[128];
    CHECK(mc_hdr_parse(pkt, 12, &h));
    mc_hdr_format(&h, line, sizeof(line));
    CHECK(strcmp(line, "tflood req v0 path 2x2 via 3fa1") == 0);

    // 3-byte hashes
    uint8_t three[] = {0x05, 0x81, 0xAB, 0xCD, 0xEF, 0x01};
    CHECK(mc_hdr_parse(three, sizeof(three), &h));
    mc_hdr_format(&h, line, sizeof(line));
    CHECK(strstr(line, "path 1x3 via abcdef"));

    // No repeater yet: heard straight from the sender
    uint8_t zero[] = {0x01, 0x00, 0x01};
    CHECK(mc_hdr_parse(zero, sizeof(zero), &h));
    mc_hdr_format(&h, line, sizeof(line));
    CHECK(strcmp(line, "flood req v0 path 0x1") == 0);
}

static void advert_name_sanitised(void) {
    char name[45] = "A\x01\xC3\xA9";
    name[4]       = 'B';
    memset(name + 5, 'x', 40);
    uint8_t  pkt[255];
    size_t   n = make_advert(pkt, 0x81, name, 45);
    mc_hdr_t h;
    CHECK(mc_hdr_parse(pkt, n, &h));
    CHECK(strncmp(h.name, "A???B", 5) == 0);
    CHECK(strlen(h.name) == 32);

    char line[128];
    mc_hdr_format(&h, line, sizeof(line));
    for (char const *c = line; *c; c++) {
        CHECK(*c >= 0x20 && *c <= 0x7E);
    }
}

static void garbage_is_undecodable(void) {
    mc_hdr_t h;
    uint8_t  one[] = {0x11};
    CHECK(!mc_hdr_parse(one, 0, &h));
    CHECK(!mc_hdr_parse(one, 1, &h));

    uint8_t size4[] = {0x11, 0xC1, 0x00, 0x00, 0x00, 0x00, 0x99};
    CHECK(!mc_hdr_parse(size4, sizeof(size4), &h));

    uint8_t long_path[] = {0x11, 0x3F, 1, 2, 3, 4};
    CHECK(!mc_hdr_parse(long_path, sizeof(long_path), &h));

    uint8_t no_payload[] = {0x11, 0x02, 0xAA, 0xBB};
    CHECK(!mc_hdr_parse(no_payload, sizeof(no_payload), &h));

    // Transport codes cut short
    uint8_t short_transport[] = {0x00, 1, 2};
    CHECK(!mc_hdr_parse(short_transport, sizeof(short_transport), &h));

    uint8_t big[2 + 185] = {0x15, 0x00};
    CHECK(!mc_hdr_parse(big, sizeof(big), &h));
    CHECK(mc_hdr_parse(big, sizeof(big) - 1, &h));
}

static void truncated_advert(void) {
    uint8_t pkt[2 + 50] = {0x11, 0x00};
    memset(pkt + 2, 0x5A, 50);
    mc_hdr_t h;
    CHECK(mc_hdr_parse(pkt, sizeof(pkt), &h));
    CHECK(h.is_advert);
    CHECK(h.name[0] == '\0');

    // Flags say location follows but the packet ends
    uint8_t cut[255];
    size_t  n = make_advert(cut, 0x92, "", 0);
    CHECK(mc_hdr_parse(cut, n, &h));
    CHECK(h.name[0] == '\0');
}

static void format_truncates(void) {
    uint8_t  pkt[255];
    size_t   n = make_advert(pkt, 0x82, "Rep1", 4);
    mc_hdr_t h;
    mc_hdr_parse(pkt, n, &h);
    char line[10];
    memset(line, '#', sizeof(line));
    size_t w = mc_hdr_format(&h, line, sizeof(line));
    CHECK(w <= 9);
    CHECK(line[9] == '\0' || line[w] == '\0');
}

int main(void) {
    flood_advert_with_name();
    transport_direct_txt();
    last_hop_of_flood();
    advert_name_sanitised();
    garbage_is_undecodable();
    truncated_advert();
    format_truncates();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("meshcore_hdr_test: all passed\n");
    return 0;
}
