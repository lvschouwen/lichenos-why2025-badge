#include "mesh_contact_fmt.h"

#include <stdio.h>

mesh_path_t mesh_path_decode(uint8_t path_len) {
    mesh_path_t p = {.flood = path_len == MESH_PATH_FLOOD};
    if (!p.flood) {
        p.hops      = path_len & 63;
        p.hash_size = (uint8_t)((path_len >> 6) + 1);
    }
    return p;
}

void mesh_format_hops(uint8_t path_len, char *out, size_t cap) {
    mesh_path_t p = mesh_path_decode(path_len);
    if (p.flood) {
        snprintf(out, cap, "flood");
    } else if (p.hops == 0) {
        snprintf(out, cap, "direct");
    } else {
        snprintf(out, cap, "%u hop%s", p.hops, p.hops == 1 ? "" : "s");
    }
}
