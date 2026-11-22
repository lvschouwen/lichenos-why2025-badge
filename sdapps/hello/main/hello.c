// The smallest SD-card app (#517): one OS call through the export table, one return value. "fw elf run" prints the
// value. Built as an ELF (see ../CMakeLists.txt), so main() is the entry (-Dmain=app_main).
#include "lichen_notify.h"


int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    bool dnd = lichen_notify_dnd();
    return 0x4c00 | (dnd ? 1 : 0); // 'L' in the high byte, so a wrong value is easy to tell from a stray 0
}
