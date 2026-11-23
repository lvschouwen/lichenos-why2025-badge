// The badge counts (#102): see badge_table.h
#include "badge_table.h"

bool badge_count_set(uint32_t *count, uint32_t *generation, uint32_t new_count) {
    if (*count == new_count) {
        return false;
    }
    *count = new_count;
    (*generation)++;
    return true;
}
