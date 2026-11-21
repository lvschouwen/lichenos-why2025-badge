#pragma once

#include "wifi_eap.h"
#include "wifi_list.h"

#include <stdbool.h>

// Putting a network into the C6 driver, and the Enterprise state around it (#496). Split out of wifi_service.c: the
// service keeps the task, the queue and the trial; this keeps what the driver holds and what `fw wifi` says about it.
// All the apply functions run on the c6wifi task (or on the boot path before the driver starts). lock is the service's
// saved_lock with its heap allocator.

// The driver came up on a (re-)attached C6: it holds no Enterprise state, and nothing is blocked
void wifi_apply_reset(void);
// The pair in use could not be applied (an Enterprise network on a C6 without 802.1X, a damaged record): the service
// must not connect, so there is no retry loop. Cleared by the next pair that applies.
bool wifi_apply_blocked(void);
// The reason and the CA state are cleared by a join that got an IP, a new request, or a Forget
void wifi_apply_clear_fail(void);
void wifi_apply_forgotten(void);

// A pair as the driver gets it. eap NULL: PSK or open, and the Enterprise state of a previous network is cleared.
bool wifi_apply_net(char const *ssid, char const *pass, wifi_eap_t const *eap, wifi_lock_t const *lock);
// A saved or kept pair: an Enterprise network has its record next to the list (takes lock). Fails closed: a record that
// is gone or damaged is a failed join, never an open network. An empty ssid clears the driver's copy.
bool wifi_apply_saved(char const *ssid, char const *pass, wifi_lock_t const *lock);
// With lock held by the caller: saves an Enterprise trial that got its IP (record and list entry)
bool wifi_apply_save_trial(char const *ssid, wifi_eap_t const *eap, wifi_lock_t const *lock);

// `fw wifi eap ...` (args = the text after "eap ") and the Enterprise lines of `fw wifi`
void wifi_apply_console_eap(char const *args, wifi_lock_t const *lock);
void wifi_apply_console_status(void);
