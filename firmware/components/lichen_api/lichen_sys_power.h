#pragma once

// The power path's system surface (#226, #216 decision 1): the parts of switching the badge off the OS must reach
// below the gate. System surface: tools/layers.conf forbids lichen_sys_* in L6. The Power app and the restart hook keep
// their sequence in the shell and main (shell_set_power_hook); this is the display's share and the final step.

// Power off (#65): both backlights off, then the panel, as before a restart. The C6 must still run (it drives the
// lights), so call it before the C6 is held in reset. Power-off task (main's power hook), not an interrupt.
void lichen_sys_power_screen_off(void);

// The rest of power off, after the saves: the screen off (as above), the motor stopped, the C6 held in reset, then
// the P4 in deep sleep with nothing to wake it. With the load that low the IP5306 cuts the cells after ~32 s; the
// power button starts the badge from cold. Saves nothing: main's power hook and its headless thermal path save first.
// Any task but an interrupt; does not return.
void lichen_sys_power_off_now(void) __attribute__((noreturn));
