// Host test: the Arduino-style file system shim that MeshCore's DataStore uses, over a plain directory (#65)
#include "SPIFFS.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void fake_drive(size_t *total, size_t *used) {
    *total = 1000;
    *used  = 250;
}

int main(void) {
    char root[] = "/tmp/fs_shim_test.XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    CHECK(!SPIFFS.begin(true)); // no root yet: the OS hands it over first (#84 T6)
    fs_shim_set_root(root);
    CHECK(SPIFFS.begin(true));

    // The app stays below its root: no "..", ".", or "//" anywhere in a path
    CHECK(!SPIFFS.exists("/../x") && !SPIFFS.exists("/a/../b") && !SPIFFS.exists("a//b") && !SPIFFS.exists("/./a"));
    File escape = SPIFFS.open("/../escape", "w", true);
    CHECK(!escape);
    char outside[300];
    snprintf(outside, sizeof(outside), "%s/../escape", root);
    CHECK(access(outside, F_OK) != 0);
    CHECK(!SPIFFS.mkdir("/a/../up") && !SPIFFS.remove("/.."));

    // Which roots format() may wipe
    CHECK(fs_shim_format_allowed("/storage/meshcore") && fs_shim_format_allowed(root));
    CHECK(!fs_shim_format_allowed("/storage") && !fs_shim_format_allowed("/") && !fs_shim_format_allowed(""));
    CHECK(!fs_shim_format_allowed(NULL) && !fs_shim_format_allowed("/storage/") && !fs_shim_format_allowed("storage/x"));
    CHECK(!fs_shim_format_allowed("/storage/c6") && !fs_shim_format_allowed("/storage/meshcore/.."));

    // Missing files: not there, and opening one for reading gives a false File
    CHECK(!SPIFFS.exists("/a/b"));
    File missing = SPIFFS.open("/a/b", "r");
    CHECK(!missing);

    // Write 300 bytes into a directory that doesn't exist yet, as DataStore's "w", create = true
    uint8_t data[300];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)i;
    }
    File out = SPIFFS.open("/a/b", "w", true);
    CHECK(out);
    CHECK(out.write(data, sizeof(data)) == sizeof(data));
    out.close();
    CHECK(SPIFFS.exists("/a/b"));

    // Read back, seek, single bytes, size
    File in = SPIFFS.open("/a/b", "r");
    uint8_t back[300] = {0};
    CHECK(in && in.size() == 300);
    CHECK(in.read(back, sizeof(back)) == sizeof(back) && memcmp(back, data, sizeof(data)) == 0);
    CHECK(in.read() == -1); // at the end
    CHECK(in.seek(10) && in.read() == 10);
    in.close();

    // "w" truncates
    out = SPIFFS.open("/a/b", "w");
    CHECK(out.write(data, 5) == 5);
    out.close();
    in = SPIFFS.open("/a/b");
    CHECK(in.size() == 5 && in.read(back, sizeof(back)) == 5); // a short file reads short, no error
    in.close();

    // Remove; mkdir of a nested directory
    CHECK(SPIFFS.remove("/a/b") && !SPIFFS.exists("/a/b"));
    CHECK(!SPIFFS.remove("/a/b"));
    CHECK(SPIFFS.mkdir("/x/y") && SPIFFS.exists("/x/y"));

    // Sizes come from the platform hook; the host's numbers just have to make sense, and the OS hook is used when set
    CHECK(SPIFFS.totalBytes() > 0 && SPIFFS.usedBytes() <= SPIFFS.totalBytes());
    fs_shim_set_drive_bytes(fake_drive);
    CHECK(SPIFFS.totalBytes() == 1000 && SPIFFS.usedBytes() == 250);
    fs_shim_set_drive_bytes(NULL);

    // format() empties the root
    out = SPIFFS.open("/c", "w");
    out.close();
    CHECK(SPIFFS.format() && !SPIFFS.exists("/c") && !SPIFFS.exists("/x/y"));
    // ... but never a root the rule refuses: the file survives
    out = SPIFFS.open("/d", "w");
    out.close();
    fs_shim_set_root("/");
    CHECK(!SPIFFS.format());
    fs_shim_set_root(root);
    CHECK(SPIFFS.exists("/d") && SPIFFS.remove("/d"));
    rmdir(root);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("fs_shim_test: all checks passed\n");
    return 0;
}
