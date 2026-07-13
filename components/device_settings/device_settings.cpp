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

#include "device_settings.h"

#include "app_config.h"
#include "board_profile.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <cstdio>
#include <cstring>

namespace {

const char* TAG = "device_settings";
const char* kNamespace = "device_cfg";

constexpr char kDefaultDeviceName[] = "Kiri Bridge";
constexpr char kDefaultWifiSsid[] = "YOUR_WIFI_SSID";
constexpr char kDefaultWifiPassword[] = "YOUR_WIFI_PASSWORD";
constexpr char kDefaultHomeKitManufacturer[] = "dkt smart home";
constexpr char kDefaultHomeKitModel[] = "Kiri Bridge";
constexpr char kDefaultHomeKitSerial[] = "KIRI-BRIDGE";
constexpr char kDefaultHomeKitSetupId[] = "DKT1";
constexpr bool kDefaultHomeKitSeparateAirflowTile = true;
constexpr uint8_t kDefaultHomeKitAdvancedMapping = device_settings::kHomeKitMapDefault;
constexpr uint8_t kHomeKitAdvancedMappingSchemaVersion = 2;
constexpr uint32_t kDefaultAcCapabilities = device_settings::kAcCapabilityDefault;
constexpr bool kDefaultUseRealCn105 = true;
constexpr int kDefaultStatusLedPin = board_profile::kDefaultStatusLedPin;
constexpr int kDefaultCn105RxPin = board_profile::kDefaultCn105RxPin;
constexpr int kDefaultCn105TxPin = board_profile::kDefaultCn105TxPin;
constexpr int kDefaultCn105BaudRate = 2400;
constexpr int kDefaultCn105DataBits = 8;
constexpr char kDefaultCn105Parity = 'E';
constexpr int kDefaultCn105StopBits = 1;
constexpr bool kDefaultCn105RxPullupEnabled = true;
constexpr bool kDefaultCn105TxOpenDrain = false;
constexpr uint32_t kDefaultCn105PollIntervalActiveMs = 15000;
constexpr uint32_t kDefaultCn105PollIntervalOffMs = 60000;

device_settings::Settings settings{};
bool initialized = false;
char setup_code_canonical[12] = "";
char setup_code_display[10] = "";
char cn105_format_name[4] = "8E1";
char homekit_target_mode_name[8] = "0,1,2";
constexpr uint32_t kLegacyAcCapabilityUpDownAirflow = 1u << 3;
constexpr uint32_t kLegacyAcCapabilityLeftRightAirflow = 1u << 4;
constexpr uint32_t kLegacyAcCapabilityKnownMask =
    device_settings::kAcCapabilityHomeKitTargetMask |
    kLegacyAcCapabilityUpDownAirflow |
    kLegacyAcCapabilityLeftRightAirflow;
constexpr uint8_t kAcCapabilitiesSchemaVersion = 2;

const char* logLevelNameLocal(esp_log_level_t level) {
    switch (level) {
        case ESP_LOG_ERROR: return "error";
        case ESP_LOG_WARN: return "warn";
        case ESP_LOG_INFO: return "info";
        case ESP_LOG_DEBUG: return "debug";
        case ESP_LOG_VERBOSE: return "verbose";
        default: return "unknown";
    }
}

bool parseHomeKitTargetModeMaskLocal(const char* value, uint8_t* out) {
    if (value == nullptr || out == nullptr) {
        return false;
    }
    uint8_t mask = 0;
    bool saw_value = false;
    bool expect_value = true;
    for (size_t i = 0; value[i] != '\0'; ++i) {
        const char c = value[i];
        if (c == '0' || c == '1' || c == '2') {
            if (!expect_value) {
                return false;
            }
            const uint8_t bit = static_cast<uint8_t>(1u << (c - '0'));
            mask |= bit;
            saw_value = true;
            expect_value = false;
        } else if (c == ',' || c == ' ' || c == '\t') {
            if (!saw_value || expect_value) {
                return false;
            }
            expect_value = true;
        } else {
            return false;
        }
    }
    if (!saw_value || expect_value || (mask & ~device_settings::kHomeKitTargetAllMask) != 0) {
        return false;
    }
    *out = mask;
    return true;
}

void formatHomeKitTargetModeMask(uint8_t mask, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    char tmp[8] = "";
    size_t pos = 0;
    for (uint8_t value = 0; value <= 2; ++value) {
        if ((mask & (1u << value)) == 0) {
            continue;
        }
        if (pos > 0 && pos + 1 < sizeof(tmp)) {
            tmp[pos++] = ',';
        }
        if (pos + 1 < sizeof(tmp)) {
            tmp[pos++] = static_cast<char>('0' + value);
        }
    }
    tmp[pos] = '\0';
    std::snprintf(out, out_len, "%s", pos == 0 ? "0,1,2" : tmp);
}

void refreshHomeKitTargetModeName() {
    formatHomeKitTargetModeMask(device_settings::homeKitTargetModeMask(),
                                homekit_target_mode_name,
                                sizeof(homekit_target_mode_name));
}

void copyString(char* out, size_t out_len, const char* value) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    std::snprintf(out, out_len, "%s", value == nullptr ? "" : value);
}

void setMessage(char* out, size_t out_len, const char* value) {
    copyString(out, out_len, value);
}

void loadDefaults() {
    copyString(settings.deviceName, sizeof(settings.deviceName), kDefaultDeviceName);
    copyString(settings.wifiSsid, sizeof(settings.wifiSsid), kDefaultWifiSsid);
    copyString(settings.wifiPassword, sizeof(settings.wifiPassword), kDefaultWifiPassword);
    settings.homeKitCode[0] = '\0';
    copyString(settings.homeKitManufacturer, sizeof(settings.homeKitManufacturer), kDefaultHomeKitManufacturer);
    copyString(settings.homeKitModel, sizeof(settings.homeKitModel), kDefaultHomeKitModel);
    copyString(settings.homeKitSerial, sizeof(settings.homeKitSerial), kDefaultHomeKitSerial);
    copyString(settings.homeKitSetupId, sizeof(settings.homeKitSetupId), kDefaultHomeKitSetupId);
    settings.homeKitSeparateAirflowTile = kDefaultHomeKitSeparateAirflowTile;
    settings.homeKitAdvancedMapping = kDefaultHomeKitAdvancedMapping;
    settings.acCapabilities = kDefaultAcCapabilities;
    settings.useRealCn105 = kDefaultUseRealCn105;
    settings.statusLedPin = kDefaultStatusLedPin;
    settings.cn105RxPin = kDefaultCn105RxPin;
    settings.cn105TxPin = kDefaultCn105TxPin;
    settings.cn105DataBits = kDefaultCn105DataBits;
    settings.cn105Parity = kDefaultCn105Parity;
    settings.cn105StopBits = kDefaultCn105StopBits;
    settings.cn105RxPullupEnabled = kDefaultCn105RxPullupEnabled;
    settings.cn105TxOpenDrain = kDefaultCn105TxOpenDrain;
    settings.cn105BaudRate = kDefaultCn105BaudRate;
    settings.pollIntervalActiveMs = kDefaultCn105PollIntervalActiveMs;
    settings.pollIntervalOffMs = kDefaultCn105PollIntervalOffMs;
    settings.logLevel = app_config::kDefaultLogLevel;
}

esp_err_t ensureNvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

bool validBaud(int value) {
    return value == 2400 || value == 4800 || value == 9600;
}

bool validGpio(int value) {
    return board_profile::validGpio(value);
}

bool validOutputGpio(int value) {
    return board_profile::validOutputGpio(value);
}

bool validDataBits(int value) {
    return value == 8;
}

bool validParity(char value) {
    return value == 'N' || value == 'E' || value == 'O';
}

bool validStopBits(int value) {
    return value == 1 || value == 2;
}

bool validSetupId(const char* value) {
    if (value == nullptr || std::strlen(value) != 4) {
        return false;
    }
    for (size_t i = 0; i < 4; ++i) {
        const char c = value[i];
        const bool ok = (c >= '0' && c <= '9') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z');
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool validAcCapabilities(uint32_t value) {
    return (value & ~device_settings::kAcCapabilityKnownMask) == 0 &&
           (value & device_settings::kAcCapabilityTargetMask) != 0;
}

uint32_t migrateLegacyAcCapabilities(uint32_t value, bool* migrated) {
    if (migrated != nullptr) {
        *migrated = false;
    }
    const bool has_new_airflow_bits =
        (value & (device_settings::kAcCapabilityUpDownAirflow | device_settings::kAcCapabilityLeftRightAirflow)) != 0;
    if (has_new_airflow_bits || (value & ~kLegacyAcCapabilityKnownMask) != 0) {
        return value;
    }

    uint32_t next = value & device_settings::kAcCapabilityHomeKitTargetMask;
    next |= device_settings::kAcCapabilityTargetDry | device_settings::kAcCapabilityTargetFan;
    if ((value & kLegacyAcCapabilityUpDownAirflow) != 0) {
        next |= device_settings::kAcCapabilityUpDownAirflow;
    }
    if ((value & kLegacyAcCapabilityLeftRightAirflow) != 0) {
        next |= device_settings::kAcCapabilityLeftRightAirflow;
    }
    if (migrated != nullptr) {
        *migrated = next != value;
    }
    return next;
}

bool validHomeKitAdvancedMapping(uint8_t value) {
    return (value & ~device_settings::kHomeKitMapKnownMask) == 0;
}

bool sanitizeHomeKitCode(const char* value, char* out_digits, size_t out_len) {
    if (out_digits == nullptr || out_len < 9) {
        return false;
    }
    size_t count = 0;
    if (value != nullptr) {
        for (size_t i = 0; value[i] != '\0'; ++i) {
            if (value[i] >= '0' && value[i] <= '9') {
                if (count >= 8) {
                    return false;
                }
                out_digits[count++] = value[i];
            } else if (value[i] != '-' && value[i] != ' ' && value[i] != '\t') {
                return false;
            }
        }
    }
    if (count != 8) {
        return false;
    }
    out_digits[count] = '\0';
    return true;
}

void refreshCodeFormats() {
    if (std::strlen(settings.homeKitCode) != 8) {
        setup_code_canonical[0] = '\0';
        setup_code_display[0] = '\0';
        return;
    }

    std::snprintf(setup_code_canonical,
                  sizeof(setup_code_canonical),
                  "%c%c%c-%c%c-%c%c%c",
                  settings.homeKitCode[0],
                  settings.homeKitCode[1],
                  settings.homeKitCode[2],
                  settings.homeKitCode[3],
                  settings.homeKitCode[4],
                  settings.homeKitCode[5],
                  settings.homeKitCode[6],
                  settings.homeKitCode[7]);

    std::snprintf(setup_code_display,
                  sizeof(setup_code_display),
                  "%c%c%c%c-%c%c%c%c",
                  settings.homeKitCode[0],
                  settings.homeKitCode[1],
                  settings.homeKitCode[2],
                  settings.homeKitCode[3],
                  settings.homeKitCode[4],
                  settings.homeKitCode[5],
                  settings.homeKitCode[6],
                  settings.homeKitCode[7]);
}

void refreshCn105FormatName() {
    std::snprintf(cn105_format_name,
                  sizeof(cn105_format_name),
                  "%d%c%d",
                  settings.cn105DataBits,
                  settings.cn105Parity,
                  settings.cn105StopBits);
}

void generateRandomHomeKitCode(char* out_digits, size_t out_len) {
    if (out_digits == nullptr || out_len < 9) {
        return;
    }
    for (size_t i = 0; i < 8; ++i) {
        out_digits[i] = static_cast<char>('0' + (esp_random() % 10));
    }
    out_digits[8] = '\0';
}

bool validLogLevel(uint8_t value) {
    return value <= static_cast<uint8_t>(ESP_LOG_VERBOSE);
}

bool loadStringSetting(nvs_handle_t handle, const char* key, char* out, size_t out_len, bool* wrote_defaults) {
    size_t len = out_len;
    const esp_err_t err = nvs_get_str(handle, key, out, &len);
    if (err == ESP_OK) {
        return true;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_set_str(handle, key, out);
        if (wrote_defaults != nullptr) {
            *wrote_defaults = true;
        }
        return true;
    }
    ESP_LOGW(TAG, "Failed reading NVS string key %s: %s", key, esp_err_to_name(err));
    return false;
}

void loadBoolSetting(nvs_handle_t handle, const char* key, bool* value, bool* wrote_defaults) {
    uint8_t raw = *value ? 1 : 0;
    const esp_err_t err = nvs_get_u8(handle, key, &raw);
    if (err == ESP_OK) {
        *value = raw != 0;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_set_u8(handle, key, raw);
        if (wrote_defaults != nullptr) {
            *wrote_defaults = true;
        }
    }
}

void loadI32Setting(nvs_handle_t handle, const char* key, int* value, bool (*validator)(int), bool* wrote_defaults) {
    int32_t raw = *value;
    const esp_err_t err = nvs_get_i32(handle, key, &raw);
    if (err == ESP_OK) {
        if (validator == nullptr || validator(raw)) {
            *value = raw;
        } else {
            ESP_LOGW(TAG, "Invalid NVS value for %s: %ld; using default %d", key, static_cast<long>(raw), *value);
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_set_i32(handle, key, *value);
        if (wrote_defaults != nullptr) {
            *wrote_defaults = true;
        }
    }
}

void loadU32Setting(nvs_handle_t handle, const char* key, uint32_t* value, uint32_t min_value, bool* wrote_defaults) {
    uint32_t raw = *value;
    const esp_err_t err = nvs_get_u32(handle, key, &raw);
    if (err == ESP_OK) {
        if (raw >= min_value) {
            *value = raw;
        } else {
            ESP_LOGW(TAG, "Invalid NVS value for %s: %lu; using default %lu",
                     key,
                     static_cast<unsigned long>(raw),
                     static_cast<unsigned long>(*value));
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_set_u32(handle, key, *value);
        if (wrote_defaults != nullptr) {
            *wrote_defaults = true;
        }
    }
}

}  // namespace

namespace device_settings {

esp_err_t init() {
    if (initialized) {
        return ESP_OK;
    }

    loadDefaults();
    esp_err_t err = ensureNvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t handle = 0;
    err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    bool wrote_defaults = false;

    loadStringSetting(handle, "device_name", settings.deviceName, sizeof(settings.deviceName), &wrote_defaults);
    loadStringSetting(handle, "wifi_ssid", settings.wifiSsid, sizeof(settings.wifiSsid), &wrote_defaults);
    loadStringSetting(handle, "wifi_pass", settings.wifiPassword, sizeof(settings.wifiPassword), &wrote_defaults);
    loadStringSetting(handle, "hk_mfr", settings.homeKitManufacturer, sizeof(settings.homeKitManufacturer), &wrote_defaults);
    loadStringSetting(handle, "hk_model", settings.homeKitModel, sizeof(settings.homeKitModel), &wrote_defaults);
    loadStringSetting(handle, "hk_serial", settings.homeKitSerial, sizeof(settings.homeKitSerial), &wrote_defaults);
    loadBoolSetting(handle, "hk_airtile", &settings.homeKitSeparateAirflowTile, &wrote_defaults);

    uint8_t stored_mapping = settings.homeKitAdvancedMapping;
    uint8_t stored_mapping_version = 0;
    nvs_get_u8(handle, "hk_map_v", &stored_mapping_version);
    err = nvs_get_u8(handle, "hk_map", &stored_mapping);
    if (err == ESP_OK) {
        if (stored_mapping_version < kHomeKitAdvancedMappingSchemaVersion) {
            stored_mapping |= device_settings::kHomeKitMapDefault;
            if (!validHomeKitAdvancedMapping(stored_mapping)) {
                ESP_LOGW(TAG, "Invalid legacy HomeKit advanced mapping in NVS: 0x%02x; using default", stored_mapping);
                stored_mapping = kDefaultHomeKitAdvancedMapping;
            }
            settings.homeKitAdvancedMapping = stored_mapping;
            nvs_set_u8(handle, "hk_map", settings.homeKitAdvancedMapping);
            nvs_set_u8(handle, "hk_map_v", kHomeKitAdvancedMappingSchemaVersion);
            wrote_defaults = true;
            ESP_LOGI(TAG, "Enabled default Dry/Fan HomeKit mappings during hk_map migration");
        } else if (validHomeKitAdvancedMapping(stored_mapping)) {
            settings.homeKitAdvancedMapping = stored_mapping;
        } else {
            ESP_LOGW(TAG, "Invalid HomeKit advanced mapping in NVS: 0x%02x; using default", stored_mapping);
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_set_u8(handle, "hk_map", settings.homeKitAdvancedMapping);
        nvs_set_u8(handle, "hk_map_v", kHomeKitAdvancedMappingSchemaVersion);
        wrote_defaults = true;
    } else {
        ESP_LOGW(TAG, "Failed reading HomeKit advanced mapping: %s; using default", esp_err_to_name(err));
    }

    uint32_t stored_capabilities = settings.acCapabilities;
    uint8_t stored_capabilities_version = 0;
    nvs_get_u8(handle, "ac_caps_v", &stored_capabilities_version);
    err = nvs_get_u32(handle, "ac_caps", &stored_capabilities);
    if (err == ESP_OK) {
        bool migrated = false;
        if (stored_capabilities_version < kAcCapabilitiesSchemaVersion) {
            stored_capabilities = migrateLegacyAcCapabilities(stored_capabilities, &migrated);
        }
        if (validAcCapabilities(stored_capabilities)) {
            settings.acCapabilities = stored_capabilities;
            if (migrated || stored_capabilities_version < kAcCapabilitiesSchemaVersion) {
                nvs_set_u32(handle, "ac_caps", settings.acCapabilities);
                nvs_set_u8(handle, "ac_caps_v", kAcCapabilitiesSchemaVersion);
                wrote_defaults = true;
                ESP_LOGI(TAG, "Migrated legacy ac_caps into 0x%08lx",
                         static_cast<unsigned long>(settings.acCapabilities));
            }
        } else {
            ESP_LOGW(TAG, "Invalid AC capabilities in NVS: 0x%08lx; using default 0x%08lx",
                     static_cast<unsigned long>(stored_capabilities),
                     static_cast<unsigned long>(settings.acCapabilities));
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        char legacy_hvac[12] = {};
        size_t legacy_hvac_len = sizeof(legacy_hvac);
        if (nvs_get_str(handle, "hk_hvac", legacy_hvac, &legacy_hvac_len) == ESP_OK) {
            uint8_t target_mask = 0;
            if (parseHomeKitTargetModeMaskLocal(legacy_hvac, &target_mask)) {
                settings.acCapabilities = (settings.acCapabilities & ~device_settings::kAcCapabilityHomeKitTargetMask) | target_mask;
                ESP_LOGI(TAG, "Migrated legacy hk_hvac=%s into ac_caps=0x%08lx",
                         legacy_hvac,
                         static_cast<unsigned long>(settings.acCapabilities));
            } else {
                ESP_LOGW(TAG, "Invalid legacy hk_hvac in NVS: %s; using default 0,1,2", legacy_hvac);
            }
        }
        nvs_set_u32(handle, "ac_caps", settings.acCapabilities);
        nvs_set_u8(handle, "ac_caps_v", kAcCapabilitiesSchemaVersion);
        wrote_defaults = true;
    } else {
        ESP_LOGW(TAG, "Failed reading AC capabilities: %s; using default", esp_err_to_name(err));
    }
    err = nvs_erase_key(handle, "hk_hvac");
    if (err == ESP_OK) {
        wrote_defaults = true;
    }

    char stored_setup_id[sizeof(settings.homeKitSetupId)] = {};
    copyString(stored_setup_id, sizeof(stored_setup_id), settings.homeKitSetupId);
    loadStringSetting(handle, "hk_setupid", stored_setup_id, sizeof(stored_setup_id), &wrote_defaults);
    if (validSetupId(stored_setup_id)) {
        copyString(settings.homeKitSetupId, sizeof(settings.homeKitSetupId), stored_setup_id);
    } else {
        ESP_LOGW(TAG, "Invalid HomeKit setup id in NVS; using default %s", settings.homeKitSetupId);
    }

    char stored_homekit_code[sizeof(settings.homeKitCode)] = {};
    size_t len = sizeof(stored_homekit_code);
    err = nvs_get_str(handle, "hk_code", stored_homekit_code, &len);
    if (err == ESP_OK && sanitizeHomeKitCode(stored_homekit_code, settings.homeKitCode, sizeof(settings.homeKitCode))) {
        // Loaded existing code.
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        generateRandomHomeKitCode(settings.homeKitCode, sizeof(settings.homeKitCode));
        nvs_set_str(handle, "hk_code", settings.homeKitCode);
        wrote_defaults = true;
    } else {
        ESP_LOGW(TAG, "Invalid HomeKit setup code in NVS; generating a replacement");
        generateRandomHomeKitCode(settings.homeKitCode, sizeof(settings.homeKitCode));
        nvs_set_str(handle, "hk_code", settings.homeKitCode);
        wrote_defaults = true;
    }

    loadBoolSetting(handle, "use_real", &settings.useRealCn105, &wrote_defaults);
    loadI32Setting(handle, "led_pin", &settings.statusLedPin, validOutputGpio, &wrote_defaults);
    loadI32Setting(handle, "rx_pin", &settings.cn105RxPin, validGpio, &wrote_defaults);
    loadI32Setting(handle, "tx_pin", &settings.cn105TxPin, validOutputGpio, &wrote_defaults);
    loadI32Setting(handle, "baud", &settings.cn105BaudRate, validBaud, &wrote_defaults);
    loadI32Setting(handle, "data_bits", &settings.cn105DataBits, validDataBits, &wrote_defaults);
    loadI32Setting(handle, "stop_bits", &settings.cn105StopBits, validStopBits, &wrote_defaults);
    loadBoolSetting(handle, "rx_pull", &settings.cn105RxPullupEnabled, &wrote_defaults);
    loadBoolSetting(handle, "tx_od", &settings.cn105TxOpenDrain, &wrote_defaults);

    char parity[2] = {settings.cn105Parity, '\0'};
    loadStringSetting(handle, "parity", parity, sizeof(parity), &wrote_defaults);
    if (validParity(parity[0])) {
        settings.cn105Parity = parity[0];
    } else {
        ESP_LOGW(TAG, "Invalid CN105 parity in NVS: %s; using default %c", parity, settings.cn105Parity);
    }

    loadU32Setting(handle, "poll_on", &settings.pollIntervalActiveMs, 1000, &wrote_defaults);
    loadU32Setting(handle, "poll_off", &settings.pollIntervalOffMs, 5000, &wrote_defaults);

    uint8_t level = static_cast<uint8_t>(settings.logLevel);
    err = nvs_get_u8(handle, "log_level", &level);
    if (err == ESP_OK) {
        if (validLogLevel(level)) {
            settings.logLevel = static_cast<esp_log_level_t>(level);
        } else {
            ESP_LOGW(TAG, "Invalid log level in NVS: %u; using default %s",
                     static_cast<unsigned>(level),
                     logLevelNameLocal(settings.logLevel));
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_set_u8(handle, "log_level", static_cast<uint8_t>(settings.logLevel));
        wrote_defaults = true;
    }

    if (wrote_defaults) {
        nvs_commit(handle);
    }

    nvs_close(handle);
    refreshCodeFormats();
    refreshCn105FormatName();
    refreshHomeKitTargetModeName();
    initialized = true;
    ESP_LOGI(TAG,
             "Loaded settings: name=%s homekit_code=%s hk_airtile=%s hk_map=0x%02x ac_caps=0x%08lx hk_hvac=%s transport=%s ledPin=%d wifi=%s cn105=rx%d/tx%d/%d/%s/rxPull=%s/txOD=%s poll_on=%lu poll_off=%lu log=%s",
             settings.deviceName,
             setup_code_display,
             settings.homeKitSeparateAirflowTile ? "on" : "off",
             settings.homeKitAdvancedMapping,
             static_cast<unsigned long>(settings.acCapabilities),
             homekit_target_mode_name,
             settings.useRealCn105 ? "real" : "mock",
             settings.statusLedPin,
             settings.wifiSsid,
             settings.cn105RxPin,
             settings.cn105TxPin,
             settings.cn105BaudRate,
             cn105_format_name,
             settings.cn105RxPullupEnabled ? "on" : "off",
             settings.cn105TxOpenDrain ? "on" : "off",
             static_cast<unsigned long>(settings.pollIntervalActiveMs),
             static_cast<unsigned long>(settings.pollIntervalOffMs),
             logLevelNameLocal(settings.logLevel));
    return ESP_OK;
}

const Settings& get() {
    return settings;
}

const char* deviceName() {
    return settings.deviceName;
}

const char* wifiSsid() {
    return settings.wifiSsid;
}

const char* wifiPassword() {
    return settings.wifiPassword;
}

const char* homeKitCodeDigits() {
    return settings.homeKitCode;
}

const char* homeKitSetupCode() {
    return setup_code_canonical;
}

const char* homeKitDisplayCode() {
    return setup_code_display;
}

const char* homeKitManufacturer() {
    return settings.homeKitManufacturer;
}

const char* homeKitModel() {
    return settings.homeKitModel;
}

const char* homeKitSerial() {
    return settings.homeKitSerial;
}

const char* homeKitSetupId() {
    return settings.homeKitSetupId;
}

bool useRealCn105() {
    return settings.useRealCn105;
}

bool homeKitSeparateAirflowTile() {
    return settings.homeKitSeparateAirflowTile;
}

uint8_t homeKitAdvancedMapping() {
    return settings.homeKitAdvancedMapping;
}

bool homeKitMapsUpDownTilt() {
    return (settings.homeKitAdvancedMapping & kHomeKitMapUpDownTilt) != 0;
}

bool homeKitMapsLeftRightTilt() {
    return (settings.homeKitAdvancedMapping & kHomeKitMapLeftRightTilt) != 0;
}

bool homeKitMapsUpDownSwing() {
    return (settings.homeKitAdvancedMapping & kHomeKitMapUpDownSwing) != 0;
}

bool homeKitMapsLeftRightSwing() {
    return (settings.homeKitAdvancedMapping & kHomeKitMapLeftRightSwing) != 0;
}

bool homeKitMapsDryModeSwitch() {
    return (settings.homeKitAdvancedMapping & kHomeKitMapDryModeSwitch) != 0;
}

bool homeKitMapsFanModeSwitch() {
    return (settings.homeKitAdvancedMapping & kHomeKitMapFanModeSwitch) != 0;
}

uint32_t acCapabilities() {
    return settings.acCapabilities;
}

uint8_t homeKitTargetModeMask() {
    const uint8_t mask = static_cast<uint8_t>(settings.acCapabilities & kAcCapabilityHomeKitTargetMask);
    return mask != 0 ? mask : kHomeKitTargetCoolMask;
}

const char* homeKitTargetModeMaskName() {
    return homekit_target_mode_name;
}

bool supportsMode(const char* mode) {
    if (mode == nullptr) {
        return false;
    }
    if (std::strcmp(mode, "AUTO") == 0) {
        return (settings.acCapabilities & kAcCapabilityTargetAuto) != 0;
    }
    if (std::strcmp(mode, "HEAT") == 0) {
        return (settings.acCapabilities & kAcCapabilityTargetHeat) != 0;
    }
    if (std::strcmp(mode, "COOL") == 0) {
        return (settings.acCapabilities & kAcCapabilityTargetCool) != 0;
    }
    if (std::strcmp(mode, "DRY") == 0) {
        return (settings.acCapabilities & kAcCapabilityTargetDry) != 0;
    }
    if (std::strcmp(mode, "FAN") == 0) {
        return (settings.acCapabilities & kAcCapabilityTargetFan) != 0;
    }
    return false;
}

bool supportsUpDownAirflow() {
    return (settings.acCapabilities & kAcCapabilityUpDownAirflow) != 0;
}

bool supportsLeftRightAirflow() {
    return (settings.acCapabilities & kAcCapabilityLeftRightAirflow) != 0;
}

int statusLedPin() {
    return settings.statusLedPin;
}

int cn105RxPin() {
    return settings.cn105RxPin;
}

int cn105TxPin() {
    return settings.cn105TxPin;
}

int cn105DataBits() {
    return settings.cn105DataBits;
}

char cn105Parity() {
    return settings.cn105Parity;
}

int cn105StopBits() {
    return settings.cn105StopBits;
}

bool cn105RxPullupEnabled() {
    return settings.cn105RxPullupEnabled;
}

bool cn105TxOpenDrain() {
    return settings.cn105TxOpenDrain;
}

int cn105BaudRate() {
    return settings.cn105BaudRate;
}

uint32_t pollIntervalActiveMs() {
    return settings.pollIntervalActiveMs;
}

uint32_t pollIntervalOffMs() {
    return settings.pollIntervalOffMs;
}

esp_log_level_t logLevel() {
    return settings.logLevel;
}

const char* logLevelName() {
    return logLevelNameLocal(settings.logLevel);
}

const char* cn105FormatName() {
    return cn105_format_name;
}

bool parseLogLevel(const char* value, esp_log_level_t* out) {
    if (value == nullptr || out == nullptr) {
        return false;
    }
    if (std::strcmp(value, "error") == 0) {
        *out = ESP_LOG_ERROR;
        return true;
    }
    if (std::strcmp(value, "warn") == 0) {
        *out = ESP_LOG_WARN;
        return true;
    }
    if (std::strcmp(value, "info") == 0) {
        *out = ESP_LOG_INFO;
        return true;
    }
    if (std::strcmp(value, "debug") == 0) {
        *out = ESP_LOG_DEBUG;
        return true;
    }
    if (std::strcmp(value, "verbose") == 0) {
        *out = ESP_LOG_VERBOSE;
        return true;
    }
    return false;
}

bool parseCn105Parity(const char* value, char* out) {
    if (value == nullptr || out == nullptr) {
        return false;
    }
    char parity = value[0];
    if (parity >= 'a' && parity <= 'z') {
        parity = static_cast<char>(parity - 'a' + 'A');
    }
    if (!validParity(parity)) {
        return false;
    }
    *out = parity;
    return true;
}

bool parseHomeKitTargetModeMask(const char* value, uint8_t* out) {
    return parseHomeKitTargetModeMaskLocal(value, out);
}

bool save(const Settings& requested, bool* reboot_required, char* message, size_t message_len) {
    if (!initialized && init() != ESP_OK) {
        setMessage(message, message_len, "settings init failed");
        return false;
    }

    Settings next = requested;
    if (next.deviceName[0] == '\0') {
        copyString(next.deviceName, sizeof(next.deviceName), kDefaultDeviceName);
    }
    if (next.homeKitManufacturer[0] == '\0') {
        copyString(next.homeKitManufacturer, sizeof(next.homeKitManufacturer), kDefaultHomeKitManufacturer);
    }
    if (next.homeKitModel[0] == '\0') {
        copyString(next.homeKitModel, sizeof(next.homeKitModel), kDefaultHomeKitModel);
    }
    if (next.homeKitSerial[0] == '\0') {
        copyString(next.homeKitSerial, sizeof(next.homeKitSerial), kDefaultHomeKitSerial);
    }
    if (next.homeKitSetupId[0] == '\0') {
        copyString(next.homeKitSetupId, sizeof(next.homeKitSetupId), kDefaultHomeKitSetupId);
    }
    if (!validGpio(next.cn105RxPin) || !validOutputGpio(next.cn105TxPin) || !validOutputGpio(next.statusLedPin)) {
        setMessage(message, message_len, "invalid GPIO pin");
        return false;
    }
    char sanitized_code[sizeof(next.homeKitCode)] = {};
    if (!sanitizeHomeKitCode(next.homeKitCode, sanitized_code, sizeof(sanitized_code))) {
        setMessage(message, message_len, "invalid HomeKit setup code");
        return false;
    }
    copyString(next.homeKitCode, sizeof(next.homeKitCode), sanitized_code);
    if (!validBaud(next.cn105BaudRate)) {
        setMessage(message, message_len, "invalid CN105 baud rate");
        return false;
    }
    if (!validDataBits(next.cn105DataBits) || !validParity(next.cn105Parity) || !validStopBits(next.cn105StopBits)) {
        setMessage(message, message_len, "invalid CN105 serial format");
        return false;
    }
    if (!validSetupId(next.homeKitSetupId)) {
        setMessage(message, message_len, "invalid HomeKit setup id");
        return false;
    }
    if (next.pollIntervalActiveMs < 1000 || next.pollIntervalOffMs < 5000) {
        setMessage(message, message_len, "poll intervals are too small");
        return false;
    }
    if (!validAcCapabilities(next.acCapabilities)) {
        setMessage(message, message_len, "invalid AC capabilities");
        return false;
    }
    if (!validHomeKitAdvancedMapping(next.homeKitAdvancedMapping)) {
        setMessage(message, message_len, "invalid HomeKit advanced mapping");
        return false;
    }

    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        setMessage(message, message_len, "nvs_open failed");
        return false;
    }

    bool needs_reboot = false;
    needs_reboot = needs_reboot || std::strcmp(settings.deviceName, next.deviceName) != 0;
    needs_reboot = needs_reboot || std::strcmp(settings.wifiSsid, next.wifiSsid) != 0;
    needs_reboot = needs_reboot || std::strcmp(settings.wifiPassword, next.wifiPassword) != 0;
    needs_reboot = needs_reboot || std::strcmp(settings.homeKitCode, next.homeKitCode) != 0;
    needs_reboot = needs_reboot || std::strcmp(settings.homeKitManufacturer, next.homeKitManufacturer) != 0;
    needs_reboot = needs_reboot || std::strcmp(settings.homeKitModel, next.homeKitModel) != 0;
    needs_reboot = needs_reboot || std::strcmp(settings.homeKitSerial, next.homeKitSerial) != 0;
    needs_reboot = needs_reboot || std::strcmp(settings.homeKitSetupId, next.homeKitSetupId) != 0;
    needs_reboot = needs_reboot || settings.homeKitSeparateAirflowTile != next.homeKitSeparateAirflowTile;
    needs_reboot = needs_reboot || settings.homeKitAdvancedMapping != next.homeKitAdvancedMapping;
    needs_reboot = needs_reboot || (settings.acCapabilities & kAcCapabilityTargetMask) != (next.acCapabilities & kAcCapabilityTargetMask);
    needs_reboot = needs_reboot || settings.useRealCn105 != next.useRealCn105;
    needs_reboot = needs_reboot || settings.statusLedPin != next.statusLedPin;
    needs_reboot = needs_reboot || settings.cn105RxPin != next.cn105RxPin;
    needs_reboot = needs_reboot || settings.cn105TxPin != next.cn105TxPin;
    needs_reboot = needs_reboot || settings.cn105BaudRate != next.cn105BaudRate;
    needs_reboot = needs_reboot || settings.cn105DataBits != next.cn105DataBits;
    needs_reboot = needs_reboot || settings.cn105Parity != next.cn105Parity;
    needs_reboot = needs_reboot || settings.cn105StopBits != next.cn105StopBits;
    needs_reboot = needs_reboot || settings.cn105RxPullupEnabled != next.cn105RxPullupEnabled;
    needs_reboot = needs_reboot || settings.cn105TxOpenDrain != next.cn105TxOpenDrain;

    nvs_set_str(handle, "device_name", next.deviceName);
    nvs_set_str(handle, "wifi_ssid", next.wifiSsid);
    nvs_set_str(handle, "wifi_pass", next.wifiPassword);
    nvs_set_str(handle, "hk_code", next.homeKitCode);
    nvs_set_str(handle, "hk_mfr", next.homeKitManufacturer);
    nvs_set_str(handle, "hk_model", next.homeKitModel);
    nvs_set_str(handle, "hk_serial", next.homeKitSerial);
    nvs_set_str(handle, "hk_setupid", next.homeKitSetupId);
    nvs_set_u8(handle, "hk_airtile", next.homeKitSeparateAirflowTile ? 1 : 0);
    nvs_set_u8(handle, "hk_map", next.homeKitAdvancedMapping);
    nvs_set_u8(handle, "hk_map_v", kHomeKitAdvancedMappingSchemaVersion);
    nvs_set_u32(handle, "ac_caps", next.acCapabilities);
    nvs_set_u8(handle, "ac_caps_v", kAcCapabilitiesSchemaVersion);
    nvs_erase_key(handle, "hk_hvac");
    nvs_set_u8(handle, "use_real", next.useRealCn105 ? 1 : 0);
    nvs_set_i32(handle, "led_pin", next.statusLedPin);
    nvs_set_i32(handle, "rx_pin", next.cn105RxPin);
    nvs_set_i32(handle, "tx_pin", next.cn105TxPin);
    nvs_set_i32(handle, "baud", next.cn105BaudRate);
    nvs_set_i32(handle, "data_bits", next.cn105DataBits);
    char parity_value[2] = {next.cn105Parity, '\0'};
    nvs_set_str(handle, "parity", parity_value);
    nvs_set_i32(handle, "stop_bits", next.cn105StopBits);
    nvs_set_u8(handle, "rx_pull", next.cn105RxPullupEnabled ? 1 : 0);
    nvs_set_u8(handle, "tx_od", next.cn105TxOpenDrain ? 1 : 0);
    nvs_set_u32(handle, "poll_on", next.pollIntervalActiveMs);
    nvs_set_u32(handle, "poll_off", next.pollIntervalOffMs);
    nvs_set_u8(handle, "log_level", static_cast<uint8_t>(next.logLevel));
    const esp_err_t commit_err = nvs_commit(handle);
    nvs_close(handle);
    if (commit_err != ESP_OK) {
        setMessage(message, message_len, "settings commit failed");
        return false;
    }

    settings = next;
    refreshCodeFormats();
    refreshCn105FormatName();
    refreshHomeKitTargetModeName();
    if (reboot_required != nullptr) {
        *reboot_required = needs_reboot;
    }
    std::snprintf(message,
                  message_len,
                  "%s",
                  needs_reboot
                      ? "Settings saved. Reboot required for device name, HomeKit metadata, HomeKit display or mapping, HomeKit supported modes, HomeKit setup code, CN105 mode, or baud changes."
                      : "Settings saved.");
    return true;
}

}  // namespace device_settings
