// The smallest SD-card app (#517): one OS call through the export table, one return value. "fw elf run" prints the
// value. Built as an ELF (see ../CMakeLists.txt), so main() is the entry (-Dmain=app_main).
#include <stdbool.h>

// What lichen_api/lichen_notify.h declares (the real header pulls in LVGL, which an app does not link): the OS resolves
// the name when the app is loaded, against the table in lichen_os/sd_apps.c.
bool lichen_notify_dnd(void);
int  esp_rom_printf(char const *fmt, ...);

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    bool dnd = lichen_notify_dnd();
    esp_rom_printf("hello from an SD app: do-not-disturb is %s\n", dnd ? "on" : "off");
    return 0x4c00 | (dnd ? 1 : 0); // 'L' in the high byte, so a wrong value is easy to tell from a stray 0
}
