# sdapps/hello (#517, #522): the smallest native SD-card app. Everything shared is in ../sdapp.cmake.
cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../sdapp.cmake)
project(hello)
sdapp_elf(hello)
