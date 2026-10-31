// ESP32's pgmspace.h, which rweather Crypto includes when ESP32 is defined (#65): flash and RAM share one address
// space, so reading "program memory" is a plain read
#pragma once

#include <stdint.h>
#include <string.h>

#define PROGMEM
#define pgm_read_byte(addr)  (*(const uint8_t *)(addr))
#define pgm_read_word(addr)  (*(const uint16_t *)(addr))
#define pgm_read_dword(addr) (*(const uint32_t *)(addr))
#define memcpy_P(dst, src, len) memcpy((dst), (src), (len))
