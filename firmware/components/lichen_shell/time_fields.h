#pragma once
// The model behind Settings > Time > Set date & time (#64): year, month, day, hour, minute and a cursor. Pure, no
// LVGL (time_set_screen.c renders it); host-tested in time_fields_test.c.
//
// Up/Down step the field under the cursor by one and wrap; the year stays in year_min..year_max (build year .. +2,
// the clock's plausible window). The day is clamped when the month or year changes. Digits overwrite the field and
// move on once it is full (4 digits for the year, 2 for the rest); Backspace restarts the field. Left/Right move.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { TF_YEAR, TF_MONTH, TF_DAY, TF_HOUR, TF_MINUTE, TF_COUNT } tf_field_t;

typedef struct {
    int v[TF_COUNT];
    int cursor;    // tf_field_t
    int typed;     // digits typed into the field under the cursor so far (0: none, the next digit starts it)
    int before;    // that field's value before the first digit, for Backspace and an unfinished entry
    int year_min, year_max;
} time_fields_t;

void tf_init(time_fields_t *tf, int year, int month, int day, int hour, int minute, int year_min, int year_max);
void tf_step(time_fields_t *tf, int delta); // Up +1, Down -1
void tf_move(time_fields_t *tf, int delta); // Right +1, Left -1; stops at the ends
void tf_digit(time_fields_t *tf, int digit);
void tf_backspace(time_fields_t *tf);
// Ends an unfinished entry (before reading the values for Enter): a part-typed field is clamped into range
void tf_commit(time_fields_t *tf);
// The field as shown: "2026", "07"; while typing the digits so far padded with '_': "20__", "1_"
void tf_field_text(time_fields_t const *tf, int field, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
