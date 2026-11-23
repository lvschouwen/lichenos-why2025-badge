# sdapps/doom (#525): DOOM (doomgeneric, GPL) as an SD-card app. Everything shared is in ../sdapp.cmake.
cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../sdapp.cmake)
project(doom)
sdapp_elf(doom)
