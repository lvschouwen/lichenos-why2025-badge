#pragma once
// MeshCore's DataStore.cpp includes <nvs_flash.h> for its factory reset. The app no longer requires the IDF's nvs_flash
// component (#123): this stands in for the one call it makes. The mesh component builds DataStore.cpp with
// nvs_flash_erase renamed to mesh_nvs_flash_erase (mesh_task.cpp), which erases MeshCore's files only.
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_flash_erase(void);
#ifdef __cplusplus
}
#endif
