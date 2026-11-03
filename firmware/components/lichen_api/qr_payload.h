#pragma once
// Text that goes into a QR code (#153). Pure, host-tested in host_tests/qr_payload_test.c; qr_screen.c shows it.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// What one QR code may hold. The screen draws it about 360 px wide: past ~330 bytes a module is under 4 px and a phone
// camera at arm's length stops reading it.
#define QR_PAYLOAD_MAX 330

// "WIFI:T:WPA;S:<ssid>;P:<password>;;" (the format phones' camera apps join from); no password gives "T:nopass".
// \ ; , : " in the values are escaped. False (out holds "") for an empty SSID or when it does not fit in cap.
bool qr_wifi_payload(char const *ssid, char const *password, char *out, size_t cap);

// Whether the text fits one QR code: not empty, at most QR_PAYLOAD_MAX bytes
bool qr_payload_ok(char const *text);

#ifdef __cplusplus
}
#endif
