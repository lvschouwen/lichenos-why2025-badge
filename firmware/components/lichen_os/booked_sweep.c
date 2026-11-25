#include "booked_sweep.h"

#include <stdio.h>

bool booked_sweep_line(char *buf, size_t cap, char const *id, uint32_t blocks, uint32_t bytes, bool unsafe) {
    if (blocks == 0 && !unsafe) {
        return false;
    }
    snprintf(buf, cap, "%s: freed %u booked blocks (%u B)%s", id, (unsigned)blocks, (unsigned)bytes,
             unsafe ? ", not freed: untracked frees" : "");
    return true;
}
