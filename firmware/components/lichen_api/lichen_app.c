// The app table (#66) behind lichen_app.h: apps register before shell_home(), home and Settings list them.
#include "lichen_app.h"

#include "esp_log.h"

static char const TAG[] = "shell";

static lichen_app_t const *apps[LICHEN_MAX_APPS];
static size_t              app_count;

void lichen_register_app(lichen_app_t const *app) {
    if (app_count >= LICHEN_MAX_APPS) {
        ESP_LOGE(TAG, "no room for app %s: LICHEN_MAX_APPS (%d) apps are registered already", app->name, LICHEN_MAX_APPS);
        return;
    }
    apps[app_count++] = app;
}

size_t lichen_app_count(void) {
    return app_count;
}

lichen_app_t const *lichen_app_at(size_t index) {
    return index < app_count ? apps[index] : NULL;
}
