#include "uart_lend.h"

bool uart_lend_take(uart_lend_t *s, uart_lend_mode_t mode) {
    if (s->mode != UART_LEND_NONE || mode == UART_LEND_NONE) {
        return false;
    }
    s->mode    = mode;
    s->pending = false;
    return true;
}

bool uart_lend_send(uart_lend_t *s, uint8_t display, uint8_t keyboard) {
    if (s->mode != UART_LEND_EXCLUSIVE) {
        return true;
    }
    s->pending  = true;
    s->display  = display;
    s->keyboard = keyboard;
    return false;
}

bool uart_lend_give(uart_lend_t *s, uint8_t *display, uint8_t *keyboard) {
    if (s->mode == UART_LEND_NONE) {
        return false;
    }
    bool resend = s->pending;
    if (resend) {
        *display  = s->display;
        *keyboard = s->keyboard;
    }
    s->mode    = UART_LEND_NONE;
    s->pending = false;
    return resend;
}
