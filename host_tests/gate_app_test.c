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
#define LICHEN_ID_CTX 0x00e0e004u // an app that registered with the OS's notification source as its id
#define TEST_ID_CTX   0x00f0f005u
#define C6_ID_CTX     0x0010f006u
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
    } else if (ctx == LICHEN_ID_CTX || ctx == TEST_ID_CTX || ctx == C6_ID_CTX) {
        *id   = ctx == LICHEN_ID_CTX ? "lichen" : ctx == TEST_ID_CTX ? "test" : "c6";
        *name = "Impostor";
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
static unsigned os_refused;
static void fake_refuse(uint32_t ctx, char const *what) {
    (void)what;
    (void)ctx;
    os_refused++;
}
static gate_app_env_t const g = {.resolve = fake_resolve, .os = fake_os, .refuse = fake_refuse};

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
    CHECK(gate_app_source(&g, NOID_CTX, "n") == NULL); // no id to post under
    CHECK(gate_app_source(&g, STALE_CTX, "n") == NULL); // an app cannot post as another by guessing its ctx
}

static void test_os_ctx_refused(void) {
    char const *id = NULL, *name = NULL;
    os_refused = 0;
    CHECK(gate_app_source(&g, OS_CTX, "n") == NULL && os_refused == 1); // an app-facing call refuses the OS's ctx
    CHECK(gate_app_caller(&g, OS_CTX, "x", &id, &name) == GATE_APP_REFUSED && os_refused == 2);
    CHECK(gate_app_caller(&g, MESH_CTX, "x", &id, &name) == GATE_APP_ID && os_refused == 2); // an app is not counted
    CHECK(gate_app_id(&g, OS_CTX, "x", &id, &name) == GATE_APP_OS && os_refused == 2); // the OS-side lookup still says OS
}

static void test_reserved_ids(void) {
    char const *id = NULL, *name = NULL;
    char        ns[16];
    CHECK(gate_app_id(&g, LICHEN_ID_CTX, "t", &id, &name) == GATE_APP_NO_ID && id == NULL); // not the OS's source
    CHECK(gate_app_source(&g, LICHEN_ID_CTX, "n") == NULL);
    CHECK(gate_app_id(&g, TEST_ID_CTX, "t", &id, &name) == GATE_APP_NO_ID);
    CHECK(gate_app_id(&g, C6_ID_CTX, "t", &id, &name) == GATE_APP_NO_ID); // the C6 image's drive folder
    CHECK(gate_app_settings_ns(&g, LICHEN_ID_CTX, "s", ns, sizeof(ns), &name) == GATE_APP_NO_ID);
}

static void test_settings_ns(void) {
    char        ns[16] = "";
    char const *name   = NULL;
    CHECK(gate_app_settings_ns(&g, MESH_CTX, "s", ns, sizeof(ns), &name) == GATE_APP_ID && strcmp(ns, "a.meshcore") == 0);
    strcpy(ns, "keep");
    CHECK(gate_app_settings_ns(&g, OS_CTX, "s", ns, sizeof(ns), &name) == GATE_APP_OS); // the OS has no app namespace
    CHECK(gate_app_settings_ns(&g, NOID_CTX, "s", ns, sizeof(ns), &name) == GATE_APP_NO_ID && strcmp(name, "Clock") == 0);
    CHECK(gate_app_settings_ns(&g, BADID_CTX, "s", ns, sizeof(ns), &name) == GATE_APP_NO_ID);
    name = NULL;
    CHECK(gate_app_settings_ns(&g, STALE_CTX, "s", ns, sizeof(ns), &name) == GATE_APP_REFUSED && name == NULL);
    CHECK(strcmp(ns, "keep") == 0);
}

static void test_batch(void) {
    CHECK(gate_batch_may_end(MESH_CTX, MESH_CTX));
    CHECK(!gate_batch_may_end(MESH_CTX, NOID_CTX)); // another ctx cannot end somebody's batch
    CHECK(!gate_batch_may_end(0, MESH_CTX));        // nobody holds one
    CHECK(!gate_batch_may_end(0, 0));
}

int main(void) {
    test_kind();
    test_source();
    test_os_ctx_refused();
    test_reserved_ids();
    test_settings_ns();
    test_batch();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("gate_app_test: ok\n");
    return 0;
}
