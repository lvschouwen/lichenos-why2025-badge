// @NAME_C@ (scaffolded by `tools/badge.sh new-app --sd`). An SD-card app is an ELF the OS loads from the card: main() is
// the entry, and every OS call goes through the export table, so include only lichen_api's public headers
// (tools/check_app_boundary.sh). Build with ./firmware/build.sh build, install with
// tools/badge.sh sdapp-push <host> @ID@.
#include "lichen_notify.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return lichen_notify_dnd() ? 1 : 0;
}
