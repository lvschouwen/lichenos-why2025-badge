// "fw sd" (#60): status, mount, unmount, a small write/read test and the explicit format
#include "sdcard.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fw_console.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_PATH  SD_LICHEN_DIR "/.sdtest"
#define TEST_BYTES (64 * 1024)
#define TEST_CHUNK 4096

static void reply_status(void) {
    if (sd_begin(2000)) {
        sd_end(true); // refreshes the free space, here on the console task rather than on LVGL's
    }
    sd_info_t info;
    sd_info(&info);
    char line[160];
    sd_status_line(&info, line, sizeof(line));
    console_reply("%s", line);
}

static uint8_t pattern(size_t i) {
    return (uint8_t)(i * 31u + 7u + (i >> 9));
}

static double mb_per_s(size_t bytes, int64_t us) {
    return us > 0 ? (double)bytes / (double)us : 0.0; // bytes per µs = MB/s
}

// Write, fsync, read back and compare TEST_BYTES; the file is removed again. Holds the card's mutex throughout.
static void cmd_test(void) {
    if (!sd_begin(2000)) {
        console_reply("sd test: card not mounted (or busy)");
        return;
    }
    uint8_t *buf = heap_caps_malloc(TEST_CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        sd_end(true);
        console_reply("sd test: out of memory");
        return;
    }
    char const *failed = NULL;
    int         err    = 0;
    int64_t     t0     = esp_timer_get_time();
    FILE       *f      = fopen(TEST_PATH, "wb");
    for (size_t off = 0; f && !failed && off < TEST_BYTES; off += TEST_CHUNK) {
        for (size_t i = 0; i < TEST_CHUNK; i++) {
            buf[i] = pattern(off + i);
        }
        if (fwrite(buf, 1, TEST_CHUNK, f) != TEST_CHUNK) {
            failed = "write";
            err    = errno;
        }
    }
    if (!f) {
        failed = "create";
        err    = errno;
    } else {
        if (!failed && (fflush(f) != 0 || fsync(fileno(f)) != 0)) {
            failed = "flush";
            err    = errno;
        }
        fclose(f);
    }
    int64_t t1 = esp_timer_get_time();
    f          = failed ? NULL : fopen(TEST_PATH, "rb");
    if (!failed && !f) {
        failed = "open";
        err    = errno;
    }
    for (size_t off = 0; f && !failed && off < TEST_BYTES; off += TEST_CHUNK) {
        if (fread(buf, 1, TEST_CHUNK, f) != TEST_CHUNK) {
            failed = "read";
            err    = errno;
            break;
        }
        for (size_t i = 0; i < TEST_CHUNK; i++) {
            if (buf[i] != pattern(off + i)) {
                failed = "compare";
                break;
            }
        }
    }
    if (f) {
        fclose(f);
    }
    int64_t t2 = esp_timer_get_time();
    unlink(TEST_PATH);
    heap_caps_free(buf);
    bool cmp_only = failed && strcmp(failed, "compare") == 0; // the card answered: count it as I/O that worked
    sd_end(!failed || cmp_only);
    if (failed) {
        console_reply("sd test failed at %s (errno %d)", failed, err);
        return;
    }
    console_reply("sd test ok: %u KB written at %.2f MB/s, read back at %.2f MB/s", TEST_BYTES / 1024,
                  mb_per_s(TEST_BYTES, t1 - t0), mb_per_s(TEST_BYTES, t2 - t1));
}

static void cmd_format(char const *word) {
    if (!sd_format_confirmed(word)) {
        console_reply("sd format erases everything on the card: fw sd format " SD_FORMAT_CONFIRM);
        return;
    }
    console_reply("sd formatting...");
    sd_format();
    reply_status();
}

static void cmd_sd(char const *args) {
    if (args[0] == '\0' || strcmp(args, "status") == 0) {
        reply_status();
    } else if (strcmp(args, "mount") == 0) {
        sd_mount();
        reply_status();
    } else if (strcmp(args, "unmount") == 0) {
        sd_unmount();
        reply_status();
    } else if (strcmp(args, "test") == 0) {
        cmd_test();
    } else if (strcmp(args, "format") == 0) {
        cmd_format("");
    } else if (strncmp(args, "format ", 7) == 0) {
        cmd_format(args + 7);
    } else {
        console_reply("sd [status | mount | unmount | test | format " SD_FORMAT_CONFIRM "]");
    }
}

void sd_console_register(void) {
    console_register("sd", "sd [status | mount | unmount | test | format " SD_FORMAT_CONFIRM "] (the SD card at /sd, #60)",
                     cmd_sd);
}
