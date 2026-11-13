// The ESP-free part of lichen_sys_storage.h, lichen_sys_update.h and lichen_sys_factory.h (#227): the SD text, the
// update auto-off steps and the factory combo, one-for-one over sd_policy.c, ota_autooff.c and factory_plan.c. Kept
// apart from the glue so host_tests/lichen_sys_pure_test.c links it. The gate's types repeat the components' (it cannot
// include them); the asserts below keep them equal, so the conversions are plain copies.
#include "lichen_sys_factory.h"
#include "lichen_sys_storage.h"
#include "lichen_sys_update.h"

#include "factory_plan.h"
#include "ota_autooff.h"
#include "sd_policy.h"

#include <stddef.h>
#include <string.h>

_Static_assert((int)LICHEN_SD_OFF == (int)SD_STATE_OFF && (int)LICHEN_SD_NO_CARD == (int)SD_STATE_NO_CARD &&
                   (int)LICHEN_SD_NOT_FAT == (int)SD_STATE_NOT_FAT && (int)LICHEN_SD_ERROR == (int)SD_STATE_ERROR &&
                   (int)LICHEN_SD_MOUNTED == (int)SD_STATE_MOUNTED && (int)LICHEN_SD_REMOVED == (int)SD_STATE_REMOVED,
               "the gate's SD states follow sdcard's");
_Static_assert((int)LICHEN_SD_IO_OK == (int)SD_IO_OK && (int)LICHEN_SD_IO_FAILED == (int)SD_IO_FAILED &&
                   (int)LICHEN_SD_IO_NEUTRAL == (int)SD_IO_NEUTRAL,
               "the gate's SD batch results follow sdcard's");

// A mirror field matches when it sits at the same offset with the same size (and so the same type width)
#define MIRROR_FIELD(mine, theirs, field)                                                                              \
    _Static_assert(offsetof(mine, field) == offsetof(theirs, field) &&                                                 \
                       sizeof(((mine *)0)->field) == sizeof(((theirs *)0)->field),                                     \
                   "the gate's " #mine "." #field " follows " #theirs)
_Static_assert(sizeof(lichen_sd_info_t) == sizeof(sd_info_t), "the gate's SD info follows sdcard's");
MIRROR_FIELD(lichen_sd_info_t, sd_info_t, state);
MIRROR_FIELD(lichen_sd_info_t, sd_info_t, total_bytes);
MIRROR_FIELD(lichen_sd_info_t, sd_info_t, free_bytes);
MIRROR_FIELD(lichen_sd_info_t, sd_info_t, free_known);
MIRROR_FIELD(lichen_sd_info_t, sd_info_t, kind);
MIRROR_FIELD(lichen_sd_info_t, sd_info_t, freq_khz);
MIRROR_FIELD(lichen_sd_info_t, sd_info_t, width);
MIRROR_FIELD(lichen_sd_info_t, sd_info_t, io_errors);
MIRROR_FIELD(lichen_sd_info_t, sd_info_t, generation);
MIRROR_FIELD(lichen_sd_info_t, sd_info_t, reason);
_Static_assert((int)LICHEN_FACTORY_NONE == (int)FACTORY_NONE && (int)LICHEN_FACTORY_SETTINGS == (int)FACTORY_SETTINGS &&
                   (int)LICHEN_FACTORY_ALL == (int)FACTORY_ALL,
               "the gate's factory levels follow lichen_boot's");
_Static_assert((int)LICHEN_FC_IDLE == (int)FC_IDLE && (int)LICHEN_FC_HOLDING == (int)FC_HOLDING &&
                   (int)LICHEN_FC_ARMED == (int)FC_ARMED && (int)LICHEN_FC_OVER == (int)FC_OVER &&
                   (int)LICHEN_FC_EV_NONE == (int)FC_EV_NONE && (int)LICHEN_FC_EV_ARM == (int)FC_EV_ARM &&
                   (int)LICHEN_FC_EV_CONFIRM == (int)FC_EV_CONFIRM && (int)LICHEN_FC_EV_CANCEL == (int)FC_EV_CANCEL,
               "the gate's factory combo follows lichen_boot's");
_Static_assert(sizeof(lichen_fc_combo_t) == sizeof(factory_combo_t), "the gate's factory combo follows lichen_boot's");
MIRROR_FIELD(lichen_fc_combo_t, factory_combo_t, state);
MIRROR_FIELD(lichen_fc_combo_t, factory_combo_t, a_down);
MIRROR_FIELD(lichen_fc_combo_t, factory_combo_t, b_down);
MIRROR_FIELD(lichen_fc_combo_t, factory_combo_t, since);

// The same fields in the same order, checked by sizeof above: a copy converts
static void info_from(sd_info_t *dst, lichen_sd_info_t const *src) {
    memcpy(dst, src, sizeof(*dst));
}

lichen_sd_io_t lichen_sys_sd_io_from_errno(int err) {
    return (lichen_sd_io_t)sd_io_from_errno(err);
}

void lichen_sys_sd_fmt_size(uint64_t bytes, char *out, size_t cap) {
    sd_fmt_size(bytes, out, cap);
}

void lichen_sys_sd_short_text(lichen_sd_info_t const *info, char *out, size_t cap) {
    sd_info_t in;
    info_from(&in, info);
    sd_short_text(&in, out, cap);
}

void lichen_sys_sd_status_line(lichen_sd_info_t const *info, char *out, size_t cap) {
    sd_info_t in;
    info_from(&in, info);
    sd_status_line(&in, out, cap);
}

uint8_t lichen_sys_update_auto_off_next(uint8_t minutes) {
    return ota_autooff_next(minutes);
}

void lichen_sys_update_auto_off_text(uint8_t minutes, char *out, size_t cap) {
    ota_autooff_text(minutes, out, cap);
}

char const *lichen_sys_factory_level_name(lichen_factory_level_t level) {
    return factory_level_name((factory_level_t)level);
}

void lichen_sys_factory_combo_init(lichen_fc_combo_t *c) {
    factory_combo_t in;
    factory_combo_init(&in);
    memcpy(c, &in, sizeof(*c));
}

lichen_fc_event_t lichen_sys_factory_combo_key(lichen_fc_combo_t *c, uint8_t code, bool pressed, bool injected,
                                               uint32_t now_ms) {
    factory_combo_t in;
    memcpy(&in, c, sizeof(in));
    fc_event_t e = factory_combo_event(&in, code, pressed, injected, now_ms);
    memcpy(c, &in, sizeof(*c));
    return (lichen_fc_event_t)e;
}

lichen_fc_event_t lichen_sys_factory_combo_tick(lichen_fc_combo_t *c, uint32_t now_ms) {
    factory_combo_t in;
    memcpy(&in, c, sizeof(in));
    fc_event_t e = factory_combo_tick(&in, now_ms);
    memcpy(c, &in, sizeof(*c));
    return (lichen_fc_event_t)e;
}

bool lichen_sys_factory_combo_swallows(lichen_fc_combo_t const *c) {
    factory_combo_t in;
    memcpy(&in, c, sizeof(in));
    return factory_combo_swallows(&in);
}
