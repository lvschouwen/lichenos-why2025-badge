// Host test for psram_guard/psram_nvs.c (#325): the PSRAM guard's NVS writes (speed record, one-shot delayline, retry
// state) check every open, set, erase and commit, and a failure reaches the nvs_fail sink (settings.write_fail) once,
// under the record's key; a set that failed is not committed; erasing a missing key is no failure.
#include "nvs.h"
#include "nvs_fail.h"
#include "psram_nvs.h"

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

// A fake NVS: each call returns what the test put in, and counts
static esp_err_t open_err, set_err, erase_err, commit_err;
static int       commits;

esp_err_t nvs_open(char const *name, nvs_open_mode_t mode, nvs_handle_t *out) {
    (void)name;
    (void)mode;
    *out = 1;
    return open_err;
}
esp_err_t nvs_set_blob(nvs_handle_t h, char const *key, void const *data, size_t len) {
    (void)h;
    (void)key;
    (void)data;
    (void)len;
    return set_err;
}
esp_err_t nvs_erase_key(nvs_handle_t h, char const *key) {
    (void)h;
    (void)key;
    return erase_err;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    commits++;
    return commit_err;
}

static int  reports;
static int  last_err;
static char last_key[16];

static void sink(char const *key, int err) {
    reports++;
    last_err = err;
    snprintf(last_key, sizeof(last_key), "%s", key);
}

static void fresh(void) {
    open_err = set_err = erase_err = commit_err = ESP_OK;
    commits = reports = last_err = 0;
    last_key[0]                  = '\0';
    fw_nvs_fail_reset();
    fw_nvs_fail_bind(sink);
}

static void all_ok_reports_nothing(void) {
    fresh();
    nvs_handle_t h;
    uint8_t      blob[4] = {0};
    CHECK(psram_nvs_open("psram_spd", &h) == ESP_OK);
    CHECK(psram_nvs_put(h, "psram_spd", blob, sizeof(blob)) == ESP_OK);
    CHECK(psram_nvs_drop(h, "psram_dl1") == ESP_OK);
    CHECK(commits == 2);
    CHECK(reports == 0);
}

static void open_failure_is_reported(void) {
    fresh();
    nvs_handle_t h;
    open_err = 0x1101; // ESP_ERR_NVS_NOT_INITIALIZED
    CHECK(psram_nvs_open("psram_rty", &h) == 0x1101);
    CHECK(reports == 1 && last_err == 0x1101 && strcmp(last_key, "psram_rty") == 0);
}

static void failed_set_is_reported_and_not_committed(void) {
    fresh();
    uint8_t blob[4] = {0};
    set_err         = 0x1105; // ESP_ERR_NVS_NOT_ENOUGH_SPACE
    CHECK(psram_nvs_put(1, "psram_spd", blob, sizeof(blob)) == 0x1105);
    CHECK(commits == 0);
    CHECK(reports == 1 && last_err == 0x1105 && strcmp(last_key, "psram_spd") == 0);
}

static void failed_commit_is_reported(void) {
    fresh();
    uint8_t blob[4] = {0};
    commit_err      = ESP_FAIL;
    CHECK(psram_nvs_put(1, "psram_rty", blob, sizeof(blob)) == ESP_FAIL);
    CHECK(reports == 1 && last_err == ESP_FAIL && strcmp(last_key, "psram_rty") == 0);
    reports = 0;
    CHECK(psram_nvs_drop(1, "psram_dl1") == ESP_FAIL);
    CHECK(reports == 1 && strcmp(last_key, "psram_dl1") == 0);
}

static void drop_of_a_missing_key_is_ok(void) {
    fresh();
    erase_err = ESP_ERR_NVS_NOT_FOUND;
    CHECK(psram_nvs_drop(1, "psram_dl1") == ESP_OK);
    CHECK(commits == 1);
    CHECK(reports == 0);
}

static void failed_erase_is_reported_and_not_committed(void) {
    fresh();
    erase_err = ESP_FAIL;
    CHECK(psram_nvs_drop(1, "psram_dl1") == ESP_FAIL);
    CHECK(commits == 0);
    CHECK(reports == 1 && strcmp(last_key, "psram_dl1") == 0);
}

// The guard writes before lichen_os binds the sink: such a failure is kept and counted on the bind
static void failure_before_the_bind_is_replayed(void) {
    fresh();
    fw_nvs_fail_reset();
    uint8_t blob[4] = {0};
    set_err         = ESP_FAIL;
    CHECK(psram_nvs_put(1, "psram_rty", blob, sizeof(blob)) == ESP_FAIL);
    CHECK(reports == 0);
    fw_nvs_fail_bind(sink);
    CHECK(reports == 1 && strcmp(last_key, "psram_rty") == 0);
}

int main(void) {
    all_ok_reports_nothing();
    open_failure_is_reported();
    failed_set_is_reported_and_not_committed();
    failed_commit_is_reported();
    drop_of_a_missing_key_is_ok();
    failed_erase_is_reported_and_not_committed();
    failure_before_the_bind_is_replayed();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_nvs_test: all passed\n");
    return 0;
}
