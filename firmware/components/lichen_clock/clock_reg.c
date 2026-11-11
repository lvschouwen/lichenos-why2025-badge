// The registry's tz key (#319): the zone the C library applies now, read back from the environment the settings
// service sets (settings_apply_tz / settings_set_tz do setenv("TZ") + tzset, so TZ is what localtime uses).
#include "clock_local.h"
#include "reg.h"

#include <stdlib.h>

// Whether tzset accepted the string is not detectable (it never fails; an unparsable TZ silently means UTC), so this
// reports the environment value, as clock_tz_label shortens it for a 15 character registry string.
reg_status_t clock_reg_tz(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    clock_tz_label(getenv("TZ"), out->s, sizeof(out->s));
    return REG_OK;
}
