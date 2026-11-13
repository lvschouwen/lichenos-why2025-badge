# SPDX-FileCopyrightText: 2026 Lucas van Schouwen
# SPDX-License-Identifier: Apache-2.0
# lora_proto.c and reg.c are shared with the P4 firmware and the host tests (#51, #308)
idf_component_register(SRCS "app_main.c" "backlight.c" "backlight_cmd.c" "lora_service.cpp" "lora_irq.c"
                            "lora_readback.c" "lora_tune.c" "reg_c6.c" "../../shared/lora/lora_proto.c" "../../shared/reg/reg.c"
                    INCLUDE_DIRS "." "../../shared/lora" "../../shared/reg"
                    REQUIRES esp_hosted
                    PRIV_REQUIRES nvs_flash esp_event esp_driver_ledc esp_driver_uart esp_driver_gpio esp_timer esp_app_format
                                  bt esp_rom)
