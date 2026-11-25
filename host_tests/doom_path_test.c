// Host test: the DOOM picture's path choice and the own-back rule (#564)
#include "doom_path.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void choice(void) {
    doom_path_t p;
    doom_path_init(&p);
    CHECK(doom_path_get(&p) == DOOM_PATH_PENDING);
    CHECK(!doom_path_engine_draws(&p)); // loading: the engine only says its first frame came
    CHECK(!doom_path_reclaimed(&p, 1)); // a stale reclaim before the surface is held changes nothing
    doom_path_chosen(&p, false);        // refused: the canvas
    CHECK(doom_path_get(&p) == DOOM_PATH_CANVAS);
    CHECK(doom_path_engine_draws(&p));
    CHECK(!doom_path_reclaimed(&p, 1)); // the canvas never has a reclaim
    doom_path_chosen(&p, true);         // a second choice does not flip a settled path
    CHECK(doom_path_get(&p) == DOOM_PATH_CANVAS);
    doom_path_init(&p);
    doom_path_chosen(&p, true);
    CHECK(doom_path_get(&p) == DOOM_PATH_VIDEO);
    CHECK(doom_path_engine_draws(&p));
}

static void pause_and_resume(void) {
    doom_path_t p;
    doom_path_init(&p);
    doom_path_chosen(&p, true);
    CHECK(doom_path_reclaimed(&p, 1)); // a dialog
    CHECK(doom_path_get(&p) == DOOM_PATH_PAUSED);
    CHECK(!doom_path_engine_draws(&p));
    CHECK(!doom_path_reclaimed(&p, 2)); // a second reclaim while paused is ignored
    doom_path_chosen(&p, true);         // resume: acquired again
    CHECK(doom_path_get(&p) == DOOM_PATH_VIDEO);
    CHECK(doom_path_reclaimed(&p, 2));
    doom_path_chosen(&p, false); // resume refused: the canvas
    CHECK(doom_path_get(&p) == DOOM_PATH_CANVAS);
}

// The #563 review note: the OS's hold-Esc pops the app itself, so the app's own hold must not queue a second back
static void own_back(void) {
    doom_path_t p;
    doom_path_init(&p);
    CHECK(doom_path_own_back_ok(&p));  // loading: the app's hold is the way out
    doom_path_chosen(&p, true);
    CHECK(!doom_path_own_back_ok(&p)); // the OS's escape is armed while the surface is held
    CHECK(doom_path_reclaimed(&p, DOOM_RECLAIM_ESCAPE));
    CHECK(!doom_path_own_back_ok(&p)); // the OS's escape is popping: no second back on top
    doom_path_init(&p);
    doom_path_chosen(&p, true);
    CHECK(doom_path_reclaimed(&p, 1));
    CHECK(doom_path_own_back_ok(&p));  // a dialog took it: no OS escape is running, the app's hold leaves
    doom_path_init(&p);
    doom_path_chosen(&p, false);
    CHECK(doom_path_own_back_ok(&p));  // the canvas path has only the app's hold
}

int main(void) {
    choice();
    pause_and_resume();
    own_back();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("doom_path_test: ok\n");
    return 0;
}
