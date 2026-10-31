#pragma once

#include <stddef.h>

// Log line text for the boot screen (#66): drops ESC [ ... <final byte> colour sequences (also one cut off at the end)
// and every CR and LF. out must hold len + 1 bytes; in and out may be the same buffer. Returns the new length.
size_t log_clean(char const *in, size_t len, char *out);
