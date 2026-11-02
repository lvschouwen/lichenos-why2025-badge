# The OS owns the entropy source (#84 V2); the wraps refuse any other caller of bootloader_random_enable (T9) and
# bootloader_random_disable (#123)
idf_component_register(SRCS "lichen_random.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bootloader_support esp_hw_support)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=bootloader_random_enable"
                                                 "-Wl,--wrap=bootloader_random_disable")
