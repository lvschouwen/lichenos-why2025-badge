#pragma once

#include "lichen_app.h"
#include "timer_calc.h"

// The Timer app's state (#152), kept apart from its screens so a countdown and a stopwatch go on when you leave them.
// It lives in RAM only: nothing is written to flash, and a restart clears it. LVGL task only (the screens, the poll
// timer and the tile all run there).

tcd_t   *timer_svc_countdown(void);
sw_t    *timer_svc_stopwatch(void);
int64_t  timer_svc_now(void); // the monotonic millisecond clock both run on

// Registers the poll that raises the notice when the countdown reaches zero, for the Timer app's ctx (its tile and its
// notices); once, from timer_app_register
void timer_svc_start(lichen_ctx_t ctx);
// The user has seen the finished timer (opened the app, pressed Reset or Start): clears the tile badge and marks the
// notice read
void timer_svc_ack(void);
