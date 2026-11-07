// Host test: do-not-disturb and the per-source switches go through the settings binding, with the badge_fw keys from
// before #235
#include "notify_store.h"

#include "notify_mute.h"

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

static struct {
    char    key[NMUTE_KEY_CAP];
    uint8_t value;
} kv[8];
static size_t kv_n;

static uint8_t fake_get(char const *key, uint8_t def) {
    for (size_t i = 0; i < kv_n; i++) {
        if (strcmp(kv[i].key, key) == 0) {
            return kv[i].value;
        }
    }
    return def;
}
static bool fake_set(char const *key, uint8_t value) {
    for (size_t i = 0; i < kv_n; i++) {
        if (strcmp(kv[i].key, key) == 0) {
            kv[i].value = value;
            return true;
        }
    }
    if (kv_n == 8) {
        return false;
    }
    strcpy(kv[kv_n].key, key);
    kv[kv_n++].value = value;
    return true;
}

int main(void) {
    CHECK(strcmp(NSTORE_KEY_DND, "notify_dnd") == 0);
    CHECK(!nstore_dnd_load(NULL) && !nstore_dnd_load(fake_get)); // not bound / missing: off
    CHECK(!nstore_dnd_save(NULL, true));
    CHECK(nstore_dnd_save(fake_set, true) && nstore_dnd_load(fake_get));
    CHECK(fake_get("notify_dnd", 0) == 1);
    CHECK(nstore_dnd_save(fake_set, false) && !nstore_dnd_load(fake_get));

    CHECK(nstore_source_load(NULL, "chat") && nstore_source_load(fake_get, "chat")); // default on
    CHECK(!nstore_source_save(NULL, "chat", false));
    CHECK(nstore_source_save(fake_set, "chat", false) && !nstore_source_load(fake_get, "chat"));
    CHECK(fake_get("n.chat", 1) == 0); // the key is nmute_key's
    CHECK(nstore_source_load(fake_get, "mail"));               // another source is untouched
    CHECK(nstore_source_save(fake_set, "chat", true) && nstore_source_load(fake_get, "chat"));
    CHECK(!nstore_source_save(fake_set, "a-source-name-too-long", false)); // no key: not saved ...
    CHECK(nstore_source_load(fake_get, "a-source-name-too-long"));         // ... and always on
    if (failures) {
        return 1;
    }
    printf("notify_store_test: ok\n");
    return 0;
}
