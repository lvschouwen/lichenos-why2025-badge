#pragma once
// mDNS (#156): the badge answers to lichen-xxxx.local while it is on Wi-Fi, and advertises its update server
// (_http._tcp) while that is on. The Wi-Fi/update-server owner calls lichen_mdns_update() with the state it sees.
#include <stdbool.h>

// "lichen-xxxx" (no ".local"), from the MAC; valid from boot
char const *lichen_mdns_name(void);

// Brings mDNS up while `net` (Wi-Fi on with an IP), down otherwise, and the update-server service up while `server`.
// Cheap to call every second; only acts on a change. Call from one task.
void lichen_mdns_update(bool net, bool server, unsigned short port);
