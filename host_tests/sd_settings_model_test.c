// Host tests for Settings > SD card's status line and actions (#99)
#include "sd_settings_model.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define CHECK_STR(got, want)                                                                                           \
    do {                                                                                                               \
        if (strcmp((got), (want)) != 0) {                                                                              \
            printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (got), (want));                               \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define GIB (1024ULL * 1024 * 1024)

static sd_info_t card(sd_state_t state) {
    sd_info_t info = {.state = state, .total_bytes = 29 * GIB + 7 * GIB / 10 + 1, .free_bytes = 28 * GIB + GIB / 10 + 1,
                      .free_known = true};
    strcpy(info.kind, "SDHC");
    return info;
}

static void status_per_state(void) {
    char      out[64];
    sd_info_t info = card(SD_STATE_MOUNTED);
    sds_status(&info, SDS_IDLE, out, sizeof(out));
    CHECK_STR(out, "Mounted: SDHC 29.7 GB, 28.1 GB free");
    info.free_known = false;
    sds_status(&info, SDS_IDLE, out, sizeof(out));
    CHECK_STR(out, "Mounted: SDHC 29.7 GB");

    sd_info_t none = {.state = SD_STATE_NO_CARD};
    sds_status(&none, SDS_IDLE, out, sizeof(out));
    CHECK_STR(out, "No card found");

    info = card(SD_STATE_NOT_FAT);
    sds_status(&info, SDS_IDLE, out, sizeof(out));
    CHECK_STR(out, "SDHC 29.7 GB: not FAT (exFAT or blank)");
    sd_info_t bare = {.state = SD_STATE_NOT_FAT};
    sds_status(&bare, SDS_IDLE, out, sizeof(out));
    CHECK_STR(out, "Card not FAT (exFAT or blank)");

    sd_info_t err = {.state = SD_STATE_ERROR};
    sds_status(&err, SDS_IDLE, out, sizeof(out));
    CHECK_STR(out, "Error: unknown");
    strcpy(err.reason, "mount failed (FR 1)");
    sds_status(&err, SDS_IDLE, out, sizeof(out));
    CHECK_STR(out, "Error: mount failed (FR 1)");

    sd_info_t off = {.state = SD_STATE_OFF};
    sds_status(&off, SDS_IDLE, out, sizeof(out));
    CHECK_STR(out, "Not mounted");
    sd_info_t removed = {.state = SD_STATE_REMOVED};
    sds_status(&removed, SDS_IDLE, out, sizeof(out));
    CHECK_STR(out, "Unmounted after I/O errors");
}

static void status_while_busy(void) {
    char      out[64];
    sd_info_t info = card(SD_STATE_MOUNTED);
    sds_status(&info, SDS_FORMATTING, out, sizeof(out));
    CHECK_STR(out, "Formatting... keep the card in");
    sds_status(&info, SDS_UNMOUNTING, out, sizeof(out));
    CHECK_STR(out, "Unmounting...");
    sd_info_t off = {.state = SD_STATE_OFF};
    sds_status(&off, SDS_MOUNTING, out, sizeof(out));
    CHECK_STR(out, "Mounting...");
}

static void status_truncates(void) {
    char      out[8];
    sd_info_t info = card(SD_STATE_MOUNTED);
    sds_status(&info, SDS_IDLE, out, sizeof(out));
    CHECK_STR(out, "Mounted");
}

static void actions_per_state(void) {
    sds_actions_t a = sds_actions(SD_STATE_MOUNTED, SDS_IDLE);
    CHECK_STR(a.mount_label, "Unmount");
    CHECK(a.mount_enabled && a.format_enabled);
    CHECK(sds_mount_job(SD_STATE_MOUNTED) == SDS_UNMOUNTING);

    sd_state_t const no_card[] = {SD_STATE_OFF, SD_STATE_NO_CARD, SD_STATE_REMOVED};
    for (size_t i = 0; i < sizeof(no_card) / sizeof(no_card[0]); i++) {
        a = sds_actions(no_card[i], SDS_IDLE);
        CHECK_STR(a.mount_label, "Mount");
        CHECK(a.mount_enabled);
        CHECK(!a.format_enabled); // nothing answered: never a format into the blue
        CHECK_STR(a.format_value, "needs a card");
        CHECK(sds_mount_job(no_card[i]) == SDS_MOUNTING);
    }

    a = sds_actions(SD_STATE_NOT_FAT, SDS_IDLE);
    CHECK_STR(a.mount_label, "Mount");
    CHECK(a.mount_enabled && a.format_enabled);
    a = sds_actions(SD_STATE_ERROR, SDS_IDLE);
    CHECK(a.mount_enabled && a.format_enabled);
}

static void busy_disables_everything(void) {
    sds_job_t const jobs[] = {SDS_MOUNTING, SDS_UNMOUNTING, SDS_FORMATTING};
    for (size_t i = 0; i < sizeof(jobs) / sizeof(jobs[0]); i++) {
        sds_actions_t a = sds_actions(SD_STATE_MOUNTED, jobs[i]);
        CHECK(!a.mount_enabled && !a.format_enabled);
        CHECK_STR(a.mount_value, "wait");
        CHECK_STR(a.format_value, "wait");
    }
}

static void results(void) {
    char      out[64];
    sd_info_t ok = card(SD_STATE_MOUNTED);
    sds_result(SDS_MOUNTING, true, &ok, out, sizeof(out));
    CHECK_STR(out, "Mounted");
    sds_result(SDS_FORMATTING, true, &ok, out, sizeof(out));
    CHECK_STR(out, "Formatted as LICHEN and mounted");
    sds_result(SDS_FORMATTING, false, &ok, out, sizeof(out)); // the card's lock was not free: still mounted
    CHECK_STR(out, "Format failed: card busy, try again");

    sd_info_t none = {.state = SD_STATE_NO_CARD};
    sds_result(SDS_MOUNTING, false, &none, out, sizeof(out));
    CHECK_STR(out, "Mount failed: no card");
    sd_info_t raw = {.state = SD_STATE_NOT_FAT};
    sds_result(SDS_MOUNTING, false, &raw, out, sizeof(out));
    CHECK_STR(out, "Mount failed: not FAT. Format makes it LICHEN");
    sd_info_t err = {.state = SD_STATE_ERROR};
    strcpy(err.reason, "format failed (FR 3)");
    sds_result(SDS_FORMATTING, false, &err, out, sizeof(out));
    CHECK_STR(out, "Format failed: format failed (FR 3)");

    sd_info_t off = {.state = SD_STATE_OFF};
    sds_result(SDS_UNMOUNTING, true, &off, out, sizeof(out));
    CHECK_STR(out, "Unmounted: safe to remove the card");
    sds_result(SDS_UNMOUNTING, false, &ok, out, sizeof(out));
    CHECK_STR(out, "Unmount failed: card busy, try again");
    sds_result(SDS_IDLE, true, &ok, out, sizeof(out));
    CHECK_STR(out, "");
}

int main(void) {
    status_per_state();
    status_while_busy();
    status_truncates();
    actions_per_state();
    busy_disables_everything();
    results();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("sd_settings_model_test: all passed\n");
    return 0;
}
