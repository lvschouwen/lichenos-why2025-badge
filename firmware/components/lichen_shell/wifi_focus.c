#include "wifi_focus.h"

size_t wifi_first_focusable(bool const *focusable, size_t n) {
    size_t i = 0;
    while (i < n && !focusable[i]) {
        i++;
    }
    return i;
}
