###############################################################################
#
# TCA8418 Keypad driver
#
# SPDX-FileCopyrightText: 2024 Mike Dunston (atanisoft)
# SPDX-License-Identifier: BSD-2-Clause
#
###############################################################################

idf_component_register(SRCS "esp_tca8418.c" "tca8418_mask.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "esp_driver_gpio" "i2c_bus")
