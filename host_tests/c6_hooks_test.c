// Host tests for c6link's hook table (#223): c6link calls the layers above it only through these, and a hook that is
// not registered (yet) does a defined default instead of a NULL call
#include "c6_hooks_priv.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static int      begins, progress_last = -1, ends, attaches, no_links, downs, ups;
static bool     end_ok;
static char     end_note[48];
static uint32_t down_wait, up_wait;
static bool     down_answer, up_answer;

static void reset_counts(void) {
    begins = ends = attaches = no_links = downs = ups = 0;
    progress_last = -1;
    end_ok        = false;
    end_note[0]   = '\0';
    down_wait = up_wait = 0;
}

static void on_begin(void) {
    begins++;
}
static void on_progress(int percent) {
    progress_last = percent;
}
static void on_end(bool ok, char const *note) {
    ends++;
    end_ok = ok;
    snprintf(end_note, sizeof(end_note), "%s", note ? note : "(null)");
}
static void on_attached(void) {
    attaches++;
}
static void on_no_link(void) {
    no_links++;
}
static bool on_down(uint32_t wait_ms) {
    downs++;
    down_wait = wait_ms;
    return down_answer;
}
static bool on_up(uint32_t wait_ms) {
    ups++;
    up_wait = wait_ms;
    return up_answer;
}
static unsigned on_stack(void) {
    return 1234;
}
static bool trial_answer;
static bool on_trial(void) {
    return trial_answer;
}

// Nothing registered: every call is safe and gives its default
static void test_unset_defaults(void) {
    c6link_set_boot_hooks(NULL);
    c6link_set_net_hooks(NULL);
    reset_counts();
    c6_hooks_step_begin();
    c6_hooks_step_progress(50);
    c6_hooks_step_end(true, "note");
    CHECK(!c6_hooks_attached()); // nobody took the link: c6link logs "no Wi-Fi"
    c6_hooks_no_link();
    CHECK(c6_hooks_driver_down(20000)); // no driver to wait for: the re-attach goes on at once
    CHECK(c6_hooks_driver_up(20000));
    CHECK(c6_hooks_stack_free() == 0);
    CHECK(c6_hooks_image_on_trial()); // #285: nobody said the image is valid, so a reset could roll it back
    CHECK(begins == 0 && ends == 0 && attaches == 0 && no_links == 0 && downs == 0 && ups == 0);
}

// A table with members left NULL: those keep their defaults, the others are called with the arguments as given
static void test_partial_tables(void) {
    reset_counts();
    c6link_boot_hooks_t const boot = {.end = on_end};
    c6link_net_hooks_t const  net  = {.attached = on_attached};
    c6link_set_boot_hooks(&boot);
    c6link_set_net_hooks(&net);
    c6_hooks_step_begin();
    c6_hooks_step_progress(10);
    c6_hooks_step_end(false, "no ESP-Hosted link");
    CHECK(begins == 0 && progress_last == -1);
    CHECK(ends == 1 && !end_ok && strcmp(end_note, "no ESP-Hosted link") == 0);
    CHECK(c6_hooks_attached() && attaches == 1);
    c6_hooks_no_link();
    CHECK(no_links == 0);
    CHECK(c6_hooks_driver_down(5) && c6_hooks_driver_up(5) && downs == 0 && ups == 0);
    CHECK(c6_hooks_stack_free() == 0);
    CHECK(c6_hooks_image_on_trial());
}

static void test_full_tables(void) {
    reset_counts();
    c6link_boot_hooks_t const boot = {
        .begin = on_begin, .progress = on_progress, .end = on_end, .image_on_trial = on_trial};
    c6link_net_hooks_t const  net  = {
        .attached = on_attached, .no_link = on_no_link, .driver_down = on_down, .driver_up = on_up, .stack_free = on_stack};
    c6link_set_boot_hooks(&boot);
    c6link_set_net_hooks(&net);
    c6_hooks_step_begin();
    c6_hooks_step_progress(73);
    c6_hooks_step_end(true, "1200 ms");
    CHECK(begins == 1 && progress_last == 73 && ends == 1 && end_ok && strcmp(end_note, "1200 ms") == 0);
    c6_hooks_no_link();
    CHECK(no_links == 1);
    // The hook's own answer comes back: a driver that did not go down in time is reported, not hidden
    down_answer = false;
    up_answer   = true;
    CHECK(!c6_hooks_driver_down(20000) && downs == 1 && down_wait == 20000);
    CHECK(c6_hooks_driver_up(15000) && ups == 1 && up_wait == 15000);
    CHECK(c6_hooks_stack_free() == 1234);
    // ota's answer comes through either way (#285)
    trial_answer = false;
    CHECK(!c6_hooks_image_on_trial());
    trial_answer = true;
    CHECK(c6_hooks_image_on_trial());
}

// The table is copied: the caller's struct may go out of scope (main.c builds it on the stack), and NULL clears it
static void test_copied_and_cleared(void) {
    reset_counts();
    {
        c6link_net_hooks_t net = {.attached = on_attached, .stack_free = on_stack};
        c6link_set_net_hooks(&net);
        memset(&net, 0, sizeof(net));
    }
    CHECK(c6_hooks_attached() && attaches == 1);
    CHECK(c6_hooks_stack_free() == 1234);
    c6link_set_net_hooks(NULL);
    CHECK(!c6_hooks_attached() && attaches == 1);
    CHECK(c6_hooks_stack_free() == 0);
}

int main(void) {
    test_unset_defaults();
    test_partial_tables();
    test_full_tables();
    test_copied_and_cleared();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("c6_hooks_test: all passed\n");
    return 0;
}
