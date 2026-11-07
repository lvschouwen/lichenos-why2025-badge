#pragma once
// What lichen_os wires into the notification service at boot (#235, #245), apart from the gate's notify_os.h: the
// settings store and the state hooks of the drivers below notify. The service pushes its state down to them; a driver
// never includes a notify header or calls notify (#216: L2 never reaches up to L3). Pure types, no ESP headers.

#include "fw_store.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Where do-not-disturb and the per-source switches are kept (the settings service, #235): notify opens no NVS. Bound
// at START_NVS, before lichen_notify_start; until then the defaults apply and nothing is saved.
void lichen_notify_bind_store(fw_get_u8_fn get, fw_set_u8_fn set);

// Told the state a driver below shows (the status LED, #74). dnd: do-not-disturb, pushed on every lichen_notify_set_dnd
// and at lichen_notify_start. unread: the history holds an unread entry, pushed on every change of the history (each
// generation step) and at start. Called in order from the task that made the change, inside the service's lock or a
// critical section: store the value and return (no blocking, no notify call). Any member may be NULL (that value is not
// pushed). Binding pushes both current values at once, so a late binding starts right.
typedef struct {
    void (*dnd)(bool on);
    void (*unread)(bool any);
} ln_state_hooks_t;
void lichen_notify_set_state_hooks(ln_state_hooks_t const *hooks); // copied; NULL unbinds

#ifdef __cplusplus
}
#endif
