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

if(NOT DEFINED KIRI_PROJECT_NAME)
    set(KIRI_PROJECT_NAME kiri_bridge)
endif()

set(HOMEKIT_PATH ${CMAKE_CURRENT_LIST_DIR}/external/esp-homekit-sdk)
set(EXTRA_COMPONENT_DIRS
    ${HOMEKIT_PATH}/components
    ${HOMEKIT_PATH}/components/homekit)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${IDF_TARGET})
project(${KIRI_PROJECT_NAME})
