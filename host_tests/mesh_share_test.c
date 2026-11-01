// Host test: channel keys and share links, contact export/import text (#54)
#include "mesh_share.h"

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

// MeshCore's Public channel key (docs/qr_codes.md's example link)
static uint8_t const PUBLIC_KEY[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                       0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};

static void channel_checks(void) {
    uint8_t secret[MESH_CHANNEL_SECRET] = {0};
    memcpy(secret, PUBLIC_KEY, 16);
    CHECK(mesh_channel_key_len(secret) == 16);
    secret[31] = 1;
    CHECK(mesh_channel_key_len(secret) == 32);

    char hex[MESH_KEY_HEX_MAX], b64[MESH_KEY_B64_MAX];
    CHECK(mesh_hex_text(PUBLIC_KEY, 16, hex, sizeof(hex)) && strcmp(hex, "8b3387e9c5cdea6ac9e5edbaa115cd72") == 0);
    CHECK(mesh_base64_text(PUBLIC_KEY, 16, b64, sizeof(b64)) && strcmp(b64, "izOH6cXN6mrJ5e26oRXNcg==") == 0);
    uint8_t k32[32];
    for (int i = 0; i < 32; i++) {
        k32[i] = (uint8_t)i;
    }
    CHECK(mesh_base64_text(k32, 32, b64, sizeof(b64)) && strcmp(b64, "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=") == 0);
    CHECK(mesh_hex_text(k32, 32, hex, sizeof(hex)) && strlen(hex) == 64);
    CHECK(!mesh_hex_text(k32, 32, hex, 64) && hex[0] == '\0'); // no room for the terminator
    CHECK(!mesh_base64_text(k32, 32, b64, 44) && b64[0] == '\0');
    CHECK(mesh_base64_text(k32, 1, b64, sizeof(b64)) && strcmp(b64, "AA==") == 0);
    CHECK(mesh_base64_text(k32, 2, b64, sizeof(b64)) && strcmp(b64, "AAE=") == 0);

    char uri[MESH_CHANNEL_URI_MAX];
    CHECK(mesh_channel_uri("Public", PUBLIC_KEY, 16, uri, sizeof(uri)));
    CHECK(strcmp(uri, "meshcore://channel/add?name=Public&secret=8b3387e9c5cdea6ac9e5edbaa115cd72") == 0);
    CHECK(mesh_channel_uri("#badgetest", PUBLIC_KEY, 16, uri, sizeof(uri)));
    CHECK(strstr(uri, "meshcore://channel/add?name=%23badgetest&secret=") == uri);
    CHECK(!mesh_channel_uri("Big", k32, 32, uri, sizeof(uri)) && uri[0] == '\0'); // the format has 16-byte keys only
    CHECK(!mesh_channel_uri("", PUBLIC_KEY, 16, uri, sizeof(uri)));
    CHECK(!mesh_channel_uri("Public", PUBLIC_KEY, 16, uri, 40) && uri[0] == '\0');
    char name[32];
    memset(name, 0xC3, 31); // every byte escaped
    name[31] = '\0';
    CHECK(mesh_channel_uri(name, PUBLIC_KEY, 16, uri, sizeof(uri)) && strlen(uri) < MESH_CHANNEL_URI_MAX);
}

// An advert as MeshCore writes it: header, [transport codes], path_len, path, payload (key, time, signature, app data)
static size_t make_advert(uint8_t *out, uint8_t route, uint8_t path_len, size_t path_bytes, size_t app_len) {
    size_t at = 0;
    out[at++] = (uint8_t)(0x04 << 2 | route);
    if (route == 0x00 || route == 0x03) {
        for (int i = 0; i < 4; i++) {
            out[at++] = 0xEE;
        }
    }
    out[at++] = path_len;
    for (size_t i = 0; i < path_bytes; i++) {
        out[at++] = 0xAA;
    }
    for (size_t i = 0; i < 100 + app_len; i++) {
        out[at++] = (uint8_t)(i + 1); // the key starts 01 02 03 ...
    }
    return at;
}

static void advert_checks(void) {
    uint8_t pkt[MESH_ADVERT_MAX], norm[MESH_ADVERT_MAX];
    size_t  len = make_advert(pkt, 0x01, 0, 0, 10);
    size_t  n   = mesh_advert_normalize(pkt, len, norm, sizeof(norm));
    CHECK(n == len && norm[0] == 0x11 && norm[1] == 0 && norm[MESH_ADVERT_PUB_AT] == 1);

    // The path a flood took and transport codes are dropped; the route becomes flood
    len = make_advert(pkt, 0x00, 0x43, 6, 10); // 3 hops of 2-byte hashes
    n   = mesh_advert_normalize(pkt, len, norm, sizeof(norm));
    CHECK(n == 2 + 110 && norm[0] == 0x11 && norm[1] == 0 && norm[2] == 1 && norm[2 + 109] == 110);
    // In place
    n = mesh_advert_normalize(pkt, len, pkt, sizeof(pkt));
    CHECK(n == 112 && pkt[0] == 0x11 && pkt[2] == 1);

    // Not an advert, too short, bad path, payload over MeshCore's 184
    len    = make_advert(pkt, 0x01, 0, 0, 10);
    pkt[0] = (uint8_t)(0x02 << 2 | 0x01);
    CHECK(mesh_advert_normalize(pkt, len, norm, sizeof(norm)) == 0);
    len = make_advert(pkt, 0x01, 0, 0, 0);
    CHECK(mesh_advert_normalize(pkt, len - 1, norm, sizeof(norm)) == 0);
    len = make_advert(pkt, 0x01, 0xC1, 4, 10); // 4-byte hashes: reserved
    CHECK(mesh_advert_normalize(pkt, len, norm, sizeof(norm)) == 0);
    len = make_advert(pkt, 0x01, 0x05, 60, 10); // path says 5 bytes, packet ends early is fine; 60 bytes of 0xAA follow
    CHECK(mesh_advert_normalize(pkt, 3, norm, sizeof(norm)) == 0);
    len = make_advert(pkt, 0x01, 0, 0, 85); // 185-byte payload
    CHECK(mesh_advert_normalize(pkt, len, norm, sizeof(norm)) == 0);
    CHECK(mesh_advert_normalize(pkt, 1, norm, sizeof(norm)) == 0);

    // Round trip through the link, also with the longest advert
    char uri[MESH_ADVERT_URI_MAX];
    len = make_advert(pkt, 0x01, 0x02, 2, 84); // 184-byte payload
    CHECK(mesh_advert_uri(pkt, len, uri, sizeof(uri)) && strlen(uri) == MESH_ADVERT_URI_MAX - 1);
    CHECK(strncmp(uri, "meshcore://1100010203", 21) == 0);
    char const *why = "x";
    n               = mesh_advert_uri_parse(uri, norm, sizeof(norm), &why);
    CHECK(n == 186 && !why && norm[0] == 0x11 && norm[2] == 1 && norm[185] == 184);
    CHECK(!mesh_advert_uri(pkt, len, uri, sizeof(uri) - 1) && uri[0] == '\0');

    // The parse: spaces, case, and what it refuses
    len = make_advert(pkt, 0x01, 0, 0, 3);
    CHECK(mesh_advert_uri(pkt, len, uri, sizeof(uri)));
    char text[MESH_ADVERT_URI_MAX + 8];
    snprintf(text, sizeof(text), "  MESHCORE://%s ", uri + 11);
    for (char *p = text + 13; *p; p++) {
        *p = (char)(*p >= 'a' && *p <= 'f' ? *p - 32 : *p);
    }
    CHECK(mesh_advert_uri_parse(text, norm, sizeof(norm), &why) == len);
    CHECK(mesh_advert_uri_parse("meshcore://11", norm, sizeof(norm), &why) == 0 && why);
    CHECK(mesh_advert_uri_parse("meshcore://110", norm, sizeof(norm), &why) == 0 && why);
    CHECK(mesh_advert_uri_parse("meshcore://zz", norm, sizeof(norm), &why) == 0 && why);
    CHECK(mesh_advert_uri_parse("http://11", norm, sizeof(norm), &why) == 0 && why);
    CHECK(mesh_advert_uri_parse("meshcore://contact/add?name=a&public_key=00&type=1", norm, sizeof(norm), &why) == 0 &&
          strstr(why, "app link"));
    CHECK(mesh_advert_uri_parse("", norm, sizeof(norm), NULL) == 0);
}

static void import_checks(void) {
    mesh_import_t st = {0};
    CHECK(mesh_import_add(&st, "abcd") != NULL && st.len == 0); // nothing started
    CHECK(mesh_import_add(&st, " meshcore://11 00") == NULL && strcmp(st.text, "meshcore://1100") == 0);
    CHECK(mesh_import_add(&st, "0102 ") == NULL && strcmp(st.text, "meshcore://11000102") == 0);
    CHECK(mesh_import_add(&st, "meshcore://22") == NULL && strcmp(st.text, "meshcore://22") == 0); // a new one
    char part[100];
    memset(part, 'a', 99);
    part[99]       = '\0';
    char const *why = NULL;
    for (int i = 0; i < 4 && !why; i++) {
        why = mesh_import_add(&st, part);
    }
    CHECK(why && st.len == 0 && st.text[0] == '\0'); // too long: dropped
    // Parts of a real link put back together parse as the whole
    uint8_t pkt[MESH_ADVERT_MAX], norm[MESH_ADVERT_MAX];
    size_t  len = make_advert(pkt, 0x01, 0, 0, 40);
    char    uri[MESH_ADVERT_URI_MAX];
    CHECK(mesh_advert_uri(pkt, len, uri, sizeof(uri)));
    size_t total = strlen(uri);
    for (size_t at = 0; at < total; at += 79) {
        char chunk[80];
        snprintf(chunk, sizeof(chunk), "%.79s", uri + at);
        CHECK(mesh_import_add(&st, chunk) == NULL);
    }
    CHECK(strcmp(st.text, uri) == 0 && mesh_advert_uri_parse(st.text, norm, sizeof(norm), NULL) == len);
}

int main(void) {
    channel_checks();
    advert_checks();
    import_checks();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_share_test: all checks passed\n");
    return 0;
}
