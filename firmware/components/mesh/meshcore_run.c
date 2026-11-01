// MeshCore running or not (#103): the state machine of mesh_run.c under a spinlock, the starts and stops on a job task.
// The mesh task itself (mesh_task.cpp) serialises mesh_start and mesh_stop as well, so the update path and a job can
// never overlap.
#include "meshcore_run.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_fs.h"
#include "lichen_settings.h"
#include "lora.h"
#include "mesh.h"

#include <stdint.h>

static char const TAG[] = "meshcore";

#define JOB_STACK    8192 // a first start loads everything, as app_main's 8 KB did at boot
#define AUTOSTART    "autostart"

static lichen_app_t const *app;
static mesh_run_t          run = MESH_RUN_INIT;
static portMUX_TYPE        run_mux = portMUX_INITIALIZER_UNLOCKED;

static bool apply(mesh_run_ev_t ev) {
    portENTER_CRITICAL(&run_mux);
    bool changed = mesh_run_event(&run, ev);
    portEXIT_CRITICAL(&run_mux);
    return changed;
}

void meshcore_run_init(lichen_app_t const *the_app) {
    app = the_app;
}

bool meshcore_autostart(void) {
    return app_settings_get_u8(app, AUTOSTART, 1) != 0;
}

bool meshcore_autostart_set(bool on) {
    bool ok = app_settings_set_u8(app, AUTOSTART, on ? 1 : 0);
    ESP_LOGW(TAG, "start at boot %s (%s)", on ? "on" : "off", ok ? "saved" : "not saved");
    return ok;
}

// The same path for boot and on demand
static bool start_now(void) {
    if (!lora_up()) {
        ESP_LOGE(TAG, "no LoRa radio: MeshCore not started");
        return false;
    }
    char const *dir = lichen_fs_app_dir(app); // /storage/meshcore, as before #84 T6
    return dir && mesh_start(dir);
}

bool meshcore_run_boot(void) {
    mesh_console_init();
    if (!meshcore_autostart()) {
        ESP_LOGW(TAG, "start at boot is off: MeshCore not started (Enter on its tile, or fw mesh start)");
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
    apply(ok ? MESH_EV_DONE_OK : MESH_EV_DONE_FAIL);
    ESP_LOGW(TAG, "MeshCore %s", start ? (ok ? "started" : "did not start") : (ok ? "stopped" : "did not stop in time"));
    vTaskDelete(NULL);
}

char const *meshcore_run_request(bool start) {
    mesh_run_t r = meshcore_run_get();
    if (mesh_run_busy(r)) {
        return "busy, try again";
    }
    if (start && r.state == MESH_RUN_RUNNING) {
        return "already running";
    }
    if (!start && r.state == MESH_RUN_STOPPED) {
        return "not running";
    }
    if (start && r.why == MESH_WHY_UPDATE) {
        return "stopped for update";
    }
    if (start && !lora_up()) {
        return "no LoRa radio";
    }
    if (!apply(start ? MESH_EV_START : MESH_EV_STOP)) {
        return "busy, try again"; // another request got in between
    }
    if (xTaskCreatePinnedToCore(job_task, "meshrun", JOB_STACK, start ? (void *)1 : NULL, 4, NULL, 0) != pdPASS) {
        // Nothing ran: the start leaves it stopped, the stop running, as they were
        apply(MESH_EV_DONE_FAIL);
        return "no memory for the job";
    }
    return NULL;
}

bool meshcore_run_stop_for_update(void) {
    apply(MESH_EV_UPDATE);
    return mesh_stop(MESH_STOP_UPDATE);
}
