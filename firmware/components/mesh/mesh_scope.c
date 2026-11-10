#include "mesh_scope.h"

#include <stdio.h>
#include <string.h>

char const *mesh_scope_parse(char const *in, char *out, size_t cap) {
    if (cap == 0) {
        return "no room";
    }
    out[0] = '\0';
    while (*in == ' ') {
        in++;
    }
    if (*in == '#') {
        in++;
    }
    size_t len = strlen(in);
    while (len > 0 && in[len - 1] == ' ') {
        len--;
    }
    if (len == 0) {
        return NULL; // None
    }
    if (len > MESH_SCOPE_MAX || len >= cap) {
        return "A region name is at most 30 characters";
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c >= 0x80;
        if (!ok) {
            out[0] = '\0';
            return "A region name takes letters, digits and '-'";
        }
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    out[len] = '\0';
    return NULL;
}

void mesh_scope_text(char const *name, char *out, size_t cap) {
    snprintf(out, cap, "%s", name && name[0] ? name : "None (everywhere)");
}

void mesh_scope_ask_text(char const *name, char *out, size_t cap) {
    snprintf(out, cap, "Adverts and messages will only travel in region %s. Contacts outside it can no longer reach you.",
             name);
}

void mesh_scope_key_text(char const *name, char *out, size_t cap) {
    snprintf(out, cap, "#%s", name);
}

mesh_scope_use_t mesh_scope_choose(bool from_console, char const *console_name) {
    if (!from_console) {
        return MESH_SCOPE_USE_DEFAULT;
    }
    return console_name && console_name[0] ? MESH_SCOPE_USE_CONSOLE : MESH_SCOPE_USE_REFUSE;
}

char const *mesh_scope_console_arg(char const *args, char *out, size_t cap) {
    if (cap == 0) {
        return "no room";
    }
    out[0]     = '\0';
    size_t len = strlen(args);
    while (len > 0 && args[len - 1] == ' ') {
        len--;
    }
    if (len < 3 || strncmp(args + len - 3, "YES", 3) != 0 || (len > 3 && args[len - 4] != ' ')) {
        return "this sets the scope of every flood from the console: add YES at the end, fw mesh scope console "
               "<region> YES";
    }
    char   region[MESH_SCOPE_MAX + 2];
    size_t region_len = len - 3;
    if (region_len >= sizeof(region)) {
        return "A region name is at most 30 characters";
    }
    memcpy(region, args, region_len);
    region[region_len] = '\0';
    char const *why    = mesh_scope_parse(region, out, cap);
    if (!why && !out[0]) {
        why = "mesh scope console <region> YES: a region name (an unset console scope refuses console floods)";
    }
    return why;
}

void mesh_scope_status_text(char const *default_name, char const *console_name, char *out, size_t cap) {
    snprintf(out, cap, "default %s | console %s", default_name && default_name[0] ? default_name : "None",
             console_name && console_name[0] ? console_name : "not set");
}
