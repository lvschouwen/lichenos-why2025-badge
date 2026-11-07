// Host test: the UART1 lend between the backlight, the C6 flasher and "fw c6log" (#250)
#include "uart_lend.h"

#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void exclusive(void) {
    uart_lend_t s = {0};
    uint8_t     d = 0xEE, k = 0xEE;

    // Not lent: every send goes out, a stray give changes nothing
    CHECK(uart_lend_send(&s, 50, 20));
    CHECK(!uart_lend_give(&s, &d, &k));
    CHECK(d == 0xEE && k == 0xEE);
    CHECK(!uart_lend_take(&s, UART_LEND_NONE));

    // Lent with no send meanwhile: nothing to send on return
    CHECK(uart_lend_take(&s, UART_LEND_EXCLUSIVE));
    CHECK(!uart_lend_give(&s, &d, &k));
    CHECK(uart_lend_send(&s, 50, 20));

    // One borrower at a time
    CHECK(uart_lend_take(&s, UART_LEND_EXCLUSIVE));
    CHECK(!uart_lend_take(&s, UART_LEND_EXCLUSIVE));
    CHECK(!uart_lend_take(&s, UART_LEND_SHARED));

    // Sends while lent are held back, the newest wins and goes out on return
    CHECK(!uart_lend_send(&s, 10, 0));
    CHECK(!uart_lend_send(&s, 70, 30));
    CHECK(uart_lend_give(&s, &d, &k));
    CHECK(d == 70 && k == 30);

    // Back to normal, and the held-back levels do not come back on the next lend
    CHECK(uart_lend_send(&s, 40, 40));
    CHECK(uart_lend_take(&s, UART_LEND_EXCLUSIVE));
    d = k = 0xEE;
    CHECK(!uart_lend_give(&s, &d, &k));
    CHECK(d == 0xEE && k == 0xEE);
    CHECK(!uart_lend_give(&s, &d, &k));

    // A blank (0 0) held back is sent like any other level
    CHECK(uart_lend_take(&s, UART_LEND_EXCLUSIVE));
    CHECK(!uart_lend_send(&s, 0, 0));
    CHECK(uart_lend_give(&s, &d, &k));
    CHECK(d == 0 && k == 0);
}

// c6log only reads: the backlight keeps writing (a key after a dim, the restart blank), nothing to resend on return
static void shared(void) {
    uart_lend_t s = {0};
    uint8_t     d = 0xEE, k = 0xEE;
    CHECK(uart_lend_take(&s, UART_LEND_SHARED));
    CHECK(!uart_lend_take(&s, UART_LEND_SHARED));
    CHECK(!uart_lend_take(&s, UART_LEND_EXCLUSIVE)); // no flash check under a running c6log
    CHECK(uart_lend_send(&s, 10, 0));
    CHECK(uart_lend_send(&s, 0, 0));
    CHECK(!uart_lend_give(&s, &d, &k));
    CHECK(d == 0xEE && k == 0xEE);
    CHECK(uart_lend_take(&s, UART_LEND_EXCLUSIVE)); // free again
    CHECK(!uart_lend_send(&s, 30, 30));
    CHECK(uart_lend_give(&s, &d, &k));
}

int main(void) {
    exclusive();
    shared();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("uart_lend_test: all passed\n");
    return 0;
}
