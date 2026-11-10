#include "mesh_cscope.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "fw_console.h"
#include "mesh.h"
#include "mesh_wipe.h"
#include "mesh_rhist_run.h"
#include "mesh_scope.h"

#include <stdio.h>
#include <string.h>

// Internal RAM, behind a mutex (not a spinlock): the name may come from PSRAM (BadgeMesh), so the copy is never made
// inside a critical section
static char cscope_name[MESH_SCOPE_MAX + 1];

// The channels whose scope is damaged (#291), as name hashes (mesh_chscope_name_hash: a collision only refuses a
// healthy channel), and whether the file is unreadable: same lock, filled by mesh_chscope_noted on the mesh task
static uint32_t chscope_damaged[MESH_CHSCOPE_MAX];
static size_t   chscope_damaged_n;
static bool     chscope_unreadable;

static StaticSemaphore_t cscope_buf;
static SemaphoreHandle_t cscope_lock; // made by the first note (MeshCore's start); NULL until then: "not set"

// Only MeshCore's start notes first (console scope, then channel scopes), once, on the mesh task's start
static SemaphoreHandle_t cscope_lock_made(void) {
    SemaphoreHandle_t lock = __atomic_load_n(&cscope_lock, __ATOMIC_ACQUIRE);
    if (!lock) {
        lock = xSemaphoreCreateMutexStatic(&cscope_buf);
        __atomic_store_n(&cscope_lock, lock, __ATOMIC_RELEASE);
    }
    return lock;
}

void mesh_chscope_noted(uint32_t const *damaged, size_t n, bool unreadable) {
    SemaphoreHandle_t lock = cscope_lock_made();
    xSemaphoreTake(lock, portMAX_DELAY);
    chscope_damaged_n  = n < MESH_CHSCOPE_MAX ? n : MESH_CHSCOPE_MAX;
    chscope_unreadable = unreadable;
    memcpy(chscope_damaged, damaged, chscope_damaged_n * sizeof(chscope_damaged[0]));
    xSemaphoreGive(lock);
    if (n || unreadable) {
        ESP_LOGW("mesh", "channel scopes: %s", unreadable ? MESH_CHSCOPE_UNREADABLE : MESH_CHSCOPE_REFUSED);
    }
}

// NULL, MESH_CHSCOPE_REFUSED or MESH_CHSCOPE_UNREADABLE (for every channel)
static char const *chscope_why(char const *channel) {
    SemaphoreHandle_t lock = __atomic_load_n(&cscope_lock, __ATOMIC_ACQUIRE);
    char const       *why  = NULL;
    uint32_t          h    = channel ? mesh_chscope_name_hash(channel) : 0;
    if (lock) {
        xSemaphoreTake(lock, portMAX_DELAY);
        why = chscope_unreadable ? MESH_CHSCOPE_UNREADABLE : NULL;
        for (size_t i = 0; !why && channel && i < chscope_damaged_n; i++) {
            why = chscope_damaged[i] == h ? MESH_CHSCOPE_REFUSED : NULL;
        }
        xSemaphoreGive(lock);
    }
    return why;
}

char const *mesh_cscope_channel_refusal(bool from_console, char const *channel) {
    return from_console ? NULL : chscope_why(channel); // a console line keeps the console scope (#290 wins)
}

void mesh_cscope_note(char const *name) {
    SemaphoreHandle_t lock = cscope_lock_made();
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

// "fw mesh scope" also lists the channels with a scope of their own (#291), one line each, slot by slot (the lock per
// slot, as "fw channels"). The info holds the key: wiped before the next slot and at the end.
static void cscope_list_channels(void) {
    mesh_channel_info_t info = {0};
    size_t              own  = 0;
    if (chscope_why(NULL)) {
        console_reply("channel scopes: %s; every screen channel send is refused until then", MESH_CHSCOPE_UNREADABLE);
        return;
    }
    for (int i = 0; i < MESH_CHSCOPE_MAX; i++) {
        size_t found = mesh_channel_info(i, NULL, &info, MESH_SET_WAIT_MS);
        mesh_wipe(info.secret, sizeof(info.secret));
        if (found == MESH_BUSY) {
            console_reply("channel scopes: MeshCore is busy, try again");
            return;
        }
        if (found == 1 && info.scope[0]) {
            bool damaged = strcmp(info.scope, MESH_CHSCOPE_DAMAGED) == 0;
            console_reply("channel %s: %s%s", info.name, damaged ? "" : "scope ",
                          damaged ? MESH_CHSCOPE_REFUSED : info.scope);
            own++;
        }
    }
    if (!own) {
        console_reply("channel scopes: none (every channel floods under the default)");
    }
}

char const *mesh_cscope_refusal(bool from_console) {
    if (!from_console) {
        return NULL; // a screen's send never reads it
    }
    char name[MESH_SCOPE_MAX + 1];
    cscope_get(name, sizeof(name));
    return mesh_scope_choose(true, name, NULL) == MESH_SCOPE_USE_REFUSE ? MESH_SCOPE_CONSOLE_UNSET : NULL;
}

void mesh_cscope_console(char const *args) {
    char name[MESH_SCOPE_MAX + 1];
    if (!*args) {
        mesh_settings_t s;
        char            line[96];
        mesh_get_settings(&s); // zeroed (None) before MeshCore is up; the console only gets here while it runs
        cscope_get(name, sizeof(name));
        mesh_scope_status_text(s.scope, name, line, sizeof(line));
        console_reply("%s", line); // the one status line badge.sh reads; the channel lines never match its pattern
        cscope_list_channels();
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
