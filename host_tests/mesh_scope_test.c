// Host test: the default flood scope's name rules and texts (#163)
#include "mesh_scope.h"

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

static char const *parse(char const *in, char *out) {
    return mesh_scope_parse(in, out, 40);
}

int main(void) {
    char out[40], text[200];
    // None: empty (also spaces, a lone '#') is no scope
    CHECK(parse("", out) == NULL && out[0] == 0);
    CHECK(parse("   ", out) == NULL && out[0] == 0);
    CHECK(parse("#", out) == NULL && out[0] == 0);
    // A name: lowercased, '#' and spaces gone
    CHECK(parse("NL-Limburg", out) == NULL && strcmp(out, "nl-limburg") == 0);
    CHECK(parse("  #eu  ", out) == NULL && strcmp(out, "eu") == 0);
    CHECK(parse("caf\xc3\xa9", out) == NULL && strcmp(out, "caf\xc3\xa9") == 0);
    // Refused: out is left empty
    CHECK(parse("a b", out) != NULL && out[0] == 0);
    CHECK(parse("a,b", out) != NULL && parse("a:b", out) != NULL && parse("a?b", out) != NULL);
    CHECK(parse("$private", out) != NULL && parse("a#b", out) != NULL && parse("a_b", out) != NULL);
    CHECK(parse("a[b", out) != NULL && parse("a\\b", out) != NULL && parse("a*b", out) != NULL);
    // 30 characters fit, 31 do not; a small buffer never overflows
    CHECK(parse("abcdefghijklmnopqrstuvwxyz0123", out) == NULL && strlen(out) == 30);
    CHECK(parse("abcdefghijklmnopqrstuvwxyz01234", out) != NULL);
    CHECK(mesh_scope_parse("abcdef", out, 4) != NULL && out[0] == 0);
    CHECK(mesh_scope_parse("abc", out, 4) == NULL && strcmp(out, "abc") == 0);
    // Texts
    mesh_scope_text("", text, sizeof(text));
    CHECK(strcmp(text, "None (everywhere)") == 0);
    mesh_scope_text(NULL, text, sizeof(text));
    CHECK(strcmp(text, "None (everywhere)") == 0);
    mesh_scope_text("nl", text, sizeof(text));
    CHECK(strcmp(text, "nl") == 0);
    mesh_scope_ask_text("nl", text, sizeof(text));
    CHECK(strcmp(text, "Adverts and messages will only travel in region nl. Contacts outside it can no longer reach you.") == 0);
    mesh_scope_key_text("nl", text, sizeof(text));
    CHECK(strcmp(text, "#nl") == 0);
    // The console scope (#290): a screen's flood keeps the default, a console line's takes the console scope, and an
    // unset console scope refuses it (never "None (everywhere)")
    CHECK(mesh_scope_choose(false, "") == MESH_SCOPE_USE_DEFAULT);
    CHECK(mesh_scope_choose(false, "zz-test") == MESH_SCOPE_USE_DEFAULT);
    CHECK(mesh_scope_choose(true, "zz-test") == MESH_SCOPE_USE_CONSOLE);
    CHECK(mesh_scope_choose(true, "") == MESH_SCOPE_USE_REFUSE);
    CHECK(mesh_scope_choose(true, NULL) == MESH_SCOPE_USE_REFUSE);
    // "fw mesh scope console <region> YES"
    CHECK(mesh_scope_console_arg("ZZ-Test YES", out, sizeof(out)) == NULL && strcmp(out, "zz-test") == 0);
    CHECK(mesh_scope_console_arg("#zz-test  YES ", out, sizeof(out)) == NULL && strcmp(out, "zz-test") == 0);
    CHECK(mesh_scope_console_arg("zz-test", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("zz-test yes", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("zz-testYES", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("YES", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("# YES", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("zz test YES", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("abcdefghijklmnopqrstuvwxyz0123 YES", out, sizeof(out)) == NULL && strlen(out) == 30);
    CHECK(mesh_scope_console_arg("abcdefghijklmnopqrstuvwxyz01234 YES", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("abcdefghijklmnopqrstuvwxyz0123456789 YES", out, sizeof(out)) != NULL);
    CHECK(mesh_scope_console_arg("", out, sizeof(out)) != NULL && out[0] == 0);
    mesh_scope_status_text("", "", text, sizeof(text));
    CHECK(strcmp(text, "default None | console not set") == 0);
    mesh_scope_status_text("zz-wide", "zz-test", text, sizeof(text));
    CHECK(strcmp(text, "default zz-wide | console zz-test") == 0);
    mesh_scope_status_text(NULL, NULL, text, sizeof(text));
    CHECK(strcmp(text, "default None | console not set") == 0);
    if (failures == 0) {
        printf("mesh_scope_test: ok\n");
    }
    return failures ? 1 : 0;
}
