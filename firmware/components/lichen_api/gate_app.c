// The gate's check of a caller's context for settings, files, badges and notices (#230 E1c), see gate_app.h.
#include "gate_app.h"

#include "app_ns.h"
#include "fs_path.h"

#include <string.h>

// An id an app may have: the app id rules, none of the drive folders the OS keeps (fs_path.h), and not the console's
// notification source
static bool id_usable(char const *id) {
    char path[16];
    return id && id[0] && strcmp(id, "test") != 0 && fs_app_dir_path("/", id, path, sizeof(path));
}

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
    if (!id_usable(app_id)) {
        return GATE_APP_NO_ID;
    }
    *id = app_id;
    return GATE_APP_ID;
}

gate_app_kind_t gate_app_caller(gate_app_env_t const *g, uint32_t ctx, char const *what, char const **id,
                                char const **name) {
    gate_app_kind_t kind = gate_app_id(g, ctx, what, id, name);
    if (kind == GATE_APP_OS) {
        g->refuse(ctx, what);
        return GATE_APP_REFUSED;
    }
    return kind;
}

char const *gate_app_source(gate_app_env_t const *g, uint32_t ctx, char const *what) {
    char const *id   = NULL;
    char const *name = NULL;
    return gate_app_caller(g, ctx, what, &id, &name) == GATE_APP_ID ? id : NULL;
}

gate_app_kind_t gate_app_settings_ns(gate_app_env_t const *g, uint32_t ctx, char const *what, char *ns, size_t cap,
                                     char const **name) {
    char const     *id   = NULL;
    gate_app_kind_t kind = gate_app_id(g, ctx, what, &id, name);
    if (kind == GATE_APP_ID && !app_ns_name(id, ns, cap)) {
        return GATE_APP_NO_ID;
    }
    return kind;
}

static bool name_taken(char const *name, char const *const *taken, size_t n) {
    for (size_t i = 0; name && i < n; i++) {
        if (taken[i] && strcmp(taken[i], name) == 0) {
            return true;
        }
    }
    return false;
}

char const *gate_app_names_refusal(char const *id, char const *alias, char const *const *taken, size_t n) {
    if (id && !id_usable(id)) {
        return "the id is reserved for the OS or not an app id";
    }
    if (alias && !id_usable(alias)) {
        return "the alias is reserved for the OS or not an app id";
    }
    if (name_taken(id, taken, n)) {
        return "another app has the id";
    }
    if (name_taken(alias, taken, n)) {
        return "another app has the alias";
    }
    return NULL;
}

bool gate_batch_may_end(uint32_t held, uint32_t ctx) {
    return held != 0 && held == ctx;
}
