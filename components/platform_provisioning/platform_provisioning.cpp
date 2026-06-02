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

#include "platform_provisioning.h"

#include "app_config.h"
#include "device_settings.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform_lock.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

#include <cstdio>
#include <cstring>

namespace {

const char* TAG = "platform_provision";
constexpr const char* kProvisioningPop = "abcd1234";
constexpr char kPlaceholderSsid[] = "YOUR_WIFI_SSID";

struct State {
    platform_lock::RecursiveMutex lock;
    bool initialized = false;
    bool taskRunning = false;
    bool managerInitialized = false;
    bool active = false;
    bool credentialsReceived = false;
    bool credentialsAccepted = false;
    bool rebootPending = false;
    bool buttonHeld = false;
    bool longPressConsumed = false;
    int64_t buttonPressedAtUs = 0;
    int64_t sessionDeadlineUs = 0;
    int64_t rebootAtUs = 0;
    char stage[24] = "idle";
    char lastResult[24] = "idle";
    char serviceName[40] = "--";
    char pendingSsid[33] = "";
    char pendingPassword[65] = "";
};

State state;
esp_event_handler_instance_t wifi_event_instance = nullptr;
esp_event_handler_instance_t ip_event_instance = nullptr;
esp_event_handler_instance_t prov_event_instance = nullptr;

void copyString(char* out, size_t out_len, const char* value) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    std::snprintf(out, out_len, "%s", value == nullptr ? "" : value);
}

bool hasStoredWifiCredentials() {
    return device_settings::wifiSsid()[0] != '\0' &&
           std::strcmp(device_settings::wifiSsid(), kPlaceholderSsid) != 0;
}

void setStageLocked(const char* value) {
    copyString(state.stage, sizeof(state.stage), value);
}

void setLastResultLocked(const char* value) {
    copyString(state.lastResult, sizeof(state.lastResult), value);
}

void clearPendingCredentialsLocked() {
    state.pendingSsid[0] = '\0';
    state.pendingPassword[0] = '\0';
    state.credentialsReceived = false;
    state.credentialsAccepted = false;
}

void buildProvisioningServiceName(char* out, size_t out_len) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(out,
                  out_len,
                  "PROV_KIRI_%02X%02X%02X%02X%02X%02X",
                  mac[0],
                  mac[1],
                  mac[2],
                  mac[3],
                  mac[4],
                  mac[5]);
}

void restoreConfiguredWifi() {
    if (!hasStoredWifiCredentials()) {
        ESP_LOGW(TAG, "Skipping WiFi restore because no saved STA credentials exist");
        return;
    }

    wifi_config_t sta_config = {};
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.ssid),
                 device_settings::wifiSsid(),
                 sizeof(sta_config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.password),
                 device_settings::wifiPassword(),
                 sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode =
        std::strlen(device_settings::wifiPassword()) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (app_config::kWifiDisablePowerSave) {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
    esp_wifi_connect();
    ESP_LOGI(TAG, "Restoring configured WiFi SSID: %s", device_settings::wifiSsid());
}

void stopProvisioningSession(const char* result, bool restore_wifi) {
    bool should_stop = false;
    {
        platform_lock::ScopedLock lock(state.lock);
        if (lock.locked()) {
            should_stop = state.managerInitialized;
            state.active = false;
            state.sessionDeadlineUs = 0;
            setStageLocked(result);
            setLastResultLocked(result);
            clearPendingCredentialsLocked();
        }
    }

    if (should_stop) {
        wifi_prov_mgr_stop_provisioning();
    }
    if (restore_wifi) {
        restoreConfiguredWifi();
    }
}

void scheduleRebootWithProvisionedWifi() {
    char ssid[33] = {};
    char password[65] = {};
    {
        platform_lock::ScopedLock lock(state.lock);
        if (!lock.locked() || state.pendingSsid[0] == '\0') {
            return;
        }
        copyString(ssid, sizeof(ssid), state.pendingSsid);
        copyString(password, sizeof(password), state.pendingPassword);
    }

    device_settings::Settings next = device_settings::get();
    copyString(next.wifiSsid, sizeof(next.wifiSsid), ssid);
    copyString(next.wifiPassword, sizeof(next.wifiPassword), password);

    bool reboot_required = false;
    char message[160] = {};
    if (!device_settings::save(next, &reboot_required, message, sizeof(message))) {
        ESP_LOGE(TAG, "Failed to save provisioned WiFi settings: %s", message);
        platform_lock::ScopedLock lock(state.lock);
        if (lock.locked()) {
            state.active = false;
            state.rebootPending = false;
            state.sessionDeadlineUs = 0;
            setStageLocked("save-failed");
            setLastResultLocked("save-failed");
        }
        return;
    }

    {
        platform_lock::ScopedLock lock(state.lock);
        if (lock.locked()) {
            state.active = false;
            state.rebootPending = true;
            state.sessionDeadlineUs = 0;
            state.rebootAtUs = esp_timer_get_time() +
                               (static_cast<int64_t>(app_config::kProvisioningRebootDelayMs) * 1000);
            setStageLocked("connected");
            setLastResultLocked("connected");
        }
    }

    ESP_LOGI(TAG, "Provisioned WiFi saved to NVS; rebooting soon");
}

void eventHandler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_PROV_EVENT) {
        if (event_id == WIFI_PROV_START) {
            platform_lock::ScopedLock lock(state.lock);
            if (lock.locked()) {
                state.active = true;
                setStageLocked("waiting");
                setLastResultLocked("running");
            }
            ESP_LOGI(TAG, "BLE provisioning started: %s", state.serviceName);
        } else if (event_id == WIFI_PROV_CRED_RECV) {
            auto* cfg = static_cast<wifi_sta_config_t*>(event_data);
            platform_lock::ScopedLock lock(state.lock);
            if (lock.locked()) {
                copyString(state.pendingSsid,
                           sizeof(state.pendingSsid),
                           reinterpret_cast<const char*>(cfg->ssid));
                copyString(state.pendingPassword,
                           sizeof(state.pendingPassword),
                           reinterpret_cast<const char*>(cfg->password));
                state.credentialsReceived = true;
                state.credentialsAccepted = false;
                setStageLocked("connecting");
                setLastResultLocked("received");
            }
            ESP_LOGI(TAG, "Provisioning received SSID=%s", state.pendingSsid);
        } else if (event_id == WIFI_PROV_CRED_SUCCESS) {
            platform_lock::ScopedLock lock(state.lock);
            if (lock.locked()) {
                state.credentialsAccepted = true;
                setStageLocked("connecting");
                setLastResultLocked("auth-ok");
            }
            ESP_LOGI(TAG, "Provisioning credentials accepted");
        } else if (event_id == WIFI_PROV_CRED_FAIL) {
            platform_lock::ScopedLock lock(state.lock);
            if (lock.locked()) {
                setStageLocked("failed");
                setLastResultLocked("connect-failed");
                state.credentialsAccepted = false;
            }
            ESP_LOGW(TAG, "Provisioning failed to connect to requested WiFi");
        } else if (event_id == WIFI_PROV_END) {
            platform_lock::ScopedLock lock(state.lock);
            if (lock.locked() && state.managerInitialized) {
                wifi_prov_mgr_deinit();
                state.managerInitialized = false;
            }
            ESP_LOGI(TAG, "Provisioning manager stopped");
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        bool should_commit = false;
        {
            platform_lock::ScopedLock lock(state.lock);
            if (lock.locked()) {
                should_commit = state.active &&
                                state.credentialsReceived &&
                                state.credentialsAccepted &&
                                !state.rebootPending;
            }
        }
        if (should_commit) {
            scheduleRebootWithProvisionedWifi();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        platform_lock::ScopedLock lock(state.lock);
        if (lock.locked() && state.active && state.credentialsReceived && !state.rebootPending) {
            setStageLocked("connecting");
        }
    }
}

esp_err_t startProvisioningSession() {
    {
        platform_lock::ScopedLock lock(state.lock);
        if (lock.locked()) {
            if (state.active || state.rebootPending) {
                return ESP_OK;
            }
            clearPendingCredentialsLocked();
            state.rebootAtUs = 0;
            state.active = true;
            state.rebootPending = false;
            state.sessionDeadlineUs = esp_timer_get_time() +
                                      (static_cast<int64_t>(app_config::kProvisioningSessionMs) * 1000);
            setStageLocked("starting");
            setLastResultLocked("running");
        }
    }

    wifi_prov_mgr_config_t prov_config = {};
    prov_config.scheme = wifi_prov_scheme_ble;
    prov_config.scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;
    prov_config.app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;
    esp_err_t err = wifi_prov_mgr_init(prov_config);
    if (err != ESP_OK) {
        platform_lock::ScopedLock lock(state.lock);
        if (lock.locked()) {
            state.active = false;
            state.sessionDeadlineUs = 0;
            setStageLocked("init-failed");
            setLastResultLocked("init-failed");
        }
        return err;
    }

    {
        platform_lock::ScopedLock lock(state.lock);
        if (lock.locked()) {
            state.managerInitialized = true;
            buildProvisioningServiceName(state.serviceName, sizeof(state.serviceName));
        }
    }

    wifi_prov_mgr_reset_provisioning();
    char service_name[40] = {};
    {
        platform_lock::ScopedLock lock(state.lock);
        if (lock.locked()) {
            copyString(service_name, sizeof(service_name), state.serviceName);
        }
    }

    const wifi_prov_security1_params_t* security_params = kProvisioningPop;
    err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1,
                                           security_params,
                                           service_name,
                                           nullptr);
    if (err != ESP_OK) {
        platform_lock::ScopedLock lock(state.lock);
        if (lock.locked()) {
            state.active = false;
            state.managerInitialized = false;
            state.sessionDeadlineUs = 0;
            setStageLocked("start-failed");
            setLastResultLocked("start-failed");
        }
        wifi_prov_mgr_deinit();
        return err;
    }

    ESP_LOGI(TAG,
             "BLE provisioning window opened for %lus: service=%s security=1 pop=%s",
             static_cast<unsigned long>(app_config::kProvisioningSessionMs / 1000),
             service_name,
             kProvisioningPop);
    restoreConfiguredWifi();
    return ESP_OK;
}

void handleButton(int64_t now_us) {
    const bool pressed =
        gpio_get_level(static_cast<gpio_num_t>(app_config::kProvisioningButtonPin)) == 0;
    bool should_start = false;

    {
        platform_lock::ScopedLock lock(state.lock);
        if (!lock.locked()) {
            return;
        }

        if (pressed) {
            if (!state.buttonHeld) {
                state.buttonHeld = true;
                state.longPressConsumed = false;
                state.buttonPressedAtUs = now_us;
            } else if (!state.longPressConsumed &&
                       (now_us - state.buttonPressedAtUs) >=
                           (static_cast<int64_t>(app_config::kProvisioningButtonLongPressMs) * 1000) &&
                       !state.active &&
                       !state.rebootPending) {
                state.longPressConsumed = true;
                should_start = true;
            }
        } else {
            state.buttonHeld = false;
            state.longPressConsumed = false;
            state.buttonPressedAtUs = 0;
        }
    }

    if (should_start) {
        const esp_err_t err = startProvisioningSession();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start BLE provisioning: %s", esp_err_to_name(err));
        }
    }
}

void provisioningTask(void*) {
    state.taskRunning = true;
    ESP_LOGI(TAG,
             "Provisioning button task started on GPIO%d (%s button)",
             static_cast<int>(app_config::kProvisioningButtonPin),
             app_config::kBoardName);

    while (true) {
        const int64_t now_us = esp_timer_get_time();
        handleButton(now_us);

        bool should_timeout = false;
        bool should_reboot = false;
        {
            platform_lock::ScopedLock lock(state.lock);
            if (lock.locked()) {
                should_timeout = state.active &&
                                 state.sessionDeadlineUs > 0 &&
                                 now_us >= state.sessionDeadlineUs &&
                                 !state.rebootPending;
                should_reboot = state.rebootPending &&
                                state.rebootAtUs > 0 &&
                                now_us >= state.rebootAtUs;
            }
        }

        if (should_timeout) {
            ESP_LOGW(TAG, "Provisioning window expired");
            stopProvisioningSession("timed-out", true);
        }
        if (should_reboot) {
            ESP_LOGI(TAG, "Rebooting after successful BLE provisioning");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }

        vTaskDelay(pdMS_TO_TICKS(app_config::kProvisioningLoopIntervalMs));
    }
}

}  // namespace

namespace platform_provisioning {

esp_err_t init() {
    if (state.initialized) {
        return ESP_OK;
    }

    gpio_config_t button_config = {};
    button_config.pin_bit_mask = 1ULL << static_cast<uint64_t>(app_config::kProvisioningButtonPin);
    button_config.mode = GPIO_MODE_INPUT;
    button_config.pull_up_en = GPIO_PULLUP_DISABLE;
    button_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    button_config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG, "button gpio_config failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            eventHandler,
                                                            nullptr,
                                                            &wifi_event_instance),
                        TAG,
                        "register WIFI_EVENT provisioning handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            eventHandler,
                                                            nullptr,
                                                            &ip_event_instance),
                        TAG,
                        "register IP_EVENT provisioning handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_PROV_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            eventHandler,
                                                            nullptr,
                                                            &prov_event_instance),
                        TAG,
                        "register WIFI_PROV_EVENT handler failed");

    {
        platform_lock::ScopedLock lock(state.lock);
        if (lock.locked()) {
            buildProvisioningServiceName(state.serviceName, sizeof(state.serviceName));
        }
    }

    const BaseType_t created = xTaskCreate(provisioningTask,
                                           "ble_provision",
                                           app_config::kProvisioningTaskStackBytes,
                                           nullptr,
                                           1,
                                           nullptr);
    if (created != pdPASS) {
        return ESP_FAIL;
    }

    {
        platform_lock::ScopedLock lock(state.lock);
        if (lock.locked()) {
            state.initialized = true;
        }
    }

    ESP_LOGI(TAG,
             "Long-press BLE provisioning enabled: button=GPIO%d hold=%lums window=%lus",
             static_cast<int>(app_config::kProvisioningButtonPin),
             static_cast<unsigned long>(app_config::kProvisioningButtonLongPressMs),
             static_cast<unsigned long>(app_config::kProvisioningSessionMs / 1000));
    return ESP_OK;
}

Status getStatus() {
    Status status{};
    status.buttonGpio = static_cast<int>(app_config::kProvisioningButtonPin);

    platform_lock::ScopedLock lock(state.lock);
    if (!lock.locked()) {
        copyString(status.lastResult, sizeof(status.lastResult), "lock-failed");
        return status;
    }

    status.initialized = state.initialized;
    status.active = state.active;
    status.credentialsReceived = state.credentialsReceived;
    status.rebootPending = state.rebootPending;
    copyString(status.stage, sizeof(status.stage), state.stage);
    copyString(status.lastResult, sizeof(status.lastResult), state.lastResult);
    copyString(status.serviceName, sizeof(status.serviceName), state.serviceName);
    copyString(status.pendingSsid, sizeof(status.pendingSsid), state.pendingSsid);

    if (state.active && state.sessionDeadlineUs > 0) {
        const int64_t remaining_us = state.sessionDeadlineUs - esp_timer_get_time();
        status.remainingMs = remaining_us > 0
            ? static_cast<uint32_t>(remaining_us / 1000)
            : 0;
    }

    return status;
}

}  // namespace platform_provisioning
