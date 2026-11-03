// Host test: the saved Wi-Fi networks list (#160): add / forget / migrate / pick the strongest known one
#include "wifi_saved.h"

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

static wifi_ap_t ap(char const *ssid, int rssi, bool open) {
    wifi_ap_t a = {0};
    strcpy(a.ssid, ssid);
    a.rssi = (int8_t)rssi;
    a.open = open;
    return a;
}

static void add_forget(void) {
    wifi_saved_t l = {0};
    CHECK(!wifi_saved_add(&l, "", "password1"));
    CHECK(!wifi_saved_add(&l, "net", "short"));
    CHECK(wifi_saved_add(&l, "one", "password1") && wifi_saved_add(&l, "two", "") && l.n == 2);
    CHECK(strcmp(l.e[0].ssid, "two") == 0 && strcmp(l.e[1].ssid, "one") == 0); // newest first
    CHECK(wifi_saved_add(&l, "one", "password2") && l.n == 2); // same SSID: new password, to the front
    CHECK(strcmp(l.e[0].ssid, "one") == 0 && strcmp(l.e[0].pass, "password2") == 0 && strcmp(l.e[1].ssid, "two") == 0);
    CHECK(l.e[1].pass[0] == '\0');
    for (int i = 0; i < 7; i++) { // more than the list holds: the oldest go
        char name[8];
        snprintf(name, sizeof(name), "n%d", i);
        CHECK(wifi_saved_add(&l, name, "password1"));
    }
    CHECK(l.n == WIFI_SAVED_MAX && strcmp(l.e[0].ssid, "n6") == 0 && wifi_saved_find(&l, "one") < 0);
    CHECK(wifi_saved_forget(&l, "n5") && l.n == WIFI_SAVED_MAX - 1 && strcmp(l.e[1].ssid, "n4") == 0);
    CHECK(!wifi_saved_forget(&l, "n5"));
    CHECK(l.e[l.n].ssid[0] == '\0' && l.e[l.n].pass[0] == '\0'); // the freed slot holds nothing
}

static void migrate(void) {
    wifi_saved_t l = {0};
    CHECK(wifi_saved_migrate(&l, "old", "oldpassword") && l.n == 1 && strcmp(l.e[0].ssid, "old") == 0);
    CHECK(!wifi_saved_migrate(&l, "other", "otherpassword") && l.n == 1); // only into an empty list
    wifi_saved_t e = {0};
    CHECK(!wifi_saved_migrate(&e, "", "") && e.n == 0);
}

static void pick(void) {
    wifi_saved_t l = {0};
    wifi_saved_add(&l, "home", "homepassword");
    wifi_saved_add(&l, "work", "workpassword");
    wifi_ap_t aps[] = {ap("stranger", -30, false), ap("home", -70, false), ap("work", -50, false)};
    int       at    = wifi_saved_pick(&l, aps, 3, 0);
    CHECK(at >= 0 && strcmp(l.e[at].ssid, "work") == 0); // strongest known, not the strongest AP
    uint32_t mask = 1u << at;
    at            = wifi_saved_pick(&l, aps, 3, mask);
    CHECK(at >= 0 && strcmp(l.e[at].ssid, "home") == 0); // work was tried: the next one
    mask |= 1u << at;
    CHECK(wifi_saved_pick(&l, aps, 3, mask) == -1); // all tried
    wifi_ap_t twin[] = {ap("home", -40, true)}; // an open AP with a saved secured name is not that network
    CHECK(wifi_saved_pick(&l, twin, 1, 0) == -1);
    CHECK(wifi_saved_pick(&l, aps, 0, 0) == -1);
}

// Evil twin: a saved OPEN network is never joined by itself, whoever broadcasts its name
static void open_never_auto(void) {
    wifi_saved_t l = {0};
    wifi_saved_add(&l, "cafe", "");
    wifi_ap_t aps[] = {ap("cafe", -30, true)};
    CHECK(wifi_saved_pick(&l, aps, 1, 0) == -1);
    wifi_saved_add(&l, "home", "homepassword");
    wifi_ap_t both[] = {ap("cafe", -30, true), ap("home", -80, false)};
    int       at     = wifi_saved_pick(&l, both, 2, 0);
    CHECK(at >= 0 && strcmp(l.e[at].ssid, "home") == 0); // the weaker secured one, not the stronger open one
}

// A stronger open twin of a saved secured name must not hide the real AP in a raw scan
static void raw_scan_twin(void) {
    wifi_saved_t l = {0};
    wifi_saved_add(&l, "home", "homepassword");
    wifi_ap_t raw[] = {ap("home", -30, true), ap("home", -60, false)};
    int       at    = wifi_saved_pick(&l, raw, 2, 0);
    CHECK(at == 0);
}

int main(void) {
    add_forget();
    migrate();
    pick();
    open_never_auto();
    raw_scan_twin();
    if (failures) {
        return 1;
    }
    printf("wifi_saved_test: ok\n");
    return 0;
}
