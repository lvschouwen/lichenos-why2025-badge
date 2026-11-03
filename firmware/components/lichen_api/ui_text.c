// See ui_text.h (#135)
#include "ui_text.h"

#include <stdio.h>
#include <string.h>

#define BULLET " \xE2\x80\xA2 " // U+2022 between hint pairs: one separator everywhere (R6)

static size_t finish(char *out, size_t n, int w) {
    if (n == 0) {
        return 0;
    }
    if (w < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)w >= n ? n - 1 : (size_t)w;
}

static size_t append(char *out, size_t n, size_t len, char const *fmt, char const *a, char const *b) {
    if (len >= n - 1) {
        return len;
    }
    return len + finish(out + len, n - len, snprintf(out + len, n - len, fmt, a, b));
}

size_t ui_hint_vbuild(char *out, size_t n, va_list ap) {
    if (n == 0) {
        return 0;
    }
    out[0]     = '\0';
    size_t len = 0;
    for (char const *key = va_arg(ap, char const *); key; key = va_arg(ap, char const *)) {
        char const *verb = va_arg(ap, char const *);
        if (!verb) {
            break;
        }
        len = append(out, n, len, len ? BULLET "%s: %s" : "%s: %s", key, verb);
    }
    return len;
}

size_t ui_hint_build(char *out, size_t n, ...) {
    va_list ap;
    va_start(ap, n);
    size_t len = ui_hint_vbuild(out, n, ap);
    va_end(ap);
    return len;
}

int32_t ui_hint_pad(int32_t keep, int32_t line, int32_t edge, int32_t gap) {
    int32_t need = (line > 0 ? line : 0) + (edge > 0 ? edge : 0) + (gap > 0 ? gap : 0);
    return keep > need ? keep : need;
}

size_t ui_confirm_hint(char *out, size_t n, char const *confirm_label, char const *cancel_label) {
    return ui_hint_build(out, n, "Enter", confirm_label, "Esc", cancel_label ? cancel_label : "cancel", NULL);
}

size_t ui_countdown_text(char *out, size_t n, char const *title, char const *prefix, bool careful,
                         char const *confirm_label, uint32_t seconds) {
    if (n == 0) {
        return 0;
    }
    char const *paren = prefix ? strrchr(prefix, '(') : NULL;
    int         lead  = paren ? (int)(paren - prefix) : 0;
    char const *verb  = careful ? "cancels" : (confirm_label && confirm_label[0] ? confirm_label : "confirms");
    bool        has   = title && title[0];
    return finish(out, n,
                  snprintf(out, n, "%s%s%.*s(%s in %u s)", has ? title : "", has ? " " : "", lead, prefix ? prefix : "",
                           verb, (unsigned)seconds));
}

bool ui_status_begin(ui_status_t *s, char const *verb) {
    bool unfinished = s->state == UI_STATUS_RUNNING;
    s->state        = UI_STATUS_RUNNING;
    snprintf(s->verb, sizeof(s->verb), "%s", verb ? verb : "");
    return unfinished;
}

bool ui_status_end(ui_status_t *s, bool failed) {
    if (s->state != UI_STATUS_RUNNING) {
        return false;
    }
    s->state = failed ? UI_STATUS_FAILED : UI_STATUS_DONE;
    return true;
}

size_t ui_status_begin_text(char *out, size_t n, char const *verb) {
    return finish(out, n, n ? snprintf(out, n, "%s...", verb ? verb : "") : 0);
}

size_t ui_status_progress_text(char *out, size_t n, char const *verb, int done, int of) {
    if (done < 0) {
        done = 0;
    }
    if (of > 0 && done > of) {
        done = of;
    }
    char const *v = verb ? verb : "";
    if (of <= 0) {
        return finish(out, n, n ? snprintf(out, n, "%s %d", v, done) : 0);
    }
    return finish(out, n, n ? snprintf(out, n, "%s %d of %d", v, done, of) : 0);
}

size_t ui_status_fail_text(char *out, size_t n, char const *why, char const *next) {
    if (n == 0) {
        return 0;
    }
    char const *w     = why ? why : "";
    size_t      wl    = strlen(w);
    bool        stop  = wl && (w[wl - 1] == '.' || w[wl - 1] == '!' || w[wl - 1] == '?');
    bool        extra = next && next[0];
    return finish(out, n, snprintf(out, n, "%s%s%s%s", w, stop ? "" : ".", extra ? " " : "", extra ? next : ""));
}

size_t ui_empty_text(char *out, size_t n, char const *what, char const *how) {
    if (n == 0) {
        return 0;
    }
    char const *w    = what ? what : "items";
    bool        have = how && how[0];
    size_t      hl   = have ? strlen(how) : 0;
    bool        stop = have && (how[hl - 1] == '.' || how[hl - 1] == '!' || how[hl - 1] == '?');
    if (!have) {
        return finish(out, n, snprintf(out, n, "No %s yet.", w));
    }
    return finish(out, n, snprintf(out, n, "No %s yet: %s%s", w, how, stop ? "" : "."));
}
