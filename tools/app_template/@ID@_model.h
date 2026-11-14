#pragma once
// The pure rules of the app: a counter that counts in steps and stops at a limit. ESP-free, so it is host-tested in
// host_tests/@ID@_model_test.c; app_@ID@.c is the screens and the saving. Keep every decision here and the screen thin.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define @UP@_COUNT_MAX 9999

typedef struct {
    uint32_t count;
    uint32_t step; // 1, 5 or 10
} @ID@_model_t;

// A model from saved values: a count past the limit is cut to it, a step that is not 1, 5 or 10 becomes 1
void @ID@_model_init(@ID@_model_t *m, uint32_t count, uint32_t step);
// Counts one step; false (nothing changed) when the limit is already reached. The count never passes the limit.
bool @ID@_model_tap(@ID@_model_t *m);
// 1 -> 5 -> 10 -> 1; anything else gives 1
uint32_t @ID@_model_next_step(uint32_t step);
// "no taps", "1 tap", "42 taps" into out (cap bytes, always terminated)
void @ID@_model_text(@ID@_model_t const *m, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
