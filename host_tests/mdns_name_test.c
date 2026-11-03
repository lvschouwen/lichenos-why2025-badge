#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mdns_name.h"

int main(void) {
    char          out[MDNS_NAME_MAX];
    uint8_t const mac[6] = {0x30, 0xed, 0xa0, 0x1b, 0x0a, 0xf5};
    mdns_name_from_mac(mac, out, sizeof(out));
    assert(strcmp(out, "lichen-0af5") == 0); // last two bytes, lowercase, zero padded

    uint8_t const zero[6] = {0};
    mdns_name_from_mac(zero, out, sizeof(out));
    assert(strcmp(out, "lichen-0000") == 0);

    char small[11]; // "lichen-0af5" needs 12
    memset(small, 'x', sizeof(small));
    mdns_name_from_mac(mac, small, sizeof(small));
    assert(small[0] == '\0'); // never a cut-off name

    mdns_name_from_mac(mac, out, 0); // nothing written, no crash
    puts("mdns_name_test: ok");
    return 0;
}
