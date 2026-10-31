#include "nav.h"

// stack[0] is always home
static int stack[NAV_MAX_DEPTH] = {NAV_HOME};
static int depth                = 1;

void nav_reset(void) {
    stack[0] = NAV_HOME;
    depth    = 1;
}

bool nav_push(int screen_id) {
    if (depth >= NAV_MAX_DEPTH || screen_id == stack[depth - 1]) { // the same screen again: a repeated Enter
        return false;
    }
    stack[depth++] = screen_id;
    return true;
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
