# Notifications (#62): implements the gate's lichen_notify.h (apps) and notify_os.h (the shell), which live in
# lichen_api since #245 (hence the peer line in tools/layers.conf, as for lichen_os); notify_bind.h is what lichen_os
# wires in at boot (the settings store, the status LED's state hooks). No LVGL here, so OS components below lichen_api
# may raise notices through a hook lichen_os gives them. Pure and host-tested: notify_ring.c (notify_ring_test.c: the
# history, coalescing, eviction, the file codec), notify_rate.c (notify_rate_test.c: the per-source token bucket and
# the toast/feedback decision), notify_mute.c (notify_mute_test.c: the per-source switch), notify_push.c
# (notify_push_test.c: the state and feedback pushes to the drivers below, #245). REQUIRES lichen_api and fw_console
# publicly: notify_ring.h, notify_push.h and notify_bind.h include the gate's notify headers and fw_store.h.
idf_component_register(SRCS "lichen_notify.c" "notify_ring.c" "notify_rate.c" "notify_mute.c" "notify_cmd.c" "notify_store.c"
                            "notify_push.c"
                       INCLUDE_DIRS "."
                       REQUIRES lichen_api fw_console
                       PRIV_REQUIRES freertos esp_timer heap storage log)
