#include "sd_policy.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

sd_mount_plan_t sd_policy_after_mount(sd_mount_outcome_t outcome) {
    switch (outcome) {
    case SD_MOUNT_OK:
        return (sd_mount_plan_t){SD_STATE_MOUNTED, true};
    case SD_MOUNT_NO_CARD:
        return (sd_mount_plan_t){SD_STATE_NO_CARD, false};
    case SD_MOUNT_NOT_FAT:
        return (sd_mount_plan_t){SD_STATE_NOT_FAT, false};
    case SD_MOUNT_ERROR:
    default:
        return (sd_mount_plan_t){SD_STATE_ERROR, false};
    }
}

sd_io_t sd_io_from_errno(int err) {
    if (err == 0) {
        return SD_IO_OK;
    }
    return err == EIO || err == ENODEV || err == ETIMEDOUT ? SD_IO_FAILED : SD_IO_NEUTRAL;
}

bool sd_policy_io(sd_io_count_t *count, sd_io_t result) {
    if (result == SD_IO_NEUTRAL) {
        return false;
    }
    if (result == SD_IO_OK) {
        count->in_row = 0;
        return false;
    }
    count->total++;
    count->in_row++;
    return count->in_row >= SD_IO_ERRORS_TO_UNMOUNT;
}

void sd_fmt_size(uint64_t bytes, char *out, size_t cap) {
    uint64_t const kib = 1024, mib = kib * 1024, gib = mib * 1024;
    if (bytes >= gib) {
        uint64_t tenths = bytes * 10 / gib; // truncated, so a card never reads larger than it is
        snprintf(out, cap, "%llu.%llu GB", (unsigned long long)(tenths / 10), (unsigned long long)(tenths % 10));
    } else if (bytes >= mib) {
        snprintf(out, cap, "%llu MB", (unsigned long long)(bytes / mib));
    } else {
        snprintf(out, cap, "%llu KB", (unsigned long long)(bytes / kib));
    }
}

char const *sd_card_kind(bool is_mmc, bool high_capacity, uint64_t bytes) {
    if (is_mmc) {
        return "MMC";
    }
    if (!high_capacity) {
        return "SDSC";
    }
    return bytes > 32ULL * 1000 * 1000 * 1000 ? "SDXC" : "SDHC";
}

void sd_status_line(sd_info_t const *info, char *out, size_t cap) {
    char total[16], free_text[16];
    sd_fmt_size(info->total_bytes, total, sizeof(total));
    switch (info->state) {
    case SD_STATE_MOUNTED:
        if (info->free_known) {
            sd_fmt_size(info->free_bytes, free_text, sizeof(free_text));
        } else {
            strcpy(free_text, "?");
        }
        snprintf(out, cap, "sd mounted /sd %s, %s free, %s %d MHz %u-bit, errors %lu", total, free_text, info->kind,
                 info->freq_khz / 1000, (unsigned)info->width, (unsigned long)info->io_errors);
        break;
    case SD_STATE_NO_CARD:
        snprintf(out, cap, "sd no card (insert one, then fw sd mount)");
        break;
    case SD_STATE_NOT_FAT:
        snprintf(out, cap, "sd card %s %s has no FAT (exFAT or blank): fw sd format " SD_FORMAT_CONFIRM " erases it",
                 info->kind, total);
        break;
    case SD_STATE_ERROR:
        snprintf(out, cap, "sd error: %s", info->reason[0] ? info->reason : "unknown");
        break;
    case SD_STATE_REMOVED:
        snprintf(out, cap, "sd removed after %d I/O errors in a row (fw sd mount to try again)", SD_IO_ERRORS_TO_UNMOUNT);
        break;
    case SD_STATE_OFF:
    default:
        snprintf(out, cap, "sd unmounted");
        break;
    }
}

void sd_short_text(sd_info_t const *info, char *out, size_t cap) {
    char total[16], free_text[16];
    switch (info->state) {
    case SD_STATE_MOUNTED:
        sd_fmt_size(info->total_bytes, total, sizeof(total));
        if (info->free_known) {
            sd_fmt_size(info->free_bytes, free_text, sizeof(free_text));
            snprintf(out, cap, "%s free of %s", free_text, total);
        } else {
            snprintf(out, cap, "%s", total);
        }
        break;
    case SD_STATE_NO_CARD:
        snprintf(out, cap, "no card");
        break;
    case SD_STATE_NOT_FAT:
        snprintf(out, cap, "not FAT, not mounted");
        break;
    case SD_STATE_ERROR:
        snprintf(out, cap, "card error");
        break;
    case SD_STATE_REMOVED:
        snprintf(out, cap, "removed");
        break;
    case SD_STATE_OFF:
    default:
        snprintf(out, cap, "not mounted");
        break;
    }
}

bool sd_format_confirmed(char const *word) {
    return word && strcmp(word, SD_FORMAT_CONFIRM) == 0;
}
