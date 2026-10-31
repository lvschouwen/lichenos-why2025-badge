// Host test: the console command splitter built with the lean firmware's "fw " prefix (#61)
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

typedef struct {
    char stdin_out[128];
    char command[CONSOLE_CMD_MAX];
    int  n_commands;
} run_t;

static void feed(console_cmd_t *s, run_t *r, char const *text) {
    for (; *text; text++) {
        char const *pass;
        size_t      n;
        switch (console_cmd_feed(s, *text, &pass, &n)) {
            case CONSOLE_CMD_PASS: strncat(r->stdin_out, pass, n); break;
            case CONSOLE_CMD_READY:
                strcpy(r->command, s->buf);
                r->n_commands++;
                break;
            default: break;
        }
    }
}

int main(void) {
    console_cmd_t s;
    run_t         r;

    console_cmd_init(&s);
    memset(&r, 0, sizeof r);
    feed(&s, &r, "fw mem\n");
    CHECK(r.n_commands == 1);
    CHECK(strcmp(r.command, "mem") == 0);

    // BadgeVMS's prefix is not a command here
    console_cmd_init(&s);
    memset(&r, 0, sizeof r);
    feed(&s, &r, "badgevms mem\n");
    CHECK(r.n_commands == 0);
    CHECK(strcmp(r.stdin_out, "badgevms mem\n") == 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("console_cmd_fw_test: all checks passed\n");
    return 0;
}
