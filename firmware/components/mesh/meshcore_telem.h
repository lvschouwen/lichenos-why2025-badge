#pragma once

#include <stdint.h>

// Request telemetry from a contact (#162), opened from the contact's details (meshcore_contact.c). LVGL task only.
// This transmits, so it asks first: a careful dialog, then the result page sends one request and shows the values, or
// "No answer: ..." with an Again row (which asks again, rate-limited to one request per contact per 30 s).
// Returns the seconds the contact must still be left alone (nothing is asked then), 0 when the dialog was shown.
uint32_t meshcore_telem_ask(uint8_t const prefix[6], char const *title);
