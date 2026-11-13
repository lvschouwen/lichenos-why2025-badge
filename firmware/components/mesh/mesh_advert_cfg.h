#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The advert, location and auto-add settings of MeshCore (#52): the rules as pure logic (host_tests/
// mesh_advert_cfg_test.c). mesh_task.cpp applies and stores them; the screens and the console go through mesh.h.

// ---- the periodic advert -----------------------------------------------------------------------------------------
// MyMesh's _prefs.advert_interval, in minutes, 0 = off. Upstream limits (CommonCLI): a local advert every 60-240 min,
// a flood advert every 3-168 h. Ours are flood adverts, so the flood limits apply: nothing under 3 h, nothing over 7 d.
#define ADVCFG_INTERVAL_MIN     180
#define ADVCFG_INTERVAL_MAX     10080
#define ADVCFG_INTERVAL_DEFAULT 720 // 12 h
// The schedule's keys in the app's settings (a.meshcore, lichen_settings.h): interval (u32 minutes), last advert (u32
// Unix seconds) and the manual advert's flood flag (u8).
#define ADV_KEY_INTERVAL "mc_advint"
#define ADV_KEY_LAST     "mc_advlast"
#define ADV_KEY_FLOOD    "mc_advfl"
// Two adverts of ours are never closer than this, whatever asked for them (a held key on Advertise, a periodic one
// due right after a manual one)
#define ADVCFG_MIN_GAP_S 30
// A periodic advert never goes out before this many seconds after the radio and clock came up
#define ADVCFG_FIRST_DELAY_S 120
#define ADVCFG_RETRY_S       60

bool advcfg_interval_ok(uint32_t minutes); // 0 or within the limits
// "off", or minutes (180), hours "6h", days "2d". NULL when it parsed into *minutes within the limits, else why not
char const *advcfg_parse_interval(char const *text, uint32_t *minutes);
// "off", "3 h", "12 h", "2 d", "200 min": for the settings row
void advcfg_interval_text(uint32_t minutes, char *out, size_t cap);
// The next value when Enter cycles through the presets: off, 3 h, 6 h, 12 h, 24 h, 2 d, 7 d, off...
uint32_t advcfg_interval_cycle(uint32_t minutes);

// When the next periodic advert is due (Unix seconds): last + interval, or first_delay after now when none went out
// yet; if the stored last advert lies in the future (the clock was set back) it counts as now. 0 = none (off, or the
// clock is not set).
uint32_t advcfg_next_due(uint32_t interval_min, uint32_t last_epoch, uint32_t now_epoch);
// Seconds until it, 0 when due; UINT32_MAX when there is none
uint32_t advcfg_next_in_s(uint32_t next_epoch, uint32_t now_epoch);
// The time to the next advert for a badge: "3h", "12m", "<1m" (cap >= 8); "" when next_in_s is UINT32_MAX. Short
// enough for the space beside a grid icon (#193): the grid marks it as a countdown with a dot, the console says "in"
void advcfg_next_badge(uint32_t next_in_s, char *out, size_t cap);
// The same for a row with room: "In 3 min", "In 12 h", "In 3 d" (a unit with a space, #136)
void advcfg_next_text(uint32_t next_in_s, char *out, size_t cap);
// A manual or periodic advert may go now (the minimum gap since the last, in uptime seconds; 0 = none yet)
// When to try the periodic advert again after a refusal: a minute on (ADVCFG_RETRY_S) for the soft reasons (clock not
// set or approximate, not set up, LoRa off, the minimum gap), a whole interval for a hard one (damaged identity)
uint32_t advcfg_retry_at(uint32_t now_epoch, uint32_t interval_min, bool hard);
bool advcfg_gap_ok(uint32_t last_s, uint32_t now_s);

// ---- location ----------------------------------------------------------------------------------------------------
// NULL when text is a decimal number within range (lat +-90, lon +-180) stored in *out, else the reason
char const *advcfg_parse_coord(char const *text, bool is_lat, double *out);
// A coordinate submitted on Advert & contacts (#405): NULL when it is to be saved (in *out), else why not, nothing
// saved. The position goes out in later adverts, so a remote key never changes it:
//   - an invalid value is refused for its own reason whoever typed it (harmless, so a remote key may try one);
//   - a valid one needs a hand's Enter (hand_ok: key_cause_hand_only_ok), else ADVCFG_COORD_HAND_ONLY;
//   - and text a hand typed all of (typed_by_hand: text_entry_typed_by_hand), else ADVCFG_COORD_TYPED_REMOTELY: a
//     value a remote key typed and left open is not saved by the owner's next Enter.
#define ADVCFG_COORD_HAND_ONLY      "saving a position takes a hand (remote keys may not)"
#define ADVCFG_COORD_TYPED_REMOTELY "typed remotely: retype it by hand"
char const *advcfg_coord_submit(char const *text, bool is_lat, bool hand_ok, bool typed_by_hand, double *out);
// Any change on Advert & contacts (interval, flood, location, sharing, auto-add; #405): NULL when hand_ok
// (key_cause_hand_only_ok of the key that asked), else ADVCFG_CHANGE_HAND_ONLY. The page's own lock: the remote key
// rule reads the focus ahead of LVGL, so a key can land on another row than the one it was judged on.
#define ADVCFG_CHANGE_HAND_ONLY "changing this takes a hand (remote keys may not)"
char const *advcfg_change_refusal(bool hand_ok);
// Both coordinates, "52.370200 4.895200"
void advcfg_coord_text(double lat, double lon, char *out, size_t cap);
// Sharing the location needs one: (0, 0) is "not set" (a node at the null island would be a default, not a place)
bool advcfg_location_set(double lat, double lon);

// ---- auto-add contacts -------------------------------------------------------------------------------------------
// MyMesh's prefs: manual_add_contacts bit 0 set = manual, then only the types in autoadd_config bits 1-4 are added;
// clear = every advert is added. autoadd_config bit 0 = overwrite the oldest non-favourite when full. autoadd_max_hops:
// 0 no limit, 1 direct only, N up to N-1 hops.
#define ADVCFG_AA_OVERWRITE 0x01
#define ADVCFG_AA_CHAT      0x02
#define ADVCFG_AA_REPEATER  0x04
#define ADVCFG_AA_ROOM      0x08
#define ADVCFG_AA_SENSOR    0x10
#define ADVCFG_AA_TYPES     (ADVCFG_AA_CHAT | ADVCFG_AA_REPEATER | ADVCFG_AA_ROOM | ADVCFG_AA_SENSOR)
#define ADVCFG_HOPS_MAX     64 // MyMesh's own cap on autoadd_max_hops

typedef struct {
    bool    on;       // adverts of unknown nodes are added at all
    uint8_t types;    // ADVCFG_AA_* type bits, meaningful when on
    bool    overwrite; // replace the oldest non-favourite when the table is full
    uint8_t max_hops; // as autoadd_max_hops
} advcfg_autoadd_t;

// The setting as MyMesh's three prefs have it, and back
void advcfg_autoadd_decode(uint8_t manual_add, uint8_t config, uint8_t max_hops, advcfg_autoadd_t *out);
void advcfg_autoadd_encode(advcfg_autoadd_t const *in, uint8_t *manual_add, uint8_t *config, uint8_t *max_hops);
// What MyMesh does with an advert from an unknown node of this ADV_TYPE_* after hops hops: the host test pins the
// rules of BaseChatMesh::onAdvertRecv to this
typedef enum { ADVCFG_ADD, ADVCFG_SKIP_TYPE, ADVCFG_SKIP_HOPS, ADVCFG_REPLACE_OLDEST, ADVCFG_FULL } advcfg_decision_t;
advcfg_decision_t advcfg_autoadd_decide(advcfg_autoadd_t const *a, uint8_t adv_type, uint8_t hops, bool table_full,
                                        bool has_non_favourite);
// The ADV_TYPE_* (1 chat, 2 repeater, 3 room, 4 sensor) as its type bit, 0 for others
uint8_t advcfg_type_bit(uint8_t adv_type);
// "any", "direct only", "up to 3 hops"
void advcfg_hops_text(uint8_t max_hops, char *out, size_t cap);
// The next value when Enter cycles: any, direct only, 2 hops ... 
uint8_t advcfg_hops_cycle(uint8_t max_hops);
// "any", "direct", or a number of hops N (0 = direct) as the console types it; *max_hops is autoadd_max_hops
char const *advcfg_parse_hops(char const *text, uint8_t *max_hops);
// "all", or names separated by commas or spaces: chat/companion, repeater, room, sensor; *types are ADVCFG_AA_* bits
char const *advcfg_parse_types(char const *text, uint8_t *types);
// "Companions, Repeaters": the types on, for a row ("no types" for none)
void advcfg_types_text(uint8_t types, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
