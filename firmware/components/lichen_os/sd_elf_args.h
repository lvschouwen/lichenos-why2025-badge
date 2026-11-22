#pragma once
// "fw elf run <path>" (#517): the argument rules, without ESP headers. Host-tested in host_tests/sd_elf_args_test.c.
// sd_apps.c is the glue. A path is taken exactly as typed (sd_path_check decides, never a rewrite): only files below
// /sd that end in .elf run, so an app can never come from internal storage (/storage) or a name the card hides.

#include "sd_path_policy.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SD_ELF_ARGS_OK,
    SD_ELF_ARGS_USAGE,    // not "run <path>"
    SD_ELF_ARGS_REFUSED,  // sd_path_check said no (*why), or the path is not below /sd
    SD_ELF_ARGS_NOT_ELF,  // the name does not end in .elf
} sd_elf_args_t;

// "run <path>" -> path (cap bytes with the NUL). *why gets sd_path_check's verdict when it matters.
sd_elf_args_t sd_elf_parse(char const *args, char *path, size_t cap, sd_path_verdict_t *why);
// One word for a refusal.
char const *sd_elf_args_text(sd_elf_args_t v, sd_path_verdict_t why);

#ifdef __cplusplus
}
#endif
