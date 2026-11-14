#pragma once
// The update server's side of the on-air list's USB hold (#420). A change the hold covers (the boot slot switch, a C6
// image file) and "fw onair on" form a Dekker pair, so the two can never both go ahead:
//   server: store "change in progress" (seq_cst), then load the hold (onair_on, seq_cst): held -> clear, refuse
//   list:   store onair_on (seq_cst), then load what is pending (these flags, seq_cst): pending -> store off, refuse
// Each side stores its own flag before it loads the other's, all seq_cst (an acquire/release pair would let a load
// pass the earlier store on the dual-core P4). No lock spans both sides. host_tests/ota_valid_test.c models it.

#include "ota_proto.h" // ota_hold_t

#include <stdbool.h>

// Marks a covered change in progress, then asks the hold: NULL to go ahead (call ota_change_end after), else what
// holds (console_usb_hold_why), with the mark already cleared. OTA task only, one change at a time.
char const *ota_change_begin(void);
// The change is over; installed: it switched the boot slot (stored before the mark is cleared, so pending never drops)
void        ota_change_end(bool installed);
// A covered change is in progress / a firmware update switched the boot slot this boot. Any task (seq_cst loads).
bool        ota_change_busy(void);
bool        ota_change_installed(void);


// The 409 for a held change when why (console_usb_hold_why or ota_change_begin) is not NULL: true then, the reply sent
bool ota_hold_refused(int fd, ota_hold_t kind, char const *why);
// Before a file in the C6 image directory is written or deleted (posix_path: its /storage path; other paths pass):
// begins the change, refused (409 sent, false) while the list holds, else forgets the verified bundle (#36: so even an
// interrupted write leaves the next boot with a full check; every writer of the C6 image directory must come through
// here). ota_change_end(false) follows whatever happened next.
bool ota_c6_change_begin(int fd, char const *posix_path);
// ota_server.c's reply writer: the head and body (ends in a newline), sent as is
void ota_send_reply(int fd, int status, char const *body);
