/* Copyright 2020-2024 Espressif Systems (Shanghai) CO LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "boot_progress.h"
#include "c6_firmware.h"
#include "c6_image.h"
#include "c6_paths.h"

#include "esp_heap_caps.h"
#include "esp_loader.h"
#include "esp_loader_io.h"
#include "esp_rom_md5.h"
#include "esp_log.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <assert.h>
#include <string.h>
#include <sys/param.h>

#define TAG "c6_firmware"

#ifndef SINGLE_TARGET_SUPPORT

// For esp32, esp32s2
#define BOOTLOADER_ADDRESS_V0 0x1000
// For esp8266, esp32s3 and later chips
#define BOOTLOADER_ADDRESS_V1 0x0
// For esp32c5 and esp32p4
#define BOOTLOADER_ADDRESS_V2 0x2000
#define PARTITION_ADDRESS     0x8000
#define APPLICATION_ADDRESS   0x10000
// An ESP partition table occupies one 4K flash sector
#define PARTITION_TABLE_MAX_LEN 0x1000

bool read_file_size(char const *filename, void **buffer, uint32_t *size, bool null_terminate) {
    FILE *f = fopen(filename, "r");
    void *b = NULL;
    if (size) {
        *size = 0;
    }

    if (!f) {
        ESP_LOGE(TAG, "Failed to open file %s", filename);
        return false;
    }

    if (fseek(f, 0, SEEK_END) == -1) {
        ESP_LOGE(TAG, "Failed to read file %s", filename);
        goto error;
    }

    size_t s = ftell(f);
    if (s == -1) {
        ESP_LOGE(TAG, "Failed to get file size  %s", filename);
        goto error;
    }
    rewind(f);

    if (buffer != NULL) {
        *buffer = NULL;
        if (null_terminate) {
            b = heap_caps_calloc(1, s + 1, MALLOC_CAP_SPIRAM);
        } else {
            b = heap_caps_malloc(s, MALLOC_CAP_SPIRAM);
        }

        if (!b) {
            ESP_LOGE(TAG, "Failed to get allocate memory %s", filename);
            goto error;
        }

        size_t r = fread(b, 1, s, f);
        if (r != s) {
            ESP_LOGE(TAG, "Failed to read file %s, expected %u got %u", filename, s, r);
            goto error;
        }

        fclose(f);

        *buffer = b;
    }

    if (!buffer) {
        fclose(f);
    }

    if (size) {
        *size = s;
    }

    return true;

error:
    heap_caps_free(b);
    fclose(f);
    return false;
}

#define STR_LEN 128

// MD5 of the whole file against the hex text of its .md5; leaves the file rewound
static bool file_matches_md5(FILE *fp, char const *md5_hex) {
    static uint8_t buf[4096];
    md5_context_t  ctx;
    uint8_t        digest[ESP_ROM_MD5_DIGEST_LEN];
    size_t         n;
    esp_rom_md5_init(&ctx);
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        esp_rom_md5_update(&ctx, buf, n);
    }
    bool read_ok = !ferror(fp);
    rewind(fp);
    esp_rom_md5_final(digest, &ctx);
    return read_ok && c6_md5_matches(md5_hex, digest);
}

static bool load_image(partition_attr_t *image, char const *file) {
    char path[STR_LEN];
    char md5_path[STR_LEN];
    char md5_name[STR_LEN];

    // The lean firmware (#61) keeps one C6 image, in C6_IMAGE_DIR on the storage drive
    if (snprintf(md5_name, STR_LEN, "%s.md5", file) >= STR_LEN || !c6_image_path(file, path, STR_LEN) ||
        !c6_image_path(md5_name, md5_path, STR_LEN)) {
        ESP_LOGE(TAG, "Path for %s too long", file);
        return false;
    }

    if (!read_file_size(path, NULL, &image->size, false) || image->size == 0) {
        ESP_LOGE(TAG, "Failed to read %s", path);
        return false;
    }

    image->fp = fopen(path, "r");
    if (!image->fp) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return false;
    }

    if (!read_file_size(md5_path, (void **)&image->md5, NULL, true) || !c6_valid_md5((char *)image->md5)) {
        ESP_LOGE(TAG, "Missing or invalid %s", md5_path);
        return false;
    }

    // The flasher compares the C6's flash against this .md5, so it must describe this .bin: pushed separately over
    // Wi-Fi they can disagree, which would reflash the C6 on every boot or skip a new image silently
    if (!file_matches_md5(image->fp, (char const *)image->md5)) {
        ESP_LOGE(TAG, "%s does not match %s", path, md5_path);
        return false;
    }

    return true;
}

// Read the partition table image, leaving the file rewound for flashing
static size_t read_partition_table(c6_binaries_t *bins, uint8_t *table) {
    size_t len = fread(table, 1, PARTITION_TABLE_MAX_LEN, bins->part.fp);
    rewind(bins->part.fp);
    return len;
}

// Make sure the application fits the app partition at APPLICATION_ADDRESS in the image's own partition table
static bool app_fits_partition(c6_binaries_t *bins) {
    static uint8_t table[PARTITION_TABLE_MAX_LEN];
    size_t         len = read_partition_table(bins, table);
    uint32_t       size;

    if (!c6_app_partition_size(table, len, bins->app.addr, &size)) {
        ESP_LOGE(TAG, "No app partition at 0x%" PRIx32, bins->app.addr);
        return false;
    }
    if (bins->app.size > size) {
        ESP_LOGE(TAG, "Application (%" PRIu32 " bytes) does not fit its %" PRIu32 " byte partition", bins->app.size,
                 size);
        return false;
    }
    return true;
}

bool c6_binaries_otadata(c6_binaries_t *bins, uint32_t *offset, uint32_t *size) {
    static uint8_t table[PARTITION_TABLE_MAX_LEN];
    size_t         len = read_partition_table(bins, table);
    return c6_otadata_region(table, len, offset, size);
}

bool get_c6_binaries(c6_binaries_t *bins) {
    bins->boot.addr = BOOTLOADER_ADDRESS_V1;
    bins->part.addr = PARTITION_ADDRESS;
    bins->app.addr  = APPLICATION_ADDRESS;
    bins->boot.fp   = NULL;
    bins->part.fp   = NULL;
    bins->app.fp    = NULL;
    bins->boot.md5  = NULL;
    bins->part.md5  = NULL;
    bins->app.md5   = NULL;

    return load_image(&bins->boot, "bootloader.bin") && load_image(&bins->part, "partitions.bin") &&
           load_image(&bins->app, "firmware.bin") && app_fits_partition(bins);
}

void free_c6_binaries(c6_binaries_t *bins) {
    if (bins->boot.fp) {
        fclose(bins->boot.fp);
    }
    if (bins->part.fp) {
        fclose(bins->part.fp);
    }
    if (bins->app.fp) {
        fclose(bins->app.fp);
    }
    heap_caps_free((void *)bins->boot.md5);
    heap_caps_free((void *)bins->part.md5);
    heap_caps_free((void *)bins->app.md5);
}

#endif

static const char *get_error_string(const esp_loader_error_t error) {
    static char const *const mapping[] = {
        "NONE",
        "UNKNOWN",
        "TIMEOUT",
        "IMAGE SIZE",
        "INVALID MD5",
        "INVALID PARAMETER",
        "INVALID TARGET",
        "UNSUPPORTED CHIP",
        "UNSUPPORTED FUNCTION",
        "INVALID RESPONSE",
        "EFUSE BLOCK IN USE",
        "EFUSE BURN FAILED",
    };

    if ((size_t)error >= sizeof(mapping) / sizeof(mapping[0])) {
        return "UNKNOWN";
    }

    return mapping[error];
}

esp_loader_error_t connect_to_target_with_stub(
    esp_loader_t *loader, const uint32_t current_transmission_rate, const uint32_t higher_transmission_rate
) {
    esp_loader_connect_args_t connect_config = ESP_LOADER_CONNECT_DEFAULT();

    esp_loader_error_t err = esp_loader_connect_with_stub(loader, &connect_config);
    if (err != ESP_LOADER_SUCCESS) {
        printf("Cannot connect to target. Error: %s\n", get_error_string(err));

        if (err == ESP_LOADER_ERROR_TIMEOUT) {
            printf("Check if the host and the target are properly connected.\n");
        } else if (err == ESP_LOADER_ERROR_INVALID_TARGET) {
            printf("You could be using an unsupported chip, or chip revision.\n");
        } else if (err == ESP_LOADER_ERROR_INVALID_RESPONSE) {
            printf("Try lowering the transmission rate or using shorter wires to connect the host and the target.\n");
        }

        // Don't leave the C6 sitting in its ROM bootloader
        esp_loader_reset_target(loader);
        return err;
    }
    printf("Connected to target\n");

    if (higher_transmission_rate != current_transmission_rate) {
        // Changes the rate on the target and then on our UART
        err = esp_loader_change_transmission_rate(loader, higher_transmission_rate);
        if (err != ESP_LOADER_SUCCESS) {
            printf("Unable to change transmission rate (%s), continuing at the current rate\n", get_error_string(err));
            // The two ends may now disagree on the rate, so start over and stay where we are
            esp_loader_reset_target(loader);
            return connect_to_target_with_stub(loader, current_transmission_rate, current_transmission_rate);
        }
        printf("Transmission rate changed.\n");
    }

    return ESP_LOADER_SUCCESS;
}

esp_loader_error_t flash_binary(esp_loader_t *loader, FILE *bin, size_t size, size_t address) {
    static uint8_t         payload[1024];

    if (!bin) {
        return ESP_LOADER_ERROR_INVALID_PARAM;
    }
    esp_loader_flash_cfg_t flash_cfg = {
        .offset     = address,
        .image_size = size,
        .block_size = sizeof(payload),
    };

    printf("Erasing flash (this may take a while)...\n");
    esp_loader_error_t err = esp_loader_flash_start(loader, &flash_cfg);
    if (err != ESP_LOADER_SUCCESS) {
        printf("Erasing flash failed with error: %s.\n", get_error_string(err));
        return err;
    }
    printf("Start programming\n");

    size_t binary_size = size;
    size_t written     = 0;

    while (size > 0) {
        size_t to_read = MIN(size, sizeof(payload));
        if (fread(payload, 1, to_read, bin) != to_read) {
            printf("\nReading the image failed at %zu bytes\n", written);
            return ESP_LOADER_ERROR_FAIL;
        }

        err = esp_loader_flash_write(loader, &flash_cfg, payload, to_read);
        if (err != ESP_LOADER_SUCCESS) {
            printf("\nPacket could not be written! Error %s.\n", get_error_string(err));
            return err;
        }

        size    -= to_read;
        written += to_read;

        int progress = (int)(((float)written / binary_size) * 100);
        printf("\rProgress: %d %%", progress);
        boot_progress_progress(BOOT_C6, progress); // per image: boot, partition table and app each count 0-100
    };

    printf("\nFinished programming\n");

    // Also verifies the MD5 of what was written
    err = esp_loader_flash_finish(loader, &flash_cfg);
    if (err != ESP_LOADER_SUCCESS) {
        printf("Flash verification failed. Error: %s\n", get_error_string(err));
        return err;
    }
    printf("Flash verified\n");

    return ESP_LOADER_SUCCESS;
}
