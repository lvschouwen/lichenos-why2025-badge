// Host test: the settings backup file (#169): allow-list, format, strict parse, secrets ignored
#include "settings_backup.h"

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

static sb_result_t parse(char const *text, sb_value_t *v, size_t *n) {
    return sb_parse(text, strlen(text), v, 64, n);
}

static void test_table(void) {
    char const *const secrets[] = {"wifi_pass", "wifi_ssid", "ota_token", "identity", "ota_wifi", "ota_auto_min", "radio_on", "radio_air",
                                   "radio_saved", "fr_pending", "r16", "pass", "chan", "prv", "mc_name_set", "autostart"};
    for (size_t i = 0; i < sb_key_count(); i++) {
        sb_key_t const *k = sb_key_at(i);
        CHECK(strlen(k->key) <= 15);
        CHECK(strlen(k->ns) <= 15);
        for (size_t j = i + 1; j < sb_key_count(); j++) {
            CHECK(!(strcmp(k->ns, sb_key_at(j)->ns) == 0 && strcmp(k->key, sb_key_at(j)->key) == 0));
        }
        for (size_t s = 0; s < sizeof(secrets) / sizeof(secrets[0]); s++) {
            CHECK(strcmp(k->key, secrets[s]) != 0);
        }
        CHECK(!strstr(k->key, "pass") && !strstr(k->key, "token") && !strstr(k->key, "secret") && !strstr(k->key, "key"));
        sb_value_t v;
        char       text[SB_STR_MAX];
        if (k->type == SB_U8) { // the default must be a value the file may hold
            snprintf(text, sizeof(text), "%u", (unsigned)k->def);
        } else {
            snprintf(text, sizeof(text), "%s", k->def_str);
        }
        CHECK(sb_value_parse(k, text, &v));
    }
    CHECK(sb_key_count() <= SB_KEYS_MAX); // settings_backup_io.c sizes its value arrays by SB_KEYS_MAX
    CHECK(sb_find("badge_fw", "home_layout") != NULL);
    CHECK(sb_find("badge_fw", "wifi_pass") == NULL);
    CHECK(sb_find("a.meshcore", "identity") == NULL);
    CHECK(sb_find("a.meshcore", "home_layout") == NULL); // right key, wrong namespace
}

static void test_roundtrip(void) {
    sb_value_t vals[32];
    size_t     n = sb_key_count();
    for (size_t i = 0; i < n; i++) {
        sb_key_t const *k = sb_key_at(i);
        char            text[SB_STR_MAX];
        if (k->type == SB_U8) {
            snprintf(text, sizeof(text), "%u", (unsigned)k->max);
        } else {
            snprintf(text, sizeof(text), "%s", "UTC0");
        }
        CHECK(sb_value_parse(k, text, &vals[i]));
    }
    char file[2048];
    CHECK(sb_format("fw-test", vals, n, file, sizeof(file)) > 0);
    CHECK(strstr(file, "# firmware: fw-test\n") != NULL);
    CHECK(strstr(file, "badge_fw.tz = UTC0\n") != NULL);
    CHECK(strstr(file, "a.weather.toff = 20\n") != NULL);
    sb_value_t back[32];
    size_t     m = 0;
    sb_result_t r = parse(file, back, &m);
    CHECK(r.err == SB_OK && m == n && r.ignored == 0);
    for (size_t i = 0; i < m; i++) {
        CHECK(sb_value_equal(&vals[i], &back[i]));
    }
    CHECK(sb_format("x", vals, n, file, 100) == 0); // too small: nothing
    size_t flen = sb_format("fw-test", vals, n, file, sizeof(file));
    CHECK(flen > 0 && sb_complete(file, flen));
    CHECK(!sb_complete(file, flen - 1));                    // cut inside the marker
    CHECK(!sb_complete(file, (size_t)(strstr(file, "# end") - file))); // cut before it, at a line boundary
    CHECK(!sb_complete("badge_fw.x = 1\n", 15));
    CHECK(!sb_complete("x" SB_END_LINE, 1 + sizeof(SB_END_LINE) - 1)); // marker glued to a longer line
    CHECK(sb_complete(SB_END_LINE, sizeof(SB_END_LINE) - 1));
}

static void test_bad_values(void) {
    sb_value_t v[32];
    size_t     n;
    sb_result_t r = parse("# ok\nbadge_fw.saver = 1\n\nbadge_fw.lock = 7\nbadge_fw.home_layout = 1\n", v, &n);
    CHECK(r.err == SB_ERR_VALUE && r.line == 4 && n == 0); // refused whole, with the line number
    r = parse("badge_fw.saver = on\n", v, &n);
    CHECK(r.err == SB_ERR_VALUE && r.line == 1);
    r = parse("badge_fw.saver = -1\n", v, &n);
    CHECK(r.err == SB_ERR_VALUE);
    r = parse("badge_fw.saver = \n", v, &n);
    CHECK(r.err == SB_ERR_VALUE);
    r = parse("badge_fw.idle_dim = 256\n", v, &n);
    CHECK(r.err == SB_ERR_VALUE);
    r = parse("badge_fw.bl_disp = 4\n", v, &n);
    CHECK(r.err == SB_ERR_VALUE);
    r = parse("badge_fw.tz = has space\n", v, &n);
    CHECK(r.err == SB_ERR_VALUE);
    r = parse("badge_fw.tz = CET-1CEST,M3.5.0,M10.5.0/3\n", v, &n);
    CHECK(r.err == SB_OK && n == 1 && strcmp(v[0].str, "CET-1CEST,M3.5.0,M10.5.0/3") == 0);
    r = parse("badge_fw.saver 1\n", v, &n);
    CHECK(r.err == SB_ERR_SYNTAX && r.line == 1);
    r = parse("saver = 1\n", v, &n);
    CHECK(r.err == SB_ERR_SYNTAX);
    r = parse("badge_fw.saver = 1\nbadge_fw.saver = 0\n", v, &n);
    CHECK(r.err == SB_ERR_DUP && r.line == 2);
    r = parse("# nothing\n\n", v, &n);
    CHECK(r.err == SB_ERR_EMPTY);
    char big[300];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    r = parse(big, v, &n);
    CHECK(r.err == SB_ERR_TOO_LONG && r.line == 1);
    CHECK(sb_parse("badge_fw.saver = 1\0x\n", 21, v, 32, &n).err == SB_ERR_SYNTAX);
    char msg[80];
    r = parse("badge_fw.lock = 7\n", v, &n);
    sb_error_text(&r, msg, sizeof(msg));
    CHECK(strstr(msg, "Line 1") != NULL);
}

static void test_secrets_ignored(void) {
    sb_value_t v[32];
    size_t     n;
    char const file[] = "badge_fw.wifi_pass = hunter2\nbadge_fw.wifi_ssid = home\nbadge_fw.ota_token = 0123456789abcdef\n"
                        "a.meshcore.identity = zz\na.meshcore.pass = x\nbadge_fw.ota_wifi = 1\nbadge_fw.radio_on = 0\n"
                        "unknown.thing = 3\nbadge_fw.saver = 0\n";
    sb_result_t r = parse(file, v, &n);
    CHECK(r.err == SB_OK && n == 1 && r.ignored == 8);
    CHECK(strcmp(v[0].key->key, "saver") == 0 && v[0].num == 0);
    // a file with only secrets has nothing to load
    r = parse("badge_fw.wifi_pass = hunter2\n", v, &n);
    CHECK(r.err == SB_ERR_EMPTY && n == 0 && r.ignored == 1);
}

static void test_format_variants(void) {
    sb_value_t v[8];
    size_t     n;
    sb_result_t r = parse("  badge_fw.saver   =   1  \r\n\tbadge_fw.lock=0\r\n# c\r\n", v, &n);
    CHECK(r.err == SB_OK && n == 2 && v[0].num == 1 && v[1].num == 0);
    r = parse("badge_fw.lock = 1", v, &n); // no trailing newline
    CHECK(r.err == SB_OK && n == 1);
}

static void test_summary(void) {
    sb_value_t v[8];
    size_t     n;
    parse("badge_fw.saver = 0\nbadge_fw.lock = 1\nbadge_fw.tz = UTC0\nbadge_fw.idle_dim = 3\nbadge_fw.idle_off = 4\n", v, &n);
    char text[200];
    sb_summary(v, 1, 0, text, sizeof(text));
    CHECK(strcmp(text, "1 setting will change: Clock screensaver") == 0);
    sb_summary(v, 3, 2, text, sizeof(text));
    CHECK(strcmp(text, "3 settings will change: Clock screensaver, Pocket lock, Time zone. 2 lines skipped.") == 0);
    sb_summary(v, 5, 0, text, sizeof(text));
    CHECK(strstr(text, "5 settings will change:") && strstr(text, "and 1 more"));
    char tiny[20];
    sb_summary(v, 5, 1, tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny));
}

// An app's keys join the table when it registers (#230 E1d): any namespace, none known to the OS
static void test_app_keys(void) {
    static sb_key_t const pager[] = {
        {"a.pager", "n_dm", "Notify", SB_U8, 0, 1, 1, NULL},
        {"a.pager", "name", "Name", SB_STR, 1, 20, 0, "x"},
    };
    size_t base = sb_key_count();
    CHECK(sb_find("a.pager", "n_dm") == NULL && sb_find("a.meshcore", "n_dm") == NULL); // no app, no key
    CHECK(sb_add_app_keys(pager, 2) && sb_key_count() == base + 2);
    CHECK(sb_find("a.pager", "n_dm") == &pager[0] && sb_key_at(base + 1) == &pager[1] && sb_key_at(base + 2) == NULL);
    CHECK(sb_find("a.meshcore", "n_dm") == NULL && sb_find("a.pager", "pass") == NULL);
    CHECK(!sb_add_app_keys(NULL, 1) && !sb_add_app_keys(pager, 0) && !sb_add_app_keys(pager, SB_KEYS_MAX));
    sb_app_keys_clear();
    CHECK(sb_key_count() == base && sb_find("a.pager", "n_dm") == NULL);
}

int main(void) {
    test_app_keys();
    test_table();
    test_roundtrip();
    test_bad_values();
    test_secrets_ignored();
    test_format_variants();
    test_summary();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("settings_backup_test: all passed\n");
    return 0;
}
