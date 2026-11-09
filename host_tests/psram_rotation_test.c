#include "psram_rotation.h"

#include <stdio.h>

static int fails;

#define EXPECT(cond)                                                            \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);               \
            fails++;                                                            \
        }                                                                       \
    } while (0)

int main(void) {
    bool none[4] = {false, false, false, false};
    EXPECT(psram_rot_next(none, 4, -1) == -1);
    EXPECT(psram_rot_next(none, 0, -1) == -1);

    bool gaps[7] = {true, false, true, true, false, false, true};
    EXPECT(psram_rot_next(gaps, 7, -1) == 0);
    EXPECT(psram_rot_next(gaps, 7, 0) == 2);
    EXPECT(psram_rot_next(gaps, 7, 2) == 3);
    EXPECT(psram_rot_next(gaps, 7, 3) == 6);
    EXPECT(psram_rot_next(gaps, 7, 6) == 0); // wraps

    // every used slot is visited exactly once per round, whatever the gaps
    int seen[7] = {0};
    int cur     = -1;
    for (int k = 0; k < 4; k++) {
        cur = psram_rot_next(gaps, 7, cur);
        seen[cur]++;
    }
    for (int i = 0; i < 7; i++) {
        EXPECT(seen[i] == (gaps[i] ? 1 : 0));
    }

    // a single used slot comes back every time; a slot freed mid-round is skipped
    bool one[3] = {false, true, false};
    EXPECT(psram_rot_next(one, 3, 1) == 1);
    gaps[3] = false;
    EXPECT(psram_rot_next(gaps, 7, 2) == 6);

    // cursor out of range (stale after a shrink) or negative other than -1 still lands on a used slot
    EXPECT(psram_rot_next(gaps, 7, 9) == 6);  // 3 was freed above, so 3..5 are skipped
    EXPECT(psram_rot_next(gaps, 7, 7) == 2);  // 8%7 = 1 is free
    EXPECT(psram_rot_next(gaps, 7, -2) == 6); // -1 mod 7 = 6
    EXPECT(psram_rot_next(gaps, 7, -9) == 6); // -8 mod 7 = 6

    // a slot added below the cursor mid-round is reached within one round, and any used slot within n steps
    bool grow[6] = {false, false, true, true, true, false};
    cur          = 4;
    grow[0]      = true;
    int steps    = 0;
    do {
        cur = psram_rot_next(grow, 6, cur);
        steps++;
    } while (cur != 0 && steps <= 6);
    EXPECT(cur == 0 && steps <= 6);
    for (int start = -1; start < 6; start++) {
        for (int target = 0; target < 6; target++) {
            if (!grow[target]) {
                continue;
            }
            int c = start;
            int n = 0;
            do {
                c = psram_rot_next(grow, 6, c);
                n++;
            } while (c != target && n <= 6);
            EXPECT(c == target && n <= 6);
        }
    }

    if (fails) {
        return 1;
    }
    printf("psram_rotation_test ok\n");
    return 0;
}
