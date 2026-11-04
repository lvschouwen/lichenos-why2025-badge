#pragma once

#include <stdbool.h>

// The MeshCore grid's saved tile order (#189): "1:chat,contacts,..." under mesh_order in MeshCore's own settings
// namespace (lichen_settings.h), over the pure tile_order.h and the ids of mesh_grid.h. Nothing is written until the
// person moves a tile; the first order (the enum's) is the absence of the key.

// order[slot] = the tile shown there (a permutation of 0..MESH_GRID_TILES-1); true when a saved order was used
bool mesh_order_load(int *order);
// Stores order, or erases the key when it is the first order; false when the store failed
bool mesh_order_save(int const *order);
// Back to the first order (erases the key); false when the store failed
bool mesh_order_reset(void);
// Whether an order is saved (Settings' "Arranged"/"Default")
bool mesh_order_arranged(void);
