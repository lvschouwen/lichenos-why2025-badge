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
