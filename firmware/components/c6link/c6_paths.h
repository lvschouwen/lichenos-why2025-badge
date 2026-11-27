#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The C6 image files on the storage drive (#61). Pure logic, no ESP headers: also built into the host tests.
#define C6_IMAGE_DIR "/storage/c6"

// The card update folder's copy of the bundle (#595): the flasher's source this boot when c6link chose it
#define C6_CARD_DIR "/sd/lichen/update/c6"

// out = dir "/" file. False for a name with '/' or "..", an empty name, or a result that doesn't fit.
bool c6_image_path_in(char const *dir, char const *file, char *out, size_t cap);
// The same in the directory the flasher reads this boot (c6_image_dir)
bool c6_image_path(char const *file, char *out, size_t cap);
// The bundle's directory for the flasher and the boot IDENT: C6_IMAGE_DIR unless c6link chose the card (#595).
// c6_image_dir_use(NULL) puts C6_IMAGE_DIR back. Boot task only, before the c6sup task reads the bundle.
void        c6_image_dir_use(char const *dir);
char const *c6_image_dir(void);

// Whether a /storage path names C6_IMAGE_DIR or anything under it (#420: the on-air list's USB hold refuses those writes
// and deletes, since the C6 drives the LoRa radio and is flashed from there at boot). Compared segment by segment,
// ignoring case and doubled slashes; a "." or ".." segment anywhere counts as inside (fails closed).
bool c6_path_in_image_dir(char const *path);

// Whether the hex text of an image's .md5 file (32 hex digits, either case) is digest. The flasher checks this before
// trusting the .md5 for anything: a .bin and .md5 pushed separately over Wi-Fi can disagree.
bool c6_md5_matches(char const *hex, uint8_t const digest[16]);
