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

#include "homekit_bridge.h"

#include "app_config.h"
#include "build_info.h"
#include "cn105_core.h"
#include "cn105_transport.h"
#include "device_settings.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"

extern "C" {
#include "hap.h"
#include "hap_apple_chars.h"
#include "hap_apple_servs.h"
}

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const char* TAG = "homekit_bridge";

constexpr uint8_t kInactive = 0;
constexpr uint8_t kActive = 1;
constexpr uint8_t kCurrentInactive = 0;
constexpr uint8_t kCurrentIdle = 1;
constexpr uint8_t kCurrentHeating = 2;
constexpr uint8_t kCurrentCooling = 3;
constexpr uint8_t kTargetAuto = 0;
constexpr uint8_t kTargetHeat = 1;
constexpr uint8_t kTargetCool = 2;
constexpr uint8_t kSwingDisabled = 0;
constexpr uint8_t kSwingEnabled = 1;
constexpr uint8_t kPositionStopped = 2;
constexpr uint8_t kDisplayFahrenheit = 1;
constexpr float kMinTargetCelsius = 10.0f;
constexpr float kMaxTargetCelsius = 31.0f;
constexpr float kTargetStepCelsius = 0.5f;
char kConfiguredNameUuid[] = "E3";

bool started = false;
int64_t last_command_us = 0;
constexpr int64_t kGracePeriodUs = 3 * 1000 * 1000;
hap_acc_t* accessory = nullptr;
hap_serv_t* heater_cooler = nullptr;
hap_serv_t* airflow_fan = nullptr;
hap_serv_t* up_down_tilt_service = nullptr;
hap_serv_t* left_right_tilt_service = nullptr;
hap_serv_t* up_down_swing_service = nullptr;
hap_serv_t* left_right_swing_service = nullptr;
hap_serv_t* dry_mode_service = nullptr;
hap_serv_t* fan_mode_service = nullptr;
hap_char_t* active_char = nullptr;
hap_char_t* current_temp_char = nullptr;
hap_char_t* current_state_char = nullptr;
hap_char_t* target_state_char = nullptr;
hap_char_t* cooling_threshold_char = nullptr;
hap_char_t* heating_threshold_char = nullptr;
hap_char_t* rotation_speed_char = nullptr;
hap_char_t* swing_mode_char = nullptr;
hap_char_t* temp_units_char = nullptr;
hap_char_t* fan_active_char = nullptr;
hap_char_t* fan_rotation_speed_char = nullptr;
hap_char_t* fan_swing_mode_char = nullptr;
hap_char_t* up_down_current_position_char = nullptr;
hap_char_t* up_down_target_position_char = nullptr;
hap_char_t* left_right_current_position_char = nullptr;
hap_char_t* left_right_target_position_char = nullptr;
hap_char_t* up_down_swing_on_char = nullptr;
hap_char_t* left_right_swing_on_char = nullptr;
hap_char_t* dry_mode_on_char = nullptr;
hap_char_t* fan_mode_on_char = nullptr;
char setup_payload[128] = "";
char airflow_service_name[96] = "";
char up_down_tilt_service_name[96] = "";
char left_right_tilt_service_name[96] = "";
char up_down_swing_service_name[96] = "";
char left_right_swing_service_name[96] = "";
char dry_mode_service_name[96] = "";
char fan_mode_service_name[96] = "";
char last_event[40] = "not-started";
char last_error[96] = "";

enum class SpecialMode : uint8_t {
    kNone,
    kDry,
    kFan,
};

SpecialMode requested_special_mode = SpecialMode::kNone;
char last_standard_mode[8] = "COOL";
char restore_mode[8] = "COOL";
bool restore_power_on = true;
bool restore_context_valid = false;
char last_fixed_vane[16] = "AUTO";
char last_fixed_wide_vane[24] = "|";
bool last_fixed_vane_valid = false;
bool last_fixed_wide_vane_valid = false;

char* hapString(const char* value) {
    return const_cast<char*>(value);
}

hap_char_t* hapCharConfiguredNameCreate(const char* name) {
    return hap_char_string_create(kConfiguredNameUuid,
                                  HAP_CHAR_PERM_PR,
                                  hapString(name));
}

esp_err_t addServiceNames(hap_serv_t* service, const char* name) {
    if (service == nullptr || name == nullptr || name[0] == '\0') {
        return ESP_FAIL;
    }
    int ret = hap_serv_add_char(service, hap_char_name_create(hapString(name)));
    ret |= hap_serv_add_char(service, hapCharConfiguredNameCreate(name));
    return ret == HAP_SUCCESS ? ESP_OK : ESP_FAIL;
}

bool equals(const char* left, const char* right) {
    return left != nullptr && right != nullptr && std::strcmp(left, right) == 0;
}

void rememberFixedVane(const char* vane) {
    if (vane == nullptr || vane[0] == '\0' || equals(vane, "SWING")) {
        return;
    }
    std::snprintf(last_fixed_vane, sizeof(last_fixed_vane), "%s", vane);
    last_fixed_vane_valid = true;
}

void rememberFixedWideVane(const char* wide_vane) {
    if (wide_vane == nullptr || wide_vane[0] == '\0' || equals(wide_vane, "SWING")) {
        return;
    }
    std::snprintf(last_fixed_wide_vane, sizeof(last_fixed_wide_vane), "%s", wide_vane);
    last_fixed_wide_vane_valid = true;
}

void observeFixedAirflowState(const cn105_core::MockState& state) {
    rememberFixedVane(state.vane);
    rememberFixedWideVane(state.wideVane);
}

const char* fixedVaneOrDefault() {
    return last_fixed_vane_valid ? last_fixed_vane : "AUTO";
}

const char* fixedWideVaneOrDefault() {
    return last_fixed_wide_vane_valid ? last_fixed_wide_vane : "|";
}

bool isStandardMode(const char* mode) {
    return equals(mode, "AUTO") || equals(mode, "HEAT") || equals(mode, "COOL");
}

SpecialMode specialModeFromName(const char* mode) {
    if (equals(mode, "DRY")) {
        return SpecialMode::kDry;
    }
    if (equals(mode, "FAN")) {
        return SpecialMode::kFan;
    }
    return SpecialMode::kNone;
}

const char* specialModeName(SpecialMode mode) {
    switch (mode) {
        case SpecialMode::kDry: return "DRY";
        case SpecialMode::kFan: return "FAN";
        case SpecialMode::kNone: return nullptr;
    }
    return nullptr;
}

SpecialMode activeSpecialModeFromMock(const cn105_core::MockState& state) {
    return equals(state.power, "ON") ? specialModeFromName(state.mode) : SpecialMode::kNone;
}

cn105_core::HalfDegreesC clampTargetHalfC(cn105_core::HalfDegreesC half_c) {
    if (half_c < cn105_core::kMinTargetHalfC) {
        return cn105_core::kMinTargetHalfC;
    }
    if (half_c > cn105_core::kMaxTargetHalfC) {
        return cn105_core::kMaxTargetHalfC;
    }
    return half_c;
}

uint8_t physicalActiveFromMock(const cn105_core::MockState& state) {
    return equals(state.power, "ON") ? kActive : kInactive;
}

uint8_t heaterCoolerActiveFromMock(const cn105_core::MockState& state) {
    return physicalActiveFromMock(state) == kActive && activeSpecialModeFromMock(state) == SpecialMode::kNone
               ? kActive
               : kInactive;
}

uint8_t targetBit(uint8_t target_state) {
    switch (target_state) {
        case kTargetAuto: return device_settings::kHomeKitTargetAutoMask;
        case kTargetHeat: return device_settings::kHomeKitTargetHeatMask;
        case kTargetCool: return device_settings::kHomeKitTargetCoolMask;
        default: return 0;
    }
}

bool targetStateAllowed(uint8_t target_state) {
    return (device_settings::homeKitTargetModeMask() & targetBit(target_state)) != 0;
}

uint8_t fallbackTargetState(uint8_t mask) {
    if ((mask & device_settings::kHomeKitTargetCoolMask) != 0) {
        return kTargetCool;
    }
    if ((mask & device_settings::kHomeKitTargetHeatMask) != 0) {
        return kTargetHeat;
    }
    return kTargetAuto;
}

uint8_t normalizeTargetState(uint8_t target_state) {
    if (targetStateAllowed(target_state)) {
        return target_state;
    }
    const uint8_t mask = device_settings::homeKitTargetModeMask();
    return fallbackTargetState(mask);
}

uint8_t targetStateFromMock(const cn105_core::MockState& state) {
    if (equals(state.mode, "HEAT")) {
        return normalizeTargetState(kTargetHeat);
    }
    if (equals(state.mode, "COOL")) {
        return normalizeTargetState(kTargetCool);
    }
    return normalizeTargetState(kTargetAuto);
}

uint8_t currentStateFromMock(const cn105_core::MockState& state) {
    if (heaterCoolerActiveFromMock(state) == kInactive) {
        return kCurrentInactive;
    }
    if (!state.operating) {
        return kCurrentIdle;
    }
    const uint8_t mask = device_settings::homeKitTargetModeMask();
    const bool can_report_heating = (mask & (device_settings::kHomeKitTargetAutoMask | device_settings::kHomeKitTargetHeatMask)) != 0;
    const bool can_report_cooling = (mask & (device_settings::kHomeKitTargetAutoMask | device_settings::kHomeKitTargetCoolMask)) != 0;
    if (equals(state.mode, "HEAT")) {
        return can_report_heating ? kCurrentHeating : kCurrentIdle;
    }
    if (equals(state.mode, "COOL") || equals(state.mode, "DRY")) {
        return can_report_cooling ? kCurrentCooling : kCurrentIdle;
    }
    if (equals(state.mode, "AUTO")) {
        if (state.roomTemperatureHalfC < state.targetTemperatureHalfC) {
            return can_report_heating ? kCurrentHeating : kCurrentIdle;
        }
        return can_report_cooling ? kCurrentCooling : kCurrentIdle;
    }
    return kCurrentIdle;
}

const char* modeFromTargetState(uint8_t target_state) {
    switch (normalizeTargetState(target_state)) {
        case kTargetHeat:
            return "HEAT";
        case kTargetCool:
            return "COOL";
        default:
            return "AUTO";
    }
}

const char* fallbackStandardMode() {
    return modeFromTargetState(fallbackTargetState(device_settings::homeKitTargetModeMask()));
}

void copyMode(char* out, size_t out_len, const char* mode) {
    std::snprintf(out, out_len, "%s", mode == nullptr ? "" : mode);
}

void rememberStandardMode(const char* mode) {
    if (isStandardMode(mode)) {
        copyMode(last_standard_mode, sizeof(last_standard_mode), mode);
    }
}

void ensureRestoreContext(const cn105_core::MockState& state) {
    if (restore_context_valid) {
        return;
    }
    restore_power_on = equals(state.power, "ON");
    const char* previous_mode = isStandardMode(state.mode) ? state.mode : last_standard_mode;
    if (!isStandardMode(previous_mode) || !device_settings::supportsMode(previous_mode)) {
        previous_mode = fallbackStandardMode();
    }
    copyMode(restore_mode, sizeof(restore_mode), previous_mode);
    restore_context_valid = true;
}

void clearRestoreContext() {
    restore_context_valid = false;
    restore_power_on = true;
    copyMode(restore_mode, sizeof(restore_mode), last_standard_mode);
}

void observeConfirmedModeState(const cn105_core::MockState& state) {
    const SpecialMode actual = activeSpecialModeFromMock(state);
    if (actual != SpecialMode::kNone) {
        ensureRestoreContext(state);
        requested_special_mode = actual;
        return;
    }

    if (isStandardMode(state.mode)) {
        rememberStandardMode(state.mode);
    }
    requested_special_mode = SpecialMode::kNone;
    clearRestoreContext();
}

void applyHvacModeValidValues() {
    if (target_state_char == nullptr || current_state_char == nullptr) {
        return;
    }
    const uint8_t mask = device_settings::homeKitTargetModeMask();
    uint8_t target_values[3] = {};
    uint8_t target_count = 0;
    for (uint8_t value = kTargetAuto; value <= kTargetCool; ++value) {
        if ((mask & targetBit(value)) != 0) {
            target_values[target_count++] = value;
        }
    }
    if (target_count > 0) {
        hap_char_add_valid_vals(target_state_char, target_values, target_count);
        hap_char_int_set_constraints(target_state_char, target_values[0], target_values[target_count - 1], 1);
    }

    uint8_t current_values[4] = {kCurrentInactive, kCurrentIdle};
    uint8_t current_count = 2;
    if ((mask & (device_settings::kHomeKitTargetAutoMask | device_settings::kHomeKitTargetHeatMask)) != 0) {
        current_values[current_count++] = kCurrentHeating;
    }
    if ((mask & (device_settings::kHomeKitTargetAutoMask | device_settings::kHomeKitTargetCoolMask)) != 0) {
        current_values[current_count++] = kCurrentCooling;
    }
    hap_char_add_valid_vals(current_state_char, current_values, current_count);
}

float fanToPercent(const char* fan) {
    if (fan == nullptr) {
        return 0.0f;
    }
    if (equals(fan, "QUIET")) {
        return 14.0f;
    }
    if (equals(fan, "1")) {
        return 28.0f;
    }
    if (equals(fan, "2")) {
        return 42.0f;
    }
    if (equals(fan, "3")) {
        return 71.0f;
    }
    if (equals(fan, "4")) {
        return 100.0f;
    }
    return 0.0f;
}

const char* percentToFan(float percent) {
    if (percent <= 0.0f) {
        return "AUTO";
    }
    if (percent <= 20.0f) {
        return "QUIET";
    }
    if (percent <= 35.0f) {
        return "1";
    }
    if (percent <= 55.0f) {
        return "2";
    }
    if (percent <= 80.0f) {
        return "3";
    }
    return "4";
}

bool legacySwingControlsUpDown() {
    if (!device_settings::homeKitSeparateAirflowTile()) {
        return device_settings::supportsUpDownAirflow();
    }
    return !(device_settings::supportsUpDownAirflow() && device_settings::homeKitMapsUpDownSwing());
}

bool legacySwingControlsLeftRight() {
    if (!device_settings::homeKitSeparateAirflowTile()) {
        return device_settings::supportsLeftRightAirflow();
    }
    return false;
}

bool mapsUpDownTilt() {
    return device_settings::homeKitSeparateAirflowTile() &&
           device_settings::supportsUpDownAirflow() &&
           device_settings::homeKitMapsUpDownTilt();
}

bool mapsLeftRightTilt() {
    return device_settings::homeKitSeparateAirflowTile() &&
           device_settings::supportsLeftRightAirflow() &&
           device_settings::homeKitMapsLeftRightTilt();
}

bool mapsUpDownSwing() {
    return device_settings::homeKitSeparateAirflowTile() &&
           device_settings::supportsUpDownAirflow() &&
           device_settings::homeKitMapsUpDownSwing();
}

bool mapsLeftRightSwing() {
    return device_settings::homeKitSeparateAirflowTile() &&
           device_settings::supportsLeftRightAirflow() &&
           device_settings::homeKitMapsLeftRightSwing();
}

bool mapsDryModeSwitch() {
    return device_settings::homeKitSeparateAirflowTile() &&
           device_settings::supportsMode("DRY") &&
           device_settings::homeKitMapsDryModeSwitch();
}

bool mapsFanModeSwitch() {
    return device_settings::homeKitSeparateAirflowTile() &&
           device_settings::supportsMode("FAN") &&
           device_settings::homeKitMapsFanModeSwitch();
}

bool hideAirflowFanSwing() {
    return mapsUpDownSwing();
}

uint8_t swingFromMock(const cn105_core::MockState& state) {
    const bool up_down_swing = legacySwingControlsUpDown() && equals(state.vane, "SWING");
    const bool left_right_swing = legacySwingControlsLeftRight() && equals(state.wideVane, "SWING");
    return up_down_swing || left_right_swing ? kSwingEnabled : kSwingDisabled;
}

bool upDownSwingFromMock(const cn105_core::MockState& state) {
    return equals(state.vane, "SWING");
}

bool leftRightSwingFromMock(const cn105_core::MockState& state) {
    return equals(state.wideVane, "SWING");
}

uint8_t positionFromTilt(int tilt) {
    if (tilt < -90) {
        tilt = -90;
    } else if (tilt > 90) {
        tilt = 90;
    }
    return static_cast<uint8_t>(((tilt + 90) * 100 + 90) / 180);
}

int tiltFromPosition(uint8_t position) {
    if (position > 100) {
        position = 100;
    }
    return static_cast<int>((static_cast<int>(position) * 180 + 50) / 100) - 90;
}

int horizontalTiltFromVane(const char* vane) {
    if (equals(vane, "1")) return 90;
    if (equals(vane, "2")) return 45;
    if (equals(vane, "4")) return -45;
    if (equals(vane, "5")) return -75;
    return 0;
}

const char* vaneFromHorizontalTilt(int angle) {
    if (angle >= 68) return "1";
    if (angle >= 23) return "2";
    if (angle >= -22) return "3";
    if (angle >= -60) return "4";
    return "5";
}

int verticalTiltFromWideVane(const char* wide_vane) {
    if (equals(wide_vane, "<<")) return -90;
    if (equals(wide_vane, "<")) return -45;
    if (equals(wide_vane, ">")) return 45;
    if (equals(wide_vane, ">>")) return 90;
    return 0;
}

const char* wideVaneFromVerticalTilt(int angle) {
    if (angle <= -68) return "<<";
    if (angle <= -23) return "<";
    if (angle <= 22) return "|";
    if (angle <= 67) return ">";
    return ">>";
}

void setLastEvent(const char* value) {
    std::strncpy(last_event, value, sizeof(last_event) - 1);
    last_event[sizeof(last_event) - 1] = '\0';
}

void setLastError(const char* value) {
    std::strncpy(last_error, value, sizeof(last_error) - 1);
    last_error[sizeof(last_error) - 1] = '\0';
}

void updateCharUInt8(hap_char_t* character, uint8_t value) {
    if (character == nullptr) {
        return;
    }
    hap_val_t hap_value = {};
    hap_value.u = value;
    hap_char_update_val(character, &hap_value);
}

void updateCharFloat(hap_char_t* character, float value) {
    if (character == nullptr) {
        return;
    }
    hap_val_t hap_value = {};
    hap_value.f = value;
    hap_char_update_val(character, &hap_value);
}

void updateCharBool(hap_char_t* character, bool value) {
    if (character == nullptr) {
        return;
    }
    hap_val_t hap_value = {};
    hap_value.b = value;
    hap_char_update_val(character, &hap_value);
}

bool applyCommand(const cn105_core::SetCommand& command) {
    if (device_settings::useRealCn105()) {
        if (!cn105_transport::queueSetCommand(command)) {
            setLastError("transport queue full");
            ESP_LOGW(TAG, "HomeKit command queue failed");
            return false;
        }
        setLastError("");
        return true;
    }

    cn105_core::Packet packet{};
    char error[96] = {};
    if (!cn105_core::buildSetPacket(command, &packet, error, sizeof(error))) {
        setLastError(error);
        ESP_LOGW(TAG, "HomeKit command build failed: %s", error);
        return false;
    }
    if (!cn105_core::applySetPacketToMock(packet.bytes, packet.length, error, sizeof(error))) {
        setLastError(error);
        ESP_LOGW(TAG, "HomeKit command mock apply failed: %s", error);
        return false;
    }
    setLastError("");
    return true;
}

int identify(hap_acc_t*) {
    ESP_LOGI(TAG, "Identify requested");
    setLastEvent("identify");
    return HAP_SUCCESS;
}

bool addClimateCommandFromWrite(hap_char_t* character, const hap_val_t& value, cn105_core::SetCommand* command) {
    if (command == nullptr) {
        return false;
    }
    if (character == active_char) {
        command->hasPower = true;
        command->power = value.u == kActive ? "ON" : "OFF";
        if (value.u == kActive) {
            const cn105_core::MockState state = cn105_core::getMockState();
            if (requested_special_mode != SpecialMode::kNone ||
                activeSpecialModeFromMock(state) != SpecialMode::kNone) {
                ensureRestoreContext(state);
                command->hasMode = true;
                command->mode = restore_mode;
            }
        }
        return true;
    }
    if (character == fan_active_char) {
        command->hasPower = true;
        command->power = value.u == kActive ? "ON" : "OFF";
        return true;
    }
    if (character == target_state_char) {
        if (!targetStateAllowed(static_cast<uint8_t>(value.u))) {
            setLastError("unsupported HomeKit target mode");
            return false;
        }
        command->hasPower = true;
        command->power = "ON";
        command->hasMode = true;
        command->mode = modeFromTargetState(static_cast<uint8_t>(value.u));
        return true;
    }
    if (character == cooling_threshold_char || character == heating_threshold_char) {
        command->hasTargetTemperature = true;
        command->targetTemperatureHalfC = clampTargetHalfC(cn105_core::celsiusToHalfDegrees(value.f));
        return true;
    }
    if (character == rotation_speed_char || character == fan_rotation_speed_char) {
        command->hasFan = true;
        command->fan = percentToFan(value.f);
        return true;
    }
    if (character == swing_mode_char || character == fan_swing_mode_char) {
        const bool controls_up_down = legacySwingControlsUpDown();
        const bool controls_left_right = legacySwingControlsLeftRight();
        if (!controls_up_down && !controls_left_right) {
            return false;
        }
        command->hasVane = controls_up_down;
        command->hasWideVane = controls_left_right;
        if (value.u == kSwingEnabled) {
            if (controls_up_down) {
                command->vane = "SWING";
            }
            if (controls_left_right) {
                command->wideVane = "SWING";
            }
        } else {
            if (controls_up_down) {
                command->vane = fixedVaneOrDefault();
            }
            if (controls_left_right) {
                command->wideVane = fixedWideVaneOrDefault();
            }
        }
        return true;
    }
    if (character == up_down_target_position_char) {
        command->hasVane = true;
        command->vane = vaneFromHorizontalTilt(tiltFromPosition(value.u));
        return true;
    }
    if (character == left_right_target_position_char) {
        command->hasWideVane = true;
        command->wideVane = wideVaneFromVerticalTilt(tiltFromPosition(value.u));
        return true;
    }
    if (character == up_down_swing_on_char) {
        command->hasVane = true;
        command->vane = value.b ? "SWING" : fixedVaneOrDefault();
        return true;
    }
    if (character == left_right_swing_on_char) {
        command->hasWideVane = true;
        command->wideVane = value.b ? "SWING" : fixedWideVaneOrDefault();
        return true;
    }
    return false;
}

void noteAcceptedCommand(const cn105_core::SetCommand& command) {
    if (command.hasVane) {
        rememberFixedVane(command.vane);
    }
    if (command.hasWideVane) {
        rememberFixedWideVane(command.wideVane);
    }
    if (command.hasMode && isStandardMode(command.mode)) {
        rememberStandardMode(command.mode);
        requested_special_mode = SpecialMode::kNone;
        clearRestoreContext();
    } else if (command.hasPower && equals(command.power, "OFF")) {
        requested_special_mode = SpecialMode::kNone;
        clearRestoreContext();
    }
}

void syncModeCharsFromCommand(const cn105_core::SetCommand& command) {
    const cn105_core::MockState state = cn105_core::getMockState();
    const bool physical_on = command.hasPower ? equals(command.power, "ON") : equals(state.power, "ON");
    const char* effective_mode = command.hasMode ? command.mode : state.mode;
    const SpecialMode special = physical_on ? specialModeFromName(effective_mode) : SpecialMode::kNone;
    updateCharUInt8(active_char, physical_on && special == SpecialMode::kNone ? kActive : kInactive);
    updateCharUInt8(fan_active_char, physical_on ? kActive : kInactive);
    updateCharBool(dry_mode_on_char, special == SpecialMode::kDry);
    updateCharBool(fan_mode_on_char, special == SpecialMode::kFan);
}

void hapEventHandler(void*, esp_event_base_t, int32_t event_id, void*) {
    switch (event_id) {
        case HAP_EVENT_CTRL_PAIRED:
            setLastEvent("controller-paired");
            break;
        case HAP_EVENT_CTRL_UNPAIRED:
            setLastEvent("controller-unpaired");
            break;
        case HAP_EVENT_CTRL_CONNECTED:
            setLastEvent("controller-connected");
            break;
        case HAP_EVENT_CTRL_DISCONNECTED:
            setLastEvent("controller-disconnected");
            break;
        case HAP_EVENT_PAIRING_STARTED:
            setLastEvent("pairing-started");
            break;
        case HAP_EVENT_PAIRING_ABORTED:
            setLastEvent("pairing-aborted");
            break;
        case HAP_EVENT_GET_ACC_COMPLETED:
            setLastEvent("get-accessories");
            break;
        case HAP_EVENT_GET_CHAR_COMPLETED:
            setLastEvent("get-characteristics");
            break;
        case HAP_EVENT_SET_CHAR_COMPLETED:
            setLastEvent("set-characteristics");
            break;
        case HAP_EVENT_PAIRING_MODE_TIMED_OUT:
            setLastEvent("pairing-timeout");
            break;
        default:
            std::snprintf(last_event, sizeof(last_event), "event-%ld", static_cast<long>(event_id));
            break;
    }
}

int heaterCoolerWrite(hap_write_data_t write_data[], int count, void*, void*) {
    cn105_core::SetCommand command{};
    bool should_apply = false;
    bool invalid_write = false;

    for (int i = 0; i < count; ++i) {
        if (write_data[i].hc == target_state_char && !targetStateAllowed(static_cast<uint8_t>(write_data[i].val.u))) {
            invalid_write = true;
            setLastError("unsupported HomeKit target mode");
            if (write_data[i].status != nullptr) {
                *(write_data[i].status) = HAP_STATUS_VAL_INVALID;
            }
            continue;
        }

        if (addClimateCommandFromWrite(write_data[i].hc, write_data[i].val, &command)) {
            should_apply = true;
        } else if (write_data[i].hc == temp_units_char) {
            updateCharUInt8(temp_units_char, kDisplayFahrenheit);
        }

        if (write_data[i].status != nullptr) {
            *(write_data[i].status) = HAP_STATUS_SUCCESS;
        }
    }

    if (invalid_write) {
        return HAP_FAIL;
    }

    if (should_apply && !applyCommand(command)) {
        for (int i = 0; i < count; ++i) {
            if (write_data[i].status != nullptr) {
                *(write_data[i].status) = HAP_STATUS_RES_ABSENT;
            }
        }
        return HAP_FAIL;
    }

    last_command_us = esp_timer_get_time();
    noteAcceptedCommand(command);
    syncModeCharsFromCommand(command);
    homekit_bridge::syncFromMock();
    setLastEvent("heater-cooler-write");
    return HAP_SUCCESS;
}

int airflowFanWrite(hap_write_data_t write_data[], int count, void*, void*) {
    cn105_core::SetCommand command{};
    bool should_apply = false;

    for (int i = 0; i < count; ++i) {
        if (addClimateCommandFromWrite(write_data[i].hc, write_data[i].val, &command)) {
            should_apply = true;
        }

        if (write_data[i].status != nullptr) {
            *(write_data[i].status) = HAP_STATUS_SUCCESS;
        }
    }

    if (should_apply && !applyCommand(command)) {
        for (int i = 0; i < count; ++i) {
            if (write_data[i].status != nullptr) {
                *(write_data[i].status) = HAP_STATUS_RES_ABSENT;
            }
        }
        return HAP_FAIL;
    }

    last_command_us = esp_timer_get_time();
    noteAcceptedCommand(command);
    syncModeCharsFromCommand(command);
    homekit_bridge::syncFromMock();
    setLastEvent("airflow-fan-write");
    return HAP_SUCCESS;
}

int advancedMappingWrite(hap_write_data_t write_data[], int count, void*, void*) {
    cn105_core::SetCommand command{};
    bool should_apply = false;

    for (int i = 0; i < count; ++i) {
        if (addClimateCommandFromWrite(write_data[i].hc, write_data[i].val, &command)) {
            should_apply = true;
        }
        if (write_data[i].status != nullptr) {
            *(write_data[i].status) = HAP_STATUS_SUCCESS;
        }
    }

    if (should_apply && !applyCommand(command)) {
        for (int i = 0; i < count; ++i) {
            if (write_data[i].status != nullptr) {
                *(write_data[i].status) = HAP_STATUS_RES_ABSENT;
            }
        }
        return HAP_FAIL;
    }

    last_command_us = esp_timer_get_time();
    noteAcceptedCommand(command);
    homekit_bridge::syncFromMock();
    setLastEvent("advanced-mapping-write");
    return HAP_SUCCESS;
}

int specialModeSwitchWrite(hap_write_data_t write_data[], int count, void*, void*) {
    const cn105_core::MockState state = cn105_core::getMockState();
    const SpecialMode actual_mode = activeSpecialModeFromMock(state);
    const SpecialMode previous_requested = requested_special_mode;
    const bool previous_restore_valid = restore_context_valid;
    const bool previous_restore_power = restore_power_on;
    char previous_restore_mode[sizeof(restore_mode)] = {};
    copyMode(previous_restore_mode, sizeof(previous_restore_mode), restore_mode);

    cn105_core::SetCommand command{};
    bool should_apply = false;
    SpecialMode next_requested = requested_special_mode;

    for (int i = 0; i < count; ++i) {
        SpecialMode target_mode = SpecialMode::kNone;
        if (write_data[i].hc == dry_mode_on_char) {
            target_mode = SpecialMode::kDry;
        } else if (write_data[i].hc == fan_mode_on_char) {
            target_mode = SpecialMode::kFan;
        }

        if (target_mode != SpecialMode::kNone) {
            if (write_data[i].val.b) {
                ensureRestoreContext(state);
                command.hasPower = true;
                command.power = "ON";
                command.hasMode = true;
                command.mode = specialModeName(target_mode);
                next_requested = target_mode;
                should_apply = true;
            } else if (requested_special_mode == target_mode ||
                       (requested_special_mode == SpecialMode::kNone && actual_mode == target_mode)) {
                ensureRestoreContext(state);
                command.hasPower = true;
                command.power = restore_power_on ? "ON" : "OFF";
                command.hasMode = true;
                command.mode = restore_mode;
                next_requested = SpecialMode::kNone;
                should_apply = true;
            }
        }

        if (write_data[i].status != nullptr) {
            *(write_data[i].status) = HAP_STATUS_SUCCESS;
        }
    }

    if (should_apply && !applyCommand(command)) {
        requested_special_mode = previous_requested;
        restore_context_valid = previous_restore_valid;
        restore_power_on = previous_restore_power;
        copyMode(restore_mode, sizeof(restore_mode), previous_restore_mode);
        for (int i = 0; i < count; ++i) {
            if (write_data[i].status != nullptr) {
                *(write_data[i].status) = HAP_STATUS_RES_ABSENT;
            }
        }
        return HAP_FAIL;
    }

    if (should_apply) {
        requested_special_mode = next_requested;
        last_command_us = esp_timer_get_time();
        noteAcceptedCommand(command);
        syncModeCharsFromCommand(command);
    } else {
        updateCharBool(dry_mode_on_char, actual_mode == SpecialMode::kDry);
        updateCharBool(fan_mode_on_char, actual_mode == SpecialMode::kFan);
    }
    setLastEvent("special-mode-write");
    return HAP_SUCCESS;
}

esp_err_t addHeaterCoolerService(hap_acc_t* target_accessory) {
    const cn105_core::MockState state = cn105_core::getMockState();
    const bool separate_airflow_tile = device_settings::homeKitSeparateAirflowTile();
    heater_cooler = hap_serv_heater_cooler_create(heaterCoolerActiveFromMock(state),
                                                  cn105_core::halfDegreesToCelsius(state.roomTemperatureHalfC),
                                                  currentStateFromMock(state),
                                                  targetStateFromMock(state));
    if (heater_cooler == nullptr) {
        setLastError("failed to create heater cooler service");
        return ESP_FAIL;
    }

    int ret = hap_serv_add_char(heater_cooler, hap_char_name_create(hapString(device_settings::deviceName())));
    ret |= hap_serv_add_char(heater_cooler,
                             hap_char_cooling_threshold_temperature_create(
                                 cn105_core::halfDegreesToCelsius(state.targetTemperatureHalfC)));
    ret |= hap_serv_add_char(heater_cooler,
                             hap_char_heating_threshold_temperature_create(
                                 cn105_core::halfDegreesToCelsius(state.targetTemperatureHalfC)));
    if (!separate_airflow_tile) {
        ret |= hap_serv_add_char(heater_cooler, hap_char_rotation_speed_create(fanToPercent(state.fan)));
        ret |= hap_serv_add_char(heater_cooler, hap_char_swing_mode_create(swingFromMock(state)));
    }
    ret |= hap_serv_add_char(heater_cooler, hap_char_temperature_display_units_create(kDisplayFahrenheit));
    if (ret != HAP_SUCCESS) {
        setLastError("failed to add heater cooler characteristics");
        return ESP_FAIL;
    }

    hap_serv_set_write_cb(heater_cooler, heaterCoolerWrite);
    hap_serv_mark_primary(heater_cooler);
    hap_acc_add_serv(target_accessory, heater_cooler);

    active_char = hap_serv_get_char_by_uuid(heater_cooler, HAP_CHAR_UUID_ACTIVE);
    current_temp_char = hap_serv_get_char_by_uuid(heater_cooler, HAP_CHAR_UUID_CURRENT_TEMPERATURE);
    current_state_char = hap_serv_get_char_by_uuid(heater_cooler, HAP_CHAR_UUID_CURRENT_HEATER_COOLER_STATE);
    target_state_char = hap_serv_get_char_by_uuid(heater_cooler, HAP_CHAR_UUID_TARGET_HEATER_COOLER_STATE);
    cooling_threshold_char = hap_serv_get_char_by_uuid(heater_cooler, HAP_CHAR_UUID_COOLING_THRESHOLD_TEMPERATURE);
    heating_threshold_char = hap_serv_get_char_by_uuid(heater_cooler, HAP_CHAR_UUID_HEATING_THRESHOLD_TEMPERATURE);
    rotation_speed_char = hap_serv_get_char_by_uuid(heater_cooler, HAP_CHAR_UUID_ROTATION_SPEED);
    swing_mode_char = hap_serv_get_char_by_uuid(heater_cooler, HAP_CHAR_UUID_SWING_MODE);
    temp_units_char = hap_serv_get_char_by_uuid(heater_cooler, HAP_CHAR_UUID_TEMPERATURE_DISPLAY_UNITS);

    if (active_char == nullptr || current_temp_char == nullptr || current_state_char == nullptr ||
        target_state_char == nullptr || cooling_threshold_char == nullptr || heating_threshold_char == nullptr ||
        temp_units_char == nullptr || (!separate_airflow_tile && (rotation_speed_char == nullptr || swing_mode_char == nullptr))) {
        setLastError("heater cooler characteristic lookup failed");
        return ESP_FAIL;
    }

    applyHvacModeValidValues();
    hap_char_float_set_constraints(cooling_threshold_char, kMinTargetCelsius, kMaxTargetCelsius, kTargetStepCelsius);
    hap_char_float_set_constraints(heating_threshold_char, kMinTargetCelsius, kMaxTargetCelsius, kTargetStepCelsius);
    return ESP_OK;
}

esp_err_t addAirflowFanService(hap_acc_t* target_accessory) {
    const cn105_core::MockState state = cn105_core::getMockState();
    airflow_fan = hap_serv_fan_v2_create(physicalActiveFromMock(state));
    if (airflow_fan == nullptr) {
        setLastError("failed to create airflow fan service");
        return ESP_FAIL;
    }

    std::snprintf(airflow_service_name, sizeof(airflow_service_name), "%s Airflow", device_settings::deviceName());
    int ret = hap_serv_add_char(airflow_fan, hap_char_name_create(hapString(airflow_service_name)));
    ret |= hap_serv_add_char(airflow_fan, hap_char_rotation_speed_create(fanToPercent(state.fan)));
    if (!hideAirflowFanSwing()) {
        ret |= hap_serv_add_char(airflow_fan, hap_char_swing_mode_create(swingFromMock(state)));
    }
    if (ret != HAP_SUCCESS) {
        setLastError("failed to add airflow fan characteristics");
        return ESP_FAIL;
    }

    hap_serv_set_write_cb(airflow_fan, airflowFanWrite);
    hap_acc_add_serv(target_accessory, airflow_fan);

    fan_active_char = hap_serv_get_char_by_uuid(airflow_fan, HAP_CHAR_UUID_ACTIVE);
    fan_rotation_speed_char = hap_serv_get_char_by_uuid(airflow_fan, HAP_CHAR_UUID_ROTATION_SPEED);
    fan_swing_mode_char = hap_serv_get_char_by_uuid(airflow_fan, HAP_CHAR_UUID_SWING_MODE);
    if (fan_active_char == nullptr || fan_rotation_speed_char == nullptr ||
        (!hideAirflowFanSwing() && fan_swing_mode_char == nullptr)) {
        setLastError("airflow fan characteristic lookup failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t addTiltService(hap_acc_t* target_accessory, bool up_down) {
    const cn105_core::MockState state = cn105_core::getMockState();
    hap_serv_t*& service = up_down ? up_down_tilt_service : left_right_tilt_service;
    char* service_name = up_down ? up_down_tilt_service_name : left_right_tilt_service_name;
    const char* position_value = up_down
                                     ? (upDownSwingFromMock(state) ? fixedVaneOrDefault() : state.vane)
                                     : (leftRightSwingFromMock(state) ? fixedWideVaneOrDefault() : state.wideVane);
    const int target_tilt = up_down ? horizontalTiltFromVane(position_value)
                                    : verticalTiltFromWideVane(position_value);
    const int current_tilt = up_down && equals(state.power, "OFF") ? -90 : target_tilt;
    const uint8_t target_position = positionFromTilt(target_tilt);
    const uint8_t current_position = positionFromTilt(current_tilt);

    service = hap_serv_window_covering_create(target_position, current_position, kPositionStopped);
    if (service == nullptr) {
        setLastError("failed to create tilt service");
        return ESP_FAIL;
    }

    std::snprintf(service_name,
                  96,
                  "%s Airflow Tilt",
                  up_down ? "Up/Down" : "Left/Right");
    int ret = addServiceNames(service, service_name);
    if (ret != HAP_SUCCESS) {
        setLastError("failed to add airflow position service name");
        return ESP_FAIL;
    }

    hap_serv_set_write_cb(service, advancedMappingWrite);
    hap_acc_add_serv(target_accessory, service);

    if (up_down) {
        up_down_current_position_char = hap_serv_get_char_by_uuid(service, HAP_CHAR_UUID_CURRENT_POSITION);
        up_down_target_position_char = hap_serv_get_char_by_uuid(service, HAP_CHAR_UUID_TARGET_POSITION);
        if (up_down_current_position_char == nullptr || up_down_target_position_char == nullptr) {
            setLastError("up/down airflow position characteristic lookup failed");
            return ESP_FAIL;
        }
    } else {
        left_right_current_position_char = hap_serv_get_char_by_uuid(service, HAP_CHAR_UUID_CURRENT_POSITION);
        left_right_target_position_char = hap_serv_get_char_by_uuid(service, HAP_CHAR_UUID_TARGET_POSITION);
        if (left_right_current_position_char == nullptr || left_right_target_position_char == nullptr) {
            setLastError("left/right airflow position characteristic lookup failed");
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t addSwingSwitchService(hap_acc_t* target_accessory, bool up_down) {
    const cn105_core::MockState state = cn105_core::getMockState();
    hap_serv_t*& service = up_down ? up_down_swing_service : left_right_swing_service;
    hap_char_t*& on_char = up_down ? up_down_swing_on_char : left_right_swing_on_char;
    char* service_name = up_down ? up_down_swing_service_name : left_right_swing_service_name;
    const bool swinging = up_down ? upDownSwingFromMock(state) : leftRightSwingFromMock(state);

    service = hap_serv_switch_create(swinging);
    if (service == nullptr) {
        setLastError("failed to create Swing switch service");
        return ESP_FAIL;
    }

    std::snprintf(service_name,
                  96,
                  "%s Airflow Swing",
                  up_down ? "Up/Down" : "Left/Right");
    if (addServiceNames(service, service_name) != ESP_OK) {
        setLastError("failed to add Swing switch name");
        return ESP_FAIL;
    }

    hap_serv_set_write_cb(service, advancedMappingWrite);
    hap_acc_add_serv(target_accessory, service);
    on_char = hap_serv_get_char_by_uuid(service, HAP_CHAR_UUID_ON);
    if (on_char == nullptr) {
        setLastError("Swing switch characteristic lookup failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t addSpecialModeSwitchService(hap_acc_t* target_accessory, SpecialMode mode) {
    const cn105_core::MockState state = cn105_core::getMockState();
    const bool dry = mode == SpecialMode::kDry;
    hap_serv_t*& service = dry ? dry_mode_service : fan_mode_service;
    hap_char_t*& on_char = dry ? dry_mode_on_char : fan_mode_on_char;
    char* service_name = dry ? dry_mode_service_name : fan_mode_service_name;
    const bool active = activeSpecialModeFromMock(state) == mode;

    service = hap_serv_switch_create(active);
    if (service == nullptr) {
        setLastError("failed to create special mode switch service");
        return ESP_FAIL;
    }

    std::snprintf(service_name,
                  96,
                  "%s %s",
                  device_settings::deviceName(),
                  dry ? "Dry Mode" : "Fan-Only Mode");
    if (addServiceNames(service, service_name) != ESP_OK) {
        setLastError("failed to add special mode switch name");
        return ESP_FAIL;
    }

    hap_serv_set_write_cb(service, specialModeSwitchWrite);
    hap_acc_add_serv(target_accessory, service);
    on_char = hap_serv_get_char_by_uuid(service, HAP_CHAR_UUID_ON);
    if (on_char == nullptr) {
        setLastError("special mode switch characteristic lookup failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

}  // namespace

namespace homekit_bridge {

esp_err_t start() {
    if (!app_config::kHomeKitEnabled) {
        setLastEvent("disabled");
        ESP_LOGI(TAG, "HomeKit disabled by app_config");
        return ESP_OK;
    }
    if (started) {
        return ESP_OK;
    }

    setLastEvent("starting");
    setLastError("");

    copyMode(last_standard_mode, sizeof(last_standard_mode), fallbackStandardMode());
    const cn105_core::MockState initial_state = cn105_core::getMockState();
    observeConfirmedModeState(initial_state);
    observeFixedAirflowState(initial_state);

    if (hap_init(HAP_TRANSPORT_WIFI) != HAP_SUCCESS) {
        setLastError("hap_init failed");
        return ESP_FAIL;
    }

    hap_acc_cfg_t cfg = {
        .name = hapString(device_settings::deviceName()),
        .model = hapString(device_settings::homeKitModel()),
        .manufacturer = hapString(device_settings::homeKitManufacturer()),
        .serial_num = hapString(device_settings::homeKitSerial()),
        .fw_rev = hapString(build_info::firmwareVersion()),
        .hw_rev = hapString(app_config::kHomeKitHardwareRevision),
        .pv = hapString("1.1.0"),
        .cid = HAP_CID_AIR_CONDITIONER,
        .identify_routine = identify,
    };

    accessory = hap_acc_create(&cfg);
    if (accessory == nullptr) {
        setLastError("hap_acc_create failed");
        return ESP_FAIL;
    }

    uint8_t product_data[] = {'D', 'K', 'T', 'M', 'I', 'T', 'S', 'U'};
    hap_acc_add_product_data(accessory, product_data, sizeof(product_data));
    hap_acc_add_wifi_transport_service(accessory, 0);

    const esp_err_t err = addHeaterCoolerService(accessory);
    if (err != ESP_OK) {
        return err;
    }
    if (device_settings::homeKitSeparateAirflowTile()) {
        const esp_err_t fan_err = addAirflowFanService(accessory);
        if (fan_err != ESP_OK) {
            return fan_err;
        }
        if (mapsUpDownTilt()) {
            const esp_err_t mapping_err = addTiltService(accessory, true);
            if (mapping_err != ESP_OK) {
                return mapping_err;
            }
        }
        if (mapsLeftRightTilt()) {
            const esp_err_t mapping_err = addTiltService(accessory, false);
            if (mapping_err != ESP_OK) {
                return mapping_err;
            }
        }
        if (mapsUpDownSwing()) {
            const esp_err_t mapping_err = addSwingSwitchService(accessory, true);
            if (mapping_err != ESP_OK) {
                return mapping_err;
            }
        }
        if (mapsLeftRightSwing()) {
            const esp_err_t mapping_err = addSwingSwitchService(accessory, false);
            if (mapping_err != ESP_OK) {
                return mapping_err;
            }
        }
        if (mapsDryModeSwitch()) {
            const esp_err_t mapping_err = addSpecialModeSwitchService(accessory, SpecialMode::kDry);
            if (mapping_err != ESP_OK) {
                return mapping_err;
            }
        }
        if (mapsFanModeSwitch()) {
            const esp_err_t mapping_err = addSpecialModeSwitchService(accessory, SpecialMode::kFan);
            if (mapping_err != ESP_OK) {
                return mapping_err;
            }
        }
    }

    hap_add_accessory(accessory);
    hap_set_setup_code(device_settings::homeKitSetupCode());
    if (hap_set_setup_id(device_settings::homeKitSetupId()) != HAP_SUCCESS) {
        setLastError("hap_set_setup_id failed");
        return ESP_FAIL;
    }

    char* payload = esp_hap_get_setup_payload(hapString(device_settings::homeKitSetupCode()),
                                              hapString(device_settings::homeKitSetupId()),
                                              false,
                                              HAP_CID_AIR_CONDITIONER);
    if (payload != nullptr) {
        std::strncpy(setup_payload, payload, sizeof(setup_payload) - 1);
        setup_payload[sizeof(setup_payload) - 1] = '\0';
        std::free(payload);
    }

    esp_event_handler_register(HAP_EVENT, ESP_EVENT_ANY_ID, hapEventHandler, nullptr);

    if (hap_start() != HAP_SUCCESS) {
        setLastError("hap_start failed");
        return ESP_FAIL;
    }

    started = true;
    syncFromMock();
    setLastEvent("started");
    ESP_LOGI(TAG,
             "HomeKit started: name=%s model=%s setup_code=%s setup_id=%s payload=%s paired=%d",
             device_settings::deviceName(),
             device_settings::homeKitModel(),
             device_settings::homeKitSetupCode(),
             device_settings::homeKitSetupId(),
             setup_payload,
             hap_get_paired_controller_count());
    return ESP_OK;
}

Status getStatus() {
    Status status{};
    status.enabled = app_config::kHomeKitEnabled;
    status.started = started;
    status.pairedControllers = started ? hap_get_paired_controller_count() : 0;
    status.accessoryName = device_settings::deviceName();
    status.model = device_settings::homeKitModel();
    status.firmwareRevision = build_info::firmwareVersion();
    status.setupCode = device_settings::homeKitSetupCode();
    status.setupId = device_settings::homeKitSetupId();
    status.setupPayload = setup_payload;
    status.lastEvent = last_event;
    status.lastError = last_error;
    return status;
}

void markDatabaseChanged() {
    hap_update_config_number();
}

void syncFromMock() {
    if (!started) {
        return;
    }
    if (device_settings::useRealCn105() &&
        (esp_timer_get_time() - last_command_us) < kGracePeriodUs) {
        return;
    }

    const cn105_core::MockState state = cn105_core::getMockState();
    observeConfirmedModeState(state);
    observeFixedAirflowState(state);
    const float target_celsius = cn105_core::halfDegreesToCelsius(state.targetTemperatureHalfC);
    updateCharUInt8(active_char, heaterCoolerActiveFromMock(state));
    updateCharFloat(current_temp_char, cn105_core::halfDegreesToCelsius(state.roomTemperatureHalfC));
    updateCharUInt8(current_state_char, currentStateFromMock(state));
    updateCharUInt8(target_state_char, targetStateFromMock(state));
    updateCharFloat(cooling_threshold_char, target_celsius);
    updateCharFloat(heating_threshold_char, target_celsius);
    updateCharFloat(rotation_speed_char, fanToPercent(state.fan));
    updateCharUInt8(swing_mode_char, swingFromMock(state));
    updateCharUInt8(temp_units_char, kDisplayFahrenheit);
    updateCharUInt8(fan_active_char, physicalActiveFromMock(state));
    updateCharFloat(fan_rotation_speed_char, fanToPercent(state.fan));
    updateCharUInt8(fan_swing_mode_char, swingFromMock(state));
    const bool up_down_swing = upDownSwingFromMock(state);
    if (!up_down_swing) {
        const int up_down_target_tilt = horizontalTiltFromVane(state.vane);
        const int up_down_current_tilt = equals(state.power, "OFF") ? -90 : up_down_target_tilt;
        updateCharUInt8(up_down_current_position_char, positionFromTilt(up_down_current_tilt));
        updateCharUInt8(up_down_target_position_char, positionFromTilt(up_down_target_tilt));
    }
    updateCharBool(up_down_swing_on_char, up_down_swing);
    const bool left_right_swing = leftRightSwingFromMock(state);
    if (!left_right_swing) {
        const int left_right_tilt = verticalTiltFromWideVane(state.wideVane);
        updateCharUInt8(left_right_current_position_char, positionFromTilt(left_right_tilt));
        updateCharUInt8(left_right_target_position_char, positionFromTilt(left_right_tilt));
    }
    updateCharBool(left_right_swing_on_char, left_right_swing);
    const SpecialMode special_mode = activeSpecialModeFromMock(state);
    updateCharBool(dry_mode_on_char, special_mode == SpecialMode::kDry);
    updateCharBool(fan_mode_on_char, special_mode == SpecialMode::kFan);
}

}  // namespace homekit_bridge
