# Kiri Bridge
# CN105 HomeKit controller for Mitsubishi heat pumps
# https://kiri.dkt.moe
# https://github.com/DickyT/kiri-homekit
#
# Copyright (c) 2026
# All Rights Reserved.
# Licensed under terms of the GPL-3.0 License.

idf_component_register(SRCS "app_main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES app_config build_info core_cn105 cn105_transport device_settings homekit_bridge lua_vm platform_fs platform_led platform_log platform_provisioning platform_uart platform_wifi web
                       PRIV_REQUIRES nvs_flash spi_flash)
