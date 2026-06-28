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

#include <stdio.h>
#include <cstring>

#include "app_config.h"
#include "build_info.h"
#include "cn105_core.h"
#include "cn105_transport.h"
#include "cn105_uart.h"
#include "device_settings.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "homekit_bridge.h"
#include "lua_vm.h"
#include "platform_fs.h"
#include "platform_led.h"
#include "platform_log.h"
#include "platform_provisioning.h"
#include "platform_wifi.h"
#include "web_server.h"

static const char* TAG = "bootstrap";

namespace {

bool fillCommandFromLuaActions(const lua_vm::RunResult& result,
                               cn105_core::SetCommand* command,
                               char* power,
                               size_t power_len,
                               char* mode,
                               size_t mode_len,
                               char* fan,
                               size_t fan_len,
                               char* vane,
                               size_t vane_len,
                               char* wide_vane,
                               size_t wide_vane_len) {
    if (command == nullptr) {
        return false;
    }

    bool any = false;
    for (size_t i = 0; i < result.actionCount; ++i) {
        const lua_vm::Action& action = result.actions[i];
        switch (action.type) {
            case lua_vm::ActionType::kSetPower:
                std::snprintf(power, power_len, "%s", action.value);
                command->hasPower = true;
                command->power = power;
                any = true;
                break;
            case lua_vm::ActionType::kSetMode:
                std::snprintf(mode, mode_len, "%s", action.value);
                command->hasMode = true;
                command->mode = mode;
                any = true;
                break;
            case lua_vm::ActionType::kSetTargetTemperatureF:
                command->hasTemperatureF = true;
                command->temperatureF = action.intValue;
                any = true;
                break;
            case lua_vm::ActionType::kSetFan:
                std::snprintf(fan, fan_len, "%s", action.value);
                command->hasFan = true;
                command->fan = fan;
                any = true;
                break;
            case lua_vm::ActionType::kSetVane:
                std::snprintf(vane, vane_len, "%s", action.value);
                command->hasVane = true;
                command->vane = vane;
                any = true;
                break;
            case lua_vm::ActionType::kSetWideVane:
                std::snprintf(wide_vane, wide_vane_len, "%s", action.value);
                command->hasWideVane = true;
                command->wideVane = wide_vane;
                any = true;
                break;
            case lua_vm::ActionType::kNone:
            default:
                break;
        }
    }
    return any;
}

void runLuaAutomationHook(const char* hook,
                          const cn105_core::MockState& state,
                          const cn105_core::MockState* previous) {
    const lua_vm::RunResult result = lua_vm::runHook(hook, state, previous);
    if (!result.scriptLoaded || !result.hookFound) {
        return;
    }
    if (!result.ok) {
        ESP_LOGW(TAG, "Lua hook %s failed: %s", hook, result.message);
        return;
    }
    if (result.actionCount == 0) {
        return;
    }

    cn105_core::SetCommand command{};
    char power[16] = {};
    char mode[16] = {};
    char fan[16] = {};
    char vane[16] = {};
    char wide_vane[32] = {};
    if (!fillCommandFromLuaActions(result,
                                   &command,
                                   power,
                                   sizeof(power),
                                   mode,
                                   sizeof(mode),
                                   fan,
                                   sizeof(fan),
                                   vane,
                                   sizeof(vane),
                                   wide_vane,
                                   sizeof(wide_vane))) {
        return;
    }

    if (device_settings::useRealCn105()) {
        if (!cn105_transport::queueSetCommand(command)) {
            ESP_LOGW(TAG, "Lua hook %s could not queue CN105 command", hook);
        }
        return;
    }

    cn105_core::Packet packet{};
    char error[96] = {};
    if (!cn105_core::buildSetPacket(command, &packet, error, sizeof(error))) {
        ESP_LOGW(TAG, "Lua hook %s command build failed: %s", hook, error);
        return;
    }
    if (!cn105_core::applySetPacketToMock(packet.bytes, packet.length, error, sizeof(error))) {
        ESP_LOGW(TAG, "Lua hook %s mock apply failed: %s", hook, error);
    }
}

bool isOn(const cn105_core::MockState& state) {
    return state.power != nullptr && std::strcmp(state.power, "ON") == 0;
}

}  // namespace

extern "C" void app_main(void) {
    const esp_err_t settings_err = device_settings::init();
    if (settings_err != ESP_OK) {
        ESP_LOGE(TAG, "Device settings init failed: %s", esp_err_to_name(settings_err));
    }
    platform_log::init();

    const esp_err_t fs_err = platform_fs::init();
    if (fs_err == ESP_OK) {
        platform_log::enablePersistentLog();
    } else {
        ESP_LOGE(TAG, "SPIFFS init failed: %s", esp_err_to_name(fs_err));
    }

    esp_chip_info_t chip_info{};
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    esp_flash_get_size(nullptr, &flash_size);

    ESP_LOGI(TAG, "Kiri Bridge bootstrap starting");
    ESP_LOGI(TAG, "Firmware version: %s", build_info::firmwareVersion());
    platform_log::logStartupSummary();
    ESP_LOGI(TAG,
             "Chip: cores=%d, revision=%d, features=%s%s%s%s",
             chip_info.cores,
             chip_info.revision,
             (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "BLE " : "",
             (chip_info.features & CHIP_FEATURE_BT) ? "BT " : "",
             (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "EmbeddedFlash" : "");
    ESP_LOGI(TAG, "Flash size: %lu MB", static_cast<unsigned long>(flash_size / (1024 * 1024)));

    const lua_vm::ProbeResult lua_probe = lua_vm::runProbe();
    ESP_LOGI(TAG,
             "Lua VM probe: ok=%d peak=%u bytes message=%s",
             lua_probe.ok,
             static_cast<unsigned>(lua_probe.peakBytes),
             lua_probe.message);

    const esp_err_t led_err = platform_led::init();
    if (led_err != ESP_OK) {
        ESP_LOGE(TAG, "Status LED init failed: %s", esp_err_to_name(led_err));
    }

    const esp_err_t uart_err = cn105_uart::init();
    if (uart_err != ESP_OK) {
        ESP_LOGE(TAG, "CN105 UART init failed: %s", esp_err_to_name(uart_err));
    }

    cn105_core::initMockState();
    char cn105_self_test_error[96] = {};
    if (!cn105_core::runSelfTest(cn105_self_test_error, sizeof(cn105_self_test_error))) {
        ESP_LOGW(TAG, "CN105 offline self-test failed: %s", cn105_self_test_error);
    }

    if (device_settings::useRealCn105()) {
        const esp_err_t transport_err = cn105_transport::start();
        if (transport_err != ESP_OK) {
            ESP_LOGE(TAG, "CN105 transport start failed: %s", esp_err_to_name(transport_err));
        } else {
            ESP_LOGI(TAG, "CN105 real transport started");
        }
    } else {
        ESP_LOGI(TAG, "CN105 transport: mock mode");
    }

    const esp_err_t wifi_err = platform_wifi::init();
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(wifi_err));
    }

    const esp_err_t provisioning_err = platform_provisioning::init();
    if (provisioning_err != ESP_OK) {
        ESP_LOGE(TAG, "BLE provisioning init failed: %s", esp_err_to_name(provisioning_err));
    }

    const esp_err_t homekit_err = homekit_bridge::start();
    if (homekit_err != ESP_OK) {
        ESP_LOGE(TAG, "HomeKit start failed: %s", esp_err_to_name(homekit_err));
    }

    const esp_err_t web_err = web_server::start();
    if (web_err != ESP_OK) {
        ESP_LOGE(TAG, "WebUI start failed: %s", esp_err_to_name(web_err));
    }

    cn105_core::MockState previous_state = cn105_core::getMockState();
    bool has_previous_state = false;
    uint32_t heartbeat = 0;
    while (true) {
        platform_wifi::maintain();
        if (cn105_core::isMockDirty()) {
            const cn105_core::MockState current_state = cn105_core::getMockState();
            homekit_bridge::syncFromMock();
            runLuaAutomationHook("on_state_changed",
                                 current_state,
                                 has_previous_state ? &previous_state : nullptr);
            if (has_previous_state && !isOn(previous_state) && isOn(current_state)) {
                runLuaAutomationHook("on_power_on", current_state, &previous_state);
            } else if (has_previous_state && isOn(previous_state) && !isOn(current_state)) {
                runLuaAutomationHook("on_power_off", current_state, &previous_state);
            }
            previous_state = current_state;
            has_previous_state = true;
            cn105_core::clearMockDirty();
        }
        ESP_LOGI(TAG, "Platform heartbeat #%lu - services are alive",
                 static_cast<unsigned long>(heartbeat));
        platform_wifi::logStatus("heartbeat");
        heartbeat++;
        vTaskDelay(pdMS_TO_TICKS(app_config::kHeartbeatIntervalMs));
    }
}
