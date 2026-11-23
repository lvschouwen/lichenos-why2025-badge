// The smallest SD-card app (#517, #523): main() hands the OS its hooks and returns. The OS loads this ELF when the tile is
// opened, runs main() once on a task of the app's own, calls open on the LVGL task and, when the last screen is closed,
// stop; then the ELF is unloaded. Built as an ELF (see ../CMakeLists.txt), so main() is the entry (-Dmain=app_main).
#include "lichen_notify.h"
#include "lichen_sd_app.h"

static void hello_open(lv_obj_t *screen) {
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, lichen_notify_dnd() ? "Hello from the SD card (quiet)" : "Hello from the SD card");
    lv_obj_center(label);
}

static bool hello_stop(void) {
    return true; // nothing runs but the screen
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    lichen_sd_hooks_t const hooks = {.size = sizeof(hooks), .open = hello_open, .stop = hello_stop};
    return lichen_sd_app_hooks(&hooks) ? 0 : 1;
}
