# The FAT drive at /storage: storage.c mounts it, storage_console.c is "fw storage" (#299)
idf_component_register(SRCS "storage.c" "storage_console.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES fatfs wear_levelling fw_console)
