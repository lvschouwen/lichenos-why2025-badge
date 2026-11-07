# ESP-Hosted 3.x co-processor firmware for the C6 on the WHY2025 badge.
# BadgeVMS flashes build/network_adapter.bin onto the C6 when Wi-Fi is selected.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(network_adapter)

# Stack frame sizes (#202), see firmware/CMakeLists.txt; the C6 build only has main and the shared code
idf_component_get_property(main_lib main COMPONENT_LIB)
if(main_lib)
    target_compile_options(${main_lib} PRIVATE -fstack-usage)
endif()
