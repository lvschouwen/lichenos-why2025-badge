#pragma once

// The plain apps (#180). Each *_app_register() adds one app to lichen_app (lichen_app.h); the order is home's order.

void keytest_app_register(void);       // app_keytest.c
void clock_app_register(void);         // app_clock.c (#122)
void weather_app_register(void);       // weather_app.c (#155)
void timer_app_register(void);         // app_timer.c (#152)
void calc_app_register(void);          // app_calc.c (#158)
void notepad_app_register(void);       // app_notes.c (#154): notes and to dos on the SD card
void qr_app_register(void);            // app_qr.c (#153)
void lora_spectrum_app_register(void); // app_lora_spectrum.c (#161)

// Registers all of the above, keeping their order among themselves. They now come after the shell's own apps (Settings,
// About, Files, ...), because main calls this from the apps boot step. Call once at start-up, before home is built.
void lichen_apps_register(void);
