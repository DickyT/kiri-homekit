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
constexpr uint8_t kDisplayFahrenheit = 1;
constexpr float kMinTargetCelsius = 10.0f;
constexpr float kMaxTargetCelsius = 31.0f;
constexpr float kTargetStepCelsius = 0.5f;

bool started = false;
int64_t last_command_us = 0;
constexpr int64_t kGracePeriodUs = 3 * 1000 * 1000;
hap_acc_t* accessory = nullptr;
hap_serv_t* heater_cooler = nullptr;
hap_serv_t* airflow_fan = nullptr;
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
char setup_payload[128] = "";
char airflow_service_name[96] = "";
char last_event[40] = "not-started";
char last_error[96] = "";

char* hapString(const char* value) {
    return const_cast<char*>(value);
}

bool equals(const char* left, const char* right) {
    return left != nullptr && right != nullptr && std::strcmp(left, right) == 0;
}

float fahrenheitToCelsius(int value_f) {
    return (static_cast<float>(value_f) - 32.0f) * 5.0f / 9.0f;
}

int celsiusToRoundedFahrenheit(float value_c) {
    return static_cast<int>(std::lround((value_c * 9.0f / 5.0f) + 32.0f));
}

int clampFahrenheit(int value_f) {
    if (value_f < 50) {
        return 50;
    }
    if (value_f > 88) {
        return 88;
    }
    return value_f;
}

uint8_t activeFromMock(const cn105_core::MockState& state) {
    return equals(state.power, "ON") ? kActive : kInactive;
}

uint8_t targetStateFromMock(const cn105_core::MockState& state) {
    if (equals(state.mode, "HEAT")) {
        return kTargetHeat;
    }
    if (equals(state.mode, "COOL")) {
        return kTargetCool;
    }
    return kTargetAuto;
}

uint8_t currentStateFromMock(const cn105_core::MockState& state) {
    if (equals(state.power, "OFF")) {
        return kCurrentInactive;
    }
    if (!state.operating) {
        return kCurrentIdle;
    }
    if (equals(state.mode, "HEAT")) {
        return kCurrentHeating;
    }
    if (equals(state.mode, "COOL") || equals(state.mode, "DRY")) {
        return kCurrentCooling;
    }
    if (equals(state.mode, "AUTO")) {
        return state.roomTemperatureF < state.targetTemperatureF ? kCurrentHeating : kCurrentCooling;
    }
    return kCurrentIdle;
}

const char* modeFromTargetState(uint8_t target_state) {
    switch (target_state) {
        case kTargetHeat:
            return "HEAT";
        case kTargetCool:
            return "COOL";
        default:
            return "AUTO";
    }
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

uint8_t swingFromMock(const cn105_core::MockState& state) {
    return equals(state.vane, "SWING") || equals(state.wideVane, "SWING") ? kSwingEnabled : kSwingDisabled;
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
    if (character == active_char || character == fan_active_char) {
        command->hasPower = true;
        command->power = value.u == kActive ? "ON" : "OFF";
        return true;
    }
    if (character == target_state_char) {
        command->hasPower = true;
        command->power = "ON";
        command->hasMode = true;
        command->mode = modeFromTargetState(static_cast<uint8_t>(value.u));
        return true;
    }
    if (character == cooling_threshold_char || character == heating_threshold_char) {
        command->hasTemperatureF = true;
        command->temperatureF = clampFahrenheit(celsiusToRoundedFahrenheit(value.f));
        return true;
    }
    if (character == rotation_speed_char || character == fan_rotation_speed_char) {
        command->hasFan = true;
        command->fan = percentToFan(value.f);
        return true;
    }
    if (character == swing_mode_char || character == fan_swing_mode_char) {
        command->hasVane = true;
        command->hasWideVane = true;
        if (value.u == kSwingEnabled) {
            command->vane = "SWING";
            command->wideVane = "SWING";
        } else {
            command->vane = "AUTO";
            command->wideVane = "|";
        }
        return true;
    }
    return false;
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

    for (int i = 0; i < count; ++i) {
        if (addClimateCommandFromWrite(write_data[i].hc, write_data[i].val, &command)) {
            should_apply = true;
        } else if (write_data[i].hc == temp_units_char) {
            updateCharUInt8(temp_units_char, kDisplayFahrenheit);
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

    homekit_bridge::syncFromMock();
    last_command_us = esp_timer_get_time();
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

    homekit_bridge::syncFromMock();
    last_command_us = esp_timer_get_time();
    setLastEvent("airflow-fan-write");
    return HAP_SUCCESS;
}

esp_err_t addHeaterCoolerService(hap_acc_t* target_accessory) {
    const cn105_core::MockState state = cn105_core::getMockState();
    const bool separate_airflow_tile = device_settings::homeKitSeparateAirflowTile();
    heater_cooler = hap_serv_heater_cooler_create(activeFromMock(state),
                                                  fahrenheitToCelsius(state.roomTemperatureF),
                                                  currentStateFromMock(state),
                                                  targetStateFromMock(state));
    if (heater_cooler == nullptr) {
        setLastError("failed to create heater cooler service");
        return ESP_FAIL;
    }

    int ret = hap_serv_add_char(heater_cooler, hap_char_name_create(hapString(device_settings::deviceName())));
    ret |= hap_serv_add_char(heater_cooler, hap_char_cooling_threshold_temperature_create(fahrenheitToCelsius(state.targetTemperatureF)));
    ret |= hap_serv_add_char(heater_cooler, hap_char_heating_threshold_temperature_create(fahrenheitToCelsius(state.targetTemperatureF)));
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

    hap_char_float_set_constraints(cooling_threshold_char, kMinTargetCelsius, kMaxTargetCelsius, kTargetStepCelsius);
    hap_char_float_set_constraints(heating_threshold_char, kMinTargetCelsius, kMaxTargetCelsius, kTargetStepCelsius);
    return ESP_OK;
}

esp_err_t addAirflowFanService(hap_acc_t* target_accessory) {
    const cn105_core::MockState state = cn105_core::getMockState();
    airflow_fan = hap_serv_fan_v2_create(activeFromMock(state));
    if (airflow_fan == nullptr) {
        setLastError("failed to create airflow fan service");
        return ESP_FAIL;
    }

    std::snprintf(airflow_service_name, sizeof(airflow_service_name), "%s Airflow", device_settings::deviceName());
    int ret = hap_serv_add_char(airflow_fan, hap_char_name_create(hapString(airflow_service_name)));
    ret |= hap_serv_add_char(airflow_fan, hap_char_rotation_speed_create(fanToPercent(state.fan)));
    ret |= hap_serv_add_char(airflow_fan, hap_char_swing_mode_create(swingFromMock(state)));
    if (ret != HAP_SUCCESS) {
        setLastError("failed to add airflow fan characteristics");
        return ESP_FAIL;
    }

    hap_serv_set_write_cb(airflow_fan, airflowFanWrite);
    hap_acc_add_serv(target_accessory, airflow_fan);

    fan_active_char = hap_serv_get_char_by_uuid(airflow_fan, HAP_CHAR_UUID_ACTIVE);
    fan_rotation_speed_char = hap_serv_get_char_by_uuid(airflow_fan, HAP_CHAR_UUID_ROTATION_SPEED);
    fan_swing_mode_char = hap_serv_get_char_by_uuid(airflow_fan, HAP_CHAR_UUID_SWING_MODE);
    if (fan_active_char == nullptr || fan_rotation_speed_char == nullptr || fan_swing_mode_char == nullptr) {
        setLastError("airflow fan characteristic lookup failed");
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

void syncFromMock() {
    if (!started) {
        return;
    }
    if (device_settings::useRealCn105() &&
        (esp_timer_get_time() - last_command_us) < kGracePeriodUs) {
        return;
    }

    const cn105_core::MockState state = cn105_core::getMockState();
    const float target_celsius = fahrenheitToCelsius(state.targetTemperatureF);
    updateCharUInt8(active_char, activeFromMock(state));
    updateCharFloat(current_temp_char, fahrenheitToCelsius(state.roomTemperatureF));
    updateCharUInt8(current_state_char, currentStateFromMock(state));
    updateCharUInt8(target_state_char, targetStateFromMock(state));
    updateCharFloat(cooling_threshold_char, target_celsius);
    updateCharFloat(heating_threshold_char, target_celsius);
    updateCharFloat(rotation_speed_char, fanToPercent(state.fan));
    updateCharUInt8(swing_mode_char, swingFromMock(state));
    updateCharUInt8(temp_units_char, kDisplayFahrenheit);
    updateCharUInt8(fan_active_char, activeFromMock(state));
    updateCharFloat(fan_rotation_speed_char, fanToPercent(state.fan));
    updateCharUInt8(fan_swing_mode_char, swingFromMock(state));
}

}  // namespace homekit_bridge
