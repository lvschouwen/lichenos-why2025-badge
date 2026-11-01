// The badge counts (#102): see badge_table.h
#include "badge_table.h"

#include "app_ns.h"

#include <string.h>

static int find(badge_table_t const *t, char const *id) {
    for (int i = 0; i < BADGE_SLOTS; i++) {
        if (t->slot[i].count && strcmp(t->slot[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static int free_slot(badge_table_t const *t) {
    for (int i = 0; i < BADGE_SLOTS; i++) {
        if (!t->slot[i].count) {
            return i;
        }
    }
    return -1;
}

static bool valid_id(char const *id) {
    char ns[16];
    return id && app_ns_name(id, ns, sizeof(ns)); // the one rule for an app id
}

bool badge_table_set(badge_table_t *t, char const *id, unsigned count) {
    if (!valid_id(id)) {
        return false;
    }
    int i = find(t, id);
    if (i < 0) {
        if (count == 0) {
            return false; // was none, stays none
        }
        i = free_slot(t);
        if (i < 0) {
            return false;
        }
        strcpy(t->slot[i].id, id); // valid_id: at most 13 characters
    } else if (t->slot[i].count == count) {
        return false;
    }
    t->slot[i].count = count;
    t->generation++;
    return true;
}

unsigned badge_table_get(badge_table_t const *t, char const *id) {
    int i = id ? find(t, id) : -1;
    return i < 0 ? 0 : t->slot[i].count;
}
