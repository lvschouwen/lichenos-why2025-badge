// Host test: the Calculator's parser and number formats (#158)
#include "calc_eval.h"

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

static void expect(char const *expr, int64_t want) {
    int64_t    got = 12345;
    size_t     pos = 0;
    calc_err_t err = calc_eval(expr, &got, &pos);
    if (err != CALC_OK || got != want) {
        printf("FAIL \"%s\": err %d got %lld want %lld\n", expr, (int)err, (long long)got, (long long)want);
        failures++;
    }
}

static void expect_err(char const *expr, calc_err_t want, size_t want_pos) {
    int64_t    got = 99;
    size_t     pos = 999;
    calc_err_t err = calc_eval(expr, &got, &pos);
    if (err != want || pos != want_pos || got != 99) {
        printf("FAIL \"%s\": err %d (want %d) at %zu (want %zu)\n", expr, (int)err, (int)want, pos, want_pos);
        failures++;
    }
}

static void formats(void) {
    char out[CALC_FMT_MAX];
    calc_fmt_dec(out, sizeof(out), -42);
    CHECK(strcmp(out, "-42") == 0);
    calc_fmt_hex(out, sizeof(out), 42);
    CHECK(strcmp(out, "0x2A") == 0);
    calc_fmt_hex(out, sizeof(out), -1);
    CHECK(strcmp(out, "0xFFFFFFFFFFFFFFFF") == 0);
    calc_fmt_bin(out, sizeof(out), 42);
    CHECK(strcmp(out, "0b10 1010") == 0);
    calc_fmt_bin(out, sizeof(out), 0);
    CHECK(strcmp(out, "0b0") == 0);
    calc_fmt_bin(out, sizeof(out), 1);
    CHECK(strcmp(out, "0b1") == 0);
    calc_fmt_bin(out, sizeof(out), 255);
    CHECK(strcmp(out, "0b1111 1111") == 0);
    calc_fmt_bin(out, sizeof(out), -1);
    CHECK(strlen(out) == 2 + 64 + 15); // 16 groups, 15 spaces
    CHECK(strncmp(out, "0b1111 1111", 11) == 0);
    calc_fmt_bin(out, sizeof(out), INT64_MIN);
    CHECK(strcmp(out, "0b1000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000") == 0);
    // a too small buffer is cut, still terminated
    char tiny[6];
    CHECK(calc_fmt_bin(tiny, sizeof(tiny), 255) == 5 && strcmp(tiny, "0b111") == 0);
    CHECK(calc_fmt_hex(tiny, sizeof(tiny), 0x123456) == 5 && strcmp(tiny, "0x123") == 0);
}

// The parser recurses once per bracket or unary sign, on the LVGL task (10 KB stack): deep input must be refused,
// not recursed into (#158 review). 8 levels still work (CALC_MAX_DEPTH 10: 8 brackets, each also passing one unary level).
static void nesting(void) {
    char s[128];
    size_t n = 0;
    for (int i = 0; i < 8; i++) s[n++] = '(';
    s[n++] = '7';
    for (int i = 0; i < 8; i++) s[n++] = ')';
    s[n] = '\0';
    expect(s, 7);
    n = 0;
    for (int i = 0; i < 60; i++) s[n++] = '(';
    s[n++] = '1';
    for (int i = 0; i < 60; i++) s[n++] = ')';
    s[n] = '\0';
    int64_t v = 0;
    size_t  at = 0;
    CHECK(calc_eval(s, &v, &at) == CALC_TOO_DEEP);
    n = 0;
    for (int i = 0; i < 99; i++) s[n++] = '~';
    s[n++] = '1';
    s[n] = '\0';
    CHECK(calc_eval(s, &v, &at) == CALC_TOO_DEEP);
    CHECK(strcmp(calc_err_text(CALC_TOO_DEEP), "Error") != 0);
}

int main(void) {
    nesting();
    // numbers
    expect("0", 0);
    expect("42", 42);
    expect("  42  ", 42);
    expect("0xFF", 255);
    expect("0xff", 255);
    expect("0XfF", 255);
    expect("0b1010", 10);
    expect("0B11", 3);
    expect("0xFFFFFFFFFFFFFFFF", -1);
    expect("0x8000000000000000", INT64_MIN);
    expect("0x7FFFFFFFFFFFFFFF", INT64_MAX);
    expect("9223372036854775807", INT64_MAX);
    expect("-9223372036854775808", INT64_MIN);
    expect("-9223372036854775807 - 1", INT64_MIN);
    expect("0b1111111111111111111111111111111111111111111111111111111111111111", -1);

    // precedence and associativity
    expect("2+3*4", 14);
    expect("(2+3)*4", 20);
    expect("10-3-2", 5);
    expect("100/10/5", 2);
    expect("2*3%4", 2);
    expect("17%5", 2);
    expect("-17%5", -2); // as in C
    expect("-17/5", -3);
    expect("7/2", 3);
    expect("1+2<<3", 24);        // + binds tighter than <<
    expect("1<<2+1", 8);
    expect("6&3|8", 10);          // & before |
    expect("6|3&1", 7);
    expect("5^3", 6);
    expect("1|2^3&4", 3);         // | lowest, then ^, then &: 1|(2^(3&4)) = 1|2 = 3
    expect("0xF0 & 0x3C", 0x30);
    expect("0xF0 | 0b1111", 0xFF);
    expect("0xFF ^ 0x0F", 0xF0);
    expect("1<<10", 1024);
    expect("1<<63", INT64_MIN);
    expect("1<<0", 1);
    expect("0x100>>4", 0x10);
    expect("-16>>2", -4);
    expect("-1>>63", -1);
    expect("~0", -1);
    expect("~0xFF", -256);
    expect("~~5", 5);
    expect("-(-5)", 5);
    expect("- -5", 5);
    expect("+5", 5);
    expect("-5+10", 5);
    expect("2*-3", -6);
    expect("2*~1", -4);
    expect("((((1))))", 1);
    expect("(1+2)*(3+4)", 21);
    expect("0x10 + 0b11 * 2 - 1", 21);
    expect("1 << 4 | 1 << 2", 20);
    expect("(1<<8)-1", 255);
    expect("4096 / 0x10", 256);
    expect("0xFFFF & ~0xF", 0xFFF0);
    expect("9223372036854775807 + 0", INT64_MAX);
    expect("-9223372036854775807-1+0", INT64_MIN);
    expect("9223372036854775807*1", INT64_MAX);
    expect("3037000499*3037000499", 9223372030926249001LL);

    // errors, with where they were found
    expect_err("", CALC_EMPTY, 0);
    expect_err("   ", CALC_EMPTY, 0);
    expect_err("5/0", CALC_DIV_ZERO, 1);
    expect_err("5%0", CALC_DIV_ZERO, 1);
    expect_err("1/(2-2)", CALC_DIV_ZERO, 1);
    expect_err("9223372036854775807+1", CALC_OVERFLOW, 19);
    expect_err("-9223372036854775807-2", CALC_OVERFLOW, 20);
    expect_err("9223372036854775808", CALC_OVERFLOW, 0);
    expect_err("99999999999999999999", CALC_OVERFLOW, 0);
    expect_err("-(9223372036854775808)", CALC_OVERFLOW, 2);
    expect_err("3037000500*3037000500", CALC_OVERFLOW, 10);
    expect_err("-9223372036854775808/-1", CALC_OVERFLOW, 20);
    expect_err("0-9223372036854775807-2", CALC_OVERFLOW, 21);
    expect_err("-(0x8000000000000000)", CALC_OVERFLOW, 0);
    expect_err("1<<64", CALC_BAD_SHIFT, 1);
    expect_err("1<<-1", CALC_BAD_SHIFT, 1);
    expect_err("1>>64", CALC_BAD_SHIFT, 1);
    expect_err("(1", CALC_PAREN, 0);
    expect_err("1)", CALC_PAREN, 1);
    expect_err("((1+2)", CALC_PAREN, 0);
    expect_err(")", CALC_PAREN, 0);
    expect_err("1+", CALC_SYNTAX, 2);
    expect_err("*3", CALC_SYNTAX, 0);
    expect_err("1 2", CALC_SYNTAX, 2);
    expect_err("1++", CALC_SYNTAX, 3);
    expect_err("abc", CALC_SYNTAX, 0);
    expect_err("1 + a", CALC_SYNTAX, 4);
    expect_err("()", CALC_PAREN, 1);
    expect_err("1.5", CALC_SYNTAX, 1);
    expect_err("0x", CALC_BAD_NUMBER, 0);
    expect_err("0b", CALC_BAD_NUMBER, 0);
    expect_err("0b12", CALC_BAD_NUMBER, 3);
    expect_err("0xG", CALC_BAD_NUMBER, 2);
    expect_err("12ab", CALC_BAD_NUMBER, 2);
    expect_err("0x1FFFFFFFFFFFFFFFF", CALC_BAD_NUMBER, 0); // 65 bits
    expect_err("0b11111111111111111111111111111111111111111111111111111111111111111", CALC_BAD_NUMBER, 0);
    expect_err("1 + (2 * 0xZ)", CALC_BAD_NUMBER, 11);

    CHECK(calc_eval(NULL, &(int64_t){0}, NULL) == CALC_EMPTY);
    CHECK(calc_eval("5/0", &(int64_t){0}, NULL) == CALC_DIV_ZERO); // err_pos may be NULL
    CHECK(strcmp(calc_err_text(CALC_DIV_ZERO), "Division by zero") == 0);
    for (int e = CALC_OK; e <= CALC_BAD_SHIFT; e++) {
        CHECK(calc_err_text((calc_err_t)e)[0] != '\0');
    }
    formats();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("calc_eval_test: ok\n");
    return 0;
}
