#include "nav.h"

// stack[0] is always home
static int      stack[NAV_MAX_DEPTH] = {NAV_HOME};
static uint32_t keys[NAV_MAX_DEPTH];
static int      depth = 1;

void nav_reset(void) {
    stack[0] = NAV_HOME;
    keys[0]  = 0;
    depth    = 1;
}

nav_open_t nav_decide(int screen_id, uint32_t key, int *frame) {
    if (key != NAV_KEY_UNIQUE) {
        for (int i = depth - 1; i >= 0; i--) {
            if (stack[i] == screen_id && keys[i] == key) {
                *frame = i;
                return i == depth - 1 ? NAV_OPEN_KEEP : NAV_OPEN_BACK_TO;
            }
        }
    }
    *frame = -1;
    return NAV_OPEN_NEW;
}

bool nav_push_keyed(int screen_id, uint32_t key) {
    int frame;
    if (depth >= NAV_MAX_DEPTH || nav_decide(screen_id, key, &frame) != NAV_OPEN_NEW) {
        return false;
    }
    stack[depth] = screen_id;
    keys[depth]  = key;
    depth++;
    return true;
}

bool nav_push(int screen_id) {
    return nav_push_keyed(screen_id, 0);
}

uint32_t nav_key_at(int index) {
    return index >= 0 && index < depth ? keys[index] : 0;
}

int nav_back(void) {
    if (depth > 1) {
        depth--;
    }
    return stack[depth - 1];
}

int nav_current(void) {
    return stack[depth - 1];
}

int nav_depth(void) {
    return depth;
}

int nav_at(int index) {
    return index >= 0 && index < depth ? stack[index] : -1;
}
