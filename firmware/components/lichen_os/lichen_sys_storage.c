// lichen_sys_storage.h (#227) over the sdcard and storage components, so the shell never links to them. The pure SD
// text is in lichen_sys_pure.c. The gate's SD types equal sd_policy.h's (asserted there), so they convert by copy.
#include "lichen_sys_storage.h"

#include "sdcard.h"
#include "storage.h"

#include <string.h>

_Static_assert(sizeof(LICHEN_SYS_SD_LICHEN_DIR) == sizeof(SD_LICHEN_DIR), "the gate's Lichen directory follows sdcard's");
_Static_assert(sizeof(LICHEN_SYS_SD_BASE) == sizeof(SD_BASE), "the gate's SD mount point follows sdcard's");
_Static_assert(sizeof(LICHEN_SYS_STORAGE_BASE) == sizeof(STORAGE_BASE), "the gate's drive mount follows storage's");

void lichen_sys_sd_info(lichen_sd_info_t *out) {
    sd_info_t in;
    sd_info(&in);
    _Static_assert(sizeof(*out) == sizeof(in), "the gate's SD info follows sdcard's");
    memcpy(out, &in, sizeof(*out));
}

bool lichen_sys_sd_mounted(void) {
    return sd_mounted();
}

bool lichen_sys_sd_mount(void) {
    return sd_mount();
}

void lichen_sys_sd_unmount(void) {
    sd_unmount();
}

bool lichen_sys_sd_format(void) {
    return sd_format();
}

bool lichen_sys_sd_begin(uint32_t wait_ms) {
    return sd_begin(wait_ms);
}

void lichen_sys_sd_end(lichen_sd_io_t result) {
    sd_end((sd_io_t)result);
}

bool lichen_sys_storage_mounted(void) {
    return storage_mounted();
}

bool lichen_sys_storage_free_bytes(uint64_t *free_bytes, uint64_t *total_bytes) {
    return storage_free_bytes(free_bytes, total_bytes);
}
