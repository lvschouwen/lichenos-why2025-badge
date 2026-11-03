# NVS encryption with the HMAC key scheme (#116), built in but off: LICHEN_NVS_ENCRYPT (default n). nvs_image.c is pure
# and host-tested (host_tests/nvs_image_test.c); lichen_nvs.c does the flash, eFuse and NVS calls. docs/nvs-encryption.md.
# IDF's own CONFIG_NVS_ENCRYPTION stays off: its nvs_flash_init() generates and burns the HMAC key when the eFuse block
# is empty, which this component never does.
idf_component_register(SRCS "nvs_image.c" "lichen_nvs.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_partition efuse esp_security esp_hw_support fw_console lichen_random log)
