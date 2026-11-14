// ota_hold.h: the in-progress marks of the on-air hold's Dekker pair (#420)
#include "ota_hold.h"

#include "c6_paths.h"
#include "c6link.h"
#include "fw_console.h"
#include "ota_path.h"

#include <string.h>

static bool s_busy;      // __atomic, seq_cst only
static bool s_installed; // __atomic, seq_cst only; never cleared (the next boot runs the new image)

char const *ota_change_begin(void) {
    __atomic_store_n(&s_busy, true, __ATOMIC_SEQ_CST);
    char const *why = console_usb_hold_why(); // seq_cst load of the list's flag (mesh_onair_run.c onair_hold)
    if (why) {
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
    }
    return why;
}

void ota_change_end(bool installed) {
    if (installed) {
        __atomic_store_n(&s_installed, true, __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
}

bool ota_change_busy(void) {
    return __atomic_load_n(&s_busy, __ATOMIC_SEQ_CST);
}

bool ota_change_installed(void) {
    return __atomic_load_n(&s_installed, __ATOMIC_SEQ_CST);
}

bool ota_hold_refused(int fd, ota_hold_t kind, char const *why) {
    if (!why) {
        return false;
    }
    char text[320];
    ota_hold_reply(kind, why, text, sizeof(text) - 1);
    strcat(text, "\n");
    ota_send_reply(fd, 409, text);
    return true;
}

bool ota_file_change_begin(int fd, char const *posix_path) {
    bool c6 = c6_path_in_image_dir(posix_path);
    if (!c6 && !ota_path_onair_held(posix_path)) {
        return true;
    }
    if (ota_hold_refused(fd, c6 ? OTA_HOLD_C6 : OTA_HOLD_FILE, ota_change_begin())) {
        return false;
    }
    if (c6) {
        c6link_bundle_changed();
    }
    return true;
}
