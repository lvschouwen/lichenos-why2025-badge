// Host test: the app settings gate with the secrets class (#288). lichen_settings.c over a namespace-aware fake NVS:
// every plain call refuses a secret key, app_secret_* reach only the declaring app's prefix in its own namespace, and
// Clear data takes the secrets with the rest. Values are made up.
#include "lichen_settings.h"
#include "lichen_sys_app.h"
#include "nvs.h"
#include "settings_fail.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// ---- the fake NVS: entries per namespace, every type as text ------------------------------------------------------

#define ENTRIES 32
static struct {
    char ns[16], key[16], value[80];
    int  used;
} nvs[ENTRIES];
static char handles[4][16]; // handle h - 1 -> namespace

static int find(nvs_handle_t h, char const *key) {
    for (int i = 0; i < ENTRIES; i++) {
        if (nvs[i].used && !strcmp(nvs[i].ns, handles[h - 1]) && !strcmp(nvs[i].key, key)) {
            return i;
        }
    }
    return -1;
}

static int count_ns(char const *ns) {
    int n = 0;
    for (int i = 0; i < ENTRIES; i++) {
        n += nvs[i].used && !strcmp(nvs[i].ns, ns);
    }
    return n;
}

static char const *raw(char const *ns, char const *key) {
    for (int i = 0; i < ENTRIES; i++) {
        if (nvs[i].used && !strcmp(nvs[i].ns, ns) && !strcmp(nvs[i].key, key)) {
            return nvs[i].value;
        }
    }
    return NULL;
}

esp_err_t nvs_open(char const *name, nvs_open_mode_t mode, nvs_handle_t *out) {
    (void)mode;
    for (int h = 0; h < 4; h++) {
        if (!handles[h][0] || !strcmp(handles[h], name)) {
            strcpy(handles[h], name);
            *out = (nvs_handle_t)h + 1;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t nvs_set_str(nvs_handle_t h, char const *key, char const *value) {
    if (strlen(key) > 15 || strlen(value) >= sizeof(nvs[0].value)) {
        return ESP_FAIL;
    }
    int i = find(h, key);
    for (int j = 0; i < 0 && j < ENTRIES; j++) {
        if (!nvs[j].used) {
            i = j;
        }
    }
    if (i < 0) {
        return ESP_FAIL;
    }
    nvs[i].used = 1;
    strcpy(nvs[i].ns, handles[h - 1]);
    strcpy(nvs[i].key, key);
    strcpy(nvs[i].value, value);
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t h, char const *key, char *out, size_t *len) {
    int i = find(h, key);
    if (i < 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    size_t need = strlen(nvs[i].value) + 1;
    if (*len < need) {
        memcpy(out, nvs[i].value, *len); // a short buffer gets part of it, so the caller's wipe is tested
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, nvs[i].value, need);
    *len = need;
    return ESP_OK;
}

static esp_err_t set_num(nvs_handle_t h, char const *key, uint64_t v) {
    char text[24];
    snprintf(text, sizeof(text), "%llu", (unsigned long long)v);
    return nvs_set_str(h, key, text);
}

static esp_err_t get_num(nvs_handle_t h, char const *key, uint64_t *v) {
    int i = find(h, key);
    if (i < 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *v = strtoull(nvs[i].value, NULL, 10);
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t h, char const *key, uint8_t value) {
    return set_num(h, key, value);
}
esp_err_t nvs_set_u32(nvs_handle_t h, char const *key, uint32_t value) {
    return set_num(h, key, value);
}
esp_err_t nvs_set_u64(nvs_handle_t h, char const *key, uint64_t value) {
    return set_num(h, key, value);
}
esp_err_t nvs_get_u8(nvs_handle_t h, char const *key, uint8_t *out) {
    uint64_t  v;
    esp_err_t err = get_num(h, key, &v);
    *out          = err == ESP_OK ? (uint8_t)v : *out;
    return err;
}
esp_err_t nvs_get_u32(nvs_handle_t h, char const *key, uint32_t *out) {
    uint64_t  v;
    esp_err_t err = get_num(h, key, &v);
    *out          = err == ESP_OK ? (uint32_t)v : *out;
    return err;
}
esp_err_t nvs_get_u64(nvs_handle_t h, char const *key, uint64_t *out) {
    return get_num(h, key, out);
}

esp_err_t nvs_erase_key(nvs_handle_t h, char const *key) {
    int i = find(h, key);
    if (i < 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    nvs[i].used = 0;
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t h) {
    for (int i = 0; i < ENTRIES; i++) {
        if (nvs[i].used && !strcmp(nvs[i].ns, handles[h - 1])) {
            nvs[i].used = 0;
        }
    }
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) {
    (void)h;
}

void settings_fail_note(char const *key, int err) {
    (void)key;
    (void)err;
}

// ---- two apps: MeshCore declares "pw", "other" declares nothing ---------------------------------------------------

#define MESH  ((lichen_ctx_t)1)
#define OTHER ((lichen_ctx_t)2)
#define OS    ((lichen_ctx_t)3)
#define NOCAP ((lichen_ctx_t)4) // MeshCore's namespace and prefix without CAP_SECRETS (#229)

static lichen_app_t const mesh_app  = {.name = "MeshCore", .id = "meshcore", .secret_prefix = "pw", .caps = CAP_SECRETS};
static lichen_app_t const other_app = {.name = "Other", .id = "other"};
static lichen_app_t const nocap_app = {.name = "MeshCore", .id = "meshcore", .secret_prefix = "pw"};

static lichen_app_t const *app_of(lichen_ctx_t ctx) {
    return ctx == MESH ? &mesh_app : ctx == OTHER ? &other_app : ctx == NOCAP ? &nocap_app : NULL;
}

// lichen_app.c's check over this table: the OS and a ctx it does not know pass (their own checks refuse those)
static int caps_refused;
bool lichen_sys_caps_ok(lichen_ctx_t ctx, uint32_t need, char const *what) {
    (void)what;
    lichen_app_t const *app = app_of(ctx);
    bool                ok  = !app || (app->caps & need) == need;
    caps_refused += !ok;
    return ok;
}

gate_app_kind_t lichen_sys_ctx_settings_ns(lichen_ctx_t ctx, char const *what, char *ns, size_t cap,
                                           char const **name) {
    (void)what;
    lichen_app_t const *app = app_of(ctx);
    if (!app) {
        return ctx == OS ? GATE_APP_OS : GATE_APP_REFUSED;
    }
    *name = app->name;
    snprintf(ns, cap, "a.%s", app->id);
    return GATE_APP_ID;
}

bool lichen_sys_ctx_resolve(lichen_ctx_t ctx, lichen_app_t const **app, char const *what) {
    (void)what;
    *app = app_of(ctx);
    return *app != NULL || ctx == OS;
}

// ---- the tests ----------------------------------------------------------------------------------------------------

static void plain_calls_refuse_secrets(void) {
    char out[32];
    // a plain copy an older image left under the prefix: not handed out, not overwritten, not erased plainly
    nvs_handle_t h;
    nvs_open("a.meshcore", NVS_READWRITE, &h);
    nvs_set_str(h, "pw0123456789ab", "made-up-1");
    app_settings_get_str(MESH, "pw0123456789ab", out, sizeof(out), "def");
    CHECK(!strcmp(out, "def"));
    CHECK(!app_settings_set_str(MESH, "pw0123456789ab", "made-up-2"));
    CHECK(!strcmp(raw("a.meshcore", "pw0123456789ab"), "made-up-1"));
    CHECK(!app_settings_erase(MESH, "pw0123456789ab") && raw("a.meshcore", "pw0123456789ab"));
    // every type, the prefix and the stored form alike
    CHECK(!app_settings_set_u8(MESH, "pwx", 1) && !app_settings_set_u32(MESH, "pwx", 1) &&
          !app_settings_set_u64(MESH, "pwx", 1));
    CHECK(app_settings_get_u8(MESH, "pwx", 7) == 7 && app_settings_get_u32(MESH, "pwx", 7) == 7 &&
          app_settings_get_u64(MESH, "pwx", 7) == 7);
    CHECK(!app_settings_set_str(MESH, "~pwx", "made-up") && !app_settings_set_u8(OTHER, "~x", 1));
    CHECK(!app_settings_set_u8(MESH, "~:moved", 0)); // the move's record is the OS's
    CHECK(raw("a.meshcore", "pwx") == NULL && raw("a.meshcore", "~pwx") == NULL && raw("a.other", "~x") == NULL);
    // keys outside the class are as before
    CHECK(app_settings_set_u8(MESH, "autostart", 0) && app_settings_get_u8(MESH, "autostart", 1) == 0);
    CHECK(app_settings_set_str(MESH, "name", "x") && app_settings_erase(MESH, "name"));
}

static void secrets_scoped_to_the_declaring_app(void) {
    char out[32];
    CHECK(app_secret_set(MESH, "pwaabbccddeeff", "made-up-3"));
    CHECK(!strcmp(raw("a.meshcore", "~pwaabbccddeeff"), "made-up-3") && !raw("a.meshcore", "pwaabbccddeeff"));
    CHECK(app_secret_get(MESH, "pwaabbccddeeff", out, sizeof(out)) && !strcmp(out, "made-up-3"));
    // a short buffer: false and wiped, no part of the value left
    char small[4] = "zzz";
    CHECK(!app_secret_get(MESH, "pwaabbccddeeff", small, sizeof(small)) && !small[0] && !small[1] && !small[2]);
    // outside the prefix, or too long to carry the mark
    CHECK(!app_secret_set(MESH, "autostart", "made-up") && !app_secret_get(MESH, "autostart", out, sizeof(out)));
    CHECK(!app_secret_set(MESH, "pw0123456789abc", "made-up"));
    char longv[70];
    memset(longv, 'a', sizeof(longv) - 1);
    longv[sizeof(longv) - 1] = '\0';
    CHECK(!app_secret_set(MESH, "pwlong", longv)); // over APP_SECRET_VALUE_MAX
    // the other app declared nothing: no secrets at all, and its "pw..." key is its own plain setting
    CHECK(!app_secret_set(OTHER, "pwaabbccddeeff", "made-up") && !app_secret_get(OTHER, "pwaabbccddeeff", out, 32));
    CHECK(app_settings_set_str(OTHER, "pwaabbccddeeff", "plain") && raw("a.other", "pwaabbccddeeff"));
    CHECK(!raw("a.other", "~pwaabbccddeeff"));
    // the OS and a made-up ctx have no secrets either
    CHECK(!app_secret_set(OS, "pwx", "made-up") && !app_secret_get((lichen_ctx_t)99, "pwx", out, sizeof(out)));
    // an empty value is a value; erase removes it, twice is fine
    CHECK(app_secret_set(MESH, "pwe", "") && app_secret_get(MESH, "pwe", out, sizeof(out)) && !out[0]);
    CHECK(app_secret_erase(MESH, "pwe") && app_secret_erase(MESH, "pwe") && !raw("a.meshcore", "~pwe"));
    CHECK(!app_secret_get(MESH, "pwe", out, sizeof(out)));
}

static void secrets_need_the_capability(void) {
    char out[32];
    CHECK(app_secret_set(MESH, "pwcap", "made-up-4"));
    caps_refused = 0;
    CHECK(!app_secret_get(NOCAP, "pwcap", out, sizeof(out)) && !out[0]);
    CHECK(!app_secret_set(NOCAP, "pwcap", "made-up-5") && !app_secret_erase(NOCAP, "pwcap"));
    CHECK(caps_refused == 3 && !strcmp(raw("a.meshcore", "~pwcap"), "made-up-4"));
    CHECK(app_secret_erase(MESH, "pwcap"));
}

static void clear_data_takes_the_secrets(void) {
    CHECK(count_ns("a.meshcore") > 0);
    CHECK(app_settings_erase_all(MESH));
    CHECK(count_ns("a.meshcore") == 0 && count_ns("a.other") > 0);
}

int main(void) {
    plain_calls_refuse_secrets();
    secrets_scoped_to_the_declaring_app();
    secrets_need_the_capability();
    clear_data_takes_the_secrets();
    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures ? 1 : 0;
}
