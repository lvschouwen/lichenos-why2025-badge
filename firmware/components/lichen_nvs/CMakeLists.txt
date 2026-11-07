# NVS encryption with the HMAC key scheme (#116), built in but off: LICHEN_NVS_ENCRYPT (default n). nvs_image.c,
# nvs_boot_policy.c and nvs_mark.c are pure and host-tested (host_tests/nvs_image_test.c, nvs_boot_policy_test.c,
# nvs_mark_test.c); lichen_nvs.c does the flash, eFuse and NVS calls. docs/nvs-encryption.md.
# IDF's own CONFIG_NVS_ENCRYPTION stays off: its nvs_flash_init() generates and burns the HMAC key when the eFuse block
# is empty, which this component never does.
idf_component_register(SRCS "nvs_image.c" "nvs_boot_policy.c" "nvs_mark.c" "lichen_nvs.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_partition efuse esp_security esp_hw_support fw_console lichen_random log
                                     app_update esp_app_format bootloader_support)
# The image mark (nvs_mark.h) is referenced by nothing: without this --gc-sections drops it from the image
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lichen_nvs_mark")
