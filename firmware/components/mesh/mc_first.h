#pragma once

#include <stdbool.h>

// MeshCore's first-open questions (#67): name, then region. Pure, host-tested in host_tests/mc_first_test.c;
// meshcore_app.c drives the screens. The flow is asked again on every open until both were answered.
typedef struct {
    bool name_asked, name_answered, name_busy; // busy: the answered name is being applied
    bool region_asked, region_answered;
} mc_first_t;

typedef enum { MC_FIRST_WAIT, MC_FIRST_ASK_NAME, MC_FIRST_ASK_REGION, MC_FIRST_HOME, MC_FIRST_FINISH } mc_first_action_t;

// What to do when the MeshCore screen is on top again (it was just opened, a question closed, or a change finished).
// A question that was asked and is not answered means Esc: back home, nothing saved for it.
mc_first_action_t mc_first_action(mc_first_t const *s);

// The events, in the order the screens produce them
void mc_first_asked_name(mc_first_t *s);              // the name entry was opened
void mc_first_name_submitted(mc_first_t *s);          // Enter: the name is answered and being applied
void mc_first_name_result(mc_first_t *s, bool ok);    // applied (ok) or not (empty, busy): not ok asks again
void mc_first_asked_region(mc_first_t *s);            // the region list was opened
void mc_first_region_chosen(mc_first_t *s);           // a preset or Custom radio was applied
