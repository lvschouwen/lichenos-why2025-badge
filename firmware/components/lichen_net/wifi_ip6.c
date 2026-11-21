#include "wifi_ip6.h"

#include <stdio.h>
#include <string.h>

wifi_ip6_class_t wifi_ip6_classify(uint8_t const a[16]) {
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) {
        return WIFI_IP6_CLASS_LINKLOCAL;
    }
    if ((a[0] & 0xfe) == 0xfc) {
        return WIFI_IP6_CLASS_ULA;
    }
    if ((a[0] & 0xe0) == 0x20) {
        return WIFI_IP6_CLASS_GLOBAL;
    }
    return WIFI_IP6_CLASS_OTHER;
}

void wifi_ip6_clear(wifi_ip6_set_t *s) {
    memset(s, 0, sizeof(*s));
}

bool wifi_ip6_add(wifi_ip6_set_t *s, uint8_t const a[16]) {
    if (wifi_ip6_classify(a) == WIFI_IP6_CLASS_OTHER || s->n >= WIFI_IP6_MAX) {
        return false;
    }
    for (unsigned i = 0; i < s->n; i++) {
        if (memcmp(s->addr[i], a, 16) == 0) {
            return false;
        }
    }
    memcpy(s->addr[s->n++], a, 16);
    return true;
}

wifi_ip6_level_t wifi_ip6_level(wifi_ip6_set_t const *s) {
    if (wifi_ip6_global_count(s) > 0) {
        return WIFI_IP6_GLOBAL;
    }
    return s->n > 0 ? WIFI_IP6_LINKLOCAL : WIFI_IP6_NONE;
}

unsigned wifi_ip6_global_count(wifi_ip6_set_t const *s) {
    unsigned n = 0;
    for (unsigned i = 0; i < s->n; i++) {
        n += wifi_ip6_classify(s->addr[i]) != WIFI_IP6_CLASS_LINKLOCAL;
    }
    return n;
}

bool wifi_ip6_best(wifi_ip6_set_t const *s, uint8_t out[16]) {
    int ula = -1;
    for (unsigned i = 0; i < s->n; i++) {
        wifi_ip6_class_t c = wifi_ip6_classify(s->addr[i]);
        if (c == WIFI_IP6_CLASS_GLOBAL) {
            memcpy(out, s->addr[i], 16);
            return true;
        }
        if (c == WIFI_IP6_CLASS_ULA && ula < 0) {
            ula = (int)i;
        }
    }
    if (ula < 0) {
        return false;
    }
    memcpy(out, s->addr[ula], 16);
    return true;
}

size_t wifi_ip6_text(uint8_t const a[16], char *out, size_t cap) {
    if (cap < WIFI_IP6_TEXT_MAX) {
        return 0;
    }
    unsigned g[8];
    for (int i = 0; i < 8; i++) {
        g[i] = (unsigned)(a[2 * i] << 8 | a[2 * i + 1]);
    }
    int best = -1, best_len = 0; // the longest zero run, the first on a tie, only when it is 2 groups or more
    for (int i = 0; i < 8;) {
        int j = i;
        while (j < 8 && g[j] == 0) {
            j++;
        }
        if (j - i > best_len && j - i >= 2) {
            best     = i;
            best_len = j - i;
        }
        i = j > i ? j : i + 1;
    }
    size_t n = 0;
    for (int i = 0; i < 8; i++) {
        if (i == best) {
            out[n++] = ':';
            i += best_len - 1; // the loop's own i++ steps past the run
            if (i == 7) {
                out[n++] = ':';
            }
            continue;
        }
        if (i > 0) {
            out[n++] = ':';
        }
        n += (size_t)snprintf(out + n, cap - n, "%x", g[i]);
    }
    out[n] = '\0';
    return n;
}

size_t wifi_ip6_prefix_text(uint8_t const a[16], char *out, size_t cap) {
    uint8_t p[16] = {0};
    memcpy(p, a, 8);
    if (cap < WIFI_IP6_TEXT_MAX + 3) {
        return 0;
    }
    size_t n = wifi_ip6_text(p, out, cap);
    memcpy(out + n, "/64", 4);
    return n + 3;
}
