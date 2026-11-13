// The NVS glue of the one-time move of the apps' plain secrets (#288): app_secret_move.c decides, this reads, writes
// and erases in the app's namespace. Nothing is logged here: main.c says the counts in one line.
#include "app_secret_move.h"
#include "lichen_os.h"

#include "app_ns.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

typedef struct {
    char         ns[16];
    nvs_handle_t h;
} env_t;

static size_t list(void *env, char const *prefix, char (*keys)[APP_SECRET_STORED_MAX], size_t cap, bool *more) {
    env_t         *e   = env;
    nvs_iterator_t it  = NULL;
    size_t         n   = 0;
    size_t         pl  = strlen(prefix);
    esp_err_t      err = nvs_entry_find(NVS_DEFAULT_PART_NAME, e->ns, NVS_TYPE_STR, &it);
    *more              = false;
    for (; err == ESP_OK; err = nvs_entry_next(&it)) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, prefix, pl) != 0) {
            continue;
        }
        if (n == cap) {
            *more = true;
            break;
        }
        strlcpy(keys[n++], info.key, APP_SECRET_STORED_MAX);
    }
    nvs_release_iterator(it);
    return n;
}

static bool get(void *env, char const *key, char *out, size_t cap) {
    size_t len = cap;
    return nvs_get_str(((env_t *)env)->h, key, out, &len) == ESP_OK;
}

static bool commit(nvs_handle_t h, esp_err_t err) {
    return err == ESP_OK && nvs_commit(h) == ESP_OK;
}

static bool set(void *env, char const *key, char const *value) {
    env_t *e = env;
    return commit(e->h, nvs_set_str(e->h, key, value));
}

static bool erase(void *env, char const *key) {
    env_t    *e   = env;
    esp_err_t err = nvs_erase_key(e->h, key);
    return err == ESP_ERR_NVS_NOT_FOUND || commit(e->h, err);
}

static bool moved_get(void *env) {
    uint8_t v = 0;
    return nvs_get_u8(((env_t *)env)->h, APP_SECRET_MOVED_KEY, &v) == ESP_OK && v == 1;
}

static bool moved_set(void *env) {
    env_t *e = env;
    return commit(e->h, nvs_set_u8(e->h, APP_SECRET_MOVED_KEY, 1));
}

app_secret_move_t app_secrets_move_app(char const *id, char const *prefix, bool other_migration,
                                       app_secret_move_count_t *c) {
    env_t e;
    *c = (app_secret_move_count_t){0};
    if (!prefix) {
        return APP_SECRET_MOVE_ALREADY; // no secrets declared: nothing to move
    }
    if (!app_ns_name(id, e.ns, sizeof(e.ns)) || nvs_open(e.ns, NVS_READWRITE, &e.h) != ESP_OK) {
        c->failed = 1;
        return APP_SECRET_MOVE_PARTIAL;
    }
    app_secret_store_t const s = {&e, list, get, set, erase, moved_get, moved_set};
    app_secret_move_t        r = app_secret_move(&s, prefix, other_migration, c);
    nvs_close(e.h);
    return r;
}
