#include "uart_lend.h"

bool uart_lend_take(uart_lend_t *s) {
    if (s->lent) {
        return false;
    }
    s->lent    = true;
    s->pending = false;
    return true;
}

bool uart_lend_send(uart_lend_t *s, uint8_t display, uint8_t keyboard) {
    if (!s->lent) {
        return true;
    }
    s->pending  = true;
    s->display  = display;
    s->keyboard = keyboard;
    return false;
}

bool uart_lend_give(uart_lend_t *s, uint8_t *display, uint8_t *keyboard) {
    if (!s->lent) {
        return false;
    }
    bool resend = s->pending;
    if (resend) {
        *display  = s->display;
        *keyboard = s->keyboard;
    }
    s->lent    = false;
    s->pending = false;
    return resend;
}
