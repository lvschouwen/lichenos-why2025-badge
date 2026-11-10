// "fw mesh"'s radio lines (#310): MeshCore's settings (what the P4 asks for), then what the C6's SX1262 applies, from
// the settings registry's cache (lora_applied_text never waits). Out of mesh_task.cpp, which is past the size line.
#include "fw_console.h"
#include "lora_app.h"
#include "mesh.h"
#include "radio_presets.h"

void mesh_radio_console(void) {
    mesh_settings_t st;
    if (mesh_get_settings(&st)) {
        int preset = radio_preset_match(st.freq, st.bw, st.sf, st.cr);
        console_reply("radio %.3f MHz, %g kHz, SF%u, CR%u, %d dBm, path hash %u B (%s)", st.freq, st.bw, st.sf, st.cr,
                      st.tx_dbm, st.path_hash_size, preset >= 0 ? radio_preset_at((size_t)preset)->title : "custom");
    }
    char applied[160];
    if (lora_applied_text(applied, sizeof(applied))) {
        console_reply("radio applied by the C6: %s", applied);
    }
}
