#include "lichen_apps.h"

void lichen_apps_register(void) {
    keytest_app_register();
    clock_app_register();
    weather_app_register();
    timer_app_register();
    calc_app_register();
    notepad_app_register();
    qr_app_register();
    lora_spectrum_app_register();
}
