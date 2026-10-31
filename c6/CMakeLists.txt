# ESP-Hosted 3.x co-processor firmware for the C6 on the WHY2025 badge.
# BadgeVMS flashes build/network_adapter.bin onto the C6 when Wi-Fi is selected.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(network_adapter)
