#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Signal history per contact (#166): the last samples of RSSI, SNR and hops from the adverts and direct messages of a
// contact, in RAM only (lost at restart). Receive only: nothing here transmits. Pure logic, no ESP headers: built into
// the host tests. The table is not locked; the caller serialises (mesh_task.cpp: a spinlock around note and copy).

#define SIG_CONTACTS 32 // contacts kept; the one heard longest ago makes room for a new one
#define SIG_SAMPLES  24 // samples per contact
#define SIG_PREFIX   6  // bytes of the public key that name a contact (as the rest of the OS)
#define SIG_PATH_MAX 16 // bytes of the last advert's path kept (a longer path is cut: the hop count stays true)

typedef enum { SIG_ADVERT, SIG_MESSAGE } sig_kind_t;

typedef struct {
    uint32_t at_s;     // seconds since boot
    int16_t  rssi_x10; // dBm * 10
    int16_t  snr_x10;  // dB * 10
    uint8_t  hops;     // hashes in the path the packet came by
    uint8_t  kind;     // sig_kind_t
} sig_sample_t;

typedef struct {
    bool         used;
    uint8_t      prefix[SIG_PREFIX];
    uint8_t      count, next;      // samples held, where the next goes
    uint32_t     heard;            // all packets noted since start (more than the ring holds)
    uint32_t     order;            // the table's counter at the last note: the least recent one is replaced
    sig_sample_t s[SIG_SAMPLES];
    bool         have_path;        // an advert's path was noted
    uint8_t      path_hops, path_hash_size, path_bytes; // path_bytes: how many of path[] are kept
    uint32_t     path_at_s;
    uint8_t      path[SIG_PATH_MAX];
} sig_contact_t;

typedef struct {
    sig_contact_t c[SIG_CONTACTS];
    uint32_t      counter;
} sig_table_t;

void sig_init(sig_table_t *t);
// One packet from the contact with this key prefix. path_len is the packet's own (hash size - 1 in bits 6-7, hops in
// bits 0-5); path its bytes. Both only matter for an advert (the path is kept as the last advert path); a message
// passes NULL/0. A full table drops the contact heard longest ago.
void sig_note(sig_table_t *t, uint8_t const prefix[SIG_PREFIX], sig_kind_t kind, int16_t rssi_x10, int16_t snr_x10,
              uint8_t path_len, uint8_t const *path, uint32_t at_s);
// The contact's record, copied; false when nothing was heard from it since start
bool sig_get(sig_table_t const *t, uint8_t const prefix[SIG_PREFIX], sig_contact_t *out);
// The samples oldest first (the graph's left to right); returns how many (up to max)
size_t sig_samples(sig_contact_t const *c, sig_sample_t *out, size_t max);

// The graph's bounds: the lowest and highest RSSI (dBm) or SNR (dB) of the n samples, whole units, widened to at least
// 10 apart. Values are rounded to whole units.
typedef enum { SIG_RSSI, SIG_SNR } sig_field_t;
int16_t sig_value(sig_sample_t const *s, sig_field_t f);
void    sig_range(sig_sample_t const *s, size_t n, sig_field_t f, int *lo, int *hi);

// "Advert" / "Message"
char const *sig_kind_name(uint8_t kind);
// "-87 dBm, 6.5 dB SNR, 2 hops, advert, 3 min ago"; age_text is the caller's
void sig_sample_text(sig_sample_t const *s, char const *age_text, char *out, size_t cap);
// "Heard 5 times since start" / "Heard once since start"
void sig_heard_text(uint32_t heard, char *out, size_t cap);
// "2 hops: a1b2, c3d4" / "direct (no hops)"; a path with more hops than were kept ends with ", ..."
void sig_path_text(sig_contact_t const *c, char *out, size_t cap);
// "direct" / "1 hop" / "N hops"
void sig_hops_text(uint8_t hops, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
