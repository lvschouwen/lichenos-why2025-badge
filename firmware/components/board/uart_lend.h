#pragma once

#include <stdbool.h>
#include <stdint.h>

// UART1 lend state (#250): the backlight owns UART1 for its "BL" lines, but the C6 flasher and "fw c6log" borrow it.
// While it is lent the backlight neither installs the driver nor writes; it keeps the newest levels asked for and
// sends them once the UART comes back. Pure (host_tests/uart_lend_test.c); backlight.c calls it under its uart_lock.
typedef struct {
    bool    lent;
    bool    pending; // a send came in while lent
    uint8_t display, keyboard;
} uart_lend_t;

// Lends the UART; false when it is already lent (one borrower at a time)
bool uart_lend_take(uart_lend_t *s);
// A send of these levels: true when it may go out now, false when lent (the levels are kept, the newest wins)
bool uart_lend_send(uart_lend_t *s, uint8_t display, uint8_t keyboard);
// Gives the UART back; true (and the levels) when a send was held back and must go out now. A give without a take
// changes nothing and returns false.
bool uart_lend_give(uart_lend_t *s, uint8_t *display, uint8_t *keyboard);
