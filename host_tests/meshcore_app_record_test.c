// Host test: what MeshCore's app record tells the OS by name (#230 E1d), pinned against the literals the OS used to
// carry: a dropped key here would erase the last advert at a factory reset (#52) or show the identity directory in Files.
#include "app_ns.h"
#include "app_secret.h"
#include "factory_plan.h"
#include "files_model.h"
#include "meshcore_app_record.h"

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

int main(void) {
    CHECK(!strcmp(MESHCORE_APP_ID, "meshcore") && !strcmp(MESHCORE_ALIAS, "mesh"));
    // the keep list, declared the way main.c does it, keeps the last advert and nothing else of the namespace
    factory_keep_app_keys_clear();
    CHECK(app_ns_declare_keep(MESHCORE_APP_ID, MESHCORE_SECRET_PREFIX, meshcore_keep_keys, factory_keep_app_key) == 0);
    CHECK(!factory_nvs_erases("a.meshcore", "mc_advlast", FACTORY_SETTINGS));
    CHECK(!factory_nvs_erases("a.meshcore", "mc_advlast", FACTORY_ALL));
    CHECK(factory_nvs_erases("a.meshcore", "mc_advint", FACTORY_ALL) && factory_nvs_erases("a.meshcore", "identity", FACTORY_ALL));
    // a bad id declares nothing and says so
    CHECK(app_ns_declare_keep("Bad id", NULL, meshcore_keep_keys, factory_keep_app_key) == 1);
    CHECK(app_ns_declare_keep("x", NULL, NULL, factory_keep_app_key) == 0);
    // the identity directory is protected and secret once the record's root is added
    files_app_roots_clear();
    CHECK(!files_secret("/storage/meshcore/id"));
    CHECK(files_add_app_root(MESHCORE_STORAGE_ROOT, MESHCORE_STORAGE_SECRET));
    CHECK(files_secret("/storage/meshcore/id") && files_protected("/storage/meshcore/id"));
    // the backup keys are the three notify switches, all in the app's own namespace
    static char const *const want[] = {"n_dm", "n_fail", "n_ment"};
    for (size_t i = 0; i < MESHCORE_BACKUP_KEYS; i++) {
        CHECK(app_ns_owns(MESHCORE_APP_ID, meshcore_backup_keys[i].ns));
        CHECK(!strcmp(meshcore_backup_keys[i].key, want[i]) && meshcore_backup_keys[i].type == SB_U8);
    }
    // the passwords are the secrets class (#288): a valid declaration, out of the backup group and the keep list
    CHECK(app_secret_prefix_ok(MESHCORE_SECRET_PREFIX) && !strcmp(MESHCORE_SECRET_PREFIX, "pw"));
    CHECK(app_secret_key_secret(MESHCORE_SECRET_PREFIX, "pw0123456789ab"));
    CHECK(app_secret_keys_plain(MESHCORE_SECRET_PREFIX, meshcore_backup_keys, MESHCORE_BACKUP_KEYS));
    for (size_t i = 0; meshcore_keep_keys[i]; i++) {
        CHECK(!app_secret_key_secret(MESHCORE_SECRET_PREFIX, meshcore_keep_keys[i]));
    }
    // a password key on the keep list is refused and the reset erases it
    static char const *const with_pw[] = {"mc_advlast", "pw0123456789ab", NULL};
    factory_keep_app_keys_clear();
    CHECK(app_ns_declare_keep(MESHCORE_APP_ID, MESHCORE_SECRET_PREFIX, with_pw, factory_keep_app_key) == 1);
    CHECK(factory_nvs_erases("a.meshcore", "pw0123456789ab", FACTORY_SETTINGS));
    CHECK(!factory_nvs_erases("a.meshcore", "mc_advlast", FACTORY_SETTINGS));
    if (failures) {
        return 1;
    }
    printf("meshcore_app_record_test: all passed\n");
    return 0;
}
