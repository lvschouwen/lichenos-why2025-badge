#pragma once
// The SD card's rules (#60), without ESP headers: host-tested in host_tests/sd_policy_test.c. sdcard.c is the glue.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SD_STATE_OFF,      // never mounted, or unmounted by "fw sd unmount"
    SD_STATE_NO_CARD,  // the card did not answer (none in the slot): there is no card detect, so this is the probe
    SD_STATE_NOT_FAT,  // a card, but no FAT on it (exFAT, blank): only "fw sd format" changes that, never Lichen itself
    SD_STATE_ERROR,    // a card that answered, then failed (read error, out of memory)
    SD_STATE_MOUNTED,
    SD_STATE_REMOVED,  // unmounted after SD_IO_ERRORS_TO_UNMOUNT failed batches in a row: pulled out, or dying
} sd_state_t;

typedef enum { SD_MOUNT_OK, SD_MOUNT_NO_CARD, SD_MOUNT_NOT_FAT, SD_MOUNT_ERROR } sd_mount_outcome_t;

// What a mount attempt leaves behind. Power (LDO VO4, which is also the card's VDD) stays on only for a mounted card:
// a missing or unusable card is unpowered until the next "fw sd mount" (Lucas, #60). Never a retry: a card is
// mounted at boot or by hand, there is no polling. The one exception is the clock fallback below, inside one attempt.
typedef struct {
    sd_state_t state;
    bool       keep_power;
} sd_mount_plan_t;

sd_mount_plan_t sd_policy_after_mount(sd_mount_outcome_t outcome);

// The card's clock (Lucas, #583): SD High Speed, 50 MHz, from the SDMMC's 200 MHz source / 4, the same source and
// divider as the C6 link on slot 1. IDF re-applies the shared source and divider on a slot switch only when the slot's
// clock, computed from the live divider and the slot's own source, differs from what it asked for. So two slots on
// different sources must never share a divider: slot 0 must never be set to exactly 40000 (160 MHz / 4) while slot 1
// runs at 50000 (200 MHz / 4), or the card would run at 50 and its check would still read 40. 50 and 20 (the default
// speed, a card that refuses High Speed: divider 8) are both safe. The one mixed state that does occur, the link at
// IDF's 40 for the first transfers after its card init next to a card at 50, can only make either slot run at 40 or 50:
// both inside High Speed.
#define SD_CLOCK_HS_KHZ      50000
#define SD_CLOCK_DEFAULT_KHZ 20000

// After a failed card init at tried_khz: the clock of the one retry (power cycled), or 0 for none. negotiated_khz is
// card->max_freq_khz, 0 when the card never got as far as the speed choice (no card: the retry would only repeat that).
uint32_t sd_policy_fallback_khz(uint32_t tried_khz, uint32_t negotiated_khz);

// How a batch of file work went (sd_end). NEUTRAL: it failed, but not because of the card (full, a bad name, no
// memory), so it says nothing about whether the card is still there.
typedef enum { SD_IO_OK, SD_IO_FAILED, SD_IO_NEUTRAL } sd_io_t;

// 0 is OK; EIO (FR_DISK_ERR, FR_INT_ERR), ENODEV (FR_NOT_READY: the CMD13 status check failed) and ETIMEDOUT are the
// card failing; any other errno is NEUTRAL
sd_io_t sd_io_from_errno(int err);

// sd_end bookkeeping: a failed batch counts, a good one resets the run, a neutral one changes nothing. True when the
// card must be unmounted now.
#define SD_IO_ERRORS_TO_UNMOUNT 3
typedef struct {
    uint32_t in_row;
    uint32_t total;
} sd_io_count_t;

bool sd_policy_io(sd_io_count_t *count, sd_io_t result);

// What sd_info() hands out: a copy of cached values, never read from the card
typedef struct {
    sd_state_t state;
    uint64_t   total_bytes; // 0 when unknown
    uint64_t   free_bytes;
    bool       free_known;
    char       kind[6];     // "SDSC", "SDHC", "SDXC", "MMC"; "" when no card answered
    int        freq_khz;
    uint8_t    width;       // data lines
    uint32_t   io_errors;   // failed batches since the last mount
    uint32_t   generation;  // grows on every successful mount, so a cached path can tell a re-inserted card
    char       reason[32];  // why the last mount failed, for SD_STATE_ERROR
} sd_info_t;

// "29.7 GB" (GiB, like a PC shows a 32 GB card), "512 MB", "12 KB"
void sd_fmt_size(uint64_t bytes, char *out, size_t cap);

// "SDSC" (byte addressing), "SDHC" (block addressing up to 32 GB), "SDXC" (above), "MMC"
char const *sd_card_kind(bool is_mmc, bool high_capacity, uint64_t bytes);

// "fw sd": "sd mounted /sd 29.7 GB, 28.1 GB free, SDHC 50 MHz 4-bit, errors 0", "sd no card", ...
void sd_status_line(sd_info_t const *info, char *out, size_t cap);

// About's row: "28.1 GB free of 29.7 GB", "no card", ...
void sd_short_text(sd_info_t const *info, char *out, size_t cap);

// "fw sd format <word>": the whole argument after "format" must be exactly this word (case matters), so a stray
// "fw sd format" or a typo never erases a card
#define SD_FORMAT_CONFIRM "ERASE"
bool sd_format_confirmed(char const *word);

#ifdef __cplusplus
}
#endif
