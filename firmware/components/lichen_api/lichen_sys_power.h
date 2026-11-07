#pragma once

// The power path's system surface (#226, #216 decision 1): the parts of switching the badge off the OS must reach
// below the gate. System surface: tools/layers.conf forbids lichen_sys_* in L6. The Power app, the thermal shutdown
// and the restart hook keep their sequence in the shell and main (shell_set_power_hook); this is the display's share.

// Power off (#65): both backlights off, then the panel, as before a restart. The C6 must still run (it drives the
// lights), so call it before the C6 is held in reset. Power-off task (main's power hook), not an interrupt.
void lichen_sys_power_screen_off(void);
