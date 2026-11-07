#include "c6_hooks_priv.h"

#include <stddef.h>

// Written by main's boot task before c6link_start creates the c6sup task (the task start orders the writes before
// every read), read-only after that: no lock
static c6link_boot_hooks_t boot;
static c6link_net_hooks_t  net;

void c6link_set_boot_hooks(c6link_boot_hooks_t const *hooks) {
    boot = hooks ? *hooks : (c6link_boot_hooks_t){0};
}

void c6link_set_net_hooks(c6link_net_hooks_t const *hooks) {
    net = hooks ? *hooks : (c6link_net_hooks_t){0};
}

void c6_hooks_step_begin(void) {
    if (boot.begin) {
        boot.begin();
    }
}

void c6_hooks_step_progress(int percent) {
    if (boot.progress) {
        boot.progress(percent);
    }
}

void c6_hooks_step_end(bool ok, char const *note) {
    if (boot.end) {
        boot.end(ok, note);
    }
}

bool c6_hooks_attached(void) {
    if (!net.attached) {
        return false;
    }
    net.attached();
    return true;
}

void c6_hooks_no_link(void) {
    if (net.no_link) {
        net.no_link();
    }
}

bool c6_hooks_driver_down(uint32_t wait_ms) {
    return net.driver_down ? net.driver_down(wait_ms) : true;
}

bool c6_hooks_driver_up(uint32_t wait_ms) {
    return net.driver_up ? net.driver_up(wait_ms) : true;
}

unsigned c6_hooks_stack_free(void) {
    return net.stack_free ? net.stack_free() : 0;
}
