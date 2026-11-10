// "fw mesh"'s radio line: MeshCore's settings (what the P4 asks the C6 for). Out of mesh_task.cpp, which is past the
// size line (#310); the C6's applied radio is "fw lora"'s.
#include "mesh_radio_line.h"

#include "fw_console.h"
#include "mesh.h"
#include "radio_presets.h"

void mesh_radio_console(void) {
    mesh_settings_t st;
    if (mesh_get_settings(&st)) {
        int preset = radio_preset_match(st.freq, st.bw, st.sf, st.cr);
        console_reply("radio %.3f MHz, %g kHz, SF%u, CR%u, %d dBm, path hash %u B (%s)", st.freq, st.bw, st.sf, st.cr,
                      st.tx_dbm, st.path_hash_size, preset >= 0 ? radio_preset_at((size_t)preset)->title : "custom");
    }
}
