#pragma once
// The SD apps' malloc blocks, as the loader keeps them (#536): export_shims.c tells the loader of every block it hands an app and
// every one the app frees, so what the app left is freed when it unloads. Only blocks made through the shims: a block the app
// hands to the OS stays the app's until the OS lets go (the loader sweeps after the app's screens, timers, tasks and queued
// UI calls are gone). Hence an app must not leave an object outside its own screens (on lv_layer_sys, say) pointing at a
// malloc'd buffer: the buffer is freed at unload. A context that is no SD app is ignored.

#include "lichen_app.h"

void sd_app_block_note(lichen_ctx_t ctx, void *ptr);   // a block the app now holds
void sd_app_block_forget(lichen_ctx_t ctx, void *ptr); // the app frees it itself
