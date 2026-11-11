// The MeshCore grid's saved tile order and its console side (#189): see mesh_order.h
#include "mesh_order.h"

#include "lichen_settings.h"
#include "lichen_ui.h"
#include "fw_console.h"
#include "meshcore_run.h"
#include "meshcore_screens.h"
#include "mesh.h"
#include "mesh_grid.h"
#include "tile_order.h"

#include <stdio.h>
#include <string.h>

#define KEY_ORDER "mesh_order"
#define ORDER_CAP 128 // eight ids of at most nine characters, their commas and the version: well under

bool mesh_order_load(int *order) {
    char const *ids[MESH_GRID_TILES];
    char        text[ORDER_CAP];
    mesh_grid_ids(ids);
    app_settings_get_str(meshcore_ctx(), KEY_ORDER, text, sizeof(text), "");
    return tile_order_merge(text, ids, MESH_GRID_TILES, order);
}

bool mesh_order_save(int const *order) {
    if (tile_order_is_default(order, MESH_GRID_TILES)) {
        return app_settings_erase(meshcore_ctx(), KEY_ORDER);
    }
    char const *ids[MESH_GRID_TILES];
    char        text[ORDER_CAP];
    mesh_grid_ids(ids);
    return tile_order_encode(ids, order, MESH_GRID_TILES, text, sizeof(text)) > 0 &&
           app_settings_set_str(meshcore_ctx(), KEY_ORDER, text);
}

bool mesh_order_reset(void) {
    return app_settings_erase(meshcore_ctx(), KEY_ORDER);
}

bool mesh_order_arranged(void) {
    int order[MESH_GRID_TILES];
    return mesh_order_load(order);
}

static void reply_order(void) {
    int  order[MESH_GRID_TILES];
    bool saved = mesh_order_load(order);
    char list[ORDER_CAP];
    size_t len = 0;
    list[0]    = '\0';
    for (int slot = 0; slot < MESH_GRID_TILES && len < sizeof(list); slot++) {
        int w = snprintf(list + len, sizeof(list) - len, "%s%s", slot ? ", " : "",
                         mesh_grid_id((mesh_grid_tile_t)order[slot]));
        len += w > 0 ? (size_t)w : 0;
    }
    console_reply("mesh order (%s): %s", saved ? "arranged" : "default", list);
}

static void reload_grid(void *arg) {
    (void)arg;
    meshcore_grid_reload();
}

void mesh_order_console(char const *args) {
    while (*args == ' ') {
        args++;
    }
    if (strcmp(args, "reset") == 0) {
        if (!mesh_order_reset()) {
            console_reply("mesh order not reset: the store failed");
            return;
        }
        if (!lichen_ui_call(reload_grid, NULL)) { // a grid on screen takes the first order at once
            console_reply("mesh order reset; reopen the grid to see it");
        }
    } else if (*args) {
        console_reply("mesh order [reset]");
        return;
    }
    reply_order();
}
