// "fw mesh"'s radio lines: MeshCore's settings (what the P4 asks the C6 for) and, since #335, what the C6 applied (the
// registry cache, as the screens show it). Out of mesh_task.cpp, which is past the size line (#310).
#include "mesh_radio_line.h"

#include "fw_console.h"
#include "lichen_reg.h"
#include "mesh.h"
#include "radio_presets.h"

#include <stdbool.h>

void mesh_applied_read(mesh_applied_t *out) {
    char        text[MESH_APPLIED_KEYS][48];
    char const *cached[MESH_APPLIED_KEYS];
    for (int i = 0; i < MESH_APPLIED_KEYS; i++) {
        cached[i] = lichen_reg_get(mesh_applied_names[i], text[i], sizeof(text[i])) ? text[i] : NULL;
    }
    mesh_applied_parse(cached, out);
}

void mesh_radio_console(void) {
    mesh_settings_t st;
    bool            known = mesh_get_settings(&st);
    if (known) {
        int preset = radio_preset_match(st.freq, st.bw, st.sf, st.cr);
        console_reply("radio %.3f MHz, %g kHz, SF%u, CR%u, %d dBm, path hash %u B (%s)", st.freq, st.bw, st.sf, st.cr,
                      st.tx_dbm, st.path_hash_size, preset >= 0 ? radio_preset_at((size_t)preset)->title : "custom");
    }
    mesh_applied_t applied;
    char           line[96];
    mesh_applied_read(&applied);
    mesh_applied_line(&applied, true, line, sizeof(line));
    bool differs = known && mesh_applied_differs(&applied, st.freq, st.bw, st.sf, st.cr, st.tx_dbm);
    console_reply("applied %s%s", line, differs ? ", differs from MeshCore's radio above" : "");
}
