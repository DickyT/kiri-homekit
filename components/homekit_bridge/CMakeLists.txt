# Kiri Bridge
# CN105 HomeKit controller for Mitsubishi heat pumps
# https://kiri.dkt.moe
# https://github.com/DickyT/kiri-homekit
#
# Copyright (c) 2026
# All Rights Reserved.
# Licensed under terms of the GPL-3.0 License.

idf_component_register(SRCS "homekit_bridge.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES qingping
                       REQUIRES app_config build_info cn105_transport core_cn105 device_settings esp_event esp_hap_apple_profiles esp_hap_core esp_timer log)
