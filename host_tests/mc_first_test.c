// Host test: MeshCore's first-open questions (#67)
#include "mc_first.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    mc_first_t s = {0};
    CHECK(mc_first_action(&s) == MC_FIRST_ASK_NAME); // first open
    s.name_asked = true;
    CHECK(mc_first_action(&s) == MC_FIRST_HOME);     // Esc on the name: home, nothing answered
    s.name_answered = true;
    s.name_busy     = true;
    CHECK(mc_first_action(&s) == MC_FIRST_WAIT);     // the name is being applied (the screen coming back is not Esc)
    s.name_busy = false;
    CHECK(mc_first_action(&s) == MC_FIRST_ASK_REGION);
    s.region_asked = true;
    CHECK(mc_first_action(&s) == MC_FIRST_HOME);     // Esc on the region
    s.region_answered = true;
    CHECK(mc_first_action(&s) == MC_FIRST_FINISH);   // only now is mc_name_set saved
    // A name that could not be applied is asked again instead of ending the flow
    mc_first_t retry = {.name_asked = false};
    CHECK(mc_first_action(&retry) == MC_FIRST_ASK_NAME);
    // Esc after the name was applied keeps the answer but never reaches FINISH
    mc_first_t esc = {.name_asked = true, .name_answered = true, .region_asked = true};
    CHECK(mc_first_action(&esc) != MC_FIRST_FINISH);
    // The real event order, not hand-set states
    {
        mc_first_t f = {0};
        CHECK(mc_first_action(&f) == MC_FIRST_ASK_NAME);
        mc_first_asked_name(&f);
        mc_first_name_submitted(&f);                     // Enter: text entry pops, done runs, the job starts
        CHECK(mc_first_action(&f) == MC_FIRST_WAIT);     // the screen's LOADED step comes after that
        mc_first_name_result(&f, true);
        CHECK(mc_first_action(&f) == MC_FIRST_ASK_REGION);
        mc_first_asked_region(&f);
        CHECK(mc_first_action(&f) == MC_FIRST_HOME);     // Esc on the list (checked before it is answered)
        mc_first_region_chosen(&f);
        CHECK(mc_first_action(&f) == MC_FIRST_FINISH);
    }
    {   // name job fails: asked again, not home
        mc_first_t f = {0};
        mc_first_asked_name(&f);
        mc_first_name_submitted(&f);
        mc_first_name_result(&f, false);
        CHECK(mc_first_action(&f) == MC_FIRST_ASK_NAME);
    }
    {   // empty name: never submitted, the done callback reports failure
        mc_first_t f = {0};
        mc_first_asked_name(&f);
        mc_first_name_result(&f, false);
        CHECK(mc_first_action(&f) == MC_FIRST_ASK_NAME);
    }
    {   // Esc on the name entry
        mc_first_t f = {0};
        mc_first_asked_name(&f);
        CHECK(mc_first_action(&f) == MC_FIRST_HOME);
    }
    if (failures) {
        return 1;
    }
    printf("mc_first_test: ok\n");
    return 0;
}
