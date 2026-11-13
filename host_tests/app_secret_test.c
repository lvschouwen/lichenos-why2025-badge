// Host test: the apps' secrets class (#288): the declared prefix, which keys are secrets, the stored name, and the
// backup and keep refusals
#include "app_ns.h"
#include "app_secret.h"

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

static void declarations(void) {
    CHECK(app_secret_prefix_ok(NULL)); // no secrets
    CHECK(app_secret_prefix_ok("pw") && app_secret_prefix_ok("tok_1") && app_secret_prefix_ok("abcdefgh"));
    CHECK(!app_secret_prefix_ok(""));          // would make every key secret
    CHECK(!app_secret_prefix_ok("abcdefghi")); // 9: too long
    CHECK(!app_secret_prefix_ok("Pw") && !app_secret_prefix_ok("p-w") && !app_secret_prefix_ok("~pw"));
    CHECK(!app_secret_prefix_ok("p:"));        // ':' is the move's record
}

static void the_class(void) {
    // the declared prefix, and the mark in every namespace
    CHECK(app_secret_key_secret("pw", "pw0123456789ab") && app_secret_key_secret("pw", "pw"));
    CHECK(app_secret_key_secret("pw", "~pw0123456789ab") && app_secret_key_secret(NULL, "~anything"));
    CHECK(app_secret_key_secret(NULL, "~:moved"));
    CHECK(!app_secret_key_secret("pw", "autostart") && !app_secret_key_secret("pw", "p") &&
          !app_secret_key_secret("pw", "xpw1"));
    // scoped to the declaring app: an app without a prefix (or another one) keeps "pw..." as a plain setting
    CHECK(!app_secret_key_secret(NULL, "pw0123456789ab") && !app_secret_key_secret("tok", "pw0123456789ab"));
    CHECK(!app_secret_key_secret("pw", NULL));
}

static void stored_names(void) {
    char out[APP_SECRET_STORED_MAX];
    CHECK(app_secret_stored_name("pw", "pw0123456789ab", out, sizeof(out)) && !strcmp(out, "~pw0123456789ab"));
    CHECK(strlen(out) == 15); // NVS's longest key
    CHECK(!app_secret_stored_name("pw", "pw0123456789abc", out, sizeof(out)) && out[0] == '\0'); // 15: no room
    CHECK(!app_secret_stored_name("pw", "autostart", out, sizeof(out)) && out[0] == '\0');
    CHECK(!app_secret_stored_name(NULL, "pw1", out, sizeof(out)));
    CHECK(!app_secret_stored_name("P", "P1", out, sizeof(out)));       // an invalid declaration stores nothing
    CHECK(!app_secret_stored_name("pw", "~pw1", out, sizeof(out)));    // the stored name is not a key
    CHECK(!app_secret_stored_name("pw", "pw1", out, APP_SECRET_STORED_MAX - 1));
    CHECK(!app_secret_stored_name("pw", NULL, out, sizeof(out)));
}

static void backup_and_keep(void) {
    sb_key_t const plain[] = {{"a.x", "n_dm", "Notify", SB_U8, 0, 1, 1, NULL}};
    sb_key_t const leak[]  = {{"a.x", "n_dm", "Notify", SB_U8, 0, 1, 1, NULL}, {"a.x", "pwabc", "Pw", SB_STR, 0, 0, 0, NULL}};
    sb_key_t const mark[]  = {{"a.x", "~n", "Stored", SB_STR, 0, 0, 0, NULL}};
    CHECK(app_secret_keys_plain("pw", plain, 1) && app_secret_keys_plain(NULL, NULL, 0));
    CHECK(!app_secret_keys_plain("pw", leak, 2));
    CHECK(app_secret_keys_plain(NULL, leak, 2)); // without a prefix "pwabc" is that app's plain setting
    CHECK(!app_secret_keys_plain(NULL, mark, 1));
}

static char const *kept[4];
static size_t      kept_n;
static bool        add(char const *ns, char const *key) {
    (void)ns;
    kept[kept_n++] = key;
    return true;
}

static void keep_list(void) {
    static char const *const keys[] = {"last", "pw0123456789ab", "~x", NULL};
    kept_n                          = 0;
    CHECK(app_ns_declare_keep("app", "pw", keys, add) == 2); // the password and the stored form are refused
    CHECK(kept_n == 1 && !strcmp(kept[0], "last"));
}

int main(void) {
    declarations();
    the_class();
    stored_names();
    backup_and_keep();
    keep_list();
    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures ? 1 : 0;
}
