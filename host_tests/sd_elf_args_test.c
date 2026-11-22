// Host test: the argument rules of "fw elf run" (#517): only .elf files below /sd, no traversal, no internal storage
#include "sd_elf_args.h"

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

static sd_elf_args_t parse(char const *args, char *path, size_t cap) {
    sd_path_verdict_t why;
    return sd_elf_parse(args, path, cap, &why);
}

static void accepted(void) {
    char path[SD_PATH_MAX + 2];
    CHECK(parse("run /sd/lichen/apps/hello/app.elf", path, sizeof(path)) == SD_ELF_ARGS_OK);
    CHECK(strcmp(path, "/sd/lichen/apps/hello/app.elf") == 0);
    CHECK(parse("run   /sd/x.ELF", path, sizeof(path)) == SD_ELF_ARGS_OK); // extra spaces, FAT is case-blind
    CHECK(strcmp(path, "/sd/x.ELF") == 0);
}

static void usage(void) {
    char path[SD_PATH_MAX + 2];
    CHECK(parse("", path, sizeof(path)) == SD_ELF_ARGS_USAGE);
    CHECK(parse("run", path, sizeof(path)) == SD_ELF_ARGS_USAGE);
    CHECK(parse("run ", path, sizeof(path)) == SD_ELF_ARGS_USAGE);
    CHECK(parse("load /sd/a.elf", path, sizeof(path)) == SD_ELF_ARGS_USAGE);
    CHECK(parse(NULL, path, sizeof(path)) == SD_ELF_ARGS_USAGE);
}

static void refused(void) {
    char path[SD_PATH_MAX + 2];
    CHECK(parse("run /storage/a.elf", path, sizeof(path)) == SD_ELF_ARGS_REFUSED); // internal storage never runs code
    CHECK(parse("run /sdx/a.elf", path, sizeof(path)) == SD_ELF_ARGS_REFUSED);
    CHECK(parse("run /sd/../storage/a.elf", path, sizeof(path)) == SD_ELF_ARGS_REFUSED);
    CHECK(parse("run a.elf", path, sizeof(path)) == SD_ELF_ARGS_REFUSED);
}

static void not_elf(void) {
    char path[SD_PATH_MAX + 2];
    CHECK(parse("run /sd/a.bin", path, sizeof(path)) == SD_ELF_ARGS_NOT_ELF);
    CHECK(parse("run /sd/.elf", path, sizeof(path)) == SD_ELF_ARGS_NOT_ELF); // a name, not an extension
    CHECK(parse("run /sd/elf", path, sizeof(path)) == SD_ELF_ARGS_NOT_ELF);
}

static void small_buffer(void) {
    char              path[8];
    sd_path_verdict_t why;
    CHECK(sd_elf_parse("run /sd/a.elf", path, sizeof(path), &why) == SD_ELF_ARGS_REFUSED);
    CHECK(why == SD_PATH_TOO_LONG);
}

int main(void) {
    accepted();
    usage();
    refused();
    not_elf();
    small_buffer();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("sd_elf_args_test: ok\n");
    return 0;
}
