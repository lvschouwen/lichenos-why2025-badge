// Settings > Apps (#233): which rows an app's page gets, the Background switch's state, and every text on the page
#include "app_page_model.h"

#include "lichen_caps.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECK_STR(got, want) do { if (strcmp((got), (want)) != 0) { \
    printf("FAIL %s:%d \"%s\" != \"%s\"\n", __FILE__, __LINE__, (got), (want)); fails++; } } while (0)

static void rows(void) {
    app_page_app_t  a = {.caps = CAP_NOTIFY, .has_background = true};
    app_page_rows_t r = app_page_rows(&a); // Weather: a background part the person may switch
    CHECK(r.listed && r.data && r.clear && !r.clear_note && r.background && r.bg == APP_BG_SWITCH && r.crashes);

    a.has_background = false; // Timer, Notes: no background part
    r = app_page_rows(&a);
    CHECK(r.listed && r.data && r.clear && r.background && r.bg == APP_BG_NONE);

    app_page_app_t mesh = {.caps = CAP_LORA_TX | CAP_LORA_RX, .has_background = true, .exempt = true, .secret_storage = true};
    r = app_page_rows(&mesh); // MeshCore: kept on, identity and keys are not cleared here
    CHECK(r.listed && r.data && !r.clear && r.clear_note && r.bg == APP_BG_LOCKED && r.crashes);

    app_page_app_t sys = {.caps = CAP_SYSTEM};
    r = app_page_rows(&sys); // an OS app with no page of its own is not listed
    CHECK(!r.listed && !r.data && !r.clear && !r.clear_note && !r.background && r.crashes);
    sys.has_hooks = true; // About, Settings: listed, no data rows, nothing to clear or switch
    r = app_page_rows(&sys);
    CHECK(r.listed && !r.data && !r.clear && !r.background && r.bg == APP_BG_NONE);
    sys.has_background = true; // an OS app's background part is never the person's to switch
    CHECK(!app_page_rows(&sys).background);

    app_page_app_t plain = {0}; // an app with no hooks that is not the OS's is listed: it has data and crashes
    CHECK(app_page_rows(&plain).listed);
}

static void caps_words(void) {
    for (unsigned bit = 0; bit < CAP_COUNT; bit++) { // a new capability needs its words
        if ((1u << bit) == CAP_SYSTEM) {
            CHECK(app_page_cap_words(bit) == NULL);
        } else {
            char const *w = app_page_cap_words(bit);
            CHECK(w && w[0] && !strchr(w, '_') && !(w[0] >= 'A' && w[0] <= 'Z' && w[1] >= 'A' && w[1] <= 'Z'));
        }
    }
    CHECK(app_page_cap_words(CAP_COUNT) == NULL);
    char b[APP_PAGE_CAPS_MAX];
    app_page_caps_text(0, b, sizeof(b));
    CHECK_STR(b, "Uses nothing special.");
    app_page_caps_text(CAP_SD | CAP_NOTIFY, b, sizeof(b));
    CHECK_STR(b, "Uses the SD card, notifications.");
    app_page_caps_text(CAP_SYSTEM | CAP_SD, b, sizeof(b));
    CHECK_STR(b, "Part of Lichen: it may use everything.");
    app_page_caps_text(CAP_BACKGROUND, b, sizeof(b));
    CHECK_STR(b, "Uses background tasks.");
    app_page_caps_text(0x1FFFFu, b, sizeof(b)); // every real one: fits the buffer whole and ends in a full stop
    CHECK(strlen(b) < sizeof(b) - 1 && b[strlen(b) - 1] == '.' && strstr(b, "the battery."));
    char small[12];
    size_t n = app_page_caps_text(0x1FFFFu, small, sizeof(small));
    CHECK(n == strlen(small) && n < sizeof(small));
    n = app_page_caps_text(CAP_SD, small, 3);
    CHECK(n == strlen(small) && n < 3);
    CHECK(app_page_caps_text(CAP_SD, small, 0) == 0);
}

static app_life_t life(life_state_t s, life_state_t rest) {
    app_life_t l = {0};
    l.state      = (uint8_t)s;
    l.rest       = (uint8_t)rest;
    return l;
}

static void running(void) {
    app_life_t l = life(LIFE_BACKGROUND, LIFE_BACKGROUND);
    CHECK(app_page_bg_running(&l));
    l = life(LIFE_OPEN, LIFE_BACKGROUND);
    CHECK(app_page_bg_running(&l));
    l = life(LIFE_SUSPENDED, LIFE_BACKGROUND);
    CHECK(app_page_bg_running(&l));
    l = life(LIFE_OPEN, LIFE_REGISTERED); // a screen open, no background part running
    CHECK(!app_page_bg_running(&l));
    l = life(LIFE_OPEN, LIFE_FAULTED);
    CHECK(!app_page_bg_running(&l));
    l = life(LIFE_REGISTERED, LIFE_REGISTERED);
    CHECK(!app_page_bg_running(&l));
    l = life(LIFE_FAULTED, LIFE_FAULTED);
    CHECK(!app_page_bg_running(&l));
    l = life(LIFE_STOPPING, LIFE_BACKGROUND); // once a stop began it is not "running"
    CHECK(!app_page_bg_running(&l));
    l = life(LIFE_STOPPED, LIFE_BACKGROUND);
    CHECK(!app_page_bg_running(&l));
}

static void background_text(void) {
    char b[32];
    app_page_bg_text(APP_BG_NONE, true, false, false, b, sizeof(b));
    CHECK_STR(b, "None");
    app_page_bg_text(APP_BG_LOCKED, false, false, false, b, sizeof(b)); // whatever the list says, the OS keeps it on
    CHECK_STR(b, "Always on");
    app_page_bg_text(APP_BG_SWITCH, true, true, false, b, sizeof(b));
    CHECK_STR(b, "On");
    app_page_bg_text(APP_BG_SWITCH, true, false, false, b, sizeof(b)); // switched on again, starts at the next boot
    CHECK_STR(b, "On after restart");
    app_page_bg_text(APP_BG_SWITCH, false, true, false, b, sizeof(b));
    CHECK_STR(b, "Off after restart");
    app_page_bg_text(APP_BG_SWITCH, false, false, false, b, sizeof(b));
    CHECK_STR(b, "Off");
    app_page_bg_text(APP_BG_SWITCH, false, false, true, b, sizeof(b)); // safe mode (#232)
    CHECK_STR(b, "Off after crashes");
    char small[4];
    size_t n = app_page_bg_text(APP_BG_SWITCH, true, false, false, small, sizeof(small));
    CHECK(n == strlen(small) && n < sizeof(small));
    CHECK(app_page_bg_text(APP_BG_SWITCH, true, false, false, small, 0) == 0);
}

static void data_and_crash_text(void) {
    char b[48];
    app_page_crash_text(0, 3, b, sizeof(b));
    CHECK_STR(b, "None in the last 3 boots");
    app_page_crash_text(1, 3, b, sizeof(b));
    CHECK_STR(b, "1 in the last 3 boots");
    app_page_crash_text(2, 3, b, sizeof(b));
    CHECK_STR(b, "2 in the last 3 boots");
    CHECK(app_page_crash_text(2, 3, b, 0) == 0);

    lichen_app_data_t d = {.ok = true};
    app_page_data_text(&d, b, sizeof(b));
    CHECK_STR(b, "None");
    app_page_sd_text(&d, b, sizeof(b));
    CHECK_STR(b, "No card");
    d.settings_bytes = 96;
    d.files_bytes    = 2000;
    d.sd_known       = true;
    app_page_data_text(&d, b, sizeof(b));
    CHECK_STR(b, "2.0 KB");
    app_page_sd_text(&d, b, sizeof(b));
    CHECK_STR(b, "None");
    d.sd_bytes = 3 * 1024 * 1024;
    app_page_sd_text(&d, b, sizeof(b));
    CHECK_STR(b, "3.0 MB");
    d.settings_bytes = 640; // settings alone count
    d.files_bytes    = 0;
    app_page_data_text(&d, b, sizeof(b));
    CHECK_STR(b, "640 B");
}

static void clear_texts(void) {
    char b[160];
    app_page_clear_question("Weather", b, sizeof(b));
    CHECK_STR(b, "Clear all data of Weather? Its settings and files are erased and cannot come back.");
    app_page_clear_question(NULL, b, sizeof(b));
    CHECK(strstr(b, "this app") != NULL);
    char small[20];
    size_t n = app_page_clear_question("Weather", small, sizeof(small));
    CHECK(n == strlen(small) && n < sizeof(small));

    app_page_clear_msg_t m;
    lichen_app_clear_t   c = {.settings_ok = true, .files_ok = true, .sd_ok = true, .bytes = 12 * 1024};
    app_page_clear_msg(&c, false, &m);
    CHECK(m.ok && strcmp(m.text, "Cleared 12 KB.") == 0 && m.next[0] == '\0');
    app_page_clear_msg(&c, true, &m); // a running app keeps its state in RAM: say so
    CHECK(m.ok && strstr(m.text, "Restart the badge") != NULL);
    c.bytes = 0;
    app_page_clear_msg(&c, false, &m);
    CHECK(m.ok && strcmp(m.text, "Cleared.") == 0);
    c.settings_ok = false;
    app_page_clear_msg(&c, false, &m);
    CHECK(!m.ok && strstr(m.text, "settings") && m.next[0]);
    c.settings_ok = true;
    c.files_ok    = false;
    app_page_clear_msg(&c, true, &m);
    CHECK(!m.ok && strstr(m.text, "files") && !strstr(m.text, "SD") && !strstr(m.text, "Restart"));
    c.files_ok = true;
    c.sd_ok    = false;
    app_page_clear_msg(&c, false, &m);
    CHECK(!m.ok && strstr(m.text, "SD card"));
}

int main(void) {
    rows();
    caps_words();
    running();
    background_text();
    data_and_crash_text();
    clear_texts();
    if (fails) {
        printf("app_page_model_test: %d failed\n", fails);
        return 1;
    }
    printf("app_page_model_test: ok\n");
    return 0;
}
