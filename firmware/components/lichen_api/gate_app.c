// The gate's check of a caller's context for settings, files, badges and notices (#230 E1c), see gate_app.h.
#include "gate_app.h"

#include "app_ns.h"

gate_app_kind_t gate_app_id(gate_app_env_t const *g, uint32_t ctx, char const *what, char const **id,
                            char const **name) {
    char const *app_id   = NULL;
    char const *app_name = NULL;
    if (!g->resolve(ctx, &app_id, &app_name, what)) {
        return GATE_APP_REFUSED;
    }
    if (ctx == g->os()) {
        return GATE_APP_OS;
    }
    *name = app_name;
    if (!app_id || !app_id[0]) {
        return GATE_APP_NO_ID;
    }
    *id = app_id;
    return GATE_APP_ID;
}

char const *gate_app_source(gate_app_env_t const *g, uint32_t ctx, char const *what) {
    char const     *id   = NULL;
    char const     *name = NULL;
    gate_app_kind_t kind = gate_app_id(g, ctx, what, &id, &name);
    return kind == GATE_APP_OS ? GATE_OS_SOURCE : kind == GATE_APP_ID ? id : NULL;
}

bool gate_app_settings_ns(gate_app_env_t const *g, uint32_t ctx, char const *what, char *ns, size_t cap) {
    char const *id   = NULL;
    char const *name = NULL;
    return gate_app_id(g, ctx, what, &id, &name) == GATE_APP_ID && app_ns_name(id, ns, cap);
}
