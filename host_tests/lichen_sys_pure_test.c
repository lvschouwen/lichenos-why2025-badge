// Host test: the ESP-free half of lichen_sys_storage / lichen_sys_update / lichen_sys_factory (#227). The gate's calls
// must answer exactly what the components' own functions do, since the shell's screens now only see the gate.
#include "lichen_sys_factory.h"
#include "lichen_sys_storage.h"
#include "lichen_sys_update.h"

#include "factory_plan.h"
#include "ota_autooff.h"
#include "sd_policy.h"
#include "sdcard.h"

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

#define GIB (1024ULL * 1024 * 1024)

static void test_sd_text(void) {
    lichen_sd_info_t info = {.state = LICHEN_SD_MOUNTED, .total_bytes = 29 * GIB, .free_bytes = 28 * GIB, .free_known = true,
                             .freq_khz = 20000, .width = 4, .io_errors = 2, .generation = 3};
    strcpy(info.kind, "SDHC");
    strcpy(info.reason, "none");
    // filled by field, not by copying the gate's struct: a mirror that drifted would show here
    sd_info_t raw = {.state = SD_STATE_MOUNTED, .total_bytes = 29 * GIB, .free_bytes = 28 * GIB, .free_known = true,
                     .freq_khz = 20000, .width = 4, .io_errors = 2, .generation = 3};
    strcpy(raw.kind, "SDHC");
    strcpy(raw.reason, "none");
    CHECK(strcmp(LICHEN_SYS_SD_LICHEN_DIR, SD_LICHEN_DIR) == 0);
    char a[96], b[96];
    lichen_sys_sd_short_text(&info, a, sizeof(a));
    sd_short_text(&raw, b, sizeof(b));
    CHECK(strcmp(a, b) == 0 && a[0]);
    lichen_sys_sd_status_line(&info, a, sizeof(a));
    sd_status_line(&raw, b, sizeof(b));
    CHECK(strcmp(a, b) == 0 && strstr(a, "mounted"));
    info.state = LICHEN_SD_NO_CARD;
    raw.state = SD_STATE_NO_CARD;
    lichen_sys_sd_short_text(&info, a, sizeof(a));
    sd_short_text(&raw, b, sizeof(b));
    CHECK(strcmp(a, b) == 0);
    lichen_sys_sd_fmt_size(512ULL * 1024 * 1024, a, sizeof(a));
    CHECK(strcmp(a, "512 MB") == 0);
}

static void test_sd_io(void) {
    CHECK(lichen_sys_sd_io_from_errno(0) == LICHEN_SD_IO_OK);
    CHECK(lichen_sys_sd_io_from_errno(5) == LICHEN_SD_IO_FAILED);  // EIO
    CHECK(lichen_sys_sd_io_from_errno(28) == LICHEN_SD_IO_NEUTRAL); // ENOSPC: a full card is not a failing one
}

static void test_update(void) {
    uint8_t m = 0;
    for (int i = 0; i < 6; i++) {
        CHECK(lichen_sys_update_auto_off_next(m) == ota_autooff_next(m));
        m = ota_autooff_next(m);
    }
    CHECK(m == 10); // 0 -> 10 -> 30 -> 60 -> 120 -> 0 -> 10
    char a[16];
    lichen_sys_update_auto_off_text(0, a, sizeof(a));
    CHECK(strcmp(a, "Never") == 0);
    lichen_sys_update_auto_off_text(120, a, sizeof(a));
    CHECK(strcmp(a, "2 h") == 0);
}

static void test_factory(void) {
    CHECK(strcmp(lichen_sys_factory_level_name(LICHEN_FACTORY_SETTINGS), factory_level_name(FACTORY_SETTINGS)) == 0);
    CHECK(strcmp(lichen_sys_factory_level_name(LICHEN_FACTORY_ALL), factory_level_name(FACTORY_ALL)) == 0);
    lichen_fc_combo_t c;
    lichen_sys_factory_combo_init(&c);
    CHECK(c.state == LICHEN_FC_IDLE);
    CHECK(lichen_sys_factory_combo_key(&c, FACTORY_COMBO_KEY_A, true, 500) == LICHEN_FC_EV_NONE);
    CHECK(lichen_sys_factory_combo_key(&c, FACTORY_COMBO_KEY_B, true, 600) == LICHEN_FC_EV_NONE);
    CHECK(c.state == LICHEN_FC_HOLDING);
    CHECK(lichen_sys_factory_combo_tick(&c, 3500) == LICHEN_FC_EV_NONE);
    CHECK(lichen_sys_factory_combo_tick(&c, 3600) == LICHEN_FC_EV_ARM);
    CHECK(c.state == LICHEN_FC_ARMED && lichen_sys_factory_combo_swallows(&c));
    CHECK(lichen_sys_factory_combo_key(&c, FACTORY_COMBO_KEY_A, false, 3700) == LICHEN_FC_EV_NONE);
    CHECK(lichen_sys_factory_combo_key(&c, FACTORY_COMBO_KEY_B, false, 3700) == LICHEN_FC_EV_NONE);
    CHECK(lichen_sys_factory_combo_key(&c, FACTORY_COMBO_ENTER, true, 4700) == LICHEN_FC_EV_CONFIRM);
    CHECK(c.state == LICHEN_FC_OVER && !lichen_sys_factory_combo_swallows(&c));
    // the cancel path, through the timeout
    lichen_sys_factory_combo_init(&c);
    lichen_sys_factory_combo_key(&c, FACTORY_COMBO_KEY_A, true, 500);
    lichen_sys_factory_combo_key(&c, FACTORY_COMBO_KEY_B, true, 600);
    lichen_sys_factory_combo_tick(&c, 3600);
    CHECK(lichen_sys_factory_combo_tick(&c, 3600 + FACTORY_COMBO_CONFIRM_MS) == LICHEN_FC_EV_CANCEL);
    CHECK(c.state == LICHEN_FC_OVER);
}

int main(void) {
    test_sd_text();
    test_sd_io();
    test_update();
    test_factory();
    if (failures) {
        printf("lichen_sys_pure_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("lichen_sys_pure_test: all passed\n");
    return 0;
}
