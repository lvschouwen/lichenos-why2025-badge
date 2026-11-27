// Host test: the SD card's rules (#60): mount outcomes and power, the clock fallback (#583), the I/O error run, sizes,
// status texts, format word
#include "sd_policy.h"

#include <errno.h>
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

static void test_mount_plan(void) {
    sd_mount_plan_t p = sd_policy_after_mount(SD_MOUNT_OK);
    CHECK(p.state == SD_STATE_MOUNTED && p.keep_power);
    // Every failure leaves the card unpowered (Lucas, #60)
    p = sd_policy_after_mount(SD_MOUNT_NO_CARD);
    CHECK(p.state == SD_STATE_NO_CARD && !p.keep_power);
    p = sd_policy_after_mount(SD_MOUNT_NOT_FAT);
    CHECK(p.state == SD_STATE_NOT_FAT && !p.keep_power);
    p = sd_policy_after_mount(SD_MOUNT_ERROR);
    CHECK(p.state == SD_STATE_ERROR && !p.keep_power);
}

// #583: a card that took High Speed and failed at 50 MHz (card init, or f_mount's first reads) gets one retry at 20;
// a missing card, a card without FAT, a card that refused High Speed and a 20 MHz try get none
static void test_clock_fallback(void) {
    CHECK(sd_policy_fallback_khz(SD_CLOCK_HS_KHZ, SD_CLOCK_HS_KHZ, SD_MOUNT_ERROR) == SD_CLOCK_DEFAULT_KHZ);
    CHECK(sd_policy_fallback_khz(SD_CLOCK_HS_KHZ, SD_CLOCK_HS_KHZ, SD_MOUNT_NO_CARD) == SD_CLOCK_DEFAULT_KHZ);
    CHECK(sd_policy_fallback_khz(SD_CLOCK_HS_KHZ, SD_CLOCK_HS_KHZ, SD_MOUNT_OK) == 0);
    CHECK(sd_policy_fallback_khz(SD_CLOCK_HS_KHZ, SD_CLOCK_HS_KHZ, SD_MOUNT_NOT_FAT) == 0);
    CHECK(sd_policy_fallback_khz(SD_CLOCK_HS_KHZ, SD_CLOCK_DEFAULT_KHZ, SD_MOUNT_ERROR) == 0); // refused HS: at 20
    CHECK(sd_policy_fallback_khz(SD_CLOCK_HS_KHZ, 0, SD_MOUNT_NO_CARD) == 0);                  // never answered
    CHECK(sd_policy_fallback_khz(SD_CLOCK_DEFAULT_KHZ, SD_CLOCK_DEFAULT_KHZ, SD_MOUNT_ERROR) == 0);
    CHECK(SD_CLOCK_HS_KHZ == 50000 && SD_CLOCK_DEFAULT_KHZ == 20000);
}

// #583: the live clock from the registers: 200 / 4 = 50 MHz, 160 / 4 = 40 (the stuck state), the probe's 400 kHz
static void test_bus_clock(void) {
    CHECK(sd_bus_khz(200, 4, 0) == 50000);
    CHECK(sd_bus_khz(160, 4, 0) == 40000);
    CHECK(sd_bus_khz(160, 8, 0) == 20000);
    CHECK(sd_bus_khz(160, 10, 20) == 400);
    CHECK(sd_bus_khz(200, 0, 0) == 0);
    char line[96];
    sd_bus_line(200, 4, 0, 0, line, sizeof(line));
    CHECK(strcmp(line, "sdmmc live: 200 MHz / 4, slot 0 50000 kHz, slot 1 50000 kHz") == 0);
    sd_bus_line(160, 10, 20, 0, line, sizeof(line));
    CHECK(strcmp(line, "sdmmc live: 160 MHz / 10, slot 0 400 kHz, slot 1 16000 kHz") == 0);
}

static void test_io_run(void) {
    sd_io_count_t c = {0};
    CHECK(!sd_policy_io(&c, SD_IO_FAILED));
    CHECK(!sd_policy_io(&c, SD_IO_FAILED));
    CHECK(!sd_policy_io(&c, SD_IO_OK)); // a good batch resets the run, not the total
    CHECK(c.in_row == 0 && c.total == 2);
    CHECK(!sd_policy_io(&c, SD_IO_FAILED));
    CHECK(!sd_policy_io(&c, SD_IO_FAILED));
    CHECK(sd_policy_io(&c, SD_IO_FAILED)); // the third in a row: unmount now
    CHECK(c.total == 5);

    // Neutral batches neither count nor reset: a full card never unmounts a healthy one, and does not hide a dying one
    sd_io_count_t n = {0};
    for (int i = 0; i < 10; i++) {
        CHECK(!sd_policy_io(&n, SD_IO_NEUTRAL));
    }
    CHECK(n.in_row == 0 && n.total == 0);
    CHECK(!sd_policy_io(&n, SD_IO_FAILED));
    CHECK(!sd_policy_io(&n, SD_IO_FAILED));
    CHECK(!sd_policy_io(&n, SD_IO_NEUTRAL));
    CHECK(n.in_row == 2);
    CHECK(sd_policy_io(&n, SD_IO_FAILED));
}

static void test_errno(void) {
    CHECK(sd_io_from_errno(0) == SD_IO_OK);
    CHECK(sd_io_from_errno(EIO) == SD_IO_FAILED);       // FR_DISK_ERR, FR_INT_ERR
    CHECK(sd_io_from_errno(ENODEV) == SD_IO_FAILED);    // FR_NOT_READY: the status check found no card
    CHECK(sd_io_from_errno(ETIMEDOUT) == SD_IO_FAILED);
    CHECK(sd_io_from_errno(ENOSPC) == SD_IO_NEUTRAL);   // full
    CHECK(sd_io_from_errno(ENAMETOOLONG) == SD_IO_NEUTRAL);
    CHECK(sd_io_from_errno(EROFS) == SD_IO_NEUTRAL);
    CHECK(sd_io_from_errno(ENOENT) == SD_IO_NEUTRAL);
    CHECK(sd_io_from_errno(ENOMEM) == SD_IO_NEUTRAL);
    CHECK(sd_io_from_errno(EEXIST) == SD_IO_NEUTRAL);
}

static void test_sizes(void) {
    char t[16];
    sd_fmt_size(31914983424ULL, t, sizeof(t)); // a "32 GB" card
    CHECK(strcmp(t, "29.7 GB") == 0);
    sd_fmt_size(512ULL << 20, t, sizeof(t));
    CHECK(strcmp(t, "512 MB") == 0);
    sd_fmt_size(1ULL << 30, t, sizeof(t));
    CHECK(strcmp(t, "1.0 GB") == 0);
    sd_fmt_size((1ULL << 30) - 1, t, sizeof(t)); // never rounded up into the next unit
    CHECK(strcmp(t, "1023 MB") == 0);
    sd_fmt_size(12 * 1024, t, sizeof(t));
    CHECK(strcmp(t, "12 KB") == 0);
    sd_fmt_size(0, t, sizeof(t));
    CHECK(strcmp(t, "0 KB") == 0);
    sd_fmt_size(2000ULL << 30, t, sizeof(t));
    CHECK(strcmp(t, "2000.0 GB") == 0);
}

static void test_kind(void) {
    CHECK(strcmp(sd_card_kind(false, false, 2000000000ULL), "SDSC") == 0);
    CHECK(strcmp(sd_card_kind(false, true, 31914983424ULL), "SDHC") == 0);
    CHECK(strcmp(sd_card_kind(false, true, 63864569856ULL), "SDXC") == 0);
    CHECK(strcmp(sd_card_kind(true, true, 8000000000ULL), "MMC") == 0);
}

static void test_texts(void) {
    char      line[160];
    sd_info_t i = {.state = SD_STATE_MOUNTED, .total_bytes = 31914983424ULL, .free_bytes = 30172626944ULL,
                   .free_known = true, .kind = "SDHC", .freq_khz = 20000, .width = 4, .io_errors = 0};
    sd_status_line(&i, line, sizeof(line));
    CHECK(strcmp(line, "sd mounted /sd 29.7 GB, 28.1 GB free, SDHC 20 MHz 4-bit, errors 0") == 0);
    sd_short_text(&i, line, sizeof(line));
    CHECK(strcmp(line, "28.1 GB free of 29.7 GB") == 0);

    i.free_known = false;
    sd_status_line(&i, line, sizeof(line));
    CHECK(strstr(line, "29.7 GB, ? free") != NULL);
    sd_short_text(&i, line, sizeof(line));
    CHECK(strcmp(line, "29.7 GB") == 0);

    i.state = SD_STATE_NO_CARD;
    sd_status_line(&i, line, sizeof(line));
    CHECK(strncmp(line, "sd no card", 10) == 0);
    sd_short_text(&i, line, sizeof(line));
    CHECK(strcmp(line, "no card") == 0);

    i.state       = SD_STATE_NOT_FAT;
    i.total_bytes = 63864569856ULL;
    strcpy(i.kind, "SDXC");
    sd_status_line(&i, line, sizeof(line));
    CHECK(strcmp(line, "sd card SDXC 59.4 GB has no FAT (exFAT or blank): fw sd format ERASE erases it") == 0);

    i.state = SD_STATE_ERROR;
    strcpy(i.reason, "mount failed (FR 1)");
    sd_status_line(&i, line, sizeof(line));
    CHECK(strcmp(line, "sd error: mount failed (FR 1)") == 0);
    i.reason[0] = '\0';
    sd_status_line(&i, line, sizeof(line));
    CHECK(strcmp(line, "sd error: unknown") == 0);

    i.state = SD_STATE_REMOVED;
    sd_status_line(&i, line, sizeof(line));
    CHECK(strcmp(line, "sd removed after 3 I/O errors in a row (fw sd mount to try again)") == 0);
    sd_short_text(&i, line, sizeof(line));
    CHECK(strcmp(line, "removed") == 0);

    i.state = SD_STATE_OFF;
    sd_status_line(&i, line, sizeof(line));
    CHECK(strcmp(line, "sd unmounted") == 0);

    // A short buffer cuts, never overflows
    char small[8];
    i.state = SD_STATE_MOUNTED;
    sd_status_line(&i, small, sizeof(small));
    CHECK(strlen(small) == 7);
}

static void test_confirm(void) {
    CHECK(sd_format_confirmed("ERASE"));
    CHECK(!sd_format_confirmed("erase"));
    CHECK(!sd_format_confirmed(""));
    CHECK(!sd_format_confirmed(NULL));
    CHECK(!sd_format_confirmed("ERASE "));
    CHECK(!sd_format_confirmed("ERASEX"));
    CHECK(!sd_format_confirmed("yes"));
}

int main(void) {
    test_mount_plan();
    test_clock_fallback();
    test_bus_clock();
    test_io_run();
    test_errno();
    test_sizes();
    test_kind();
    test_texts();
    test_confirm();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
