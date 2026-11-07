// The Calculator's expression parser (#158): recursive descent over the grammar in calc_eval.h, one function per
// precedence level. Written from scratch so the app needs no third-party library.
#include "calc_eval.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char const *s;
    size_t      pos;
    calc_err_t  err;
    size_t      err_pos;
    bool        allow_min; // set by a unary minus right in front of a decimal literal: 2^63 is then -2^63's magnitude
    bool        got_min;   // the literal just read was exactly 2^63 and returned as INT64_MIN
    unsigned    depth;     // parse_unary calls in progress: every bracket and sign level passes through it
} parser_t;

static int64_t parse_or(parser_t *p);

static void fail(parser_t *p, calc_err_t err, size_t at) {
    if (p->err == CALC_OK) { // the first problem is the one worth reporting
        p->err     = err;
        p->err_pos = at;
    }
}

static void skip_space(parser_t *p) {
    while (p->s[p->pos] == ' ' || p->s[p->pos] == '\t') {
        p->pos++;
    }
}

// The next non-space character, without consuming it
static char peek(parser_t *p) {
    skip_space(p);
    return p->s[p->pos];
}

// True (and consumed) when the operator text follows; "<<" must not match a lone "<", so whole strings are compared
static bool accept(parser_t *p, char const *op) {
    skip_space(p);
    size_t len = strlen(op);
    if (strncmp(p->s + p->pos, op, len) == 0) {
        p->pos += len;
        return true;
    }
    return false;
}

static int digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool is_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// Digits of a 0x / 0b literal: all 64 bits are allowed, the result is the bit pattern
static int64_t parse_radix(parser_t *p, unsigned base, size_t start) {
    uint64_t value  = 0;
    size_t   digits = 0;
    while (is_alnum(p->s[p->pos])) {
        int d = digit_value(p->s[p->pos]);
        if (d < 0 || (unsigned)d >= base) {
            fail(p, CALC_BAD_NUMBER, p->pos);
            return 0;
        }
        if (value >> (base == 16 ? 60 : 63)) { // the next digit would push a bit out
            fail(p, CALC_BAD_NUMBER, start);
            return 0;
        }
        value = value * base + (unsigned)d;
        digits++;
        p->pos++;
    }
    if (digits == 0) {
        fail(p, CALC_BAD_NUMBER, start);
    }
    return (int64_t)value;
}

static int64_t parse_decimal(parser_t *p, size_t start) {
    uint64_t value = 0;
    while (is_alnum(p->s[p->pos])) {
        char c = p->s[p->pos];
        if (c < '0' || c > '9') {
            fail(p, CALC_BAD_NUMBER, p->pos);
            return 0;
        }
        unsigned d = (unsigned)(c - '0');
        if (value > (UINT64_MAX - d) / 10) {
            fail(p, CALC_OVERFLOW, start);
            return 0;
        }
        value = value * 10 + d;
        p->pos++;
    }
    if (value > (uint64_t)INT64_MAX) {
        if (value != (uint64_t)INT64_MAX + 1 || !p->allow_min) {
            fail(p, CALC_OVERFLOW, start);
            return 0;
        }
        p->got_min = true; // the unary minus returns INT64_MIN as it is
    }
    return (int64_t)value;
}

static int64_t parse_number(parser_t *p) {
    size_t start = p->pos;
    if (p->s[p->pos] == '0' && (p->s[p->pos + 1] == 'x' || p->s[p->pos + 1] == 'X')) {
        p->pos += 2;
        return parse_radix(p, 16, start);
    }
    if (p->s[p->pos] == '0' && (p->s[p->pos + 1] == 'b' || p->s[p->pos + 1] == 'B')) {
        p->pos += 2;
        return parse_radix(p, 2, start);
    }
    return parse_decimal(p, start);
}

static int64_t parse_primary(parser_t *p) {
    char c = peek(p);
    if (c == '(') {
        size_t open = p->pos;
        p->pos++;
        int64_t v = parse_or(p);
        if (p->err != CALC_OK) {
            return 0;
        }
        if (!accept(p, ")")) {
            fail(p, CALC_PAREN, open);
            return 0;
        }
        return v;
    }
    if (c >= '0' && c <= '9') {
        return parse_number(p);
    }
    if (c == '\0') {
        fail(p, CALC_SYNTAX, p->pos);
    } else if (c == ')') {
        fail(p, CALC_PAREN, p->pos);
    } else {
        fail(p, CALC_SYNTAX, p->pos);
    }
    return 0;
}

static int64_t parse_unary_level(parser_t *p);

// Bounded recursion: the deepest chain is ( ... ( via parse_or..parse_primary, or ~~~~ via parse_unary itself, and
// every level of either passes here, so one counter caps both.
static int64_t parse_unary(parser_t *p) {
    if (p->depth >= CALC_MAX_DEPTH) {
        fail(p, CALC_TOO_DEEP, p->pos);
        return 0;
    }
    p->depth++;
    int64_t v = parse_unary_level(p);
    p->depth--;
    return v;
}

static int64_t parse_unary_level(parser_t *p) {
    char c = peek(p);
    if (c == '-') {
        size_t at = p->pos;
        p->pos++;
        skip_space(p);
        p->allow_min = p->s[p->pos] >= '0' && p->s[p->pos] <= '9';
        p->got_min   = false;
        int64_t v    = parse_unary(p);
        bool    min  = p->got_min;
        p->allow_min = p->got_min = false;
        if (p->err != CALC_OK) {
            return 0;
        }
        if (min) {
            return INT64_MIN; // -9223372036854775808
        }
        if (v == INT64_MIN) {
            fail(p, CALC_OVERFLOW, at);
            return 0;
        }
        return -v;
    }
    if (c == '+') {
        p->pos++;
        return parse_unary(p);
    }
    if (c == '~') {
        p->pos++;
        return ~parse_unary(p);
    }
    return parse_primary(p);
}

static int64_t parse_mul(parser_t *p) {
    int64_t left = parse_unary(p);
    while (p->err == CALC_OK) {
        char   c  = peek(p);
        size_t at = p->pos;
        if (c != '*' && c != '/' && c != '%') {
            break;
        }
        p->pos++;
        int64_t right = parse_unary(p);
        if (p->err != CALC_OK) {
            return 0;
        }
        if (c == '*') {
            if (__builtin_mul_overflow(left, right, &left)) {
                fail(p, CALC_OVERFLOW, at);
            }
        } else if (right == 0) {
            fail(p, CALC_DIV_ZERO, at);
        } else if (left == INT64_MIN && right == -1) {
            fail(p, CALC_OVERFLOW, at);
        } else {
            left = c == '/' ? left / right : left % right;
        }
    }
    return p->err == CALC_OK ? left : 0;
}

static int64_t parse_add(parser_t *p) {
    int64_t left = parse_mul(p);
    while (p->err == CALC_OK) {
        char   c  = peek(p);
        size_t at = p->pos;
        if (c != '+' && c != '-') {
            break;
        }
        p->pos++;
        int64_t right = parse_mul(p);
        if (p->err != CALC_OK) {
            return 0;
        }
        bool over = c == '+' ? __builtin_add_overflow(left, right, &left) : __builtin_sub_overflow(left, right, &left);
        if (over) {
            fail(p, CALC_OVERFLOW, at);
        }
    }
    return p->err == CALC_OK ? left : 0;
}

static int64_t parse_shift(parser_t *p) {
    int64_t left = parse_add(p);
    while (p->err == CALC_OK) {
        bool   shl = false;
        size_t at  = p->pos;
        if (accept(p, "<<")) {
            shl = true;
        } else if (!accept(p, ">>")) {
            break;
        }
        int64_t count = parse_add(p);
        if (p->err != CALC_OK) {
            return 0;
        }
        if (count < 0 || count > 63) {
            fail(p, CALC_BAD_SHIFT, at);
        } else if (shl) {
            left = (int64_t)((uint64_t)left << count);
        } else {
            left >>= count; // arithmetic on every target we build for (gcc documents it)
        }
    }
    return p->err == CALC_OK ? left : 0;
}

static int64_t parse_and(parser_t *p) {
    int64_t left = parse_shift(p);
    while (p->err == CALC_OK && peek(p) == '&') {
        p->pos++;
        int64_t right = parse_shift(p);
        left &= right;
    }
    return p->err == CALC_OK ? left : 0;
}

static int64_t parse_xor(parser_t *p) {
    int64_t left = parse_and(p);
    while (p->err == CALC_OK && peek(p) == '^') {
        p->pos++;
        int64_t right = parse_and(p);
        left ^= right;
    }
    return p->err == CALC_OK ? left : 0;
}

static int64_t parse_or(parser_t *p) {
    int64_t left = parse_xor(p);
    while (p->err == CALC_OK && peek(p) == '|') {
        p->pos++;
        int64_t right = parse_xor(p);
        left |= right;
    }
    return p->err == CALC_OK ? left : 0;
}

calc_err_t calc_eval(char const *expr, int64_t *out, size_t *err_pos) {
    parser_t p = {.s = expr ? expr : ""};
    if (peek(&p) == '\0') {
        if (err_pos) {
            *err_pos = 0;
        }
        return CALC_EMPTY;
    }
    int64_t v = parse_or(&p);
    if (p.err == CALC_OK && peek(&p) != '\0') {
        fail(&p, p.s[p.pos] == ')' ? CALC_PAREN : CALC_SYNTAX, p.pos);
    }
    if (p.err != CALC_OK) {
        if (err_pos) {
            *err_pos = p.err_pos;
        }
        return p.err;
    }
    *out = v;
    return CALC_OK;
}

char const *calc_err_text(calc_err_t err) {
    switch (err) {
    case CALC_OK:         return "OK";
    case CALC_EMPTY:      return "Type an expression, for example 0xFF & 0b1010";
    case CALC_SYNTAX:     return "Not an expression I understand";
    case CALC_BAD_NUMBER: return "Not a valid number (or more than 64 bits)";
    case CALC_PAREN:      return "Brackets do not match";
    case CALC_DIV_ZERO:   return "Division by zero";
    case CALC_OVERFLOW:   return "Result does not fit in 64 bits";
    case CALC_BAD_SHIFT:  return "Shift by 0 to 63 places only";
    case CALC_TOO_DEEP:   return "Too many brackets or signs inside each other";
    }
    return "Error";
}

size_t calc_fmt_dec(char *out, size_t n, int64_t value) {
    int w = snprintf(out, n, "%lld", (long long)value);
    return w < 0 ? 0 : (size_t)w < n ? (size_t)w : n - 1;
}

size_t calc_fmt_hex(char *out, size_t n, int64_t value) {
    int w = snprintf(out, n, "0x%llX", (unsigned long long)value);
    return w < 0 ? 0 : (size_t)w < n ? (size_t)w : n - 1;
}

size_t calc_fmt_bin(char *out, size_t n, int64_t value) {
    if (n == 0) {
        return 0;
    }
    uint64_t v   = (uint64_t)value;
    int      top = 0; // the highest set bit; 0 for 0 so one digit shows
    for (int i = 63; i > 0; i--) {
        if ((v >> i) & 1) {
            top = i;
            break;
        }
    }
    size_t w = 0;
    #define PUT(ch) do { if (w + 1 < n) out[w++] = (ch); } while (0)
    PUT('0');
    PUT('b');
    for (int i = top; i >= 0; i--) {
        PUT(((v >> i) & 1) ? '1' : '0');
        if (i > 0 && i % 4 == 0) {
            PUT(' ');
        }
    }
    #undef PUT
    out[w] = '\0';
    return w;
}
