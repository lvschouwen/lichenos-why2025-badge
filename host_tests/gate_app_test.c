// Host test: the gate's context check for settings, files, badges and notices (#230 E1c, gate_app.c): a refused ctx
// resolves to nothing (and is the only one the table logs), the OS and an app without an id are told apart, the
// notice source is the ctx's app and never the caller's string, the settings namespace follows the ctx.
#include "gate_app.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                                                                      \
    do {                                                                                                              \
        if (!(c)) {                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                       \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

#define OS_CTX    0x00a0a000u
#define MESH_CTX  0x00b0b001u
#define NOID_CTX  0x00c0c002u
#define BADID_CTX 0x00d0d003u // an id NVS cannot take as a namespace
#define STALE_CTX (MESH_CTX ^ 0x100u)

static unsigned refused;

static bool fake_resolve(uint32_t ctx, char const **id, char const **name, char const *what) {
    (void)what;
    if (ctx == OS_CTX) {
        *id = *name = NULL;
    } else if (ctx == MESH_CTX) {
        *id   = "meshcore";
        *name = "MeshCore";
    } else if (ctx == NOID_CTX) {
        *id   = NULL;
        *name = "Clock";
    } else if (ctx == BADID_CTX) {
        *id   = "Not Valid!";
        *name = "Odd";
    } else {
        refused++; // 0, stale, made up
        return false;
    }
    return true;
}
static uint32_t fake_os(void) {
    return OS_CTX;
}
static gate_app_env_t const g = {.resolve = fake_resolve, .os = fake_os};

static void test_kind(void) {
    char const *id = "untouched", *name = "untouched";
    CHECK(gate_app_id(&g, MESH_CTX, "t", &id, &name) == GATE_APP_ID && strcmp(id, "meshcore") == 0);
    id = name = "untouched";
    CHECK(gate_app_id(&g, OS_CTX, "t", &id, &name) == GATE_APP_OS && strcmp(id, "untouched") == 0);
    CHECK(gate_app_id(&g, NOID_CTX, "t", &id, &name) == GATE_APP_NO_ID && strcmp(name, "Clock") == 0);
    id = name = "untouched";
    refused   = 0;
    CHECK(gate_app_id(&g, STALE_CTX, "t", &id, &name) == GATE_APP_REFUSED && refused == 1);
    CHECK(gate_app_id(&g, 0, "t", &id, &name) == GATE_APP_REFUSED && refused == 2);
    CHECK(strcmp(id, "untouched") == 0 && strcmp(name, "untouched") == 0); // a refused ctx hands out nothing
}

static void test_source(void) {
    CHECK(strcmp(gate_app_source(&g, MESH_CTX, "n"), "meshcore") == 0);
    CHECK(strcmp(gate_app_source(&g, OS_CTX, "n"), "lichen") == 0);
    CHECK(gate_app_source(&g, NOID_CTX, "n") == NULL); // no id to post under: dropped, not posted as the OS
    CHECK(gate_app_source(&g, STALE_CTX, "n") == NULL); // an app cannot post as another by guessing its ctx
}

static void test_settings_ns(void) {
    char ns[16] = "";
    CHECK(gate_app_settings_ns(&g, MESH_CTX, "s", ns, sizeof(ns)) && strcmp(ns, "a.meshcore") == 0);
    strcpy(ns, "keep");
    CHECK(!gate_app_settings_ns(&g, OS_CTX, "s", ns, sizeof(ns))); // the OS has no app namespace
    CHECK(!gate_app_settings_ns(&g, NOID_CTX, "s", ns, sizeof(ns)));
    CHECK(!gate_app_settings_ns(&g, BADID_CTX, "s", ns, sizeof(ns)));
    CHECK(!gate_app_settings_ns(&g, STALE_CTX, "s", ns, sizeof(ns)));
    CHECK(strcmp(ns, "keep") == 0);
}

int main(void) {
    test_kind();
    test_source();
    test_settings_ns();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("gate_app_test: ok\n");
    return 0;
}
