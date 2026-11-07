// Host tests for notify_push.c (#245): the notification service tells the status LED (L2) do-not-disturb and "anything
// unread" through hooks, instead of the LED reading notify; and its feedback dispatch fires sound, vibrate and led as
// lichen_notify_feedback always did.
#include "notify_push.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// The calls in the order they came: 'D'/'d' dnd on/off, 'U'/'u' unread yes/no, 'S' sound, 'V' vibrate, 'L'/'l' led
static char calls[32];
static int  last_level = -1;

static void note(char c) {
    size_t n = strlen(calls);
    if (n + 1 < sizeof(calls)) {
        calls[n]     = c;
        calls[n + 1] = '\0';
    }
}

static void on_dnd(bool on) {
    note(on ? 'D' : 'd');
}
static void on_unread(bool any) {
    note(any ? 'U' : 'u');
}
static void on_sound(ln_level_t level) {
    last_level = (int)level;
    note('S');
}
static void on_vibrate(ln_level_t level) {
    last_level = (int)level;
    note('V');
}
static void on_led(ln_level_t level, bool on) {
    last_level = (int)level;
    note(on ? 'L' : 'l');
}

static void reset(void) {
    calls[0]   = '\0';
    last_level = -1;
}

static ln_state_hooks_t const both = {.dnd = on_dnd, .unread = on_unread};

static void bind_tells_current_state(void) {
    npush_t p = {0};
    reset();
    npush_dnd(&p, true); // before anyone listens: kept, told nobody
    npush_unread(&p, true);
    CHECK(strcmp(calls, "") == 0);
    npush_bind(&p, &both); // the LED bound late still starts right
    CHECK(strcmp(calls, "DU") == 0);
}

static void changes_pushed_in_order(void) {
    npush_t p = {0};
    npush_bind(&p, &both);
    reset();
    npush_unread(&p, true);
    npush_dnd(&p, true);
    npush_unread(&p, false);
    npush_dnd(&p, false);
    CHECK(strcmp(calls, "UDud") == 0);
    reset();
    npush_unread(&p, false); // unchanged still pushed: the hook only stores it
    CHECK(strcmp(calls, "u") == 0);
}

static void null_members_and_unbind(void) {
    npush_t                p    = {0};
    ln_state_hooks_t const only = {.unread = on_unread};
    reset();
    npush_bind(&p, &only);
    npush_dnd(&p, true);
    npush_unread(&p, true);
    CHECK(strcmp(calls, "uU") == 0); // the bind's unread, then the change; no dnd hook
    reset();
    npush_bind(&p, NULL);
    npush_dnd(&p, false);
    npush_unread(&p, false);
    CHECK(strcmp(calls, "") == 0);
    CHECK(!p.dnd && !p.unread); // still tracked for the next bind
    npush_bind(&p, &both);
    CHECK(strcmp(calls, "du") == 0);
}

static void feedback_dispatch(void) {
    ln_feedback_t const all = {.sound = on_sound, .vibrate = on_vibrate, .led = on_led};
    reset();
    npush_feedback(&all, LN_WARN, true);
    CHECK(strcmp(calls, "SVL") == 0);
    CHECK(last_level == LN_WARN);
    reset();
    npush_feedback(&all, LN_ERROR, false); // the toast goes: only the LED hears it
    CHECK(strcmp(calls, "l") == 0);
    CHECK(last_level == LN_ERROR);
    ln_feedback_t const vib = {.vibrate = on_vibrate}; // what lichen_os registers today
    reset();
    npush_feedback(&vib, LN_INFO, true);
    npush_feedback(&vib, LN_INFO, false);
    CHECK(strcmp(calls, "V") == 0);
    ln_feedback_t const none = {0};
    reset();
    npush_feedback(&none, LN_INFO, true);
    CHECK(strcmp(calls, "") == 0);
}

int main(void) {
    bind_tells_current_state();
    changes_pushed_in_order();
    null_members_and_unbind();
    feedback_dispatch();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("notify_push_test: all passed\n");
    return 0;
}
