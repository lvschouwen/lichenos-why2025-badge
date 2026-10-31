// Host test for the console command splitter (#53)
#include "console_cmd.h"

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

// Feeds text; collects what went to stdin and the commands that came out
typedef struct {
    char stdin_out[256];
    char commands[4][CONSOLE_CMD_MAX];
    int  n_commands;
    int  too_long;
} run_t;

static void feed(console_cmd_t *s, run_t *r, char const *text) {
    for (; *text; text++) {
        char const *pass;
        size_t      n;
        switch (console_cmd_feed(s, *text, &pass, &n)) {
            case CONSOLE_CMD_PASS: strncat(r->stdin_out, pass, n); break;
            case CONSOLE_CMD_READY:
                if (r->n_commands < 4) {
                    strcpy(r->commands[r->n_commands], s->buf);
                }
                r->n_commands++;
                break;
            case CONSOLE_CMD_TOO_LONG: r->too_long++; break;
            case CONSOLE_CMD_IDLE: break;
        }
    }
}

int main(void) {
    console_cmd_t s;
    run_t         r;

    // Plain input passes through untouched
    console_cmd_init(&s);
    memset(&r, 0, sizeof r);
    feed(&s, &r, "hello world\nprint(1)\r\n");
    CHECK(strcmp(r.stdin_out, "hello world\nprint(1)\r\n") == 0);
    CHECK(r.n_commands == 0);

    // A command is taken out of the stream, its line end included
    console_cmd_init(&s);
    memset(&r, 0, sizeof r);
    feed(&s, &r, "badgevms run ota_receiver\r\nnext\n");
    CHECK(r.n_commands == 1);
    CHECK(strcmp(r.commands[0], "run ota_receiver") == 0);
    CHECK(strcmp(r.stdin_out, "next\n") == 0);

    // A partial prefix that fails is released in order, nothing lost
    console_cmd_init(&s);
    memset(&r, 0, sizeof r);
    feed(&s, &r, "badge\nbad apple\nb\n");
    CHECK(strcmp(r.stdin_out, "badge\nbad apple\nb\n") == 0);
    CHECK(r.n_commands == 0);

    // The prefix only counts at the start of a line
    console_cmd_init(&s);
    memset(&r, 0, sizeof r);
    feed(&s, &r, "x badgevms run y\n");
    CHECK(strcmp(r.stdin_out, "x badgevms run y\n") == 0);
    CHECK(r.n_commands == 0);

    // Commands and input interleave; a command right after a line of input is still found
    console_cmd_init(&s);
    memset(&r, 0, sizeof r);
    feed(&s, &r, "abc\nbadgevms help\nbadgevms run a\ndef");
    CHECK(r.n_commands == 2);
    CHECK(strcmp(r.commands[0], "help") == 0);
    CHECK(strcmp(r.commands[1], "run a") == 0);
    CHECK(strcmp(r.stdin_out, "abc\ndef") == 0);

    // An overlong command is dropped and reported, the stream recovers
    console_cmd_init(&s);
    memset(&r, 0, sizeof r);
    char long_line[CONSOLE_CMD_MAX + 40];
    memset(long_line, 'z', sizeof long_line - 1);
    long_line[sizeof long_line - 1] = '\0';
    feed(&s, &r, CONSOLE_CMD_PREFIX);
    feed(&s, &r, long_line);
    feed(&s, &r, "\nbadgevms help\n");
    CHECK(r.too_long == 1);
    CHECK(r.n_commands == 1);
    CHECK(strcmp(r.commands[0], "help") == 0);
    CHECK(r.stdin_out[0] == '\0');

    // An empty command is still a command (help), and a bare CR ends it too
    console_cmd_init(&s);
    memset(&r, 0, sizeof r);
    feed(&s, &r, "badgevms \rq");
    CHECK(r.n_commands == 1);
    CHECK(strcmp(r.commands[0], "") == 0);
    CHECK(strcmp(r.stdin_out, "q") == 0);

    // A held prefix byte is released by an idle flush and the line then can't become a command
    console_cmd_init(&s);
    memset(&r, 0, sizeof r);
    feed(&s, &r, "b");
    CHECK(r.stdin_out[0] == '\0');
    {
        char const *pass;
        size_t      n;
        CHECK(console_cmd_flush(&s, &pass, &n) == CONSOLE_CMD_PASS);
        CHECK(n == 1 && pass[0] == 'b');
        strncat(r.stdin_out, pass, n);
        CHECK(console_cmd_flush(&s, &pass, &n) == CONSOLE_CMD_IDLE); // nothing held now
    }
    feed(&s, &r, "adgevms help\n");
    CHECK(strcmp(r.stdin_out, "badgevms help\n") == 0);
    CHECK(r.n_commands == 0);

    // A flush never breaks a command being typed, and cancels the pending line-end swallow after one
    console_cmd_init(&s);
    memset(&r, 0, sizeof r);
    feed(&s, &r, "badgevms ru");
    {
        char const *pass;
        size_t      n;
        CHECK(console_cmd_flush(&s, &pass, &n) == CONSOLE_CMD_IDLE);
    }
    feed(&s, &r, "n x\n");
    CHECK(r.n_commands == 1 && strcmp(r.commands[0], "run x") == 0);
    {
        char const *pass;
        size_t      n;
        console_cmd_flush(&s, &pass, &n);
    }
    feed(&s, &r, "\n"); // an Enter typed later is real input
    CHECK(strcmp(r.stdin_out, "\n") == 0);

    // C6 image identifiers end up in an APPS:[<id>] path, so only plain names get through (#54)
    CHECK(console_c6_id_valid("why2025_firmware_ota_c6"));
    CHECK(console_c6_id_valid("c6_radio_check"));
    CHECK(!console_c6_id_valid(""));
    CHECK(!console_c6_id_valid("a]b"));
    CHECK(!console_c6_id_valid("a.b"));
    CHECK(!console_c6_id_valid("FLASH0:x"));
    CHECK(!console_c6_id_valid("with space"));
    CHECK(!console_c6_id_valid("Upper"));
    CHECK(console_c6_id_valid("abcdefghijklmnopqrstuvwxyz0123456789_abcdefghijk")); // 48 chars
    CHECK(!console_c6_id_valid("abcdefghijklmnopqrstuvwxyz0123456789_abcdefghijkl"));

    // "c6log [ms]": how long to copy the C6 console, 10 s by default, at most a minute (#56)
    uint32_t ms = 0;
    CHECK(console_c6log_ms("", &ms) && ms == 10000);
    CHECK(console_c6log_ms("2500", &ms) && ms == 2500);
    CHECK(console_c6log_ms("60000", &ms) && ms == 60000);
    CHECK(!console_c6log_ms("60001", &ms));
    CHECK(!console_c6log_ms("0", &ms));
    CHECK(!console_c6log_ms("-5", &ms));
    CHECK(!console_c6log_ms("10s", &ms));
    CHECK(!console_c6log_ms("99999999999999999999", &ms));

    // "memhog <bytes-left> <seconds>": hold DMA RAM until only bytes-left are free, for 1-600 s (#60)
    uint32_t left, secs;
    CHECK(console_memhog_args("4096 30", &left, &secs) && left == 4096 && secs == 30);
    CHECK(console_memhog_args("0 1", &left, &secs) && left == 0 && secs == 1);
    CHECK(console_memhog_args("1048576 600", &left, &secs) && left == 1048576 && secs == 600);
    CHECK(!console_memhog_args("4096 601", &left, &secs));
    CHECK(!console_memhog_args("4096 0", &left, &secs));
    CHECK(!console_memhog_args("4096", &left, &secs));
    CHECK(!console_memhog_args("", &left, &secs));
    CHECK(!console_memhog_args("4096  30", &left, &secs));
    CHECK(!console_memhog_args("4k 30", &left, &secs));
    CHECK(!console_memhog_args("4096 30 1", &left, &secs));
    CHECK(!console_memhog_args("99999999999 30", &left, &secs));

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("console_cmd_test: all checks passed\n");
    return 0;
}
