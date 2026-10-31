// Host test: the Set date & time fields (#64)
#include "time_fields.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool is(time_fields_t const *tf, int y, int mo, int d, int h, int mi) {
    return tf->v[TF_YEAR] == y && tf->v[TF_MONTH] == mo && tf->v[TF_DAY] == d && tf->v[TF_HOUR] == h &&
           tf->v[TF_MINUTE] == mi;
}

static bool text_is(time_fields_t const *tf, int field, char const *want) {
    char t[8];
    tf_field_text(tf, field, t, sizeof(t));
    return strcmp(t, want) == 0;
}

static void wraps(void) {
    time_fields_t tf;
    tf_init(&tf, 2026, 12, 31, 23, 59, 2026, 2028);
    CHECK(tf.cursor == TF_YEAR);
    tf_move(&tf, 1);
    tf_step(&tf, 1); // December -> January, no carry into the year
    CHECK(is(&tf, 2026, 1, 31, 23, 59));
    tf_step(&tf, -1);
    CHECK(is(&tf, 2026, 12, 31, 23, 59));
    tf_move(&tf, 2);
    tf_step(&tf, 1);
    CHECK(is(&tf, 2026, 12, 31, 0, 59));
    tf_move(&tf, 1);
    tf_step(&tf, 1);
    CHECK(is(&tf, 2026, 12, 31, 0, 0));
    tf_step(&tf, -1);
    CHECK(is(&tf, 2026, 12, 31, 0, 59));
    tf_move(&tf, 5); // stops at the end
    CHECK(tf.cursor == TF_MINUTE);
    tf_move(&tf, -9);
    CHECK(tf.cursor == TF_YEAR);
    tf_step(&tf, 1);
    tf_step(&tf, 1);
    CHECK(tf.v[TF_YEAR] == 2028);
    tf_step(&tf, 1); // the year wraps inside its range
    CHECK(tf.v[TF_YEAR] == 2026);
    tf_step(&tf, -1);
    CHECK(tf.v[TF_YEAR] == 2028);
    tf_move(&tf, 2); // day 31 wraps to 1, 1 back to 31 in December
    tf_step(&tf, 1);
    CHECK(tf.v[TF_DAY] == 1);
    tf_step(&tf, -1);
    CHECK(tf.v[TF_DAY] == 31);
}

static void clamps(void) {
    time_fields_t tf;
    tf_init(&tf, 2027, 1, 31, 12, 0, 2026, 2028);
    tf_move(&tf, 1);
    tf_step(&tf, 1); // 31 Jan -> Feb 2027: 28
    CHECK(is(&tf, 2027, 2, 28, 12, 0));
    tf_init(&tf, 2028, 1, 31, 12, 0, 2026, 2028);
    tf_move(&tf, 1);
    tf_step(&tf, 1); // leap year: 29
    CHECK(is(&tf, 2028, 2, 29, 12, 0));
    tf_move(&tf, -1);
    tf_step(&tf, 1); // 29 Feb 2028 -> 2026 (wrapped): 28
    CHECK(is(&tf, 2026, 2, 28, 12, 0));
    tf_init(&tf, 2020, 2, 30, 25, 61, 2026, 2028); // out-of-range prefill is clamped
    CHECK(is(&tf, 2026, 2, 28, 23, 59));
    tf_init(&tf, 2026, 3, 31, 8, 5, 2026, 2028);
    tf_move(&tf, 2); // day 31 in March wraps up to 1, down from 1 to 31
    tf_step(&tf, 1);
    CHECK(tf.v[TF_DAY] == 1);
    tf_init(&tf, 2026, 4, 30, 8, 5, 2026, 2028);
    tf_move(&tf, 2);
    tf_step(&tf, 1); // April has 30
    CHECK(tf.v[TF_DAY] == 1);
    tf_step(&tf, -1);
    CHECK(tf.v[TF_DAY] == 30);
}

static void typing(void) {
    time_fields_t tf;
    tf_init(&tf, 2026, 10, 31, 14, 5, 2026, 2028);
    CHECK(text_is(&tf, TF_YEAR, "2026") && text_is(&tf, TF_MINUTE, "05") && text_is(&tf, TF_MONTH, "10"));
    tf_digit(&tf, 2);
    CHECK(text_is(&tf, TF_YEAR, "2___") && tf.cursor == TF_YEAR);
    tf_digit(&tf, 0);
    tf_digit(&tf, 2);
    CHECK(text_is(&tf, TF_YEAR, "202_"));
    tf_digit(&tf, 7); // full: on to the month
    CHECK(tf.v[TF_YEAR] == 2027 && tf.cursor == TF_MONTH && text_is(&tf, TF_YEAR, "2027"));
    tf_digit(&tf, 0);
    CHECK(text_is(&tf, TF_MONTH, "0_"));
    tf_digit(&tf, 3);
    CHECK(tf.v[TF_MONTH] == 3 && tf.cursor == TF_DAY);
    tf_digit(&tf, 2);
    tf_digit(&tf, 8);
    tf_digit(&tf, 0);
    tf_digit(&tf, 2);
    tf_digit(&tf, 3);
    tf_digit(&tf, 0);
    CHECK(is(&tf, 2027, 3, 28, 2, 30) && tf.cursor == TF_MINUTE);
    tf_digit(&tf, 9); // last field: stays, entry restarts on the next digit
    tf_digit(&tf, 9);
    CHECK(tf.v[TF_MINUTE] == 59 && tf.cursor == TF_MINUTE); // 99 clamped
    tf_digit(&tf, 4);
    tf_digit(&tf, 5);
    CHECK(tf.v[TF_MINUTE] == 45);

    // out of range when full: clamped
    tf_init(&tf, 2026, 10, 31, 14, 5, 2026, 2028);
    tf_digit(&tf, 9);
    tf_digit(&tf, 9);
    tf_digit(&tf, 9);
    tf_digit(&tf, 9);
    CHECK(tf.v[TF_YEAR] == 2028);
    tf_digit(&tf, 1);
    tf_digit(&tf, 9);
    CHECK(tf.v[TF_MONTH] == 12);
    tf_digit(&tf, 0);
    tf_digit(&tf, 0);
    CHECK(tf.v[TF_DAY] == 1); // day 00 -> 1
    tf_digit(&tf, 3);
    tf_digit(&tf, 0);
    CHECK(tf.v[TF_HOUR] == 23);

    // the day follows a month typed after it
    tf_init(&tf, 2026, 1, 31, 14, 5, 2026, 2028);
    tf_move(&tf, 1);
    tf_digit(&tf, 0);
    tf_digit(&tf, 2);
    CHECK(tf.v[TF_DAY] == 28);
}

static void backspace_and_moves(void) {
    time_fields_t tf;
    tf_init(&tf, 2026, 10, 31, 14, 5, 2026, 2028);
    tf_move(&tf, 3);
    tf_digit(&tf, 2);
    CHECK(text_is(&tf, TF_HOUR, "2_"));
    tf_backspace(&tf); // restarts the field: the old value is back
    CHECK(tf.v[TF_HOUR] == 14 && text_is(&tf, TF_HOUR, "14"));
    tf_digit(&tf, 0);
    tf_digit(&tf, 9);
    CHECK(tf.v[TF_HOUR] == 9 && tf.cursor == TF_MINUTE);
    tf_backspace(&tf); // nothing typed: no change
    CHECK(tf.v[TF_MINUTE] == 5 && tf.cursor == TF_MINUTE);

    // moving away from a part-typed field ends it: one digit is that value
    tf_move(&tf, -1);
    tf_digit(&tf, 7);
    tf_move(&tf, 1);
    CHECK(tf.v[TF_HOUR] == 7 && text_is(&tf, TF_HOUR, "07"));
    // a step on a part-typed field starts from the typed value
    tf_digit(&tf, 3);
    tf_step(&tf, 1);
    CHECK(tf.v[TF_MINUTE] == 4 && text_is(&tf, TF_MINUTE, "04"));
    // a part-typed year is not a year: commit restores it
    tf_move(&tf, -9);
    tf_digit(&tf, 2);
    tf_digit(&tf, 0);
    tf_commit(&tf);
    CHECK(tf.v[TF_YEAR] == 2026 && text_is(&tf, TF_YEAR, "2026"));
    tf_digit(&tf, 5);
    tf_commit(&tf); // "5" as a month... but this is the year field still: restored
    CHECK(tf.v[TF_YEAR] == 2026);
    tf_move(&tf, 1);
    tf_digit(&tf, 5);
    tf_commit(&tf);
    CHECK(tf.v[TF_MONTH] == 5 && tf.cursor == TF_MONTH);
    tf_digit(&tf, -1); // not a digit
    tf_digit(&tf, 10);
    CHECK(tf.v[TF_MONTH] == 5 && tf.typed == 0);
}

int main(void) {
    wraps();
    clamps();
    typing();
    backspace_and_moves();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("time_fields_test: ok\n");
    return 0;
}
