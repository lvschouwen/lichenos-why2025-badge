#pragma once

#include <stdbool.h>
#include <stddef.h>

// The index of the first row focus can land on (#361), or n when none. The Wi-Fi page's rows can start with a note
// that is not in the focus group, so rows[0] is not always the one to focus.
size_t wifi_first_focusable(bool const *focusable, size_t n);
