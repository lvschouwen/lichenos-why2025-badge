// Host test: what "fw sd cat" may read and how it prints it (#378): the roots, traversal and dot tricks, the deny list
// (the update token, the MeshCore identity and keys, key files, NVS backups), long names, the arguments, the lines
#include "sd_cat_fmt.h"
#include "sd_path_policy.h"

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

#define V(path) sd_path_check(path)

static void normal_files(void) {
    CHECK(V("/sd/lichen/settings.txt") == SD_PATH_OK); // the settings backup (#169): the bench reads it back
    CHECK(V("/sd/weather.csv") == SD_PATH_OK);
    CHECK(V("/sd/lichen/apps/weather/log.csv") == SD_PATH_OK);
    CHECK(V("/storage/lora.toml") == SD_PATH_OK);
    CHECK(V("/sd/.hidden") == SD_PATH_OK); // a leading dot is a name, not a trick
    CHECK(V("/storage/meshcore/contacts3") == SD_PATH_OK); // public keys only
}

static void roots(void) {
    CHECK(V("/sd") == SD_PATH_ROOT);
    CHECK(V("/storage") == SD_PATH_ROOT);
    CHECK(V("/SD/a.txt") == SD_PATH_ROOT); // the VFS matches mount points exactly
    CHECK(V("/sdx/a.txt") == SD_PATH_ROOT);
    CHECK(V("/dev/uart/0") == SD_PATH_ROOT);
    CHECK(V("/spiflash/a") == SD_PATH_ROOT);
    CHECK(V("sd/a.txt") == SD_PATH_BAD);
    CHECK(V("") == SD_PATH_BAD);
    CHECK(V(NULL) == SD_PATH_BAD);
}

static void traversal(void) {
    CHECK(V("/sd/../storage/ota_token.txt") == SD_PATH_BAD);
    CHECK(V("/sd/../storage/lora.toml") == SD_PATH_BAD);
    CHECK(V("/sd/a/../../storage/x") == SD_PATH_BAD);
    CHECK(V("/sd/./a.txt") == SD_PATH_BAD);
    CHECK(V("/sd//a.txt") == SD_PATH_BAD);
    CHECK(V("//sd/a.txt") == SD_PATH_BAD);
    CHECK(V("/sd/a.txt/") == SD_PATH_BAD);
    CHECK(V("/sd/..") == SD_PATH_BAD);
    CHECK(V("/sd/...") == SD_PATH_BAD);
    CHECK(V("/sd/a\\..\\b") == SD_PATH_BAD); // FatFs takes '\\' as a separator
    CHECK(V("/sd/0:a.txt") == SD_PATH_BAD);  // a drive prefix
    CHECK(V("/sd/a\nb") == SD_PATH_BAD);
    CHECK(V("/sd/a*.txt") == SD_PATH_BAD);
    CHECK(V("/sd/ a.txt") == SD_PATH_BAD);  // an invisible leading space
    CHECK(V("/sd/a.txt ") == SD_PATH_BAD);  // FAT drops trailing spaces and dots
    CHECK(V("/sd/a.txt.") == SD_PATH_BAD);
}

static void token(void) {
    CHECK(V("/storage/ota_token.txt") == SD_PATH_DENIED);
    CHECK(V("/storage/OTA_TOKEN.TXT") == SD_PATH_DENIED);
    CHECK(V("/sd/ota_token.txt") == SD_PATH_DENIED);    // a copy on the card
    CHECK(V("/sd/ota_token.bak") == SD_PATH_DENIED);
    CHECK(V("/sd/ota_token/x.txt") == SD_PATH_DENIED);  // nothing below a directory named so
    CHECK(V("/foo/ota_token.txt") == SD_PATH_DENIED);    // deny wins over the roots
    // dot tricks that FAT would open as the token are not paths at all
    CHECK(V("/storage/ota_token.txt.") != SD_PATH_OK);
    CHECK(V("/sd/OTA_TO~1.TXT") == SD_PATH_BAD); // an 8.3 alias may be any long name
}

static void identity_and_keys(void) {
    CHECK(V("/storage/meshcore/identity/_main.id") == SD_PATH_DENIED);
    CHECK(V("/storage/meshcore/identity/_main.id.new") == SD_PATH_DENIED);
    CHECK(V("/storage/meshcore/identity/anything") == SD_PATH_DENIED);
    CHECK(V("/storage/MeshCore/Identity/x") == SD_PATH_DENIED);
    CHECK(V("/storage/meshcore/_main.id") == SD_PATH_DENIED); // the older place (DataStore.cpp)
    CHECK(V("/sd/backup/_main.id") == SD_PATH_DENIED);        // a copy anywhere
    CHECK(V("/storage/meshcore/channels2") == SD_PATH_DENIED); // the channel keys
    CHECK(V("/storage/meshcore/new_prefs") == SD_PATH_DENIED); // may hold a Wi-Fi password
    CHECK(V("/storage/meshcore/prefs.json") == SD_PATH_DENIED);
    CHECK(V("/sd/node.key") == SD_PATH_DENIED);
    CHECK(V("/sd/cert.PEM") == SD_PATH_DENIED);
    CHECK(V("/sd/nvs.bin") == SD_PATH_DENIED);
    CHECK(V("/sd/nvs-plain.bin") == SD_PATH_DENIED); // docs/nvs-encryption.md's backup
    CHECK(V("/sd/NVS_backup.csv") == SD_PATH_DENIED);
    CHECK(V("/sd/keys.txt") == SD_PATH_OK); // only the named patterns
    // MeshCore's secret names at any depth, as a directory too, whatever the case: copies on the card
    char const *const copies[] = {"/sd/identity/x.bin",      "/sd/backup/IDENTITY/x",     "/sd/a/b/channels2",
                                  "/sd/meshcore/new_prefs",  "/sd/old/Prefs.JSON",       "/sd/node_prefs",
                                  "/sd/x/com_prefs",         "/sd/s_contacts",            "/storage/meshcore/s_contacts",
                                  "/storage/meshcore/node_prefs", "/sd/channels2/readme.txt"};
    for (size_t i = 0; i < sizeof(copies) / sizeof(copies[0]); i++) {
        CHECK(V(copies[i]) == SD_PATH_DENIED);
    }
    CHECK(V("/sd/identity.txt") == SD_PATH_OK); // the name, not a prefix
    CHECK(V("/sd/channels") == SD_PATH_OK);
}

// FatFs folds the case of non-ASCII names with its own tables: no byte >= 0x80 is let through (#378 review)
static void non_ascii(void) {
    CHECK(V("/sd/caf\xc3\xa9.txt") == SD_PATH_BAD);
    CHECK(V("/storage/ota_to\xe2\x80\x8bken.txt") == SD_PATH_BAD); // a zero-width space
    CHECK(V("/sd/\xc4\xb0" "dentity/x") == SD_PATH_BAD);               // a dotted capital I
    CHECK(V("/sd/a\x7f") == SD_PATH_BAD);
    CHECK(V("/sd/\x80") == SD_PATH_BAD);
}

static void long_names(void) {
    char p[SD_PATH_MAX + 32];
    strcpy(p, "/sd/");
    memset(p + 4, 'a', SD_PATH_MAX - 4);
    p[SD_PATH_MAX] = '\0';
    CHECK(V(p) == SD_PATH_OK); // exactly the limit
    p[SD_PATH_MAX]     = 'a';
    p[SD_PATH_MAX + 1] = '\0';
    CHECK(V(p) == SD_PATH_TOO_LONG);
    // a long path with the token hidden at its end: refused either way
    memset(p + 4, 'a', SD_PATH_MAX);
    strcpy(p + SD_PATH_MAX + 4 - 14, "/ota_token.txt");
    CHECK(V(p) != SD_PATH_OK);
    // the argument parser keeps a path too long to check as too long, never cuts it to fit
    char args[SD_PATH_MAX + 40];
    memset(args, 'b', sizeof(args));
    memcpy(args, "/sd/", 4);
    args[sizeof(args) - 1] = '\0';
    sd_cat_args_t a;
    CHECK(sd_cat_parse(args, &a) && V(a.path) == SD_PATH_TOO_LONG);
}

static void parse(void) {
    sd_cat_args_t a;
    CHECK(sd_cat_parse(" /sd/a.txt", &a) && !strcmp(a.path, "/sd/a.txt") && a.offset == 0 && a.len == SD_CAT_DEFAULT &&
          !a.hex);
    CHECK(sd_cat_parse("-x /sd/a.txt 512 16", &a) && a.hex && a.offset == 512 && a.len == 16);
    CHECK(sd_cat_parse("/sd/a.txt 7 -x", &a) && a.hex && a.offset == 7 && a.len == SD_CAT_DEFAULT);
    CHECK(sd_cat_parse("/sd/a.txt 0 1024", &a) && a.len == SD_CAT_MAX);
    CHECK(!sd_cat_parse("/sd/a.txt 0 1025", &a)); // at most 1024 bytes per call
    CHECK(!sd_cat_parse("/sd/a.txt 0 0", &a));
    CHECK(!sd_cat_parse("/sd/a.txt -1", &a));
    CHECK(!sd_cat_parse("/sd/a.txt 4294967296", &a));
    CHECK(!sd_cat_parse("/sd/a.txt 1 2 3", &a));
    CHECK(!sd_cat_parse("", &a));
    CHECK(!sd_cat_parse("-x", &a));
}

static void lines(void) {
    char          out[SD_CAT_LINE_CAP];
    uint8_t const hex[] = {'A', 'B', 0x00, 0xff, '\n'};
    size_t        n     = sd_cat_hex_line(0x200, hex, sizeof(hex), out, sizeof(out));
    CHECK(n == strlen(out));
    CHECK(!strncmp(out, "00000200  41 42 00 ff 0a ", 25));
    CHECK(strstr(out, "|AB...|") != NULL);
    uint8_t full[SD_CAT_HEX_ROW];
    memset(full, 0x80, sizeof(full));
    CHECK(sd_cat_hex_line(0xffffffffu, full, sizeof(full), out, sizeof(out)) < sizeof(out));

    uint8_t const text[] = "a\tb\r\nsecond\x1b[2J\n";
    size_t        used;
    sd_cat_text_line(text, sizeof(text) - 1, &used, out, sizeof(out));
    CHECK(!strcmp(out, "a b") && used == 5);
    sd_cat_text_line(text + used, sizeof(text) - 1 - used, &used, out, sizeof(out));
    CHECK(!strcmp(out, "second.[2J")); // an escape byte can't drive the terminal
    uint8_t long_line[200];
    memset(long_line, 'z', sizeof(long_line));
    n = sd_cat_text_line(long_line, sizeof(long_line), &used, out, sizeof(out));
    CHECK(n == SD_CAT_TEXT_COL && used == SD_CAT_TEXT_COL);
    uint8_t const crs[] = "\r\r\r";
    sd_cat_text_line(crs, 3, &used, out, sizeof(out));
    CHECK(used == 3 && out[0] == '\0');
}

int main(void) {
    normal_files();
    roots();
    traversal();
    token();
    identity_and_keys();
    non_ascii();
    long_names();
    parse();
    lines();
    if (failures) {
        printf("sd_path_policy_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("sd_path_policy_test: ok\n");
    return 0;
}
