#include "dialog_countdown.h"

void dlg_countdown_start(dlg_countdown_t *d, uint32_t now_ms, uint32_t seconds) {
    d->start_ms = now_ms;
    d->total_ms = seconds * 1000u;
    d->decided  = false;
    d->careful  = false;
    d->enter_up = false;
}

void dlg_countdown_enter_up(dlg_countdown_t *d) {
    d->enter_up = true;
}

void dlg_countdown_start_careful(dlg_countdown_t *d, uint32_t now_ms, uint32_t seconds) {
    dlg_countdown_start(d, now_ms, seconds);
    d->careful = true;
}

static dlg_result_t decide(dlg_countdown_t *d, dlg_result_t r) {
    if (d->decided || r == DLG_WAIT) {
        return DLG_WAIT;
    }
    d->decided = true;
    return r;
}

dlg_result_t dlg_countdown_key(dlg_countdown_t *d, dlg_key_t key, uint32_t now_ms) {
    switch (key) {
    case DLG_KEY_ENTER:
        if (d->careful) {
            bool fresh  = d->enter_up && now_ms - d->start_ms >= DLG_CAREFUL_ARM_MS;
            d->enter_up = false;
            if (!fresh) {
                return DLG_WAIT;
            }
        }
        return decide(d, DLG_CONFIRM);
    case DLG_KEY_ESC:
        return decide(d, DLG_CANCEL);
    default:
        return DLG_WAIT;
    }
}

dlg_result_t dlg_countdown_tick(dlg_countdown_t *d, uint32_t now_ms) {
    return now_ms - d->start_ms >= d->total_ms ? decide(d, d->careful ? DLG_CANCEL : DLG_CONFIRM) : DLG_WAIT;
}

uint32_t dlg_countdown_left_s(dlg_countdown_t const *d, uint32_t now_ms) {
    uint32_t gone = now_ms - d->start_ms;
    if (gone >= d->total_ms) {
        return 0;
    }
    return (d->total_ms - gone + 999u) / 1000u;
}
