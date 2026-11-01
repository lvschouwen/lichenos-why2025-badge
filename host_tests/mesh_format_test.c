// Host test: age and contact type texts (#61)
#include "mesh_format.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static char const *age(uint32_t s) {
    static char out[24];
    mesh_format_age(s, out, sizeof(out));
    return out;
}

int main(void) {
    CHECK(strcmp(age(0), "just now") == 0);
    CHECK(strcmp(age(4), "just now") == 0);
    CHECK(strcmp(age(42), "42 s ago") == 0);
    CHECK(strcmp(age(60), "1 min ago") == 0);
    CHECK(strcmp(age(3599), "59 min ago") == 0);
    CHECK(strcmp(age(7200), "2 h ago") == 0);
    CHECK(strcmp(age(3 * 86400 + 5), "3 d ago") == 0);

    // A message's age (#48): live by uptime, loaded from the journal by the wall clock or "before restart"
    char a[24];
    mesh_format_msg_age(false, 100, 5000, 160, 9000, a, sizeof(a));
    CHECK(strcmp(a, "1 min ago") == 0);
    mesh_format_msg_age(false, 100, 0, 50, 0, a, sizeof(a)); // clock before the message
    CHECK(strcmp(a, "just now") == 0);
    mesh_format_msg_age(true, 0, 5000, 160, 5000 + 3 * 86400, a, sizeof(a));
    CHECK(strcmp(a, "3 d ago") == 0);
    mesh_format_msg_age(true, 0, 5000, 160, 0, a, sizeof(a)); // clock not set now
    CHECK(strcmp(a, "before restart") == 0);
    mesh_format_msg_age(true, 0, 0, 160, 9000, a, sizeof(a)); // clock was not set then
    CHECK(strcmp(a, "before restart") == 0);
    mesh_format_msg_age(true, 0, 9000, 160, 5000, a, sizeof(a)); // clock went back
    CHECK(strcmp(a, "just now") == 0);

    CHECK(strcmp(mesh_type_name(1), "chat") == 0);
    CHECK(strcmp(mesh_type_name(2), "repeater") == 0);
    CHECK(strcmp(mesh_type_name(3), "room") == 0);
    CHECK(strcmp(mesh_type_name(4), "sensor") == 0);
    CHECK(strcmp(mesh_type_name(0), "?") == 0 && strcmp(mesh_type_name(99), "?") == 0);

    char name[32];
    CHECK(mesh_name_clean("  Lucas badge  ", name, sizeof(name)) && strcmp(name, "Lucas badge") == 0);
    CHECK(!mesh_name_clean("   ", name, sizeof(name)) && name[0] == '\0');
    CHECK(!mesh_name_clean("", name, sizeof(name)));
    CHECK(mesh_name_clean("a\tb\x01" "c", name, sizeof(name)) && strcmp(name, "abc") == 0);
    CHECK(mesh_name_clean("0123456789012345678901234567890123456789", name, sizeof(name)) && strlen(name) == 31);
    // 30 ASCII bytes then a 2-byte character: it doesn't fit in 31, so it goes whole
    CHECK(mesh_name_clean("012345678901234567890123456789\xc3\xa9", name, sizeof(name)) && strlen(name) == 30);
    // A cut that leaves a space at the end: trimmed again
    CHECK(mesh_name_clean("012345678901234567890123456789 xyz", name, sizeof(name)) && strlen(name) == 30);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_format_test: all checks passed\n");
    return 0;
}
