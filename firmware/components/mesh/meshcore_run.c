// MeshCore running or not (#103): the state machine of mesh_run.c under a spinlock, the starts and stops on a job task.
// The mesh task itself (mesh_task.cpp) serialises mesh_start and mesh_stop as well, so the update path and a job can
// never overlap.
#include "meshcore_run.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "key_cause.h"
#include "lichen_fs.h"
#include "lichen_settings.h"
#include "lichen_task.h"
#include "lora_app.h"
#include "mesh.h"
#include "mesh_format.h"
#include "mesh_ctab_run.h"
#include "mesh_pw_sweep.h"
#include "mesh_remote.h"

#include <stdint.h>
#include <string.h>

static char const TAG[] = "meshcore";

#define JOB_STACK    8192 // a first start loads everything, as app_main's 8 KB did at boot
#define LEFTOVER_MS  500   // after a stop: the mesh task leaves the OS's book a moment after it said it stopped
#define LATE_STOP_MS 60000 // a stop that outlasted mesh_stop's 10 s: the state stays STOPPING this much longer
#define AUTOSTART    "autostart"

static lichen_ctx_t        app_ctx; // lichen_register_app's answer: every gate call takes it (#230 E1b)
static mesh_run_t          run = MESH_RUN_INIT;
static portMUX_TYPE        run_mux = portMUX_INITIALIZER_UNLOCKED;

static bool apply(mesh_run_ev_t ev) {
    portENTER_CRITICAL(&run_mux);
    bool changed = mesh_run_event(&run, ev);
    portEXIT_CRITICAL(&run_mux);
    return changed;
}

void meshcore_run_init(lichen_ctx_t ctx) {
    app_ctx = ctx;
}

lichen_ctx_t meshcore_ctx(void) {
    static bool told;
    if (app_ctx == LICHEN_CTX_NONE && !told) {
        // Before meshcore_app_register (or the app table was full): every gate call is refused, so say why once
        told = true;
        ESP_LOGE(TAG, "meshcore_ctx() before meshcore_run_init: no context, the OS refuses the call");
    }
    return app_ctx;
}

// The remembered passwords are the app's secrets (#288, MESHCORE_SECRET_PREFIX): never through the plain settings
bool meshcore_pw_get(uint8_t const prefix[6], char *out, size_t cap) {
    char key[RR_PW_KEY_MAX];
    rr_pw_key(prefix, key, sizeof(key));
    return app_secret_get(app_ctx, key, out, cap) && out[0] != 0;
}

bool meshcore_pw_set(uint8_t const prefix[6], char const *password) {
    char key[RR_PW_KEY_MAX];
    rr_pw_key(prefix, key, sizeof(key));
    return password[0] ? app_secret_set(app_ctx, key, password) : app_secret_erase(app_ctx, key);
}

bool meshcore_pw_forget(uint8_t const prefix[6]) {
    char key[RR_PW_KEY_MAX];
    rr_pw_key(prefix, key, sizeof(key));
    return app_secret_erase(app_ctx, key);
}

bool meshcore_autostart(void) {
    return app_settings_get_u8(app_ctx, AUTOSTART, 1) != 0;
}

bool meshcore_autostart_set(bool on) {
    bool ok = app_settings_set_u8(app_ctx, AUTOSTART, on ? 1 : 0);
    ESP_LOGI(TAG, "start at boot %s (%s)", on ? "on" : "off", ok ? "saved" : "not saved");
    return ok;
}

// #413: once per boot, after the contacts are loaded, the remembered passwords of contacts that are gone are erased
// (a rollback to a pre-#288 image deletes a contact without its stored secret). Names only, no value is read.
#define SWEEP_LOCK_WAIT_MS 20 // a lookup that cannot get the mesh lock in this long ends the sweep for this boot

static size_t sweep_list(void *env, char (*keys)[APP_SECRET_KEY_BUF], size_t cap, size_t skip, bool *more) {
    (void)env;
    return app_secret_list(app_ctx, keys, cap, skip, more);
}

static pw_contact_t sweep_contact(void *env, uint8_t const prefix[MF_PREFIX]) {
    (void)env;
    mesh_contact_detail_t d;
    size_t                r = mesh_contact_detail(prefix, &d, SWEEP_LOCK_WAIT_MS);
    return r == 1 ? PW_CONTACT_THERE : r == 0 ? PW_CONTACT_GONE : PW_CONTACT_UNKNOWN;
}

static bool sweep_erase(void *env, char const *key) {
    (void)env;
    return app_secret_erase(app_ctx, key);
}

static void sweep_leftover_passwords(void) {
    static bool           done; // boot-only: the complete-load flag is the boot load's, never cleared (mesh_task.cpp)
    static pw_sweep_io_t const io = {NULL, sweep_list, sweep_contact, sweep_erase};
    if (done) {
        return;
    }
    done          = true;
    // An erase cannot be undone: only when contacts3 was read completely and the whole list lives on /storage (with a
    // card in use /storage holds the core set, merged with the card by the mesh task a while later)
    bool trusted = mesh_contacts_load_complete() && mesh_ctab_run_place() == MESH_CTAB_ON_STORAGE;
    size_t left   = 0;
    bool   stopped = false;
    size_t erased  = pw_sweep(&io, trusted, &left, &stopped);
    if (stopped) {
        ESP_LOGI(TAG, "password sweep ended early (mesh busy); kept the rest"); // no key, no name
    }
    if (!trusted) {
        ESP_LOGI(TAG, "contact list not known complete: remembered passwords not checked");
    } else if (erased) {
        ESP_LOGI(TAG, "dropped %u remembered password(s) of removed contacts, %u kept", (unsigned)erased, (unsigned)left);
    }
}

// The same path for boot and on demand
static bool start_now(void) {
    if (!lora_up()) {
        ESP_LOGE(TAG, "no LoRa radio: MeshCore not started");
        return false;
    }
    char const *dir = lichen_fs_app_dir(app_ctx); // /storage/meshcore, as before #84 T6
    if (!dir) {
        mesh_identity_no_files(); // #110: no drive is "unreadable", never "no identity yet"
        return false;
    }
    bool ok = mesh_start(dir);
    if (ok) {
        sweep_leftover_passwords();
    }
    return ok;
}

bool meshcore_run_boot(void) {
    mesh_console_init();
    if (!meshcore_autostart()) {
        ESP_LOGI(TAG, "start at boot is off: MeshCore not started (Enter on its tile, or fw mesh start)");
        return true; // as asked: not a failed boot step
    }
    if (!apply(MESH_EV_START)) {
        return false;
    }
    bool ok = start_now();
    apply(ok ? MESH_EV_DONE_OK : MESH_EV_DONE_FAIL);
    return ok;
}

mesh_run_t meshcore_run_get(void) {
    bool running = mesh_running();
    portENTER_CRITICAL(&run_mux);
    if (!mesh_run_busy(run)) {
        mesh_run_event(&run, running ? MESH_EV_SEEN_UP : MESH_EV_SEEN_DOWN);
    }
    mesh_run_t r = run;
    portEXIT_CRITICAL(&run_mux);
    return r;
}

static void job_task(void *arg) {
    bool start = arg != NULL;
    bool ok    = start ? start_now() : mesh_stop(MESH_STOP_USER);
    if (!start && !ok) {
        // The task still stops (and frees the radio) when it gets there: STOPPING until it has, not RUNNING again
        for (uint32_t waited = 0; mesh_stop_pending() && waited < LATE_STOP_MS; waited += 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ok = !mesh_running() && !mesh_stop_pending();
    }
    apply(ok ? MESH_EV_DONE_OK : MESH_EV_DONE_FAIL);
    ESP_LOG_LEVEL(
        ok ? ESP_LOG_INFO : ESP_LOG_ERROR,
        TAG,
        "MeshCore %s",
        start ? (ok ? "started" : "did not start") : (ok ? "stopped" : "did not stop in time")
    );
    if (!start && ok) {
        // A stopped MeshCore runs nothing: a job still running (a scan, a settings save) is named by the OS (#181)
        size_t left = lichen_task_leftovers(meshcore_ctx(), LEFTOVER_MS);
        ESP_LOGI(TAG, "MeshCore stopped: %u task(s) left over", (unsigned)left);
    }
}

void meshcore_run_set_paused(bool paused) {
    apply(paused ? MESH_EV_PAUSE : MESH_EV_UNPAUSE);
}

// cause: what asked for a start (#400; key_cause.h), judged after the radio's own reasons so they are the ones shown
static char const *request(bool start, key_cause_t cause) {
    mesh_run_t r = meshcore_run_get();
    if (start && r.paused) {
        return MESH_RUN_PAUSED_REFUSAL; // before "busy": the reason is the radio's owner, not a job
    }
    if (mesh_run_busy(r)) {
        return mc_busy_note();
    }
    char const *no = start ? mesh_run_start_refusal(r, lora_up(), cause)
                           : (r.state == MESH_RUN_STOPPED ? "not running" : NULL);
    if (no) {
        if (strcmp(no, MESH_RUN_HAND_ONLY_REFUSAL) == 0) {
            // A start may send a due periodic advert: an injected key never starts it, whichever path it took
            ESP_LOGI(TAG, "%s ignored: starting MeshCore takes a hand (or fw mesh start)", key_cause_name(cause));
        }
        return no;
    }
    if (!apply(start ? MESH_EV_START : MESH_EV_STOP)) {
        return mc_busy_note(); // another request got in between
    }
    if (!lichen_task_start(meshcore_ctx(), "meshrun", job_task, start ? (void *)1 : NULL, JOB_STACK, LICHEN_PRIO_JOB, NULL)) {
        // Nothing ran: the start leaves it stopped, the stop running, as they were
        apply(MESH_EV_DONE_FAIL);
        return "no memory for the job";
    }
    return NULL;
}

// The UI's: its cause is the key (or console command) behind the screen work running now (key_cause.h)
char const *meshcore_run_request(bool start) {
    return request(start, start ? key_cause_now() : KEY_CAUSE_NONE);
}

char const *meshcore_run_request_system(bool start) {
    return request(start, KEY_CAUSE_SYSTEM);
}

bool meshcore_run_stop_for_update(void) {
    apply(MESH_EV_UPDATE);
    return mesh_stop(MESH_STOP_UPDATE);
}
