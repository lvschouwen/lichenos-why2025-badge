# The LittleFS drive at /storage (#300): storage.c mounts it, storage_console.c is "fw storage" (#299)
idf_component_register(SRCS "storage.c" "storage_console.c" "file_replace.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES joltwallet__littlefs esp_partition freertos fw_console)
