/****************************************************************************
 * Kiri Bridge
 * https://kiri.dkt.moe
 * https://github.com/DickyT/kiri-homekit
 * Copyright (c) 2026. Licensed under GPL-3.0.
 ****************************************************************************/
#pragma once
#include <cstddef>
#include <cstdint>

namespace qingping {
struct Reading {
    uint8_t address[6] = {};
    int16_t temperatureTenthsC = 0;
    uint16_t humidityTenthsPercent = 0;
    int batteryPercent = -1;
};
// Parses Qingping FDCD service data; returns false for incomplete or invalid frames.
bool parseFdcd(const uint8_t* data, size_t length, Reading* out);
}
