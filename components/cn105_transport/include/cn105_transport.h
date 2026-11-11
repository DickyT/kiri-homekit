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

#include "cn105_core.h"
#include "esp_err.h"

namespace cn105_transport {

struct Status {
    bool taskRunning = false;
    bool connected = false;
    uint32_t connectAttempts = 0;
    uint32_t pollCycles = 0;
    uint32_t rxPackets = 0;
    uint32_t rxErrors = 0;
    uint32_t txPackets = 0;
    uint32_t setsPending = 0;
    uint32_t remoteTemperatureAcks = 0;
    uint32_t remoteTemperatureAckId = 0;
    char phase[24] = "idle";
    char lastError[96] = "";
};

struct ApplyResult {
    bool confirmed = false;
    uint8_t attempts = 0;
    char message[96] = "";
};

struct RefreshResult {
    bool completed = false;
    uint8_t receivedMask = 0;
    char message[96] = "";
};

esp_err_t start();
Status getStatus();
bool queueSetCommand(const cn105_core::SetCommand& command);
bool queueRemoteTemperature(bool enabled, cn105_core::HalfDegreesC roomHalfC, uint32_t* requestId = nullptr);
bool queueSetCommandAndConfirm(const cn105_core::SetCommand& command, ApplyResult* result);
bool requestFullInfoPollAndWait(RefreshResult* result);

}  // namespace cn105_transport
