#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The C6 image files on the storage drive (#61). Pure logic, no ESP headers: also built into the host tests.
#define C6_IMAGE_DIR "/storage/c6"

// out = C6_IMAGE_DIR "/" file. False for a name with '/' or "..", an empty name, or a result that doesn't fit.
bool c6_image_path(char const *file, char *out, size_t cap);

// Whether a /storage path names C6_IMAGE_DIR or anything under it (#420: the on-air list's USB hold refuses those writes
// and deletes, since the C6 drives the LoRa radio and is flashed from there at boot). Compared segment by segment,
// ignoring case and doubled slashes; a "." or ".." segment anywhere counts as inside (fails closed).
bool c6_path_in_image_dir(char const *path);

// Whether the hex text of an image's .md5 file (32 hex digits, either case) is digest. The flasher checks this before
// trusting the .md5 for anything: a .bin and .md5 pushed separately over Wi-Fi can disagree.
bool c6_md5_matches(char const *hex, uint8_t const digest[16]);
