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

#include "esp_err.h"

namespace homekit_bridge {

struct Status {
    bool enabled = false;
    bool started = false;
    int pairedControllers = 0;
    const char* accessoryName = "";
    const char* model = "";
    const char* firmwareRevision = "";
    const char* setupCode = "";
    const char* setupId = "";
    const char* setupPayload = "";
    const char* lastEvent = "";
    const char* lastError = "";
};

esp_err_t start();
Status getStatus();
void markDatabaseChanged();
void syncFromMock();

}  // namespace homekit_bridge
