/****************************************************************************
 * Kiri Bridge
 * https://kiri.dkt.moe
 * https://github.com/DickyT/kiri-homekit
 * Copyright (c) 2026. Licensed under GPL-3.0.
 ****************************************************************************/
#include "qingping_parser.h"

namespace qingping {
bool parseFdcd(const uint8_t* data, size_t length, Reading* out) {
    if (!data || !out || length < 8 || (data[0] & 0x3f) != 0x08) return false;
    Reading result{};
    for (size_t i = 0; i < 6; ++i) result.address[i] = data[7 - i];
    bool temperatureFound = false;
    size_t pos = 8;
    while (pos < length) {
        if (length - pos < 2) return false;
        const uint8_t type = data[pos++], size = data[pos++];
        if (size > length - pos) return false;
        if (type == 0x01 && size == 4) {
            const uint16_t t = static_cast<uint16_t>(data[pos]) | (static_cast<uint16_t>(data[pos + 1]) << 8);
            const int signedT = t >= 0x8000 ? static_cast<int>(t) - 0x10000 : t;
            const uint16_t humidity = static_cast<uint16_t>(data[pos + 2]) | (static_cast<uint16_t>(data[pos + 3]) << 8);
            if (signedT < -400 || signedT > 850 || humidity > 1000) return false;
            result.temperatureTenthsC = static_cast<int16_t>(signedT);
            result.humidityTenthsPercent = humidity;
            temperatureFound = true;
        } else if (type == 0x02 && size == 1 && data[pos] <= 100) {
            result.batteryPercent = data[pos];
        }
        pos += size;
    }
    if (!temperatureFound) return false;
    *out = result;
    return true;
}
}
