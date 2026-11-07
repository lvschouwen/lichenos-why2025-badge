#pragma once

#include <stddef.h>
#include <stdint.h>

// The Calculator app's arithmetic (#158), pure so host_tests/calc_eval_test.c covers it.
//
// Integers in 64 bits, signed. Literals: decimal (255), hex (0xFF) and binary (0b1010); a hex or binary literal may use
// all 64 bits (0xFFFFFFFFFFFFFFFF is -1), a decimal one must fit a signed 64-bit number. Operators, tightest first:
//   unary + - ~     * / %     + -     << >>     &     ^     |
// as in C, with parentheses. Spaces are ignored. Arithmetic that does not fit in 64 bits is an error, never a wrapped
// number; shifts are bit operations (<< drops the bits that fall off, >> keeps the sign) and take a count of 0..63.
typedef enum {
    CALC_OK = 0,
    CALC_EMPTY,       // nothing to calculate
    CALC_SYNTAX,      // a character or a place where something else was expected
    CALC_BAD_NUMBER,  // 0x or 0b without digits, a digit that does not belong (0b12, 12ab), more than 64 bits
    CALC_PAREN,       // a missing ( or )
    CALC_DIV_ZERO,    // / or % by zero
    CALC_OVERFLOW,    // does not fit in 64 bits
    CALC_BAD_SHIFT,   // a shift count below 0 or above 63
    CALC_TOO_DEEP,    // more than CALC_MAX_DEPTH brackets / signs inside each other (the parser recurses per level)
} calc_err_t;

#define CALC_MAX_DEPTH 10 // nesting levels: one bracket level is 288 B of the caller's stack (-Os, riscv32), and the caller is the LVGL task (10 KB, ~4 KB spare)

// Evaluates expr. On success *out holds the value. On an error *err_pos (when not NULL) is the byte offset the problem
// was found at, for a caret or a message.
calc_err_t calc_eval(char const *expr, int64_t *out, size_t *err_pos);

// A plain sentence for the error ("Division by zero"), for the screen; never NULL
char const *calc_err_text(calc_err_t err);

// The value in the three bases, NUL-terminated, each returns the length written (always fits when n >= CALC_FMT_MAX):
//   decimal  "-42"
//   hex      "0xFFFFFFFFFFFFFFD6" (two's complement, no leading zeros: "0x2A")
//   binary   "0b10 1010" (groups of four from the right, a space between)
#define CALC_FMT_MAX 96
size_t calc_fmt_dec(char *out, size_t n, int64_t value);
size_t calc_fmt_hex(char *out, size_t n, int64_t value);
size_t calc_fmt_bin(char *out, size_t n, int64_t value);
