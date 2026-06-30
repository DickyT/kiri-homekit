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

#include "cn105_transport.h"

#include "app_config.h"
#include "cn105_core.h"
#include "device_settings.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

namespace {

const char* TAG = "cn105_transport";
constexpr uint8_t kConfirmInfoAttempts = 3;
constexpr uint32_t kConfirmInfoRetryMs = 100;
constexpr uint32_t kConfirmInfoFinalWaitMs = 500;
constexpr uint32_t kFullInfoRefreshTimeoutMs = 3500;
constexpr uint8_t kInfoMaskControl = 0x01;
constexpr uint8_t kInfoMaskRoom = 0x02;
constexpr uint8_t kInfoMaskRuntime = 0x04;
constexpr uint8_t kInfoMaskAll = kInfoMaskControl | kInfoMaskRoom | kInfoMaskRuntime;

enum class Phase {
    kDisconnected,
    kConnecting,
    kIdle,
    kAwaitingInfoResponse,
    kAwaitingSetAck,
};

struct TransportState {
    Phase phase = Phase::kDisconnected;
    bool taskRunning = false;
    bool connected = false;
    uint32_t connectAttempts = 0;
    uint32_t pollCycles = 0;
    uint32_t rxPackets = 0;
    uint32_t rxErrors = 0;
    uint32_t txPackets = 0;
    uint32_t setAcks = 0;
    uint32_t controlInfoUpdates = 0;
    uint32_t roomInfoUpdates = 0;
    uint32_t runtimeInfoUpdates = 0;
    int64_t lastSetAckUs = 0;
    int64_t lastControlInfoUs = 0;
    bool immediateControlInfoRequested = false;
    uint8_t pendingInfoCodes[3] = {};
    uint8_t pendingInfoCount = 0;
    uint8_t pendingInfoIndex = 0;
    char lastError[96] = "";

    uint8_t rxBuf[cn105_core::kPacketLen] = {};
    size_t rxIndex = 0;
    size_t rxExpected = 0;
    int64_t rxLastByteUs = 0;

    uint8_t awaitingInfoCode = 0;
    bool forceFastPolling = false;
    int64_t requestSentUs = 0;
    int64_t lastPollUs = 0;
    int64_t lastConnectAttemptUs = 0;
};

TransportState ts;
QueueHandle_t set_queue = nullptr;

struct QueuedSetCommand {
    bool optimistic = true;
    bool hasPower = false;
    char power[16] = "";
    bool hasMode = false;
    char mode[16] = "";
    bool hasTemperatureF = false;
    int temperatureF = 77;
    bool hasFan = false;
    char fan[16] = "";
    bool hasVane = false;
    char vane[16] = "";
    bool hasWideVane = false;
    char wideVane[32] = "";
};

const char* phaseName(Phase p) {
    switch (p) {
        case Phase::kDisconnected: return "disconnected";
        case Phase::kConnecting: return "connecting";
        case Phase::kIdle: return "idle";
        case Phase::kAwaitingInfoResponse: return "awaiting-info";
        case Phase::kAwaitingSetAck: return "awaiting-set-ack";
    }
    return "unknown";
}

int64_t nowUs() {
    return static_cast<int64_t>(esp_timer_get_time());
}

bool elapsedMs(int64_t since_us, uint32_t ms) {
    return (nowUs() - since_us) >= static_cast<int64_t>(ms) * 1000;
}

void setError(const char* msg) {
    std::strncpy(ts.lastError, msg, sizeof(ts.lastError) - 1);
    ts.lastError[sizeof(ts.lastError) - 1] = '\0';
}

void copyString(char* out, size_t out_len, const char* value) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    if (value == nullptr) {
        out[0] = '\0';
        return;
    }
    std::strncpy(out, value, out_len - 1);
    out[out_len - 1] = '\0';
}

QueuedSetCommand queuedFromCommand(const cn105_core::SetCommand& command, bool optimistic) {
    QueuedSetCommand queued{};
    queued.optimistic = optimistic;
    queued.hasPower = command.hasPower;
    copyString(queued.power, sizeof(queued.power), command.power);
    queued.hasMode = command.hasMode;
    copyString(queued.mode, sizeof(queued.mode), command.mode);
    queued.hasTemperatureF = command.hasTemperatureF;
    queued.temperatureF = command.temperatureF;
    queued.hasFan = command.hasFan;
    copyString(queued.fan, sizeof(queued.fan), command.fan);
    queued.hasVane = command.hasVane;
    copyString(queued.vane, sizeof(queued.vane), command.vane);
    queued.hasWideVane = command.hasWideVane;
    copyString(queued.wideVane, sizeof(queued.wideVane), command.wideVane);
    return queued;
}

cn105_core::SetCommand commandView(const QueuedSetCommand& queued) {
    cn105_core::SetCommand command{};
    command.hasPower = queued.hasPower;
    command.power = queued.hasPower ? queued.power : nullptr;
    command.hasMode = queued.hasMode;
    command.mode = queued.hasMode ? queued.mode : nullptr;
    command.hasTemperatureF = queued.hasTemperatureF;
    command.temperatureF = queued.temperatureF;
    command.hasFan = queued.hasFan;
    command.fan = queued.hasFan ? queued.fan : nullptr;
    command.hasVane = queued.hasVane;
    command.vane = queued.hasVane ? queued.vane : nullptr;
    command.hasWideVane = queued.hasWideVane;
    command.wideVane = queued.hasWideVane ? queued.wideVane : nullptr;
    return command;
}

bool reportMismatch(char* message,
                    size_t message_len,
                    const char* field,
                    const char* requested,
                    const char* actual) {
    if (message != nullptr && message_len > 0) {
        std::snprintf(message,
                      message_len,
                      "CN105 mismatch: %s requested=%s actual=%s",
                      field,
                      requested != nullptr ? requested : "",
                      actual != nullptr ? actual : "");
    }
    return false;
}

bool reportWideVaneMismatch(char* message, size_t message_len, const char* requested, const char* actual) {
    if (message != nullptr && message_len > 0) {
        std::snprintf(message,
                      message_len,
                      "Left/right airflow not confirmed (%s -> %s). Disable it in AC Capabilities if unsupported.",
                      requested != nullptr ? requested : "",
                      actual != nullptr ? actual : "");
    }
    return false;
}

bool reportMismatch(char* message, size_t message_len, const char* field, int requested, int actual) {
    if (message != nullptr && message_len > 0) {
        std::snprintf(message, message_len, "CN105 mismatch: %s requested=%d actual=%d", field, requested, actual);
    }
    return false;
}

bool commandMatchesState(const cn105_core::SetCommand& command, char* mismatch, size_t mismatch_len) {
    const cn105_core::MockState state = cn105_core::getMockState();
    if (command.hasPower && command.power != nullptr && std::strcmp(state.power, command.power) != 0) {
        return reportMismatch(mismatch, mismatch_len, "power", command.power, state.power);
    }
    if (command.hasMode && command.mode != nullptr && std::strcmp(state.mode, command.mode) != 0) {
        return reportMismatch(mismatch, mismatch_len, "mode", command.mode, state.mode);
    }
    if (command.hasTemperatureF && state.targetTemperatureF != command.temperatureF) {
        return reportMismatch(mismatch, mismatch_len, "temperature_f", command.temperatureF, state.targetTemperatureF);
    }
    if (command.hasFan && command.fan != nullptr && std::strcmp(state.fan, command.fan) != 0) {
        return reportMismatch(mismatch, mismatch_len, "fan", command.fan, state.fan);
    }
    if (command.hasVane && command.vane != nullptr && std::strcmp(state.vane, command.vane) != 0) {
        return reportMismatch(mismatch, mismatch_len, "vane", command.vane, state.vane);
    }
    if (command.hasWideVane && command.wideVane != nullptr && std::strcmp(state.wideVane, command.wideVane) != 0) {
        return reportWideVaneMismatch(mismatch, mismatch_len, command.wideVane, state.wideVane);
    }
    return true;
}

void requestImmediateControlInfo() {
    ts.immediateControlInfoRequested = true;
}

void scheduleFullInfoPoll() {
    ts.pendingInfoCodes[0] = 0x02;
    ts.pendingInfoCodes[1] = 0x03;
    ts.pendingInfoCodes[2] = 0x06;
    ts.pendingInfoCount = 3;
    ts.pendingInfoIndex = 0;
}

bool uartSend(const uint8_t* data, size_t len) {
    const int written = uart_write_bytes(app_config::kCn105UartPort, data, len);
    if (written < 0 || static_cast<size_t>(written) != len) {
        setError("uart_write_bytes failed");
        return false;
    }
    ts.txPackets++;
    return true;
}

void sendConnect() {
    cn105_core::Packet packet{};
    char error[96] = {};
    if (!cn105_core::buildConnectPacket(&packet, error, sizeof(error))) {
        setError(error);
        return;
    }
    if (uartSend(packet.bytes, packet.length)) {
        ts.phase = Phase::kConnecting;
        ts.requestSentUs = nowUs();
        ts.connectAttempts++;
        ESP_LOGI(TAG, "CONNECT sent (attempt %lu)", static_cast<unsigned long>(ts.connectAttempts));
    }
}

void sendInfoRequest(uint8_t code) {
    cn105_core::Packet packet{};
    char error[96] = {};
    if (!cn105_core::buildInfoPacket(code, &packet, error, sizeof(error))) {
        setError(error);
        return;
    }
    if (uartSend(packet.bytes, packet.length)) {
        ts.phase = Phase::kAwaitingInfoResponse;
        ts.awaitingInfoCode = code;
        ts.requestSentUs = nowUs();
    }
}

bool sendNextPendingInfo() {
    if (ts.pendingInfoIndex >= ts.pendingInfoCount) {
        ts.pendingInfoCount = 0;
        ts.pendingInfoIndex = 0;
        return false;
    }
    const uint8_t code = ts.pendingInfoCodes[ts.pendingInfoIndex++];
    sendInfoRequest(code);
    return true;
}

void sendSetPacket(const cn105_core::SetCommand& command, bool optimistic) {
    cn105_core::Packet packet{};
    char error[96] = {};
    if (!cn105_core::buildSetPacket(command, &packet, error, sizeof(error))) {
        setError(error);
        ESP_LOGW(TAG, "SET build failed: %s", error);
        return;
    }
    if (uartSend(packet.bytes, packet.length)) {
        ts.phase = Phase::kAwaitingSetAck;
        ts.requestSentUs = nowUs();
        if (command.hasPower && command.power != nullptr && std::strcmp(command.power, "ON") == 0) {
            ts.forceFastPolling = true;
        }
        if (optimistic) {
            // HomeKit remains optimistic so the Home app feels responsive; WebUI
            // uses the confirm path and waits for a real INFO update instead.
            if (!cn105_core::applySetPacketToMock(packet.bytes, packet.length, error, sizeof(error))) {
                ESP_LOGW(TAG, "SET optimistic state apply failed: %s", error);
            }
        }
        ESP_LOGI(TAG, "SET sent");
    }
}

bool shouldUseFastPolling() {
    if (ts.forceFastPolling) {
        return true;
    }
    const cn105_core::MockState state = cn105_core::getMockState();
    return state.power != nullptr && std::strcmp(state.power, "ON") == 0;
}

uint32_t currentPollIntervalMs() {
    return shouldUseFastPolling() ? device_settings::pollIntervalActiveMs()
                                  : device_settings::pollIntervalOffMs();
}

size_t expectedLenForCommand(uint8_t command) {
    // CONNECT ACK packets can be 7 bytes long (payload length 0x01), so ACKs
    // must be parsed using the standard "payload length + 6" framing logic.
    (void) command;
    return 0;
}

void resetRx() {
    ts.rxIndex = 0;
    ts.rxExpected = 0;
}

void handlePacket(const uint8_t* bytes, size_t len) {
    ts.rxPackets++;

    const uint8_t cmd = bytes[1];
    const uint8_t expected_checksum = cn105_core::checksum(bytes, len - 1);
    if (bytes[len - 1] != expected_checksum) {
        ts.rxErrors++;
        setError("rx checksum mismatch");
        ESP_LOGW(TAG, "RX checksum mismatch: got=%02X expected=%02X", bytes[len - 1], expected_checksum);
        return;
    }

    switch (cmd) {
        case 0x7A:
        case 0x7B:
            if (ts.phase == Phase::kConnecting) {
                ts.connected = true;
                ts.phase = Phase::kIdle;
                ts.forceFastPolling = true;
                ts.lastPollUs = 0;
                scheduleFullInfoPoll();
                cn105_core::setConnected(true);
                setError("");
                ESP_LOGI(TAG, "CONNECT acknowledged (0x%02X)", cmd);
            }
            break;
        case 0x62:
            if (ts.phase == Phase::kAwaitingInfoResponse) {
                if (cn105_core::applyInfoResponseToState(bytes, len) && len > 5) {
                    switch (bytes[5]) {
                        case 0x02:
                            ts.controlInfoUpdates++;
                            ts.lastControlInfoUs = nowUs();
                            break;
                        case 0x03:
                            ts.roomInfoUpdates++;
                            break;
                        case 0x06:
                            ts.runtimeInfoUpdates++;
                            break;
                        default:
                            break;
                    }
                }
                if (ts.awaitingInfoCode == 0x02) {
                    ts.forceFastPolling = false;
                }
                ts.phase = Phase::kIdle;
            }
            break;
        case 0x61:
            if (ts.phase == Phase::kAwaitingSetAck) {
                ts.setAcks++;
                ts.lastSetAckUs = nowUs();
                ts.phase = Phase::kIdle;
                ts.lastPollUs = 0;
                ESP_LOGI(TAG, "SET acknowledged");
            }
            break;
        default:
            ESP_LOGD(TAG, "RX unexpected command 0x%02X", cmd);
            break;
    }
}

void processRxByte(uint8_t byte) {
    if (ts.rxIndex == 0 && byte != 0xFC) {
        return;
    }

    ts.rxBuf[ts.rxIndex++] = byte;
    ts.rxLastByteUs = nowUs();

    if (ts.rxIndex == 2) {
        const size_t short_len = expectedLenForCommand(byte);
        if (short_len > 0) {
            ts.rxExpected = short_len;
        }
    }

    if (ts.rxIndex == 5 && ts.rxExpected == 0) {
        ts.rxExpected = static_cast<size_t>(ts.rxBuf[4]) + 6;
        if (ts.rxExpected > cn105_core::kPacketLen) {
            ESP_LOGW(TAG, "RX expected length %u exceeds max, resetting", static_cast<unsigned>(ts.rxExpected));
            ts.rxErrors++;
            resetRx();
            return;
        }
    }

    if (ts.rxExpected > 0 && ts.rxIndex >= ts.rxExpected) {
        handlePacket(ts.rxBuf, ts.rxIndex);
        resetRx();
    }

    if (ts.rxIndex >= cn105_core::kPacketLen) {
        ts.rxErrors++;
        resetRx();
    }
}

void drainUart() {
    uint8_t byte;
    while (uart_read_bytes(app_config::kCn105UartPort, &byte, 1, 0) == 1) {
        processRxByte(byte);
    }
}

void checkRxTimeout() {
    if (ts.rxIndex > 0 && elapsedMs(ts.rxLastByteUs, app_config::kCn105RxByteTimeoutMs)) {
        ESP_LOGD(TAG, "RX timeout after %u bytes, resetting", static_cast<unsigned>(ts.rxIndex));
        ts.rxErrors++;
        resetRx();
    }
}

void checkResponseTimeout() {
    if (ts.phase == Phase::kConnecting && elapsedMs(ts.requestSentUs, app_config::kCn105ResponseTimeoutMs)) {
        ESP_LOGW(TAG, "CONNECT response timeout");
        ts.phase = Phase::kDisconnected;
        setError("connect timeout");
    }
    if (ts.phase == Phase::kAwaitingInfoResponse && elapsedMs(ts.requestSentUs, app_config::kCn105ResponseTimeoutMs)) {
        ESP_LOGW(TAG, "INFO 0x%02X response timeout", ts.awaitingInfoCode);
        ts.phase = Phase::kIdle;
        setError("info response timeout");
    }
    if (ts.phase == Phase::kAwaitingSetAck && elapsedMs(ts.requestSentUs, app_config::kCn105ResponseTimeoutMs)) {
        ESP_LOGW(TAG, "SET ACK timeout");
        ts.phase = Phase::kIdle;
        setError("set ack timeout");
    }
}

void transportTask(void*) {
    ts.taskRunning = true;
    ESP_LOGI(TAG, "Transport task started");

    while (true) {
        drainUart();
        checkRxTimeout();
        checkResponseTimeout();

        switch (ts.phase) {
            case Phase::kDisconnected:
                if (elapsedMs(ts.lastConnectAttemptUs, app_config::kCn105ConnectRetryMs)) {
                    ts.lastConnectAttemptUs = nowUs();
                    sendConnect();
                }
                break;

            case Phase::kIdle: {
                QueuedSetCommand queued{};
                if (set_queue && xQueueReceive(set_queue, &queued, 0) == pdTRUE) {
                    const cn105_core::SetCommand command = commandView(queued);
                    sendSetPacket(command, queued.optimistic);
                    break;
                }
                if (ts.immediateControlInfoRequested) {
                    ts.immediateControlInfoRequested = false;
                    sendInfoRequest(0x02);
                    break;
                }
                if (sendNextPendingInfo()) {
                    break;
                }
                if (elapsedMs(ts.lastPollUs, currentPollIntervalMs())) {
                    scheduleFullInfoPoll();
                    ts.pollCycles++;
                    ts.lastPollUs = nowUs();
                    sendNextPendingInfo();
                }
                break;
            }

            case Phase::kConnecting:
            case Phase::kAwaitingInfoResponse:
            case Phase::kAwaitingSetAck:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

}  // namespace

namespace cn105_transport {

esp_err_t start() {
    if (ts.taskRunning) {
        return ESP_OK;
    }

    set_queue = xQueueCreate(2, sizeof(QueuedSetCommand));
    if (set_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create SET command queue");
        return ESP_FAIL;
    }

    const BaseType_t ret = xTaskCreate(transportTask, "cn105_tx", app_config::kCn105TransportStackBytes, nullptr, 5, nullptr);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create transport task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

Status getStatus() {
    Status status{};
    status.taskRunning = ts.taskRunning;
    status.connected = ts.connected;
    status.connectAttempts = ts.connectAttempts;
    status.pollCycles = ts.pollCycles;
    status.rxPackets = ts.rxPackets;
    status.rxErrors = ts.rxErrors;
    status.txPackets = ts.txPackets;
    status.setsPending = set_queue ? static_cast<uint32_t>(uxQueueMessagesWaiting(set_queue)) : 0;
    std::strncpy(status.phase, phaseName(ts.phase), sizeof(status.phase) - 1);
    std::strncpy(status.lastError, ts.lastError, sizeof(status.lastError) - 1);
    return status;
}

bool queueSetCommand(const cn105_core::SetCommand& command) {
    if (set_queue == nullptr) {
        return false;
    }
    const QueuedSetCommand queued = queuedFromCommand(command, true);
    return xQueueSend(set_queue, &queued, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool queueSetCommandAndConfirm(const cn105_core::SetCommand& command, ApplyResult* result) {
    if (result != nullptr) {
        *result = {};
    }
    if (set_queue == nullptr || !ts.taskRunning) {
        if (result != nullptr) {
            copyString(result->message, sizeof(result->message), "CN105 transport is not running");
        }
        return false;
    }
    if (!ts.connected) {
        if (result != nullptr) {
            copyString(result->message, sizeof(result->message), "CN105 transport is not connected");
        }
        return false;
    }

    const QueuedSetCommand queued = queuedFromCommand(command, false);
    const uint32_t ack_baseline = ts.setAcks;
    if (xQueueSend(set_queue, &queued, pdMS_TO_TICKS(150)) != pdTRUE) {
        if (result != nullptr) {
            copyString(result->message, sizeof(result->message), "transport queue full");
        }
        return false;
    }

    const int64_t ack_deadline_us = nowUs() + static_cast<int64_t>(app_config::kCn105ResponseTimeoutMs + 200) * 1000;
    bool saw_ack = false;
    while (nowUs() < ack_deadline_us) {
        if (ts.setAcks != ack_baseline) {
            saw_ack = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!saw_ack) {
        if (result != nullptr) {
            copyString(result->message, sizeof(result->message), "CN105 SET ACK timeout");
        }
        return false;
    }

    for (uint8_t info_attempt = 1; info_attempt <= kConfirmInfoAttempts; ++info_attempt) {
        if (result != nullptr) {
            result->attempts = info_attempt;
        }
        char mismatch[sizeof(result->message)] = {};
        const uint32_t info_baseline = ts.controlInfoUpdates;
        requestImmediateControlInfo();
        const uint32_t wait_ms = info_attempt == kConfirmInfoAttempts ? kConfirmInfoFinalWaitMs : kConfirmInfoRetryMs;
        const int64_t info_deadline_us = nowUs() + static_cast<int64_t>(wait_ms) * 1000;
        while (nowUs() < info_deadline_us) {
            if (ts.controlInfoUpdates != info_baseline && ts.lastControlInfoUs >= ts.lastSetAckUs) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (ts.controlInfoUpdates != info_baseline && ts.lastControlInfoUs >= ts.lastSetAckUs) {
            if (commandMatchesState(command, mismatch, sizeof(mismatch))) {
                if (result != nullptr) {
                    result->confirmed = true;
                    copyString(result->message, sizeof(result->message), "CN105 state confirmed");
                }
                return true;
            }
        }
        if (info_attempt == kConfirmInfoAttempts && mismatch[0] != '\0' && result != nullptr) {
            copyString(result->message, sizeof(result->message), mismatch);
        }
    }

    if (result != nullptr && result->message[0] == '\0') {
        copyString(result->message, sizeof(result->message), "CN105 did not confirm the requested state");
    }
    return false;
}

bool requestFullInfoPollAndWait(RefreshResult* result) {
    if (result != nullptr) {
        *result = {};
    }
    if (!ts.taskRunning || !ts.connected) {
        if (result != nullptr) {
            copyString(result->message, sizeof(result->message), "CN105 transport is not connected");
        }
        return false;
    }

    const uint32_t control_baseline = ts.controlInfoUpdates;
    const uint32_t room_baseline = ts.roomInfoUpdates;
    const uint32_t runtime_baseline = ts.runtimeInfoUpdates;
    scheduleFullInfoPoll();

    const int64_t deadline_us = nowUs() + static_cast<int64_t>(kFullInfoRefreshTimeoutMs) * 1000;
    while (nowUs() < deadline_us) {
        uint8_t mask = 0;
        if (ts.controlInfoUpdates != control_baseline) {
            mask |= kInfoMaskControl;
        }
        if (ts.roomInfoUpdates != room_baseline) {
            mask |= kInfoMaskRoom;
        }
        if (ts.runtimeInfoUpdates != runtime_baseline) {
            mask |= kInfoMaskRuntime;
        }
        if (result != nullptr) {
            result->receivedMask = mask;
        }
        if ((mask & kInfoMaskAll) == kInfoMaskAll) {
            if (result != nullptr) {
                result->completed = true;
                copyString(result->message, sizeof(result->message), "CN105 full info refresh completed");
            }
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (result != nullptr) {
        copyString(result->message, sizeof(result->message), "CN105 full info refresh timed out");
    }
    return false;
}

}  // namespace cn105_transport
