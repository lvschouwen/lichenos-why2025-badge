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
// mounted at boot or by hand, there is no polling.
typedef struct {
    sd_state_t state;
    bool       keep_power;
} sd_mount_plan_t;

sd_mount_plan_t sd_policy_after_mount(sd_mount_outcome_t outcome);

// sd_end(ok) bookkeeping: a failed batch counts, a good one resets the run. True when the card must be unmounted now.
#define SD_IO_ERRORS_TO_UNMOUNT 3
typedef struct {
    uint32_t in_row;
    uint32_t total;
} sd_io_count_t;

bool sd_policy_io(sd_io_count_t *count, bool ok);

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

// "fw sd": "sd mounted /sd 29.7 GB, 28.1 GB free, SDHC 20 MHz 4-bit, errors 0", "sd no card", ...
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
