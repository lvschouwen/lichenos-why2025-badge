// Host test: every sdapps/<id>/manifest parses with the real lichen_manifest_parse and its directory name as the id (#552).
// The loader refuses an app whose manifest does not ("bad manifest (bad line)" for a comment over the line limit), and
// nothing at build time parsed them. The manifests are the arguments (CMake globs sdapps/*/manifest), so a new app is covered.
#include "lichen_manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_file(char const *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL %s: cannot open\n", path);
        return 1;
    }
    static char text[4096];
    size_t      len = fread(text, 1, sizeof(text), f);
    int         over = fgetc(f) != EOF;
    fclose(f);
    if (over) {
        printf("FAIL %s: longer than the %zu bytes this test reads\n", path, sizeof(text));
        return 1;
    }
    // the directory name is the id: .../sdapps/<id>/manifest
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        printf("FAIL %s: no directory in the path\n", path);
        return 1;
    }
    *slash = '\0';
    char const       *id = strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir;
    lichen_manifest_t m;
    unsigned          line = 0;
    lichen_manifest_err_t err = lichen_manifest_parse(text, len, id, &m, &line);
    if (err != LICHEN_MANIFEST_OK) {
        printf("FAIL %s:%u: manifest error %d (1 arg, 2 line, 3 key, 4 duplicate, 5 id, 6 value, 7 version, 8 missing)\n", path,
               line, (int)err);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("FAIL: no sdapps/*/manifest was given (the CMake glob found none)\n");
        return 1;
    }
    int failures = 0;
    for (int i = 1; i < argc; i++) {
        failures += parse_file(argv[i]);
    }
    if (!failures) {
        printf("sdapp_manifests_test: %d manifest(s) ok\n", argc - 1);
    }
    return failures ? 1 : 0;
}
