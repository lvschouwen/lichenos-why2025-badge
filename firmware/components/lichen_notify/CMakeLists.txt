# Notifications (#62): lichen_notify.h is part of the app API (lichen_api REQUIRES this component); notify_os.h is the
# shell's side. No LVGL here, so OS components below lichen_api may raise notices too. Pure and host-tested:
# notify_ring.c (notify_ring_test.c: the history, coalescing, eviction, the file codec), notify_rate.c
# (notify_rate_test.c: the per-source token bucket and the toast/feedback decision), notify_mute.c (notify_mute_test.c: the per-source switch).
idf_component_register(SRCS "lichen_notify.c" "notify_ring.c" "notify_rate.c" "notify_mute.c" "notify_cmd.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES freertos esp_timer heap nvs_flash storage fw_console log)
