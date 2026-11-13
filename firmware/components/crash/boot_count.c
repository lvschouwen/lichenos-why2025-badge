// The boot counter (#409): +1 in NVS on every boot, so the soak sampler (#407) can tell an unplanned reboot from its
// "fw soak" lines even when the uptime alone would not show it (a reboot between two samples that ran longer than
// the gap). Why NVS and not RTC no-init or LP RAM: those survive a software restart but not every reset (a power cut,
// the CH340 EN reset, a brownout, #330), and a counter a reset loses would go back or stall exactly when the sampler
// needs it. NVS survives all of them; factory_plan.c keeps the key through a factory reset, so only erasing the nvs
// partition itself (a full flash erase) starts it again at 1.
// Cost: one small NVS write per boot, from crash_check_early, before the display starts: a flash write then stalls
// nothing on the glass (#31), and nothing writes it again until the next boot.
#include "crash.h"

#include "fw_console.h"
#include "nvs.h"
#include "nvs_fail.h"

#include <stdatomic.h>

#define BOOT_COUNT_KEY "boot_count" // badge_fw; kept by factory_plan.c's keep_keys

static char const TAG[] = "boot";

static atomic_uint s_count;
static atomic_bool s_ok; // false: this boot's number was not saved, so it is not a trustworthy count
static bool        s_done;

void boot_count_bump(void) {
    nvs_handle_t nvs;
    if (s_done) {
        return; // once per boot: the boot task only
    }
    s_done = true;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return; // no NVS (fw nvs-down, a broken partition): unknown this boot
    }
    uint32_t  count = 0;
    esp_err_t err   = nvs_get_u32(nvs, BOOT_COUNT_KEY, &count);
    // A read that failed for another reason must not restart the count at 1: the sampler would see it go back. The
    // ceiling (4 billion boots) is unknown too rather than a number that stalls.
    bool ok = (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) && count < UINT32_MAX;
    if (ok) {
        count++;
        ok = fw_nvs_ok(TAG, BOOT_COUNT_KEY, nvs_set_u32(nvs, BOOT_COUNT_KEY, count)) &&
             fw_nvs_ok(TAG, BOOT_COUNT_KEY, nvs_commit(nvs));
    }
    nvs_close(nvs);
    atomic_store(&s_count, count);
    atomic_store(&s_ok, ok);
}

bool boot_count(uint32_t *count) {
    *count = atomic_load(&s_count);
    return atomic_load(&s_ok);
}
