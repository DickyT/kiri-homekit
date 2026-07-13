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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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

constexpr size_t kAutomationPendingTargets = 4;
constexpr size_t kAutomationBurstLimit = 6;
constexpr int64_t kAutomationTargetLifetimeUs = 15LL * 1000 * 1000;
constexpr int64_t kAutomationBurstWindowUs = 60LL * 1000 * 1000;
constexpr uint8_t kAutomationErrorLimit = 3;

struct AutomationTarget {
    bool active = false;
    int64_t expiresAtUs = 0;
    bool hasPower = false;
    char power[16] = {};
    bool hasMode = false;
    char mode[16] = {};
    bool hasTargetTemperature = false;
    cn105_core::HalfDegreesC targetTemperatureHalfC = 0;
    bool hasFan = false;
    char fan[16] = {};
    bool hasVane = false;
    char vane[16] = {};
    bool hasWideVane = false;
    char wideVane[32] = {};
};

AutomationTarget automation_targets[kAutomationPendingTargets]{};
int64_t automation_action_times[kAutomationBurstLimit]{};
size_t automation_action_time_count = 0;
uint8_t automation_consecutive_errors = 0;
QueueHandle_t automation_action_queue = nullptr;

struct AutomationCommandJob {
    bool hasPower = false;
    char power[16] = {};
    bool hasMode = false;
    char mode[16] = {};
    bool hasTargetTemperature = false;
    cn105_core::HalfDegreesC targetTemperatureHalfC = 0;
    bool hasFan = false;
    char fan[16] = {};
    bool hasVane = false;
    char vane[16] = {};
    bool hasWideVane = false;
    char wideVane[32] = {};
};

bool equals(const char* left, const char* right) {
    return left != nullptr && right != nullptr && std::strcmp(left, right) == 0;
}

void disableAutomation(const char* reason) {
    char error[96] = {};
    if (!lua_vm::setEnabled(false, error, sizeof(error))) {
        ESP_LOGE(TAG, "Failed disabling Lua automation after %s: %s", reason, error);
        return;
    }
    ESP_LOGE(TAG, "Lua automation disabled: %s", reason);
}

void noteAutomationFailure(const char* reason) {
    if (automation_consecutive_errors < UINT8_MAX) {
        automation_consecutive_errors++;
    }
    if (automation_consecutive_errors >= kAutomationErrorLimit) {
        disableAutomation(reason);
        automation_consecutive_errors = 0;
    }
}

bool allowAutomationActionBatch() {
    const int64_t now = esp_timer_get_time();
    size_t kept = 0;
    for (size_t i = 0; i < automation_action_time_count; ++i) {
        if (now - automation_action_times[i] < kAutomationBurstWindowUs) {
            automation_action_times[kept++] = automation_action_times[i];
        }
    }
    automation_action_time_count = kept;
    if (automation_action_time_count >= kAutomationBurstLimit) {
        disableAutomation("more than 6 action batches in 60 seconds");
        automation_action_time_count = 0;
        return false;
    }
    automation_action_times[automation_action_time_count++] = now;
    return true;
}

bool commandHasChanges(const cn105_core::SetCommand& command) {
    return command.hasPower || command.hasMode || command.hasTargetTemperature || command.hasFan ||
           command.hasVane || command.hasWideVane;
}

AutomationCommandJob automationJobFromCommand(const cn105_core::SetCommand& command) {
    AutomationCommandJob job{};
    job.hasPower = command.hasPower;
    job.hasMode = command.hasMode;
    job.hasTargetTemperature = command.hasTargetTemperature;
    job.targetTemperatureHalfC = command.targetTemperatureHalfC;
    job.hasFan = command.hasFan;
    job.hasVane = command.hasVane;
    job.hasWideVane = command.hasWideVane;
    if (command.hasPower) std::snprintf(job.power, sizeof(job.power), "%s", command.power);
    if (command.hasMode) std::snprintf(job.mode, sizeof(job.mode), "%s", command.mode);
    if (command.hasFan) std::snprintf(job.fan, sizeof(job.fan), "%s", command.fan);
    if (command.hasVane) std::snprintf(job.vane, sizeof(job.vane), "%s", command.vane);
    if (command.hasWideVane) std::snprintf(job.wideVane, sizeof(job.wideVane), "%s", command.wideVane);
    return job;
}

cn105_core::SetCommand commandFromAutomationJob(const AutomationCommandJob& job) {
    cn105_core::SetCommand command{};
    command.hasPower = job.hasPower;
    command.power = job.hasPower ? job.power : nullptr;
    command.hasMode = job.hasMode;
    command.mode = job.hasMode ? job.mode : nullptr;
    command.hasTargetTemperature = job.hasTargetTemperature;
    command.targetTemperatureHalfC = job.targetTemperatureHalfC;
    command.hasFan = job.hasFan;
    command.fan = job.hasFan ? job.fan : nullptr;
    command.hasVane = job.hasVane;
    command.vane = job.hasVane ? job.vane : nullptr;
    command.hasWideVane = job.hasWideVane;
    command.wideVane = job.hasWideVane ? job.wideVane : nullptr;
    return command;
}

void automationActionTask(void*) {
    AutomationCommandJob job{};
    while (true) {
        if (xQueueReceive(automation_action_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const cn105_core::SetCommand command = commandFromAutomationJob(job);
        cn105_transport::ApplyResult result{};
        const bool ok = cn105_transport::queueSetCommandAndConfirm(command, &result);
        lua_vm::recordActionResult(ok && result.confirmed, result.attempts, result.message);
        if (!ok || !result.confirmed) {
            ESP_LOGW(TAG, "Lua CN105 action failed after %u attempt(s): %s",
                     static_cast<unsigned>(result.attempts), result.message);
        } else {
            ESP_LOGI(TAG, "Lua CN105 action confirmed after %u attempt(s)",
                     static_cast<unsigned>(result.attempts));
        }
    }
}

bool initAutomationActionWorker() {
    automation_action_queue = xQueueCreate(4, sizeof(AutomationCommandJob));
    if (automation_action_queue == nullptr) {
        return false;
    }
    return xTaskCreate(automationActionTask, "lua_cn105", 4096, nullptr, 4, nullptr) == pdPASS;
}

void removeNoOpFields(const cn105_core::MockState& state, cn105_core::SetCommand* command) {
    if (command == nullptr) {
        return;
    }
    if (command->hasPower && equals(command->power, state.power)) command->hasPower = false;
    if (command->hasMode && equals(command->mode, state.mode)) command->hasMode = false;
    if (command->hasTargetTemperature && command->targetTemperatureHalfC == state.targetTemperatureHalfC) {
        command->hasTargetTemperature = false;
    }
    if (command->hasFan && equals(command->fan, state.fan)) command->hasFan = false;
    if (command->hasVane && equals(command->vane, state.vane)) command->hasVane = false;
    if (command->hasWideVane && equals(command->wideVane, state.wideVane)) command->hasWideVane = false;
}

void rememberAutomationTarget(const cn105_core::SetCommand& command) {
    const int64_t now = esp_timer_get_time();
    size_t slot = 0;
    for (size_t i = 0; i < kAutomationPendingTargets; ++i) {
        if (!automation_targets[i].active || automation_targets[i].expiresAtUs <= now) {
            slot = i;
            break;
        }
        if (automation_targets[i].expiresAtUs < automation_targets[slot].expiresAtUs) {
            slot = i;
        }
    }

    AutomationTarget& target = automation_targets[slot];
    target = {};
    target.active = true;
    target.expiresAtUs = now + kAutomationTargetLifetimeUs;
    target.hasPower = command.hasPower;
    target.hasMode = command.hasMode;
    target.hasTargetTemperature = command.hasTargetTemperature;
    target.targetTemperatureHalfC = command.targetTemperatureHalfC;
    target.hasFan = command.hasFan;
    target.hasVane = command.hasVane;
    target.hasWideVane = command.hasWideVane;
    if (command.hasPower) std::snprintf(target.power, sizeof(target.power), "%s", command.power);
    if (command.hasMode) std::snprintf(target.mode, sizeof(target.mode), "%s", command.mode);
    if (command.hasFan) std::snprintf(target.fan, sizeof(target.fan), "%s", command.fan);
    if (command.hasVane) std::snprintf(target.vane, sizeof(target.vane), "%s", command.vane);
    if (command.hasWideVane) std::snprintf(target.wideVane, sizeof(target.wideVane), "%s", command.wideVane);
}

bool targetMatchesState(const AutomationTarget& target, const cn105_core::MockState& state) {
    return (!target.hasPower || equals(target.power, state.power)) &&
           (!target.hasMode || equals(target.mode, state.mode)) &&
           (!target.hasTargetTemperature || target.targetTemperatureHalfC == state.targetTemperatureHalfC) &&
           (!target.hasFan || equals(target.fan, state.fan)) &&
           (!target.hasVane || equals(target.vane, state.vane)) &&
           (!target.hasWideVane || equals(target.wideVane, state.wideVane));
}

bool consumeAutomationEcho(const cn105_core::MockState& state) {
    const int64_t now = esp_timer_get_time();
    for (AutomationTarget& target : automation_targets) {
        if (!target.active) {
            continue;
        }
        if (target.expiresAtUs <= now) {
            target.active = false;
            continue;
        }
        if (targetMatchesState(target, state)) {
            target.active = false;
            return true;
        }
    }
    return false;
}

bool fillCommandFromLuaActions(const lua_vm::RunResult& result,
                               const cn105_core::MockState& state,
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
                command->hasTargetTemperature = true;
                command->targetTemperatureHalfC = cn105_core::fahrenheitSetpointToHalfDegrees(action.intValue);
                any = true;
                break;
            case lua_vm::ActionType::kSetTargetTemperatureC:
                command->hasTargetTemperature = true;
                command->targetTemperatureHalfC = static_cast<cn105_core::HalfDegreesC>(action.intValue);
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
    removeNoOpFields(state, command);
    return any && commandHasChanges(*command);
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
        noteAutomationFailure(result.message);
        return;
    }
    automation_consecutive_errors = 0;
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
                                   state,
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
    if (!allowAutomationActionBatch()) {
        return;
    }

    if (device_settings::useRealCn105()) {
        lua_vm::recordActionQueued();
        const AutomationCommandJob job = automationJobFromCommand(command);
        if (automation_action_queue == nullptr ||
            xQueueSend(automation_action_queue, &job, pdMS_TO_TICKS(100)) != pdTRUE) {
            constexpr const char* error = "automation confirmation queue is full";
            ESP_LOGW(TAG, "Lua hook %s could not queue CN105 confirmation", hook);
            lua_vm::recordActionResult(false, 0, error);
            noteAutomationFailure(error);
            return;
        }
        rememberAutomationTarget(command);
        return;
    }

    cn105_core::Packet packet{};
    char error[96] = {};
    if (!cn105_core::buildSetPacket(command, &packet, error, sizeof(error))) {
        ESP_LOGW(TAG, "Lua hook %s command build failed: %s", hook, error);
        lua_vm::recordActionQueued();
        lua_vm::recordActionResult(false, 0, error);
        return;
    }
    lua_vm::recordActionQueued();
    if (!cn105_core::applySetPacketToMock(packet.bytes, packet.length, error, sizeof(error))) {
        ESP_LOGW(TAG, "Lua hook %s mock apply failed: %s", hook, error);
        lua_vm::recordActionResult(false, 0, error);
        noteAutomationFailure(error);
    } else {
        rememberAutomationTarget(command);
        lua_vm::recordActionResult(true, 1, "mock state confirmed");
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
             "Lua VM self-test: ok=%d checks=%u/%u peak=%u bytes message=%s",
             lua_probe.ok,
             static_cast<unsigned>(lua_probe.checksPassed),
             static_cast<unsigned>(lua_probe.checksTotal),
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
        if (!initAutomationActionWorker()) {
            ESP_LOGE(TAG, "Lua automation confirmation worker failed to start");
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
            if (consumeAutomationEcho(current_state)) {
                ESP_LOGI(TAG, "Skipping Lua hooks for automation-originated state confirmation");
            } else {
                runLuaAutomationHook("on_state_changed",
                                     current_state,
                                     has_previous_state ? &previous_state : nullptr);
                if (has_previous_state && !isOn(previous_state) && isOn(current_state)) {
                    runLuaAutomationHook("on_power_on", current_state, &previous_state);
                } else if (has_previous_state && isOn(previous_state) && !isOn(current_state)) {
                    runLuaAutomationHook("on_power_off", current_state, &previous_state);
                }
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
