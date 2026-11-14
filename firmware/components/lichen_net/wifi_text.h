#pragma once
// The bounded text copy of the Wi-Fi service (#457), ESP-free so it is host-tested (wifi_text_test.c).
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// src copied into dst (cap bytes with the NUL), cut at cap - 1 bytes or at a NUL: it also takes the driver's
// unterminated 32-byte SSID field. A NULL src copies as "". cap 0 or a NULL dst writes nothing. No snprintf: it is
// called with interrupts off (#5). Never logs: the text can be a credential.
void wifi_text_copy(char *dst, size_t cap, char const *src);

#ifdef __cplusplus
}
#endif
