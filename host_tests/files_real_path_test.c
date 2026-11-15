// Host test (#443): files_ops.c built as on the badge (no FILES_HOST_TEST). real_path() must fill the caller's buffer:
// the copy keeps the buffer of its part file, not the returned pointer, and opened an unfilled one on the badge.
#include "../firmware/components/lichen_shell/files_ops.c"

#include <assert.h>

int main(void) {
    char buf[FILES_PATH_MAX + 64];
    memset(buf, 'x', sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    char const *got = real_path("/sd/wp-test.bin", buf, sizeof(buf));
    assert(strcmp(buf, "/sd/wp-test.bin") == 0);
    assert(got == buf);
    real_path_with("/sd/wp-test.bin", FILES_PART_SUFFIX, buf, sizeof(buf));
    assert(strcmp(buf, "/sd/wp-test.bin.lichen-part") == 0);
    puts("files_real_path_test: ok");
    return 0;
}
