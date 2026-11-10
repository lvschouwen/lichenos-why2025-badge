#!/usr/bin/env python3
"""Host test of tools/check_rules.py (#287): each check fires on a fixture, stays quiet on the compliant twin, and the
baseline ratchet (new, worse, equal) works. Run with python3 -I."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import check_rules as cr  # noqa: E402


class Tree:
    def __init__(self, tc, files, baselines=None):
        self.tmp = tempfile.TemporaryDirectory()
        tc.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name
        for name, body in files.items():
            path = os.path.join(self.root, "firmware/components/x", name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(body)
        self.base = os.path.join(self.root, "tools")
        os.makedirs(self.base)
        for name, body in (baselines or {}).items():
            with open(os.path.join(self.base, name), "w") as f:
                f.write(body)

    def keys(self, check, allow=()):
        files = list(cr.tree_files(self.root))
        fn = {"cs-psram": cr.check_cs_psram, "task-class": cr.check_task_class, "lvgl-task": cr.check_lvgl_task,
              "log-level": lambda f: cr.check_log_level(f, list(allow))}[check]
        return sorted(cr.key_str(self.root, k) for k in fn(files))

    def run(self, check):
        return cr.run(self.root, self.base, [check], quiet=True)


CS_BAD = """\
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static char EXT_RAM_BSS_ATTR big[4096];
static char small[8];
void bad(void) {
    portENTER_CRITICAL(&mux);
    memcpy(small, big, 8);
    portEXIT_CRITICAL(&mux);
}
void bad_alloc(void) {
    portENTER_CRITICAL(&mux);
    char *p = malloc(4);
    portEXIT_CRITICAL(&mux);
}
void bad_local(void) {
    char *buf = lichen_alloc(NULL, 100, LICHEN_MEM_BULK);
    taskENTER_CRITICAL(&mux);
    buf[0] = 1;
    taskEXIT_CRITICAL(&mux);
}
"""
CS_OK = """\
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static char EXT_RAM_BSS_ATTR big[4096];
static char small[8];
void good(void) {
    char copy[8];
    memcpy(copy, big, 8); // PSRAM outside the section
    portENTER_CRITICAL(&mux);
    memcpy(small, copy, 8);
    portEXIT_CRITICAL(&mux);
}
void other(void) {
    char *buf = lichen_alloc(NULL, 100, LICHEN_MEM_BULK);
    char *p = 0;
    (void)buf;
    portENTER_CRITICAL(&mux);
    p = small; // a different function's local called buf is not PSRAM here
    portEXIT_CRITICAL(&mux);
}
"""


class CsPsram(unittest.TestCase):
    def test_fires(self):
        keys = Tree(self, {"a.c": CS_BAD}).keys("cs-psram")
        self.assertEqual(keys, ["firmware/components/x/a.c:bad:psram:big",
                                "firmware/components/x/a.c:bad_alloc:malloc",
                                "firmware/components/x/a.c:bad_local:psram:buf"])

    def test_quiet(self):
        self.assertEqual(Tree(self, {"a.c": CS_OK}).keys("cs-psram"), [])

    def test_comment_and_string_ignored(self):
        src = 'void f(void){ portENTER_CRITICAL(&m); /* malloc(1) */ log("free(x)"); portEXIT_CRITICAL(&m); }\n'
        self.assertEqual(Tree(self, {"a.c": src}).keys("cs-psram"), [])


LOG_BAD = 'void f(void) {\n    ESP_LOGE(TAG, "send failed, will retry (%d)", n);\n}\n' \
          'void g(void) { ESP_LOGE(TAG, "a, b"\n "dropped %s", x); }\n'
LOG_OK = 'void f(void) { ESP_LOGW(TAG, "will retry"); ESP_LOGE(TAG, "mount failed"); /* ESP_LOGE(TAG, "again") */ }\n'


class LogLevel(unittest.TestCase):
    def test_fires_including_comma_in_text_and_split_literal(self):
        keys = Tree(self, {"a.c": LOG_BAD}).keys("log-level")
        self.assertEqual(keys, ["firmware/components/x/a.c:f:retry", "firmware/components/x/a.c:g:dropped"])

    def test_quiet(self):
        self.assertEqual(Tree(self, {"a.c": LOG_OK}).keys("log-level"), [])

    def test_allow_list(self):
        keys = Tree(self, {"a.c": LOG_BAD}).keys("log-level", allow=[("x/a.c", "will retry")])
        self.assertEqual(keys, ["firmware/components/x/a.c:g:dropped"])


TASK_SRC = """\
static void ret_task(void *a) { for (;;) { if (a) return; } }
static void leak_task(void *a) { for (;;) { if (a) break; } }
static void inner_ok_task(void *a) { for (;;) { switch ((int)(intptr_t)a) { case 1: break; } } }
static void falls_task(void *a) { while (!stop) { work(); } }
static void flash_task(void *a) { for (;;) { helper(); } }
static void helper(void) { nvs_set_i32(h, "k", 1); }
static void noret_task(void *a) { vTaskDelay(1); lichen_sys_power_off_now(); }
static void fine_task(void *a) { for (;;) { vTaskDelay(1); } }
void start(void) {
    lichen_task_start_in(NULL, "a", ret_task, NULL, 4096, P, LICHEN_STACK_LP_RAM, NULL);
    lichen_task_start_in(NULL, "b", leak_task, NULL, 4096, P, LICHEN_STACK_PSRAM, NULL);
    lichen_task_start_in(NULL, "c", inner_ok_task, NULL, 4096, P, LICHEN_STACK_LP_RAM, NULL);
    lichen_task_start_in(NULL, "d", falls_task, NULL, 4096, P, LICHEN_STACK_LP_RAM, NULL);
    lichen_task_start_in(NULL, "e", flash_task, NULL, 4096, P, LICHEN_STACK_PSRAM, NULL);
    lichen_task_start_in(NULL, "f", flash_task, NULL, 4096, P, LICHEN_STACK_LP_RAM, NULL);
    lichen_task_start_in(NULL, "g", fine_task, NULL, 4096, P, LICHEN_STACK_PSRAM, NULL);
    lichen_task_start_in(NULL, "h", missing_task, NULL, 4096, P, LICHEN_STACK_PSRAM, NULL);
    lichen_task_start_in(NULL, "i", ret_task, NULL, 4096, P, LICHEN_STACK_INTERNAL, NULL);
    lichen_task_start_in(NULL, "j", ret_task, NULL, 4096, P, some_variable, NULL);
}
"""


class TaskClass(unittest.TestCase):
    def test_all_rules(self):
        keys = Tree(self, {"a.c": TASK_SRC}).keys("task-class")
        p = "firmware/components/x/a.c:"
        self.assertEqual(keys, [p + "falls_task:falls-off-the-end", p + "flash_task:flash:nvs_set_i32",
                                p + "leak_task:breaks-out-of-loop", p + "missing_task:task-function-not-in-file",
                                p + "ret_task:class-not-a-literal", p + "ret_task:returns"])

    def test_noreturn_call_and_endless_loop_pass(self):
        src = TASK_SRC.split("void start")[0] + "void start(void) {\n" \
            "    lichen_task_start_in(NULL, \"n\", noret_task, NULL, 1, P, LICHEN_STACK_LP_RAM, NULL);\n" \
            "    lichen_task_start_in(NULL, \"f\", fine_task, NULL, 1, P, LICHEN_STACK_PSRAM, NULL);\n}\n"
        self.assertEqual(Tree(self, {"a.c": src}).keys("task-class"), [])


LV_SRC = """\
static void bad_task(void *a) { lv_label_set_text(l, "x"); }
static void locked_task(void *a) {
    if (lichen_sys_ui_lock(0)) { lv_obj_clean(o); lichen_sys_ui_unlock(); }
    lv_obj_del(o);
}
static void cb(void *a) { lv_obj_clean(a); }
static void ui_task(void *a) { lichen_ui_call(cb, a); }
static void raw_task(void *a) { lv_timer_handler(); }
void start(void) {
    lichen_task_start(NULL, "a", bad_task, NULL, 1, P, NULL);
    lichen_task_start(NULL, "b", locked_task, NULL, 1, P, NULL);
    lichen_task_start(NULL, "c", ui_task, NULL, 1, P, NULL);
    xTaskCreate(raw_task, "r", 1, NULL, 1, NULL);
}
"""


class LvglTask(unittest.TestCase):
    def test_fires_outside_the_lock_only(self):
        p = "firmware/components/x/a.c:"
        self.assertEqual(Tree(self, {"a.c": LV_SRC}).keys("lvgl-task"),
                         [p + "bad_task:lv_label_set_text", p + "locked_task:lv_obj_del",
                          p + "raw_task:lv_timer_handler"])


class Ratchet(unittest.TestCase):
    KEY = "firmware/components/x/a.c:f:retry"

    def test_new_fails(self):
        self.assertEqual(Tree(self, {"a.c": LOG_BAD}).run("log-level"), 2)

    def test_equal_baseline_passes_and_worse_fails(self):
        base = {"rules-baseline-log-level.txt": self.KEY + "\t1\nfirmware/components/x/a.c:g:dropped\t1\n"}
        self.assertEqual(Tree(self, {"a.c": LOG_BAD}, base).run("log-level"), 0)
        worse = LOG_BAD + 'void f2(void) {}\n'.replace("f2", "f") + 'void k(void){}\n'
        base = {"rules-baseline-log-level.txt": self.KEY + "\t1\n"}
        self.assertEqual(Tree(self, {"a.c": LOG_BAD}, base).run("log-level"), 1)  # g is new
        self.assertEqual(Tree(self, {"a.c": worse}, base).run("log-level"), 1)

    def test_shrunk_baseline_passes(self):
        base = {"rules-baseline-log-level.txt": self.KEY + "\t3\n"}
        self.assertEqual(Tree(self, {"a.c": LOG_OK}, base).run("log-level"), 0)


if __name__ == "__main__":
    unittest.main()
