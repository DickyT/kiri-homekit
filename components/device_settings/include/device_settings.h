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
#include "esp_err.h"
#include "esp_log.h"

#include <cstdint>

namespace device_settings {

inline constexpr uint32_t kAcCapabilityTargetAuto = 1u << 0;
inline constexpr uint32_t kAcCapabilityTargetHeat = 1u << 1;
inline constexpr uint32_t kAcCapabilityTargetCool = 1u << 2;
inline constexpr uint32_t kAcCapabilityUpDownAirflow = 1u << 3;
inline constexpr uint32_t kAcCapabilityLeftRightAirflow = 1u << 4;
inline constexpr uint32_t kAcCapabilityTargetMask =
    kAcCapabilityTargetAuto | kAcCapabilityTargetHeat | kAcCapabilityTargetCool;
inline constexpr uint32_t kAcCapabilityAirflowMask =
    kAcCapabilityUpDownAirflow | kAcCapabilityLeftRightAirflow;
inline constexpr uint32_t kAcCapabilityKnownMask = kAcCapabilityTargetMask | kAcCapabilityAirflowMask;
inline constexpr uint32_t kAcCapabilityDefault = kAcCapabilityKnownMask;

inline constexpr uint8_t kHomeKitTargetAutoMask = static_cast<uint8_t>(kAcCapabilityTargetAuto);
inline constexpr uint8_t kHomeKitTargetHeatMask = static_cast<uint8_t>(kAcCapabilityTargetHeat);
inline constexpr uint8_t kHomeKitTargetCoolMask = static_cast<uint8_t>(kAcCapabilityTargetCool);
inline constexpr uint8_t kHomeKitTargetAllMask = static_cast<uint8_t>(kAcCapabilityTargetMask);

struct Settings {
    char deviceName[64] = "";
    char wifiSsid[33] = "";
    char wifiPassword[65] = "";
    char homeKitCode[9] = "";
    char homeKitManufacturer[64] = "";
    char homeKitModel[64] = "";
    char homeKitSerial[64] = "";
    char homeKitSetupId[5] = "";
    bool homeKitSeparateAirflowTile = true;
    uint32_t acCapabilities = kAcCapabilityDefault;
    bool useRealCn105 = true;
    int statusLedPin = board_profile::kDefaultStatusLedPin;
    int cn105RxPin = board_profile::kDefaultCn105RxPin;
    int cn105TxPin = board_profile::kDefaultCn105TxPin;
    int cn105DataBits = 8;
    char cn105Parity = 'E';
    int cn105StopBits = 1;
    bool cn105RxPullupEnabled = true;
    bool cn105TxOpenDrain = false;
    int cn105BaudRate = 2400;
    uint32_t pollIntervalActiveMs = 15000;
    uint32_t pollIntervalOffMs = 60000;
    esp_log_level_t logLevel = ESP_LOG_ERROR;
};

esp_err_t init();
const Settings& get();

const char* deviceName();
const char* wifiSsid();
const char* wifiPassword();
const char* homeKitCodeDigits();
const char* homeKitSetupCode();
const char* homeKitDisplayCode();
const char* homeKitManufacturer();
const char* homeKitModel();
const char* homeKitSerial();
const char* homeKitSetupId();
bool homeKitSeparateAirflowTile();
uint32_t acCapabilities();
uint8_t homeKitTargetModeMask();
const char* homeKitTargetModeMaskName();
bool supportsUpDownAirflow();
bool supportsLeftRightAirflow();
bool useRealCn105();
int statusLedPin();
int cn105RxPin();
int cn105TxPin();
int cn105DataBits();
char cn105Parity();
int cn105StopBits();
bool cn105RxPullupEnabled();
bool cn105TxOpenDrain();
int cn105BaudRate();
uint32_t pollIntervalActiveMs();
uint32_t pollIntervalOffMs();
esp_log_level_t logLevel();
const char* logLevelName();
const char* cn105FormatName();

bool save(const Settings& requested, bool* reboot_required, char* message, size_t message_len);
bool parseLogLevel(const char* value, esp_log_level_t* out);
bool parseCn105Parity(const char* value, char* out);
bool parseHomeKitTargetModeMask(const char* value, uint8_t* out);

}  // namespace device_settings
