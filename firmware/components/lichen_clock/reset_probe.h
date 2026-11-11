#pragma once
// The reset-survival probe (#330 spike, CONFIG_LICHEN_RESET_PROBE); see reset_probe.c

#ifdef __cplusplus
extern "C" {
#endif

// Once, early at boot (clock_boot_init): read and log the three markers, then rewrite them
void reset_probe_boot(void);
// "fw resetprobe arm|show"
void reset_probe_console_register(void);

#ifdef __cplusplus
}
#endif
