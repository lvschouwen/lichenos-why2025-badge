// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "reg_ids.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The settings registry (#307/#308): one static table per chip, one entry per key. A get returns what the hardware or
// driver applies now (get_applied), never the request; a set is always answered with the value applied afterwards.
// Pure C, no ESP headers: built into the C6 image, the P4 firmware and the host tests.
//
// A table is a const array of reg_entry_t in flash; the functions take (table, count). The P4 mirrors the C6's keys
// in its own table with a getter that asks over REG_GET, so names, types and units of a C6 key appear in both tables.

// Wire values (lora_reg_val_t.type / .status): append-only, never renumbered
typedef enum {
    REG_T_U8 = 1,
    REG_T_I16,
    REG_T_U32,
    REG_T_I32,
    REG_T_BOOL,
    REG_T_ENUM, // 0..max, named by enum_names
    REG_T_STR,  // up to REG_STR_MAX characters; local to one chip, never sent over the wire
} reg_type_t;

typedef enum {
    REG_OK,
    REG_UNKNOWN, // no key with that id (or name)
    REG_RANGE,   // value outside the key's type or min..max
    REG_GUARDED, // the key changes what the badge transmits (TX): set only through its guarded path
    REG_RO,      // read-only key
    REG_FAILED,  // the setter or the getter failed in the driver
} reg_status_t;

#define REG_F_RO     0x01 // no set through the registry
#define REG_F_TX     0x02 // changes what the badge transmits: a set answers GUARDED (#307 rule, radio keys)
#define REG_F_REBOOT 0x04 // a set is saved and applies after a restart; until then applied differs from saved

#define REG_STR_MAX  15
#define REG_NAME_MAX 23 // longest dotted name, so a console column can be fixed

// One value; which member is valid follows the entry's type: u for U8/U32/BOOL/ENUM, i for I16/I32, s for STR. The
// wire carries u, the same 32 bits as i, so an I16 travels sign-extended.
typedef union {
    uint32_t u;
    int32_t  i;
    char     s[REG_STR_MAX + 1];
} reg_value_t;

typedef struct reg_entry reg_entry_t;

// Fill *out with the applied value; REG_OK, or REG_FAILED when the driver cannot tell
typedef reg_status_t (*reg_get_fn)(reg_entry_t const *e, reg_value_t *out);
// Apply v (already range-checked); REG_OK or REG_FAILED. Saving goes through the settings service under saved_key.
typedef reg_status_t (*reg_set_fn)(reg_entry_t const *e, reg_value_t const *v);

struct reg_entry {
    uint16_t           id;         // reg_id_t
    uint8_t            type;       // reg_type_t
    uint8_t            flags;      // REG_F_*
    char const        *name;       // dotted, lower case: "lora.freq"
    char const        *unit;       // "Hz", "%", "dBm"; NULL for none
    int64_t            min, max;   // inclusive, on top of the type's own range; for STR the length; for ENUM 0..count-1
    char const *const *enum_names; // ENUM: names of min..max indexed by value, NULL = shown as numbers
    reg_get_fn         get_applied;
    reg_set_fn         set;       // NULL = read-only
    char const        *saved_key; // the settings key the value is saved under (#224), NULL when it is not saved
};

reg_entry_t const *reg_find_id(reg_entry_t const *tab, size_t n, uint16_t id);
reg_entry_t const *reg_find_name(reg_entry_t const *tab, size_t n, char const *name);

// For `fw get [key|prefix]`: NULL or "" matches every key; "lora" and "lora." match "lora.freq"; "lora.freq" matches
// itself; "lora.f" matches nothing (a prefix ends at a dot)
bool reg_name_matches(char const *name, char const *query);

// Text to value, syntax only: decimal (no leading zeros, '-' only for I16/I32, no '+' or spaces) or 0x hex numbers that
// fit 32 bits; BOOL also on/off/true/false; ENUM a name or
// its number; STR up to REG_STR_MAX characters. false = not a value of this type. Range is reg_check's job.
bool reg_parse(reg_entry_t const *e, char const *text, reg_value_t *out);

// REG_OK, or REG_RANGE when v is outside the type or the entry's min..max. min == max == 0 means the type's own range,
// except for ENUM, whose range is always min..max
reg_status_t reg_check(reg_entry_t const *e, reg_value_t const *v);

// The value as text without unit (ENUM by name, BOOL as on/off); bytes written without the NUL, 0 if cap is too small
size_t reg_format(reg_entry_t const *e, reg_value_t const *v, char *buf, size_t cap);

// The applied value; REG_FAILED (and *out zeroed) when the entry has no getter, the getter fails or its value does not
// fit the key's type
reg_status_t reg_get(reg_entry_t const *e, reg_value_t *out);
// reg_get through another getter of the same key, with the same checks: the P4's cached read (#341) passes the
// domain's no-wait getter. NULL get is REG_FAILED.
reg_status_t reg_get_with(reg_entry_t const *e, reg_get_fn get, reg_value_t *out);

// The one set path: TX -> GUARDED, RO or no setter -> RO, outside the range -> RANGE, else the setter. Whatever the
// outcome, *applied is read back afterwards; when that read fails the result is REG_FAILED, whatever the set said.
reg_status_t reg_set(reg_entry_t const *e, reg_value_t const *v, reg_value_t *applied);

// What goes into a REG_VAL besides id, seq and status
typedef struct {
    uint8_t  type;  // reg_type_t of the key, 0 for an unknown id
    uint32_t value; // the applied value as its 32-bit pattern, 0 for an unknown id
} reg_reply_t;

// A REG_GET (set == NULL) or REG_SET (*set = the wire value) from the peer: an unknown id is REG_UNKNOWN with *out
// zeroed; a STR key is not served (REG_FAILED). Every other answer carries the key's type and its applied value.
reg_status_t reg_serve(reg_entry_t const *tab, size_t n, uint16_t id, uint32_t const *set, reg_reply_t *out);

// A reg_type_t this build knows: a REG_VAL with any other type is dropped, not formatted
bool reg_type_known(uint8_t type);

// A table's own consistency, for each chip's host test: the index of the first bad entry, or n when all are fine.
// Bad: a name that is not lower-case dotted ([a-z0-9_] segments, two or more, at most REG_NAME_MAX), an id or name
// seen before, an unknown type, no getter, min > max, a TX key with a setter but no RO flag, a STR longer than
// REG_STR_MAX, an ENUM without names for every value 0..max (min must be 0, max at most 255).
size_t reg_table_check(reg_entry_t const *tab, size_t n);

#ifdef __cplusplus
}
#endif
