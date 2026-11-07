#pragma once

#include <stdbool.h>
#include <stdint.h>

// UART1 lend state (#250): the backlight owns UART1 for its "BL" lines, but the C6 flasher and "fw c6log" borrow it.
// EXCLUSIVE (the flasher, its own driver and the SLIP stream): the backlight neither installs the driver nor writes; it
// keeps the newest levels asked for and sends them once the UART comes back. SHARED (c6log, which only reads): the
// driver stays installed for the whole lend and BL lines still go out through it. Pure (host_tests/uart_lend_test.c);
// backlight.c calls it under its uart_lock.
typedef enum { UART_LEND_NONE, UART_LEND_SHARED, UART_LEND_EXCLUSIVE } uart_lend_mode_t;

typedef struct {
    uart_lend_mode_t mode;
    bool             pending; // a send came in during an exclusive lend
    uint8_t          display, keyboard;
} uart_lend_t;

// Lends the UART (mode SHARED or EXCLUSIVE); false when it is already lent (one borrower at a time)
bool uart_lend_take(uart_lend_t *s, uart_lend_mode_t mode);
// A send of these levels: true when it may go out now, false during an exclusive lend (the levels are kept, the newest
// wins)
bool uart_lend_send(uart_lend_t *s, uint8_t display, uint8_t keyboard);
// Gives the UART back; true (and the levels) when a send was held back and must go out now. A give without a take
// changes nothing and returns false.
bool uart_lend_give(uart_lend_t *s, uint8_t *display, uint8_t *keyboard);
