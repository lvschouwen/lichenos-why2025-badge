// Host test: log line cleaning for the boot screen (#66)
#include "log_clean.h"

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

static char out[128];

static size_t run(char const *in) {
    return log_clean(in, strlen(in), out);
}

int main(void) {
    CHECK(run("\033[0;33mW (123) mesh: hi\033[0m") == 16);
    CHECK(strcmp(out, "W (123) mesh: hi") == 0);

    CHECK(run("plain") == 5 && strcmp(out, "plain") == 0);

    // Unterminated sequence at the end: dropped, nothing after it survives
    CHECK(run("abc\033[") == 3 && strcmp(out, "abc") == 0);
    CHECK(run("abc\033[0;3") == 3 && strcmp(out, "abc") == 0);
    // A lone ESC not followed by [ is kept
    CHECK(run("a\033b") == 3);

    CHECK(run("one\ntwo\r\n") == 6 && strcmp(out, "onetwo") == 0);

    CHECK(run("\033[0m\r\n") == 0 && out[0] == '\0');
    CHECK(run("") == 0 && out[0] == '\0');

    // In place
    char buf[32] = "\033[0;32mI ok\033[0m\n";
    CHECK(log_clean(buf, strlen(buf), buf) == 4 && strcmp(buf, "I ok") == 0);

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
