#include "sd_elf_args.h"

#include <ctype.h>
#include <string.h>

static bool has_elf_suffix(char const *p, size_t n) {
    return n > 5 && p[n - 5] != '/' && p[n - 4] == '.' && tolower((unsigned char)p[n - 3]) == 'e' &&
           tolower((unsigned char)p[n - 2]) == 'l' && tolower((unsigned char)p[n - 1]) == 'f';
}

sd_elf_args_t sd_elf_parse(char const *args, char *path, size_t cap, sd_path_verdict_t *why) {
    *why = SD_PATH_OK;
    if (!args || strncmp(args, "run ", 4) != 0) {
        return SD_ELF_ARGS_USAGE;
    }
    char const *p = args + 4;
    while (*p == ' ') {
        p++;
    }
    size_t n = strlen(p);
    if (n == 0) {
        return SD_ELF_ARGS_USAGE;
    }
    *why = sd_path_check(p);
    // the volume check: /sd/ itself, not /storage (sd_path_check allows both) and not a sibling such as /sdx
    if (*why != SD_PATH_OK || strncmp(p, "/sd/", 4) != 0) {
        return SD_ELF_ARGS_REFUSED;
    }
    if (!has_elf_suffix(p, n)) {
        return SD_ELF_ARGS_NOT_ELF;
    }
    if (n + 1 > cap) {
        *why = SD_PATH_TOO_LONG;
        return SD_ELF_ARGS_REFUSED;
    }
    memcpy(path, p, n + 1);
    return SD_ELF_ARGS_OK;
}

char const *sd_elf_args_text(sd_elf_args_t v, sd_path_verdict_t why) {
    switch (v) {
    case SD_ELF_ARGS_OK:
        return "ok";
    case SD_ELF_ARGS_USAGE:
        return "usage: elf run <path under /sd, ending in .elf>";
    case SD_ELF_ARGS_REFUSED:
        return why == SD_PATH_OK ? "only files below /sd" : sd_path_verdict_text(why);
    case SD_ELF_ARGS_NOT_ELF:
        return "not an .elf file";
    }
    return "?";
}
