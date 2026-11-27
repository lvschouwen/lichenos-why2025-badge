#pragma once
// The C6 bundle on the SD card (#595), c6link's own glue around c6_boot_policy.h's card rules: which directory the
// flasher reads this boot, and what happens after IDENT. Boot task only (c6link_start), in this order.

#include <stdbool.h>
#include <stddef.h>

// md5 text and size of the six files of the bundle in dir, as c6_bundle_key; files (may be NULL) gets how many of the
// six are there. False (out "") when any is missing or a .md5 is malformed.
bool c6_bundle_key_in(char const *dir, char *out, size_t cap, unsigned *files);

// Chooses the source: true when the flasher takes the card bundle (c6_image_dir is C6_CARD_DIR then, and the card's
// batch lock is held until c6_card_end_reading). False: /storage/c6, nothing held. Notices for a bundle ignored.
bool c6_card_begin(void);
// The flasher and the IDENT expectation are done with the card: the lock goes, c6_image_dir is C6_IMAGE_DIR again
void c6_card_end_reading(void);
// After the attach and IDENT: copy a proven card bundle into /storage/c6, or record one that failed (c6_card_after)
// on_c6: the C6's flash holds the source bundle (the flasher wrote it, or its check passed)
void c6_card_finish(bool on_c6, bool ident_ok);
