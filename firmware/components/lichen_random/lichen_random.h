#pragma once
// Random numbers for the OS and its apps (#84 V2). The P4 has no radio of its own for esp_random() to draw on, so real
// entropy needs the bootloader's ADC-based source on. The OS enables it once at boot and leaves it on: toggling it
// clashes with the SAR ADC that battery and sensor code (#11, #69) will use, and the OS owns any later ADC hand-over.
// Apps must never call bootloader_random_* themselves.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     lichen_random_start(void); // once, early in app_main; idempotent
void     lichen_random_fill(void *buf, size_t len);
uint32_t lichen_random_u32(void);

#ifdef __cplusplus
}
#endif
