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
                       REQUIRES board_profile esp_driver_gpio esp_driver_uart esp_event esp_http_server esp_netif esp_wifi led_strip nvs_flash ota_handler wifi_provisioning)

# Embed the bundled installer page (built upstream by webui esbuild) as
# rodata. The symbol pair _binary_installer_html_{start,end} is referenced
# from app_main.cpp via extern asm declarations.
set(INSTALLER_HTML "${CMAKE_CURRENT_LIST_DIR}/../../../webui/dist/installer.html")
if(NOT EXISTS "${INSTALLER_HTML}")
    message(FATAL_ERROR
        "Installer bundle missing: ${INSTALLER_HTML}\n"
        "Expected the parent CMakeLists.txt to have built it via npm run build:installer.")
endif()
target_add_binary_data(${COMPONENT_LIB} "${INSTALLER_HTML}" BINARY)
