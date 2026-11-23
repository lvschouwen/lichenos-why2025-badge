// @NAME_C@ (scaffolded by `tools/badge.sh new-app --sd`). An SD-card app is an ELF the OS loads from the card when its tile
// is opened: main() runs once on a task of its own and hands the OS its hooks (lichen_sd_app_hooks); the OS calls open on
// the LVGL task and stop when the last screen is closed, then unloads the ELF. Every OS call goes through the export table,
// so include only lichen_api's public headers (tools/check_app_boundary.sh). Caps in the manifest are bare names, e.g.
// caps=NOTIFY,SD. Build with ./firmware/build.sh build, install with tools/badge.sh sdapp-push <host> @ID@.
#include "lichen_notify.h"
#include "lichen_sd_app.h"

static void app_open(lv_obj_t *screen) {
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "@NAME_C@");
    lv_obj_center(label);
}

// The last screen closed: end whatever main() or a task of the app still runs, then return true
static bool app_stop(void) {
    return true;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    lichen_sd_hooks_t const hooks = {.size = sizeof(hooks), .open = app_open, .stop = app_stop};
    return lichen_sd_app_hooks(&hooks) ? 0 : 1;
}
