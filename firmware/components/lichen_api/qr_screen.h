#pragma once
// A QR code on a screen of its own (#153): the code big and square on white, the text it holds below. Anything that wants
// to hand a link or a string to a phone uses this ("Show as QR" on a contact and a channel, the QR code app).

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opens the screen on top of the stack. LVGL context (a row's action, a text entry's done). kind is the small line of
// the heading ("Contact QR code"), name the subject in LARGE; payload is what the code holds (QR_PAYLOAD_MAX bytes at
// most, qr_payload.h). caption is the text under the code; NULL shows the payload there. A caption that may not be
// shown (a password in a Wi-Fi payload) is passed here in its safe form. The strings are copied for the life of the
// screen and wiped when it closes. False when the payload does not fit or the screen is open already.
bool qr_screen_open(char const *kind, char const *name, char const *payload, char const *caption);

#ifdef __cplusplus
}
#endif
