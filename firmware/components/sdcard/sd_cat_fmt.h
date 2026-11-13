#pragma once
// "fw sd cat" (#378): the arguments and the output lines, without ESP headers: host-tested in
// host_tests/sd_path_policy_test.c. sd_console.c reads the file.

#include "sd_path_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_CAT_DEFAULT  512  // bytes read when no length is given
#define SD_CAT_MAX      1024 // the most one call reads: the reply stays a screenful, the buffer small
#define SD_CAT_HEX_ROW  16   // bytes per hex line
#define SD_CAT_TEXT_COL 80   // a text line longer than this goes on over several replies
#define SD_CAT_LINE_CAP 96   // the out buffer both line formatters need (a hex row is 77 bytes and a NUL)

typedef struct {
    char     path[SD_PATH_MAX + 2]; // one byte over SD_PATH_MAX, so a longer path stays too long
    uint32_t offset;
    uint32_t len;
    bool     hex;
} sd_cat_args_t;

// "[-x] <path> [offset [len]]" (-x may stand anywhere). Decimal numbers; len is 1..SD_CAT_MAX. False: bad usage.
// The path is only copied here: sd_path_check decides about it.
bool sd_cat_parse(char const *args, sd_cat_args_t *out);

// "00000200  41 42 ... |AB..|" for n (<= SD_CAT_HEX_ROW) bytes at file offset off. Returns the length written.
size_t sd_cat_hex_line(uint32_t off, uint8_t const *b, size_t n, char *out, size_t cap);
// One text line from b[0..n): up to a newline (not printed, but consumed) or SD_CAT_TEXT_COL bytes. CR is dropped, a tab
// becomes a space, other bytes outside printable ASCII show as '.'. *consumed is > 0 whenever n > 0 and
// cap >= SD_CAT_LINE_CAP.
size_t sd_cat_text_line(uint8_t const *b, size_t n, size_t *consumed, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
