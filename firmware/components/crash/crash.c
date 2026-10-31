#include "crash.h"

#include "crash_log.h"
#include "esp_core_dump.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "nvs.h"
#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CRASH_DIR    STORAGE_BASE "/crash"
#define CRASH_LOG    CRASH_DIR "/log.txt"
#define KEEP_RECORDS 20
#define LOG_MAX      (KEEP_RECORDS * 256 + 256)
#define B64_LINE     76

static char const TAG[] = "crash";

static bool s_recovered;
static int  s_reset_reason = -1; // esp_reset_reason() of this boot, for fw crash

static crash_reset_t crash_reset_of(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_PANIC: return CRASH_RST_PANIC;
        case ESP_RST_INT_WDT: return CRASH_RST_INT_WDT;
        case ESP_RST_TASK_WDT: return CRASH_RST_TASK_WDT;
        case ESP_RST_WDT: return CRASH_RST_WDT;
        case ESP_RST_BROWNOUT: return CRASH_RST_BROWNOUT;
        default: return CRASH_RST_OTHER;
    }
}

static char const *reason_name(crash_reset_t r) {
    switch (r) {
        case CRASH_RST_PANIC: return "panic";
        case CRASH_RST_INT_WDT: return "int_wdt";
        case CRASH_RST_TASK_WDT: return "task_wdt";
        case CRASH_RST_WDT: return "wdt";
        case CRASH_RST_BROWNOUT: return "brownout";
        default: return "other";
    }
}

// Transient buffers live in PSRAM: internal DMA memory is scarce and the display needs it
static void *psram_alloc(size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

static esp_partition_t const *dump_partition(void) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
}

// Where the dump on flash is, as an offset into the coredump partition; false when there is none or its size is not
// believable (the size field comes from flash)
static bool dump_location(esp_partition_t const **part, size_t *off, size_t *size) {
    size_t addr = 0;
    *part       = dump_partition();
    if (!*part || esp_core_dump_image_get(&addr, size) != ESP_OK || esp_core_dump_image_check() != ESP_OK) {
        return false;
    }
    if (*size == 0 || *size > (*part)->size || addr < (*part)->address || addr - (*part)->address > (*part)->size - *size) {
        return false;
    }
    *off = addr - (*part)->address;
    return true;
}

// A dump is fresh when it differs from the one of the last recorded crash: the CRC32 of its trailing SHA256 (it covers
// the whole dump) is kept in NVS when a crash is recorded. (Two crashes with byte-identical dumps would count once.)
static bool dump_sum(uint32_t *sum) {
    esp_partition_t const *part;
    size_t                 off, size;
    uint8_t                tail[32];
    if (!dump_location(&part, &off, &size) || size < sizeof(tail) ||
        esp_partition_read(part, off + size - sizeof(tail), tail, sizeof(tail)) != ESP_OK) {
        return false;
    }
    *sum = esp_rom_crc32_le(0, tail, sizeof(tail));
    return true;
}

static uint32_t stored_dump_sum(void) {
    nvs_handle_t nvs;
    uint32_t     v = 0;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, "dump_sum", &v);
        nvs_close(nvs);
    }
    return v;
}

static void store_dump_sum(uint32_t v) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, "dump_sum", v);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// Reads the whole log (it is small); returns its length
static size_t read_log(char *buf, size_t cap) {
    buf[0]  = '\0';
    FILE *f = fopen(CRASH_LOG, "r");
    if (!f) {
        return 0;
    }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n;
}

static void append_record(char const *record) {
    mkdir(CRASH_DIR, 0777); // exists after the first crash
    char *buf = psram_alloc(LOG_MAX);
    if (!buf) {
        return;
    }
    size_t len = read_log(buf, LOG_MAX - 256);
    if (len && buf[len - 1] != '\n') {
        buf[len++] = '\n';
    }
    len += (size_t)snprintf(buf + len, LOG_MAX - len, "%s\n", record);
    len = crash_log_trim(buf, len, KEEP_RECORDS);
    FILE *f = fopen(CRASH_LOG, "w");
    if (f) {
        fwrite(buf, 1, len, f);
        fclose(f);
    } else {
        ESP_LOGE(TAG, "cannot write %s: errno %d", CRASH_LOG, errno);
    }
    free(buf);
}

static uint32_t bump_count(void) {
    nvs_handle_t nvs;
    uint32_t     count = 0;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return 0;
    }
    nvs_get_u32(nvs, "crash_count", &count);
    count++;
    nvs_set_u32(nvs, "crash_count", count);
    nvs_commit(nvs);
    nvs_close(nvs);
    return count;
}

static uint32_t get_count(void) {
    nvs_handle_t nvs;
    uint32_t     count = 0;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, "crash_count", &count);
        nvs_close(nvs);
    }
    return count;
}

// An image bigger than its slot would be cut by the partition table; say so at every boot
static void check_image_fits(void) {
    esp_partition_t const *slot = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    esp_partition_t const *run  = esp_ota_get_running_partition();
    esp_image_metadata_t   meta;
    if (!slot || !run) {
        return;
    }
    esp_partition_pos_t const pos = {.offset = run->address, .size = run->size};
    if (esp_image_get_metadata(&pos, &meta) == ESP_OK && meta.image_len > slot->size) {
        ESP_LOGE(TAG, "the running image (%u bytes) is bigger than ota_1 (%u bytes): updates to that slot will not fit",
                 (unsigned)meta.image_len, (unsigned)slot->size);
    }
}

static void show(void) {
    esp_partition_t const *part;
    size_t                 off, size;
    console_reply("crash: %u since the last clear, this boot %s (reset reason %d)", (unsigned)get_count(),
                  s_recovered ? "follows a crash" : "is clean", s_reset_reason);
    if (dump_location(&part, &off, &size)) {
        console_reply("crash: core dump on flash, %u bytes of %u (fw crash dump)", (unsigned)size, (unsigned)part->size);
    } else {
        console_reply("crash: no core dump on flash");
    }
    char *buf = psram_alloc(LOG_MAX);
    if (!buf) {
        return;
    }
    size_t len = read_log(buf, LOG_MAX);
    if (len == 0) {
        console_reply("crash: log is empty");
    }
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        console_reply("crash: %s", line);
    }
    free(buf);
}

// Core dump as base64 between markers; tools/badge.sh crash decodes it. The CRC32 is over the raw bytes.
// Flash into PSRAM goes through a temporary internal buffer as big as the read; small reads keep that small
static bool read_dump(esp_partition_t const *part, size_t off, uint8_t *dst, size_t size) {
    uint8_t chunk[512];
    for (size_t done = 0; done < size;) {
        size_t n = size - done > sizeof(chunk) ? sizeof(chunk) : size - done;
        if (esp_partition_read(part, off + done, chunk, n) != ESP_OK) {
            return false;
        }
        memcpy(dst + done, chunk, n);
        done += n;
    }
    return true;
}

static void dump(void) {
    esp_partition_t const *part;
    size_t                 off, size;
    if (!dump_location(&part, &off, &size)) {
        console_reply("crash: no usable core dump on flash");
        return;
    }
    size_t   b64_cap = size / 3 * 4 + 8 + size / 57 + 8;
    uint8_t *raw     = psram_alloc(size);
    char    *b64     = psram_alloc(b64_cap);
    size_t   n       = 0;
    if (!raw || !b64 || !read_dump(part, off, raw, size) ||
        (n = crash_b64_lines(raw, size, b64, b64_cap, B64_LINE)) == 0) {
        console_reply("crash: cannot read or encode the dump");
        free(raw);
        free(b64);
        return;
    }
    uint32_t crc = esp_rom_crc32_le(0, raw, size);
    // After a rollback the running image is not the one that crashed: the dump names the crashed image by its ELF SHA
    char                     elf[APP_ELF_SHA256_SZ] = "";
    esp_core_dump_summary_t *info                   = psram_alloc(sizeof(*info));
    if (info && esp_core_dump_get_summary(info) == ESP_OK) {
        crash_elf_sha_clean(elf, sizeof(elf), (char const *)info->app_elf_sha256);
    }
    free(info);
    console_reply("crash: elf %s", elf[0] ? elf : "?");
    console_reply("crash: version %s", esp_app_get_description()->version); // the running image's
    printf("--- coredump begin %u ---\n", (unsigned)size);
    // In pieces: the console UART driver and stdout are happier than with one 170 KB write
    for (size_t o = 0; o < n;) {
        size_t chunk = n - o > 4096 ? 4096 : n - o;
        fwrite(b64 + o, 1, chunk, stdout);
        fflush(stdout);
        o += chunk;
        vTaskDelay(1);
    }
    printf("\n--- coredump end %08x ---\n", (unsigned)crc);
    free(raw);
    free(b64);
}

static void crash_test_panic(void) __attribute__((noinline, noreturn));
static void crash_test_panic(void) {
    console_reply("crash: test panic, the badge restarts");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    abort();
}

// Interrupts off on core 0 (the console task's core): the interrupt watchdog fires
static void crash_test_wdt(void) __attribute__((noinline, noreturn));
static void crash_test_wdt(void) {
    console_reply("crash: test wdt, the badge restarts");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    portDISABLE_INTERRUPTS();
    for (;;) {
        __asm__ volatile("nop");
    }
}

static void crash_command(char const *args) {
    if (*args == '\0') {
        show();
    } else if (strcmp(args, "dump") == 0) {
        dump();
    } else if (strcmp(args, "clear") == 0) {
        esp_err_t err = esp_core_dump_image_erase();
        unlink(CRASH_LOG);
        nvs_handle_t nvs;
        if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_key(nvs, "crash_count");
            nvs_erase_key(nvs, "dump_sum");
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        console_reply(err == ESP_OK ? "crash: cleared" : "crash: erasing the core dump failed");
    } else if (strcmp(args, "test panic") == 0) {
        crash_test_panic();
    } else if (strcmp(args, "test wdt") == 0) {
        crash_test_wdt();
    } else {
        console_reply("crash [dump | clear | test panic|wdt]");
    }
}

void crash_check(void) {
    check_image_fits();
    console_register("crash", "crash [dump | clear | test panic|wdt] (crash log and core dump)", crash_command);
    esp_reset_reason_t const why    = esp_reset_reason();
    crash_reset_t const      reason = crash_reset_of(why);
    s_reset_reason = (int)why;
    ESP_LOGW(TAG, "reset reason %d (%s)", (int)why, reason_name(reason));

    uint32_t sum   = 0;
    bool     fresh = dump_sum(&sum) && sum != stored_dump_sum();
    if (!crash_is_crash(reason, fresh)) {
        if (reason == CRASH_RST_WDT) {
            ESP_LOGI(TAG, "watchdog reset without a new core dump: a USB reset, not recorded");
        }
        return;
    }
    s_recovered = true;

    char     task[16] = "?";
    char     elf[APP_ELF_SHA256_SZ] = "";
    uint32_t pc       = 0;
    if (fresh) {
        esp_core_dump_summary_t *sum_info = psram_alloc(sizeof(*sum_info));
        if (sum_info && esp_core_dump_get_summary(sum_info) == ESP_OK) {
            strlcpy(task, sum_info->exc_task, sizeof(task));
            pc = sum_info->exc_pc;
            crash_elf_sha_clean(elf, sizeof(elf), (char const *)sum_info->app_elf_sha256); // the crashed image
        }
        free(sum_info);
        store_dump_sum(sum);
    }
    uint32_t count = bump_count();
    char     when[32];
    time_t   now = time(NULL);
    if (now > 1700000000) { // the clock is set only once Wi-Fi time arrived; otherwise number the crash
        strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S", gmtime(&now));
    } else {
        snprintf(when, sizeof(when), "#%u", (unsigned)count);
    }
    char record[256];
    // No backtrace here: the decoder (tools/badge.sh crash) gives it
    crash_record_format(record, sizeof(record), when, esp_app_get_description()->version, elf, reason_name(reason), task, pc,
                        NULL, 0);
    append_record(record);
    ESP_LOGW(TAG, "last reset was a crash: %s", record);
}

bool crash_recovered(void) {
    return s_recovered;
}
