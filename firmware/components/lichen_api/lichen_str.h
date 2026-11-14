#pragma once
// One bounded string copy (#445). Pure, host-tested in host_tests/lichen_str_test.c.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Copies src into dst (cap bytes) and always terminates when cap >= 1; src NULL is "". Returns true when src was cut
// (more than cap - 1 chars), false when it fit whole. cap == 0 writes nothing and is "cut" for any non-empty src.
bool lichen_str_copy(char *dst, size_t cap, char const *src);

// The same for the first n chars of src, stopping at a NUL before that (so an unterminated field of n bytes is fine).
// "Cut" means a char within those n was left out.
bool lichen_str_copy_n(char *dst, size_t cap, char const *src, size_t n);

#ifdef __cplusplus
}
#endif
