# Kiri Bridge
# CN105 HomeKit controller for Mitsubishi heat pumps
# https://kiri.dkt.moe
# https://github.com/DickyT/kiri-homekit
#
# Copyright (c) 2026
# All Rights Reserved.
# Licensed under terms of the GPL-3.0 License.

cmake_minimum_required(VERSION 3.16)

include(${CMAKE_CURRENT_LIST_DIR}/build_version.cmake OPTIONAL)

# Stage 1: bundle the installer WebUI (esbuild + terser) into webui/dist/installer.html.
# The main CMakeLists in components/web embeds it; here we just rebuild the bundle
# at configure time so installer changes pick it up. The component-level CMakeLists
# in main/ then declares the binary embed via target_add_binary_data.
set(WEBUI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../webui")
file(GLOB_RECURSE INSTALLER_WEBUI_SOURCES
    "${WEBUI_DIR}/src/installer.tsx"
    "${WEBUI_DIR}/src/styles.css")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${INSTALLER_WEBUI_SOURCES}
    "${WEBUI_DIR}/tools/bundle.mjs"
    "${WEBUI_DIR}/package.json")

if(NOT EXISTS "${WEBUI_DIR}/node_modules/.package-lock.json")
    message(FATAL_ERROR
        "WebUI dependencies are not installed.\n"
        "Run:  cd webui && npm install\n"
        "Then re-run idf.py build.")
endif()

execute_process(
    COMMAND npm run build:installer --silent
    WORKING_DIRECTORY "${WEBUI_DIR}"
    RESULT_VARIABLE WEBUI_INSTALLER_RESULT
    OUTPUT_VARIABLE WEBUI_INSTALLER_STDOUT
    ERROR_VARIABLE WEBUI_INSTALLER_STDERR)
if(NOT WEBUI_INSTALLER_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Installer WebUI bundle failed (exit ${WEBUI_INSTALLER_RESULT}).\n"
        "stdout:\n${WEBUI_INSTALLER_STDOUT}\n"
        "stderr:\n${WEBUI_INSTALLER_STDERR}")
endif()
message(STATUS "Installer bundle:\n${WEBUI_INSTALLER_STDOUT}")

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../../components/board_profile
    ${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__led_strip
    ${CMAKE_CURRENT_LIST_DIR}/../../components/ota_handler)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${IDF_TARGET})
project(kiri_installer)
