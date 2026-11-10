#include "mesh_cscope.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "fw_console.h"
#include "mesh.h"
#include "mesh_rhist_run.h"
#include "mesh_scope.h"

#include <stdio.h>
#include <string.h>

// Internal RAM, behind a mutex (not a spinlock): the name may come from PSRAM (BadgeMesh), so the copy is never made
// inside a critical section
static char cscope_name[MESH_SCOPE_MAX + 1];

static StaticSemaphore_t cscope_buf;
static SemaphoreHandle_t cscope_lock; // made by the first note (MeshCore's start); NULL until then: "not set"

void mesh_cscope_note(char const *name) {
    SemaphoreHandle_t lock = __atomic_load_n(&cscope_lock, __ATOMIC_ACQUIRE);
    if (!lock) {
        lock = xSemaphoreCreateMutexStatic(&cscope_buf); // only MeshCore's start notes first, once
        __atomic_store_n(&cscope_lock, lock, __ATOMIC_RELEASE);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    snprintf(cscope_name, sizeof(cscope_name), "%s", name ? name : "");
    xSemaphoreGive(lock);
}

static void cscope_get(char *out, size_t cap) {
    SemaphoreHandle_t lock = __atomic_load_n(&cscope_lock, __ATOMIC_ACQUIRE);
    out[0]                 = '\0';
    if (lock) {
        xSemaphoreTake(lock, portMAX_DELAY);
        snprintf(out, cap, "%s", cscope_name);
        xSemaphoreGive(lock);
    }
}

char const *mesh_cscope_refusal(bool from_console) {
    if (!from_console) {
        return NULL; // a screen's send never reads it
    }
    char name[MESH_SCOPE_MAX + 1];
    cscope_get(name, sizeof(name));
    return mesh_scope_choose(true, name) == MESH_SCOPE_USE_REFUSE ? MESH_SCOPE_CONSOLE_UNSET : NULL;
}

void mesh_cscope_console(char const *args) {
    char name[MESH_SCOPE_MAX + 1];
    if (!*args) {
        mesh_settings_t s;
        char            line[96];
        mesh_get_settings(&s); // zeroed (None) before MeshCore is up; the console only gets here while it runs
        cscope_get(name, sizeof(name));
        mesh_scope_status_text(s.scope, name, line, sizeof(line));
        console_reply("%s", line);
        return;
    }
    if (strncmp(args, "console ", 8) != 0) {
        console_reply("mesh scope | mesh scope console <region> YES");
        return;
    }
    char        before[MESH_SCOPE_MAX + 1];
    char const *why = mesh_scope_console_arg(args + 8, name, sizeof(name));
    cscope_get(before, sizeof(before));
    why = why ? why : mesh_set_console_scope(name);
    if (why) {
        console_reply("console scope not changed: %s", why);
        return;
    }
    console_reply("console scope %s: every flood from the console travels only in that region", name);
    // The status history polls a console line switched on were meant for the old region: they go off (#290)
    size_t off = strcmp(before, name) != 0 ? mesh_rhist_off_console() : 0;
    if (off) {
        console_reply("%u status history poll(s) from the console switched off (the scope changed): fw mesh history "
                      "<who> on <min> YES to start them again", (unsigned)off);
    }
}
