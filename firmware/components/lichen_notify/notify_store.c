#include "notify_store.h"

#include "notify_mute.h"

bool nstore_dnd_load(fw_get_u8_fn get) {
    return get && get(NSTORE_KEY_DND, 0) != 0;
}

bool nstore_dnd_save(fw_set_u8_fn set, bool on) {
    return set && set(NSTORE_KEY_DND, on ? 1 : 0);
}

bool nstore_source_load(fw_get_u8_fn get, char const *source) {
    char key[NMUTE_KEY_CAP];
    return !get || !nmute_key(source, key, sizeof(key)) || get(key, 1) != 0;
}

bool nstore_source_save(fw_set_u8_fn set, char const *source, bool on) {
    char key[NMUTE_KEY_CAP];
    return set && nmute_key(source, key, sizeof(key)) && set(key, on ? 1 : 0);
}
