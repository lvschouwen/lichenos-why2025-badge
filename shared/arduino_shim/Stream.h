// Minimal Arduino Stream for MeshCore (identity read/write, hex printing): byte in, byte out, text out
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

class Stream {
public:
    virtual ~Stream() = default;
    virtual size_t write(uint8_t b) = 0;
    virtual int    read() = 0; // -1 when nothing is left
    virtual int    available() { return 0; }
    virtual int    peek() { return -1; }
    virtual void   flush() {}

    virtual size_t write(uint8_t const *buf, size_t len) {
        size_t n = 0;
        while (n < len && write(buf[n])) {
            n++;
        }
        return n;
    }
    size_t readBytes(uint8_t *buf, size_t len) {
        size_t n = 0;
        int    c;
        while (n < len && (c = read()) >= 0) {
            buf[n++] = (uint8_t)c;
        }
        return n;
    }
    size_t print(char const *s) { return write((uint8_t const *)s, strlen(s)); }
    size_t print(char c) { return write((uint8_t)c); }
    size_t println(char const *s = "") { return print(s) + print('\n'); }
    // Numbers as Arduino's Print: base 10 only (MeshCore's ConfigSerializer), doubles with a number of decimals
    size_t print(int v, int base = 10) { return print((long)v, base); }
    size_t print(unsigned v, int base = 10) { return print((unsigned long)v, base); }
    size_t print(long v, int base = 10) { return printf("%ld", ((void)base, v)); }
    size_t print(unsigned long v, int base = 10) { return printf("%lu", ((void)base, v)); }
    size_t print(double v, int digits = 2) { return printf("%.*f", digits, v); }
    size_t printf(char const *fmt, ...) __attribute__((format(printf, 2, 3))) {
        char    buf[160];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        return n > 0 ? print(buf) : 0;
    }
};
