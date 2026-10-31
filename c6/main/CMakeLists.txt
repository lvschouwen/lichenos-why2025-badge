# SPDX-FileCopyrightText: 2026 Lucas van Schouwen
# SPDX-License-Identifier: Apache-2.0
# lora_proto.c is shared with the P4 firmware and the host tests (#51)
idf_component_register(SRCS "app_main.c" "backlight_cmd.c" "lora_service.cpp" "lora_irq.c" "../../shared/lora/lora_proto.c"
                    INCLUDE_DIRS "." "../../shared/lora"
                    REQUIRES esp_hosted
                    PRIV_REQUIRES nvs_flash esp_event esp_driver_ledc esp_driver_uart esp_driver_gpio esp_timer)
