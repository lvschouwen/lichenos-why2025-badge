// Host test: the OS name on the boot screen and in About (#236)
#include "os_name.h"

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

int main(void) {
    CHECK(strcmp(lichen_os_name(), "Lichen OS 2") == 0);
    char line[48];
    CHECK(strcmp(lichen_os_version_line("fw-abc1234", line, sizeof(line)), "Lichen OS 2 (fw-abc1234)") == 0);
    CHECK(strcmp(lichen_os_version_line(NULL, line, sizeof(line)), "Lichen OS 2 (?)") == 0);
    char tiny[8];
    CHECK(strcmp(lichen_os_version_line("fw-abc1234", tiny, sizeof(tiny)), "Lichen ") == 0);
    CHECK(lichen_os_version_line("x", tiny, 0) == tiny);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("os_name_test: all passed\n");
    return 0;
}
