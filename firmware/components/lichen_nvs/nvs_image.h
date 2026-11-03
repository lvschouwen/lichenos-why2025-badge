#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// NVS encryption (#116), the pure part: what is in the nvs partition (plaintext, encrypted or empty), what the boot does
// about it, and the RAM copy a migration carries over. Host-tested in host_tests/nvs_image_test.c; lichen_nvs.c does
// the flash, eFuse and NVS calls.
//
// Looking at the partition never needs the key: IDF's XTS encryption leaves the page header and the entry-state table
// in the clear and encrypts only the 32-byte entries. A plaintext entry header carries a CRC over itself; an encrypted
// one decrypts to noise, so the CRC of the first written entry of a page tells the two apart (2^-32 odds of a false
// "plain").

#define NVS_IMG_PAGE_SIZE 4096

typedef enum {
    NVS_IMG_EMPTY,     // no written entry anywhere: fresh or erased
    NVS_IMG_PLAIN,     // readable without a key
    NVS_IMG_ENCRYPTED, // entries that do not check out as plaintext
    NVS_IMG_UNKNOWN,   // could not be looked at (no partition, no memory, read error): never acted on
} nvs_image_kind_t;

typedef struct {
    unsigned plain; // pages whose first written entry has a valid CRC
    unsigned bad;   // pages whose first written entry does not
} nvs_image_tally_t;

// Adds one page (NVS_IMG_PAGE_SIZE bytes, as read from flash) to the tally. Pages with no written entry count for nothing.
void             nvs_image_tally_page(nvs_image_tally_t *t, uint8_t const *page);
// More valid than invalid pages is plaintext (a few damaged pages must not turn a plain partition into "encrypted" and
// have the migration skipped); no valid page but some invalid is encrypted
nvs_image_kind_t nvs_image_tally_kind(nvs_image_tally_t const *t);

typedef enum {
    NVS_ACT_PLAIN_INIT,  // nvs_flash_init(): the behaviour of a build without this feature
    NVS_ACT_SECURE_INIT, // open the encrypted partition with the derived keys
    NVS_ACT_REFUSE,      // key present, partition not classifiable: touch nothing, NVS stays down this boot
    NVS_ACT_MIGRATE,     // plaintext now, key present: copy to RAM, erase, init encrypted, write back
} nvs_action_t;

// The one policy. `enabled` is the Kconfig switch; `key_burned` says an HMAC key already sits in the configured eFuse
// block. With a key, an UNKNOWN partition is neither opened, migrated nor erased (REFUSE); without one it is plain as ever. Nothing here ever asks for a key to be created: no key means plain NVS, whatever the partition holds.
nvs_action_t nvs_image_decide(bool enabled, bool key_burned, nvs_image_kind_t kind);

// 64 bytes, the XTS key pair as IDF's nvs_sec_cfg_t lays it out (eky then tky). False when either half is all zero
// (a failed HMAC leaves the buffer as it was) or the halves are equal (IDF's own collision check).
bool nvs_image_keys_ok(uint8_t const eky[32], uint8_t const tky[32]);

// ---- the RAM copy of a migration ----------------------------------------------------------------------------------

#define NVS_SNAP_NAME_MAX 16 // namespace and key, with the NUL (NVS_NS_NAME_MAX_SIZE, NVS_KEY_NAME_MAX_SIZE)

typedef struct {
    char     ns[NVS_SNAP_NAME_MAX];
    char     key[NVS_SNAP_NAME_MAX];
    uint8_t  type; // an nvs_type_t
    uint64_t num;  // integer types, widened
    void    *data; // strings (with the NUL) and blobs
    size_t   len;
} nvs_snap_entry_t;

typedef struct {
    nvs_snap_entry_t *e;
    size_t            n;
    size_t            cap;
} nvs_snap_t;

// Copies the entry in (data is copied). False when out of memory or a name does not fit; the snapshot is then unusable
// for a migration and the caller must not erase anything.
bool nvs_snap_add(nvs_snap_t *s, char const *ns, char const *key, uint8_t type, uint64_t num, void const *data,
                  size_t len);
// Overwrites every copied byte with zeros, then frees (the copy holds the Wi-Fi password and the repeater passwords)
void nvs_snap_wipe(nvs_snap_t *s);
// Zeroes the data without freeing (wipe = this + free; split for the test)
void nvs_snap_zero(nvs_snap_t *s);
// Same namespace, key, type and value
bool nvs_snap_entry_same(nvs_snap_entry_t const *a, nvs_snap_entry_t const *b);
