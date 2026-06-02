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

#include "board_profile.h"
#include "driver/uart.h"
#include "esp_log.h"

namespace app_config {

inline constexpr char kDeviceName[] = "Kiri Bridge";
inline constexpr char kPhaseName[] = "ESP-IDF stable baseline";
inline constexpr const char* kBoardId = board_profile::kBoardId;
inline constexpr const char* kBoardName = board_profile::kBoardName;
inline constexpr const char* kIdfTarget = board_profile::kIdfTarget;

inline constexpr esp_log_level_t kDefaultLogLevel = ESP_LOG_ERROR;
inline constexpr uint32_t kHeartbeatIntervalMs = 5000;
inline constexpr uint16_t kWebServerPort = 8080;
inline constexpr uint32_t kWebServerStackBytes = 12288;
inline constexpr uint8_t kWebServerMaxOpenSockets = 3;
inline constexpr uint8_t kWebServerMaxUriHandlers = 32;
inline constexpr size_t kWebFileListMaxItems = 8;
inline constexpr size_t kStatusLedPixels = 1;
inline constexpr uint8_t kStatusLedBrightness = 24;
inline constexpr uint32_t kStatusLedUpdateIntervalMs = 100;
inline constexpr uint32_t kStatusLedBlinkPeriodMs = 400;
inline constexpr uint32_t kStatusLedCommHoldMs = 700;
inline constexpr size_t kStatusLedTaskStackBytes = 3072;

inline constexpr bool kHomeKitEnabled = true;
inline constexpr char kHomeKitHardwareRevision[] = "1.0";

inline constexpr char kSpiffsBasePath[] = "/spiffs";
inline constexpr char kSpiffsPartitionLabel[] = "spiffs";
inline constexpr int kSpiffsMaxOpenFiles = 8;

inline constexpr bool kPersistentLogEnabled = true;
inline constexpr size_t kPersistentLogMaxTotalBytes = 192 * 1024;
inline constexpr size_t kPersistentLogBufferBytes = 4096;
inline constexpr size_t kPersistentLogLineBytes = 256;
inline constexpr size_t kPersistentLogReadChunkBytes = 1024;
inline constexpr uint32_t kPersistentLogFlushIntervalMs = 30000;
inline constexpr size_t kPersistentLogPruneIntervalBytes = 4096;
inline constexpr char kPersistentLogTimezone[] = "PST8PDT,M3.2.0,M11.1.0";

inline constexpr bool kWifiDisablePowerSave = true;
inline constexpr uint32_t kWifiConnectTimeoutMs = 15000;
inline constexpr uint32_t kWifiReconnectIntervalMs = 10000;
inline constexpr int kProvisioningButtonPin = board_profile::kProvisioningButtonPin;
inline constexpr uint32_t kProvisioningButtonLongPressMs = 3000;
inline constexpr uint32_t kProvisioningSessionMs = 5 * 60 * 1000;
inline constexpr uint32_t kProvisioningLoopIntervalMs = 50;
inline constexpr uint32_t kProvisioningRebootDelayMs = 1500;
inline constexpr size_t kProvisioningTaskStackBytes = 4096;

inline constexpr uart_port_t kCn105UartPort = UART_NUM_1;
inline constexpr int kCn105RxBufferBytes = 256;
inline constexpr int kCn105TxBufferBytes = 256;
inline constexpr uint32_t kCn105ConnectRetryMs = 10000;
inline constexpr uint32_t kCn105ResponseTimeoutMs = 1000;
inline constexpr uint32_t kCn105RxByteTimeoutMs = 120;
inline constexpr size_t kCn105TransportStackBytes = 4096;

}  // namespace app_config
