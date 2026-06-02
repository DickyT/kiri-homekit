/****************************************************************************
 * Kiri Bridge
 * CN105 HomeKit controller for Mitsubishi heat pumps
 * https://kiri.dkt.moe
 * https://github.com/DickyT/kiri-homekit
 *
 * Copyright (c) 2026
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 ****************************************************************************/

#pragma once

#include "driver/gpio.h"
#include "sdkconfig.h"

namespace board_profile {

#if CONFIG_IDF_TARGET_ESP32S3
inline constexpr char kBoardId[] = "atoms3-lite";
inline constexpr char kBoardName[] = "M5Stack AtomS3 Lite";
inline constexpr char kIdfTarget[] = "esp32s3";
inline constexpr int kDefaultStatusLedPin = 35;
inline constexpr int kDefaultCn105RxPin = 2;
inline constexpr int kDefaultCn105TxPin = 1;
inline constexpr int kProvisioningButtonPin = 41;
#else
inline constexpr char kBoardId[] = "atom-lite";
inline constexpr char kBoardName[] = "M5Stack ATOM Lite";
inline constexpr char kIdfTarget[] = "esp32";
inline constexpr int kDefaultStatusLedPin = 27;
inline constexpr int kDefaultCn105RxPin = 26;
inline constexpr int kDefaultCn105TxPin = 32;
inline constexpr int kProvisioningButtonPin = 39;
#endif

inline bool validGpio(int value) {
    return value >= 0 && GPIO_IS_VALID_GPIO(value);
}

inline bool validOutputGpio(int value) {
    return value >= 0 && GPIO_IS_VALID_OUTPUT_GPIO(value);
}

}  // namespace board_profile
