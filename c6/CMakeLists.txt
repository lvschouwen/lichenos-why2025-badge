# ESP-Hosted 3.x co-processor firmware for the C6 on the WHY2025 badge.
# BadgeVMS flashes build/network_adapter.bin onto the C6 when Wi-Fi is selected.
cmake_minimum_required(VERSION 3.16)

# Reproducible image (#279): the version is a hash of everything the C6 image is built from, and the compile
# date is off (c6/sdkconfig.defaults), so identical inputs give a byte-identical network_adapter.bin. Hashed:
# c6/ sources and config, shared/ and dependencies.lock (pins esp_hosted and the other managed components).
file(GLOB_RECURSE c6_hash_files CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_LIST_DIR}/main/*" "${CMAKE_CURRENT_LIST_DIR}/../shared/*")
list(APPEND c6_hash_files "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
     "${CMAKE_CURRENT_LIST_DIR}/partitions.esp32c6.csv" "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock")
list(SORT c6_hash_files)
set(c6_hash_input "")
foreach(f IN LISTS c6_hash_files)
    file(SHA256 "${f}" h)
    string(APPEND c6_hash_input "${h}")
endforeach()
string(SHA256 c6_hash "${c6_hash_input}")
string(SUBSTRING "${c6_hash}" 0 12 c6_hash)
set(PROJECT_VER "c6-${c6_hash}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(network_adapter)

# Stack frame sizes (#202), see firmware/CMakeLists.txt; the C6 build only has main and the shared code
idf_component_get_property(main_lib main COMPONENT_LIB)
if(main_lib)
    target_compile_options(${main_lib} PRIVATE -fstack-usage)
endif()
