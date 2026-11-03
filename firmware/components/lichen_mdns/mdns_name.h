#pragma once
// The badge's name on the LAN (#156): lichen-xxxx, xxxx = the last two bytes of the MAC in lowercase hex. No owner
// name: the name is broadcast to everyone on the network.
#include <stddef.h>
#include <stdint.h>

#define MDNS_NAME_MAX 16 // "lichen-abcd" plus NUL

// Writes the host name for a MAC; always NUL terminated, empty when cap is too small
void mdns_name_from_mac(uint8_t const mac[6], char *out, size_t cap);
