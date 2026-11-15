// See crash_twdt.h (#479)
#include "crash_twdt.h"

#if __has_include("sdkconfig.h")
#include "sdkconfig.h" // the host test defines the symbol itself
#endif

#ifdef CONFIG_LICHEN_DEV_COMMANDS

#include <stdatomic.h>

static atomic_flag s_running = ATOMIC_FLAG_INIT;

bool crash_twdt_parse(char const *rest, int *core) {
    if (rest[0] == '\0') {
        *core = 0;
        return true;
    }
    if (rest[0] != ' ' || rest[1] < '0' || rest[1] >= '0' + CRASH_TWDT_CORES || rest[2] != '\0') {
        return false;
    }
    *core = rest[1] - '0';
    return true;
}

bool crash_twdt_claim(void) {
    return !atomic_flag_test_and_set(&s_running);
}

void crash_twdt_release(void) {
    atomic_flag_clear(&s_running);
}

#endif
