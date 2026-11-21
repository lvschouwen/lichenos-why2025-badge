#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Wi-Fi and mDNS, the shell's side (#244, #216 decision 1): the Wi-Fi screens, the analyser, the status bar's link
// look and Settings go through here, never to lichen_net / lichen_mdns headers. System surface: tools/layers.conf
// forbids lichen_sys_* in L6. The types repeat lichen_net's (the gate cannot include it); lichen_sys_pure_view.c keeps
// them equal with asserts. The pure helpers forward one-for-one to lichen_net's pure code (wifi_list.c,
// wifi_analyser.c).

#define LICHEN_SYS_WIFI_SSID_MAX       32 // bytes, without the NUL
#define LICHEN_SYS_WIFI_SAVED_MAX      5  // saved networks
#define LICHEN_SYS_WIFI_AN_CHANNELS    13
#define LICHEN_SYS_WIFI_METER_LEN      60
#define LICHEN_SYS_WIFI_RSSI_NONE      INT8_MIN // no reading
#define LICHEN_SYS_WIFI_AN_SCAN_GAP_MS 10000    // scans disturb the link: never faster than this
#define LICHEN_SYS_MDNS_NAME_MAX       16       // "lichen-abcd" plus NUL

typedef struct {
    char    ssid[LICHEN_SYS_WIFI_SSID_MAX + 1];
    int8_t  rssi;
    bool    open;    // no password needed
    uint8_t channel; // 1-13 on 2.4 GHz, 0 when unknown
    bool    eap;     // Enterprise (802.1X): the screen asks for a login instead of a password (#496)
} lichen_sys_wifi_ap_t;

typedef enum {
    LICHEN_SYS_WIFI_JOIN_NONE,
    LICHEN_SYS_WIFI_JOIN_PENDING,
    LICHEN_SYS_WIFI_JOIN_TRYING,
    LICHEN_SYS_WIFI_JOIN_JOINED,
    LICHEN_SYS_WIFI_JOIN_FAILED_KEPT_OLD,
} lichen_sys_wifi_join_t;

typedef struct {
    uint8_t count[LICHEN_SYS_WIFI_AN_CHANNELS + 1]; // index = channel 1..13
    int8_t  best[LICHEN_SYS_WIFI_AN_CHANNELS + 1];  // strongest RSSI on the channel, RSSI_NONE when empty
} lichen_sys_wifi_chan_stats_t;

// The last LICHEN_SYS_WIFI_METER_LEN readings of one AP, a ring (use the functions below)
typedef struct {
    int8_t  v[LICHEN_SYS_WIFI_METER_LEN];
    uint8_t n;
    uint8_t head;
} lichen_sys_wifi_meter_t;

// A WPA2/WPA3-Enterprise login (#496), the shell's copy of lichen_net's wifi_eap_t (asserted equal in
// lichen_sys_pure_view.c). The password is write-only: nothing here prints it, the caller wipes its copy.
#define LICHEN_SYS_WIFI_EAP_IDENTITY_MAX 63
#define LICHEN_SYS_WIFI_EAP_USER_MAX     63
#define LICHEN_SYS_WIFI_EAP_PASS_MAX     127
#define LICHEN_SYS_WIFI_EAP_CA_MAX       95
#define LICHEN_SYS_WIFI_EAP_SERVER_MAX   127
typedef struct {
    uint8_t method; // LICHEN_SYS_WIFI_EAP_PEAP / TTLS
    uint8_t phase2; // LICHEN_SYS_WIFI_EAP_P2_MSCHAPV2 / PAP; PEAP is always MSCHAPv2
    char    identity[LICHEN_SYS_WIFI_EAP_IDENTITY_MAX + 1];
    char    username[LICHEN_SYS_WIFI_EAP_USER_MAX + 1];
    char    password[LICHEN_SYS_WIFI_EAP_PASS_MAX + 1];
    char    ca_path[LICHEN_SYS_WIFI_EAP_CA_MAX + 1];
    char    server[LICHEN_SYS_WIFI_EAP_SERVER_MAX + 1];
} lichen_sys_wifi_eap_t;
enum { LICHEN_SYS_WIFI_EAP_PEAP = 0, LICHEN_SYS_WIFI_EAP_TTLS = 1 };
enum { LICHEN_SYS_WIFI_EAP_P2_MSCHAPV2 = 0, LICHEN_SYS_WIFI_EAP_P2_PAP = 1 };
// The first thing wrong with a login, in the order the screen asks for the fields
typedef enum {
    LICHEN_SYS_WIFI_EAP_OK,
    LICHEN_SYS_WIFI_EAP_BAD_METHOD,
    LICHEN_SYS_WIFI_EAP_BAD_IDENTITY,
    LICHEN_SYS_WIFI_EAP_BAD_USER,
    LICHEN_SYS_WIFI_EAP_BAD_PASSWORD,
    LICHEN_SYS_WIFI_EAP_BAD_CA,
    LICHEN_SYS_WIFI_EAP_BAD_SERVER,
    LICHEN_SYS_WIFI_EAP_PAP_NO_CA,
    LICHEN_SYS_WIFI_EAP_PAP_NO_NAME,
} lichen_sys_wifi_eap_problem_t;
// What the applied login does about the server certificate
typedef enum {
    LICHEN_SYS_WIFI_EAP_CA_NONE,       // not an Enterprise network
    LICHEN_SYS_WIFI_EAP_CA_VERIFIED,   // CA and server name
    LICHEN_SYS_WIFI_EAP_CA_UNVERIFIED, // no CA: an impostor access point could take the login
    LICHEN_SYS_WIFI_EAP_CA_ONLY,       // a CA alone: any certificate that CA signed passes
} lichen_sys_wifi_eap_ca_t;

// The Wi-Fi service's state and credentials, for the status bar, About, Settings and the Wi-Fi screens (#284): quick,
// never waiting on a lock that may be held for long, except where said.
// The link has an IP. Any task.
bool                   lichen_sys_wifi_up(void);
// As lichen_sys_wifi_up; ip (may be NULL with cap 0) gets the address. Any task.
bool                   lichen_sys_wifi_ip(char *ip, size_t cap);
// The IPv6 address to show (a global one from SLAAC), "" while there is none beyond the link. Returns whether there is
// one. Any task.
bool                   lichen_sys_wifi_ip6(char *ip, size_t cap);
// A join is under way (status bar amber). Any task.
bool                   lichen_sys_wifi_joining(void);
// The badge has a network saved (first-run setup skips the Wi-Fi step, the status bar tells off from down). Any task.
bool                   lichen_sys_wifi_has_credentials(void);
// Signal of the connected AP, false while unknown. Any task.
bool                   lichen_sys_wifi_rssi(int8_t *out);
// Saves a network and applies it (Settings > Wi-Fi). Any task.
bool                   lichen_sys_wifi_join(char const *ssid, char const *pass);
// Saves an Enterprise network and applies it (#496): a trial join, saved only once it gets an IP; the login is copied, so
// the caller wipes its own. False when the login is invalid, there is no C6 link or the request could not be queued. Any task.
bool                   lichen_sys_wifi_join_eap(char const *ssid, lichen_sys_wifi_eap_t const *eap);
// Why the last Enterprise join did not start ("" when none): a reason in words for the screen. Any task.
char const            *lichen_sys_wifi_eap_fail_text(void);
// What the applied configuration does about the server certificate. Any task.
lichen_sys_wifi_eap_ca_t lichen_sys_wifi_eap_ca(void);
// Forgets one saved network (#160). Any task.
bool                   lichen_sys_wifi_forget(char const *ssid);
// The saved SSIDs only, read under the list lock (#240); SIZE_MAX = busy (#249). Any task.
size_t                 lichen_sys_wifi_saved_names(char (*out)[33], size_t max);
// Scans and returns the cleaned list (strongest per SSID). Blocks for seconds. Not on the LVGL task.
bool                   lichen_sys_wifi_scan(lichen_sys_wifi_ap_t *out, size_t max, size_t *n);
// Scans and returns every AP with its channel, for the analyser. Blocks for seconds. Not on the LVGL task.
bool                   lichen_sys_wifi_scan_raw(lichen_sys_wifi_ap_t *out, size_t max, size_t *n);
// The outcome of the last join; tried and kept get the SSIDs. Any task.
lichen_sys_wifi_join_t lichen_sys_wifi_join_state(char *tried, size_t tried_cap, char *kept, size_t kept_cap);

// A join state of lichen_net as the gate's value (lichen_sys_wifi_join_state hands the service's state through here).
// Pure, any task.
lichen_sys_wifi_join_t lichen_sys_wifi_join_of(int state);
// Drops empty SSIDs, keeps the strongest entry per SSID, sorts strongest first; returns the new count. Pure, any task.
size_t                 lichen_sys_wifi_list_clean(lichen_sys_wifi_ap_t *aps, size_t n);
// Copies an SSID with control bytes and invalid UTF-8 as \xNN; always terminated; returns the length. Pure, any task.
size_t                 lichen_sys_wifi_ssid_sanitize(char const *in, char *out, size_t cap);
// Signal bars 0-4. Pure, any task.
int                    lichen_sys_wifi_bars(int8_t rssi);
// A bar fill 0-100 for a level. Pure, any task.
int                    lichen_sys_wifi_level_pct(int8_t rssi);
// SSID 1-32 bytes, password empty (open network) or 8-63 bytes. Pure, any task.
bool                   lichen_sys_wifi_cred_valid(char const *ssid, char const *pass);
// The first thing wrong with a login. Pure, any task.
lichen_sys_wifi_eap_problem_t lichen_sys_wifi_eap_problem(lichen_sys_wifi_eap_t const *eap);
// The refusal in plain words ("" for OK). Pure, any task.
char const                   *lichen_sys_wifi_eap_problem_text(lichen_sys_wifi_eap_problem_t p);
// A CA file path as a field accepts it (empty: none). Pure, any task.
bool                   lichen_sys_wifi_eap_ca_path_valid(char const *path);
// A server name as a field accepts it (empty: none). Pure, any task.
bool                   lichen_sys_wifi_eap_server_valid(char const *name);
// Counts every change of the saved credentials, so a cached SSID knows to read again. Any task.
uint32_t               lichen_sys_wifi_cred_generation(void);
// APs per 2.4 GHz channel (hidden ones too). Pure, any task.
void   lichen_sys_wifi_chan_stats(lichen_sys_wifi_ap_t const *aps, size_t n, lichen_sys_wifi_chan_stats_t *out);
// Empties a signal history. Pure, any task.
void   lichen_sys_wifi_meter_clear(lichen_sys_wifi_meter_t *m);
// Adds a reading (RSSI_NONE when a scan did not see the AP). Pure, any task.
void   lichen_sys_wifi_meter_push(lichen_sys_wifi_meter_t *m, int8_t rssi);
// Reading i, 0 = oldest held; RSSI_NONE past the end. Pure, any task.
int8_t lichen_sys_wifi_meter_at(lichen_sys_wifi_meter_t const *m, size_t i);
// The strongest AP called ssid in a raw scan; false when it was not seen. Pure, any task.
bool lichen_sys_wifi_an_lookup(lichen_sys_wifi_ap_t const *aps, size_t n, char const *ssid, lichen_sys_wifi_ap_t *out);
// A scan is due when none ran yet or LICHEN_SYS_WIFI_AN_SCAN_GAP_MS passed (the clock may wrap). Pure, any task.
bool lichen_sys_wifi_an_scan_due(bool ever, uint32_t last_ms, uint32_t now_ms);

// "lichen-xxxx" (no ".local"), from the MAC; valid from boot. Any task.
char const *lichen_sys_mdns_name(void);
