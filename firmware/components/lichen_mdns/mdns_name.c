#include "mdns_name.h"

#include <stdio.h>

void mdns_name_from_mac(uint8_t const mac[6], char *out, size_t cap) {
    if (!cap) {
        return;
    }
    int n = snprintf(out, cap, "lichen-%02x%02x", mac[4], mac[5]);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
    }
}
