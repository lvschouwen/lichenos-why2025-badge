#include "sd_settings_model.h"

#include <stdio.h>

static char const *busy_text(sds_job_t job) {
    switch (job) {
    case SDS_MOUNTING: return "Mounting...";
    case SDS_UNMOUNTING: return "Unmounting...";
    case SDS_FORMATTING: return "Formatting... keep the card in";
    case SDS_IDLE:
    default: return NULL;
    }
}

// "SDXC 59.4 GB", "SDHC", "" (what is known about a card that answered)
static void card_text(sd_info_t const *info, char *out, size_t cap) {
    char total[16] = "";
    if (info->total_bytes) {
        sd_fmt_size(info->total_bytes, total, sizeof(total));
    }
    snprintf(out, cap, "%s%s%s", info->kind, info->kind[0] && total[0] ? " " : "", total);
}

void sds_status(sd_info_t const *info, sds_job_t job, char *out, size_t cap) {
    char card[24], free_text[16];
    if (busy_text(job)) {
        snprintf(out, cap, "%s", busy_text(job));
        return;
    }
    card_text(info, card, sizeof(card));
    switch (info->state) {
    case SD_STATE_MOUNTED:
        if (info->free_known) {
            sd_fmt_size(info->free_bytes, free_text, sizeof(free_text));
            snprintf(out, cap, "Mounted: %s, %s free", card, free_text);
        } else {
            snprintf(out, cap, "Mounted: %s", card);
        }
        break;
    case SD_STATE_NO_CARD: snprintf(out, cap, "No card found"); break;
    case SD_STATE_NOT_FAT:
        snprintf(out, cap, "%s%snot FAT (exFAT or blank)", card, card[0] ? ": " : "Card ");
        break;
    case SD_STATE_ERROR: snprintf(out, cap, "Error: %s", info->reason[0] ? info->reason : "unknown"); break;
    case SD_STATE_REMOVED: snprintf(out, cap, "Unmounted after I/O errors"); break;
    case SD_STATE_OFF:
    default: snprintf(out, cap, "Not mounted"); break;
    }
}

sds_job_t sds_mount_job(sd_state_t state) {
    return state == SD_STATE_MOUNTED ? SDS_UNMOUNTING : SDS_MOUNTING;
}

sds_actions_t sds_actions(sd_state_t state, sds_job_t job) {
    bool          mounted = state == SD_STATE_MOUNTED;
    bool          busy    = job != SDS_IDLE;
    bool          card    = mounted || state == SD_STATE_NOT_FAT || state == SD_STATE_ERROR;
    sds_actions_t a       = {
              .mount_label    = mounted ? "Unmount" : "Mount",
              .mount_value    = busy ? "wait" : mounted ? "safe to remove" : "",
              .mount_enabled  = !busy,
              .format_value   = busy ? "wait" : card ? "erases the card" : "needs a card",
              .format_enabled = !busy && card,
    };
    return a;
}

static void failed(char const *what, sd_info_t const *after, char *out, size_t cap) {
    switch (after->state) {
    case SD_STATE_NO_CARD: snprintf(out, cap, "%s failed: no card", what); break;
    case SD_STATE_NOT_FAT: snprintf(out, cap, "%s failed: not FAT. Format makes it LICHEN", what); break;
    case SD_STATE_ERROR:
        snprintf(out, cap, "%s failed: %s", what, after->reason[0] ? after->reason : "card error");
        break;
    default: snprintf(out, cap, "%s failed: card busy, try again", what); break;
    }
}

void sds_result(sds_job_t job, bool ok, sd_info_t const *after, char *out, size_t cap) {
    switch (job) {
    case SDS_MOUNTING:
        if (ok) {
            snprintf(out, cap, "Mounted");
        } else {
            failed("Mount", after, out, cap);
        }
        break;
    case SDS_UNMOUNTING:
        snprintf(out, cap, ok ? "Unmounted: safe to remove the card" : "Unmount failed: card busy, try again");
        break;
    case SDS_FORMATTING:
        if (ok) {
            snprintf(out, cap, "Formatted as LICHEN and mounted");
        } else {
            failed("Format", after, out, cap);
        }
        break;
    case SDS_IDLE:
    default: out[0] = '\0'; break;
    }
}
