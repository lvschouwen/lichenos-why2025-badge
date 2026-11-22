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
    return sd_elf_parse(args, NULL, path, cap, &why);
}

static void accepted(void) {
    char path[SD_PATH_MAX + 2];
    CHECK(parse("run /sd/lichen/apps/hello/app.elf", path, sizeof(path)) == SD_ELF_ARGS_OK);
    CHECK(strcmp(path, "/sd/lichen/apps/hello/app.elf") == 0);
    CHECK(parse("run   /sd/x.ELF", path, sizeof(path)) == SD_ELF_ARGS_OK); // extra spaces, FAT is case-blind
    CHECK(strcmp(path, "/sd/x.ELF") == 0);
    CHECK(parse("run /storage/hello.elf", path, sizeof(path)) == SD_ELF_ARGS_OK); // the bench pushes here over Wi-Fi
    CHECK(strcmp(path, "/storage/hello.elf") == 0);
    CHECK(sd_elf_on_sd("/sd/a.elf") && !sd_elf_on_sd("/storage/a.elf"));
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
    CHECK(parse("run /storage/../sd/a.elf", path, sizeof(path)) == SD_ELF_ARGS_REFUSED);
    CHECK(parse("run /storagex/a.elf", path, sizeof(path)) == SD_ELF_ARGS_REFUSED);
    CHECK(parse("run /storage", path, sizeof(path)) == SD_ELF_ARGS_REFUSED);
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

// The on-air hold (#289, #420): files the update upload can write must not run while it is on
static void held(void) {
    char              path[SD_PATH_MAX + 2];
    sd_path_verdict_t why;
    CHECK(sd_elf_parse("run /sd/a.elf", "on air", path, sizeof(path), &why) == SD_ELF_ARGS_HELD);
    CHECK(sd_elf_parse("run /storage/a.elf", "on air", path, sizeof(path), &why) == SD_ELF_ARGS_HELD);
    CHECK(sd_elf_parse("run /sd/a.bin", "on air", path, sizeof(path), &why) == SD_ELF_ARGS_HELD); // any path
    CHECK(sd_elf_parse("run /sd/a.elf", NULL, path, sizeof(path), &why) == SD_ELF_ARGS_OK);
    CHECK(sd_elf_parse("run /storage/a.elf", NULL, path, sizeof(path), &why) == SD_ELF_ARGS_OK);
    CHECK(sd_elf_parse("run", "on air", path, sizeof(path), &why) == SD_ELF_ARGS_USAGE); // usage stays usage
}

// The policy's length limit with the buffer the console uses (SD_PATH_MAX + 2): the longest path passes, one byte more is
// refused by sd_path_check and says so
static void policy_length(void) {
    char              path[SD_PATH_MAX + 2];
    char              args[SD_PATH_MAX + 64];
    sd_path_verdict_t why;
    size_t            n = SD_PATH_MAX; // the whole path, "/sd/" and ".elf" included
    char              name[SD_PATH_MAX + 8];
    memset(name, 'a', sizeof(name));
    memcpy(name, "/sd/", 4);
    memcpy(name + n - 4, ".elf", 4);
    name[n] = '\0';
    snprintf(args, sizeof(args), "run %s", name);
    CHECK(sd_elf_parse(args, NULL, path, sizeof(path), &why) == SD_ELF_ARGS_OK);
    CHECK(strlen(path) == SD_PATH_MAX);
    memcpy(name + n - 4, "a.elf", 6); // one byte longer
    snprintf(args, sizeof(args), "run %s", name);
    CHECK(sd_elf_parse(args, NULL, path, sizeof(path), &why) == SD_ELF_ARGS_REFUSED);
    CHECK(why == SD_PATH_TOO_LONG);
}

static void small_buffer(void) {
    char              path[8];
    sd_path_verdict_t why;
    CHECK(sd_elf_parse("run /sd/a.elf", NULL, path, sizeof(path), &why) == SD_ELF_ARGS_REFUSED);
    CHECK(why == SD_PATH_TOO_LONG);
}

int main(void) {
    accepted();
    usage();
    refused();
    not_elf();
    held();
    policy_length();
    small_buffer();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("sd_elf_args_test: ok\n");
    return 0;
}
