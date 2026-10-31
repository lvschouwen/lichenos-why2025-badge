// Host test: what the badge says about an update after the reboot into it (#76)
#include "update_status.h"

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
    char line[64];

    CHECK(update_result(NULL, "fw-b", false) == UPD_NONE);
    CHECK(update_result("", "fw-b", false) == UPD_NONE);
    CHECK(update_result("", "fw-b", true) == UPD_NONE);
    CHECK(update_result("fw-a", "fw-b", false) == UPD_UPDATED);
    CHECK(update_result("fw-b", "fw-a", true) == UPD_ROLLED_BACK);
    CHECK(update_result("fw-a", "fw-a", true) == UPD_ROLLED_BACK);
    CHECK(update_result("fw-a", "fw-a", false) == UPD_NONE);

    size_t n = update_line(UPD_UPDATED, "fw-a", "fw-b", line, sizeof(line));
    CHECK(strcmp(line, "Updated to fw-b") == 0 && n == strlen(line));
    n = update_line(UPD_ROLLED_BACK, "fw-b", "fw-a", line, sizeof(line));
    CHECK(strcmp(line, "Update failed, back on fw-a") == 0 && n == strlen(line));
    n = update_line(UPD_NONE, "fw-a", "fw-a", line, sizeof(line));
    CHECK(line[0] == '\0' && n == 0);

    // A long version is cut to the buffer, always terminated
    char small[12];
    n = update_line(UPD_UPDATED, "a", "v1.2.3-45-gabcdef0-dirty", small, sizeof(small));
    CHECK(strlen(small) == sizeof(small) - 1 && n == strlen(small));
    CHECK(strncmp(small, "Updated to ", 11) == 0);
    CHECK(update_line(UPD_UPDATED, "a", "b", small, 0) == 0);

    CHECK(update_pct(0, 0) == -1);
    CHECK(update_pct(5, 0) == -1);
    CHECK(update_pct(0, 1000) == 0);
    CHECK(update_pct(500, 1000) == 50);
    CHECK(update_pct(1000, 1000) == 100);
    CHECK(update_pct(2000, 1000) == 100);
    CHECK(update_pct(4000000, 4194304) == 95);

    // The stall limit: much longer once a firmware image is fully received (flash write and checks report nothing)
    CHECK(update_stall_limit_ms(true, 100, 1000) == 30000);
    CHECK(update_stall_limit_ms(true, 1000, 1000) == 180000);
    CHECK(update_stall_limit_ms(false, 1000, 1000) == 30000);
    CHECK(update_stall_limit_ms(true, 0, 0) == 30000);

    // After boot: show the line at once, forget the stored version only once the new image has proved itself
    upd_boot_t b = update_after_boot("", "fw-b", false, false);
    CHECK(b.result == UPD_NONE && !b.erase);
    b = update_after_boot("fw-a", "fw-b", false, false); // on trial: a crash now rolls back and must still be told
    CHECK(b.result == UPD_UPDATED && !b.erase);
    b = update_after_boot("fw-a", "fw-b", false, true);
    CHECK(b.result == UPD_UPDATED && b.erase);
    b = update_after_boot("fw-b", "fw-a", true, true);
    CHECK(b.result == UPD_ROLLED_BACK && b.erase);
    b = update_after_boot("fw-a", "fw-a", false, true); // nothing to say, nothing to keep
    CHECK(b.result == UPD_NONE && b.erase);
    b = update_after_boot("fw-a", "fw-a", false, false);
    CHECK(b.result == UPD_NONE && b.erase);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("update_status_test: ok\n");
    return 0;
}
