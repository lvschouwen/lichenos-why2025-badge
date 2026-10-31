// MeshCore's home tile text (#67): see mesh_tile.h
#include "mesh_tile.h"

#include <stdio.h>
#include <string.h>

#define DOT "\xE2\x80\xA2" // U+2022: Montserrat on the badge has it, not U+00B7

void mesh_tile_text(mesh_tile_t const *t, char *line1, size_t c1, char *line2, size_t c2) {
    uint32_t unread = t->unread_total;
    char     nodes[24];
    snprintf(nodes, sizeof(nodes), "%u %s", (unsigned)t->contacts, t->contacts == 1 ? "node" : "nodes");
    if (c1) {
        if (unread == 0) {
            snprintf(line1, c1, "%s", nodes);
        } else if (unread > MESH_TILE_UNREAD_CAP) {
            snprintf(line1, c1, "%u+ unread " DOT " %s", (unsigned)MESH_TILE_UNREAD_CAP, nodes);
        } else {
            snprintf(line1, c1, "%u unread " DOT " %s", (unsigned)unread, nodes);
        }
    }
    if (c2) {
        if (t->latest[0]) {
            mesh_sanitize(t->latest, strlen(t->latest), line2, c2); // cuts on a character boundary
        } else {
            snprintf(line2, c2, "Listening on Public");
        }
    }
}

void mesh_tile_on_message(mesh_tile_t *t, char const *channel, char const *text) {
    char line[MESH_CHANNEL_MAX + MESH_TEXT_MAX + 8];
    snprintf(line, sizeof(line), "[%s] %s", channel, text);
    mesh_sanitize(line, strlen(line), t->latest, sizeof(t->latest));
}
