// Host test: PMP and PMA entry text for "fw pmp" (#608)
#include "pmp_fmt.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define EQ(got, want)                                                                                                  \
    do {                                                                                                               \
        if (strcmp((got), (want)) != 0) {                                                                              \
            printf("FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, (got), (want));                         \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    char out[96];

    // Unused entry, whatever its address
    pmp_entry_format(out, sizeof out, 7, 0x00, 0x1234, 0);
    EQ(out, "pmp7 off");

    // The v6.1 bootloader's entry 0 shape: NAPOT 0x2000_0000-0x2fff_ffff RW (#608 B4)
    pmp_entry_format(out, sizeof out, 0, 0x18 | 0x3, (0x20000000u >> 2) | ((0x10000000u >> 3) - 1), 0);
    EQ(out, "pmp0 - NAPOT rw- 0x20000000-0x2fffffff (256 MB)");

    // Locked 4 KB NAPOT, r-x
    pmp_entry_format(out, sizeof out, 4, 0x80 | 0x18 | 0x5, (0x4ff18000u >> 2) | ((0x1000u >> 3) - 1), 0);
    EQ(out, "pmp4 L NAPOT r-x 0x4ff18000-0x4ff18fff (4 KB)");

    // TOR uses the previous entry's address as its base
    pmp_entry_format(out, sizeof out, 11, 0x80 | 0x08 | 0x1, 0x4ff5b900u >> 2, 0x4ff5b800u >> 2);
    EQ(out, "pmp11 L TOR r-- 0x4ff5b800-0x4ff5b8ff (256 B)");

    // An empty TOR range says so
    pmp_entry_format(out, sizeof out, 5, 0x08 | 0x7, 0x100, 0x200);
    CHECK(strstr(out, "(empty)") != NULL);

    // NA4: four bytes
    pmp_entry_format(out, sizeof out, 2, 0x10 | 0x1, 0x40000000u >> 2, 0);
    EQ(out, "pmp2 - NA4 r-- 0x40000000-0x40000003 (4 B)");

    // All-ones NAPOT covers the whole 34-bit space
    pmp_entry_format(out, sizeof out, 15, 0x18 | 0x7, 0xffffffffu, 0);
    EQ(out, "pmp15 - NAPOT rwx 0x00000000-0x3ffffffff (16 GB)");

    // A locked OFF entry still shows its lock (it pins the entry and a TOR base)
    pmp_entry_format(out, sizeof out, 9, 0x80, 0, 0);
    EQ(out, "pmp9 L OFF ---");

    // PMA as the ROM leaves it on rev v1.0 (#608 runs 6 and 8): 0-14 off but non-cacheable, 15 NAPOT rwx over L2MEM/ROM
    pma_entry_format(out, sizeof out, 3, 0x08000000, 0, 0);
    EQ(out, "pma3 off nc");
    pma_entry_format(out, sizeof out, 15, 0xc000001d, 0x11ffffff, 0);
    EQ(out, "pma15 - NAPOT rwx 0x40000000-0x4fffffff (256 MB)");
    pma_entry_format(out, sizeof out, 1, 0x40000000 | (1u << 29) | (1u << 27) | (1u << 26) | 0x11, 0x50000000u >> 2,
                     0x40000000u >> 2);
    EQ(out, "pma1 L TOR r-- nc wt 0x40000000-0x4fffffff (256 MB)");

    // Never overruns a short buffer, and reports what it wrote
    size_t n = pmp_entry_format(out, 12, 0, 0x18 | 0x3, 0x0bffffff, 0);
    CHECK(strlen(out) == 11 && n == 11);
    n = pma_entry_format(out, 8, 15, 0xc000001d, 0x11ffffff, 0);
    CHECK(strlen(out) == 7 && n == 7);
    CHECK(pmp_entry_format(out, 0, 0, 0, 0, 0) == 0);

    if (failures) {
        printf("pmp_fmt_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("pmp_fmt_test: ok\n");
    return 0;
}
