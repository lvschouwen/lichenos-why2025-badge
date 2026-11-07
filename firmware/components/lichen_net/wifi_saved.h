#pragma once

#include "wifi_list.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The saved networks (#160): up to WIFI_SAVED_MAX SSID/password pairs, most recently joined first. Pure list logic,
// ESP-free; wifi_list.c keeps the list in NVS. Passwords are never printed or logged.
#define WIFI_SAVED_MAX 5

typedef struct wifi_saved_s {
    size_t n;
    struct {
        char ssid[33];
        char pass[64];
    } e[WIFI_SAVED_MAX];
} wifi_saved_t;

// Index of the SSID in the list, -1 if it is not saved
int  wifi_saved_find(wifi_saved_t const *l, char const *ssid);
// Saves a valid pair at the front (an SSID that is saved already takes the new password and moves to the front); the
// oldest entry goes when the list is full. False for an invalid pair.
bool wifi_saved_add(wifi_saved_t *l, char const *ssid, char const *pass);
// Removes the SSID; false if it was not saved
bool wifi_saved_forget(wifi_saved_t *l, char const *ssid);
// The strongest scanned AP that is a saved network and whose bit (1 << index) is not in skip_mask; returns its index in
// the list or -1. Saved open networks (empty password) are never picked, and an open AP never matches a saved
// network that has a password (an open twin would be a trap): those are joined only when the user picks them.
int  wifi_saved_pick(wifi_saved_t const *l, wifi_ap_t const *aps, size_t n, uint32_t skip_mask);
// Wipes the passwords from memory
void wifi_saved_wipe(wifi_saved_t *l);
