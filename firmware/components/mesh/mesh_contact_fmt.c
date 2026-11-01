#include "mesh_contact_fmt.h"

#include "mesh_format.h"

#include <stdint.h>
#include <stdio.h>

#include <string.h>

mesh_path_t mesh_path_decode(uint8_t path_len) {
    mesh_path_t p = {.flood = path_len == MESH_PATH_FLOOD};
    if (!p.flood) {
        p.hops      = path_len & 63;
        p.hash_size = (uint8_t)((path_len >> 6) + 1);
    }
    return p;
}

void mesh_format_hops(uint8_t path_len, char *out, size_t cap) {
    mesh_path_t p = mesh_path_decode(path_len);
    if (p.flood) {
        snprintf(out, cap, "flood");
    } else if (p.hops == 0) {
        snprintf(out, cap, "direct");
    } else {
        snprintf(out, cap, "%u hop%s", p.hops, p.hops == 1 ? "" : "s");
    }
}

char const *mesh_contact_type_name(uint8_t type) {
    switch (type) {
        case 1: return "companion";
        case 2: return "repeater";
        case 3: return "room";
        case 4: return "sensor";
        default: return "unknown";
    }
}

void mesh_hex(uint8_t const *bytes, size_t n, size_t group, char *out, size_t cap) {
    static char const digits[] = "0123456789abcdef";
    size_t            at       = 0;
    if (cap == 0) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        bool space = group && i > 0 && i % group == 0;
        if (at + (space ? 3 : 2) >= cap) {
            break;
        }
        if (space) {
            out[at++] = ' ';
        }
        out[at++] = digits[bytes[i] >> 4];
        out[at++] = digits[bytes[i] & 15];
    }
    out[at] = '\0';
}

void mesh_format_route(uint8_t path_len, uint8_t const *path, char *out, size_t cap) {
    mesh_path_t p = mesh_path_decode(path_len);
    mesh_format_hops(path_len, out, cap);
    if (p.flood || p.hops == 0 || cap == 0) {
        return;
    }
    size_t at  = strlen(out);
    at        += (size_t)snprintf(out + at, at < cap ? cap - at : 0, " via ");
    for (size_t h = 0; h < p.hops && at < cap; h++) {
        size_t first = (size_t)h * p.hash_size;
        if (first + p.hash_size > MESH_CONTACT_PATH_MAX) {
            break; // a byte that does not fit MeshCore's path: never read past it
        }
        if (h > 0) {
            at += (size_t)snprintf(out + at, cap - at, ",");
        }
        if (at < cap) {
            mesh_hex(path + first, p.hash_size, 0, out + at, cap - at);
            at += strlen(out + at);
        }
    }
    if (p.hash_size > 1 && at < cap) {
        snprintf(out + at, cap - at, " (%u-byte hashes)", p.hash_size);
    }
}

// RFC 3986 unreserved bytes stay, a space is '+', everything else (UTF-8 included) is %XX
static size_t url_encode(char const *in, char *out, size_t cap) {
    static char const digits[] = "0123456789ABCDEF";
    size_t            at       = 0;
    for (; *in; in++) {
        unsigned char c    = (unsigned char)*in;
        bool          keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '.' || c == '_' || c == '~';
        size_t need = keep || c == ' ' ? 1 : 3;
        if (at + need >= cap) {
            return SIZE_MAX;
        }
        if (keep || c == ' ') {
            out[at++] = c == ' ' ? '+' : (char)c;
        } else {
            out[at++] = '%';
            out[at++] = digits[c >> 4];
            out[at++] = digits[c & 15];
        }
    }
    out[at] = '\0';
    return at;
}

bool mesh_contact_uri(char const *name, uint8_t const pub[MESH_CONTACT_PUB_LEN], uint8_t type, char *out, size_t cap) {
    char enc[3 * MESH_CONTACT_NAME_MAX + 1];
    char key[2 * MESH_CONTACT_PUB_LEN + 1];
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (url_encode(name, enc, sizeof(enc)) == SIZE_MAX) {
        return false;
    }
    mesh_hex(pub, MESH_CONTACT_PUB_LEN, 0, key, sizeof(key));
    int n = snprintf(out, cap, "meshcore://contact/add?name=%s&public_key=%s&type=%u", enc, key, type);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool mesh_contact_before(uint8_t flags_a, uint32_t age_a, uint8_t flags_b, uint32_t age_b) {
    bool fav_a = flags_a & MESH_CONTACT_FAV, fav_b = flags_b & MESH_CONTACT_FAV;
    if (fav_a != fav_b) {
        return fav_a;
    }
    return age_a < age_b;
}

// "52.123456, -4.500000" without floats (newlib-nano's printf has none)
static void format_degrees(int32_t e6, char *out, size_t cap) {
    uint32_t mag = e6 < 0 ? (uint32_t)(-(int64_t)e6) : (uint32_t)e6;
    snprintf(
        out,
        cap,
        "%s%lu.%06lu",
        e6 < 0 ? "-" : "",
        (unsigned long)(mag / 1000000),
        (unsigned long)(mag % 1000000)
    );
}

size_t mesh_contact_lines(mesh_contact_detail_t const *d, mesh_contact_line_t *out, size_t max) {
    char   buf[MESH_LINE_MAX - 16], lat[16], lon[16];
    size_t n = 0;
    for (; n < max && n < MESH_CONTACT_LINES; n++) {
        char  *t   = out[n].text;
        size_t cap = sizeof(out[n].text);
        switch (n) {
            case MESH_LINE_TYPE: snprintf(t, cap, "Type: %s", mesh_contact_type_name(d->type)); break;
            case MESH_LINE_ID:
                mesh_hex(d->pub, 6, 0, buf, sizeof(buf));
                snprintf(t, cap, "ID: %s", buf);
                break;
            case MESH_LINE_KEY:
                mesh_hex(d->pub, MESH_CONTACT_PUB_LEN, 4, buf, sizeof(buf));
                snprintf(t, cap, "Key: %s", buf);
                break;
            case MESH_LINE_HEARD:
                mesh_format_age(d->age_s, buf, sizeof(buf));
                snprintf(t, cap, "Last heard: %s", buf);
                break;
            case MESH_LINE_ROUTE:
                mesh_format_route(d->path_len, d->path, buf, sizeof(buf));
                snprintf(t, cap, "Route: %s", buf);
                break;
            default:
                if (d->lat_e6 == 0 && d->lon_e6 == 0) {
                    snprintf(t, cap, "Location: not shared");
                } else {
                    format_degrees(d->lat_e6, lat, sizeof(lat));
                    format_degrees(d->lon_e6, lon, sizeof(lon));
                    snprintf(t, cap, "Location: %s, %s", lat, lon);
                }
                break;
        }
    }
    return n;
}
