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

#include "cn105_core.h"

#include "esp_log.h"
#include "platform_lock.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

const char* TAG = "cn105_core";

static const int PACKET_LEN = 22;
static const int CONNECT_LEN = 8;
static const int HEADER_LEN = 8;
static const int INFOHEADER_LEN = 5;

// Standard CN105 handshake. Byte 1 can be changed to 0x5B for installer mode later.
static const uint8_t CONNECT[CONNECT_LEN] = {
    0xFC, 0x5A, 0x01, 0x30, 0x02, 0xCA, 0x01, 0xA8
};

// SET packet prefix. The following 16 data bytes describe which fields changed and their new values.
static const uint8_t HEADER[HEADER_LEN] = {
    0xFC, 0x41, 0x01, 0x30, 0x10, 0x01, 0x00, 0x00
};

// INFO request prefix. Byte 5 carries the specific info code, then zero padding and checksum.
static const uint8_t INFOHEADER[INFOHEADER_LEN] = {
    0xFC, 0x42, 0x01, 0x30, 0x10
};

// SET byte 6 bit masks: power, mode, temperature, fan, up/down airflow.
// Mitsubishi calls this field "vane"; physically it drives the horizontal flap.
static const uint8_t CONTROL_PACKET_1[5] = {
    0x01, 0x02, 0x04, 0x08, 0x10
};

// SET byte 7 bit masks: left/right airflow.
// Mitsubishi calls this field "wide vane"; physically it drives the vertical vanes.
static const uint8_t CONTROL_PACKET_2[1] = {
    0x01
};

static const uint8_t POWER[2] = { 0x00, 0x01 };
static const char* POWER_MAP[2] = { "OFF", "ON" };

static const uint8_t MODE[5] = { 0x01, 0x02, 0x03, 0x07, 0x08 };
static const char* MODE_MAP[5] = { "HEAT", "DRY", "COOL", "FAN", "AUTO" };

// Legacy temperature is whole Celsius only and reversed: 0x00 = 31C, 0x0F = 16C.
// Encoding B preserves the indoor unit's native half-Celsius precision.
static const uint8_t TEMP[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};
static const int TEMP_MAP[16] = {
    31, 30, 29, 28, 27, 26, 25, 24,
    23, 22, 21, 20, 19, 18, 17, 16
};

static const uint8_t FAN[6] = { 0x00, 0x01, 0x02, 0x03, 0x05, 0x06 };
static const char* FAN_MAP[6] = { "AUTO", "QUIET", "1", "2", "3", "4" };

static const uint8_t VANE[7] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07 };
static const char* VANE_MAP[7] = { "AUTO", "1", "2", "3", "4", "5", "SWING" };

static const uint8_t WIDEVANE[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x08, 0x0C, 0x00 };
static const char* WIDEVANE_MAP[8] = { "<<", "<", "|", ">", ">>", "<>", "SWING", "AIRFLOW CONTROL" };

int roomTempFromByte(uint8_t value) {
    return static_cast<int>(value) + 10;
}

cn105_core::MockState mock_state;
char last_packet_hex[cn105_core::kMaxHexLen] = "";
char last_error[96] = "";
platform_lock::RecursiveMutex state_lock;
bool mock_dirty = false;

void setError(char* error, size_t error_len, const char* message) {
    if (error != nullptr && error_len > 0) {
        std::snprintf(error, error_len, "%s", message);
    }
}

bool equals(const char* a, const char* b) {
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

int lookupIndex(const char* const* map, size_t len, const char* value) {
    for (size_t i = 0; i < len; ++i) {
        if (equals(map[i], value)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const char* lookupString(const char* const* map, const uint8_t* values, size_t len, uint8_t value) {
    for (size_t i = 0; i < len; ++i) {
        if (values[i] == value) {
            return map[i];
        }
    }
    return map[0];
}

int lookupInt(const int* map, const uint8_t* values, size_t len, uint8_t value, int fallback) {
    for (size_t i = 0; i < len; ++i) {
        if (values[i] == value) {
            return map[i];
        }
    }
    return fallback;
}

int clampInt(int value, int min, int max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

float fahrenheitToNearestHalfCelsius(int fahrenheit) {
    const float celsius = (static_cast<float>(fahrenheit) - 32.0f) / 1.8f;
    return std::round(celsius * 2.0f) / 2.0f;
}

// Mitsubishi's Fahrenheit remotes use a lookup table rather than plain math.
// Keep Fahrenheit Web/Lua setpoints on the same table as the physical remote.
// Each entry is {fahrenheit, celsius_x10} where celsius_x10 is 0.1C units.
struct FahrenheitEntry { int f; int c10; };
static const FahrenheitEntry F_TO_C[] = {
    {50,100},{51,105},{52,110},{53,115},{54,120},{55,125},{56,135},{57,140},
    {58,145},{59,150},{60,155},{61,160},{62,165},{63,170},{64,175},{65,180},
    {66,185},{67,190},{68,200},{69,210},{70,215},{71,220},{72,225},{73,230},
    {74,235},{75,240},{76,245},{77,250},{78,255},{79,260},{80,265},{81,270},
    {82,275},{83,280},{84,285},{85,290},{86,295},{87,300},{88,305},
};
constexpr int kFTableSize = sizeof(F_TO_C) / sizeof(F_TO_C[0]);

float lookupCelsiusForFahrenheit(int fahrenheit) {
    for (int i = 0; i < kFTableSize; ++i) {
        if (F_TO_C[i].f == fahrenheit) {
            return static_cast<float>(F_TO_C[i].c10) / 10.0f;
        }
    }
    return fahrenheitToNearestHalfCelsius(fahrenheit);
}

int lookupFahrenheitForTargetHalfC(cn105_core::HalfDegreesC half_c) {
    for (int i = 0; i < kFTableSize; ++i) {
        if (F_TO_C[i].c10 == static_cast<int>(half_c) * 5) {
            return F_TO_C[i].f;
        }
    }
    return static_cast<int>(std::lround((static_cast<float>(half_c) / 2.0f) * 1.8f + 32.0f));
}

uint8_t encodeLegacyTemperatureByte(cn105_core::HalfDegreesC half_c) {
    int whole = (static_cast<int>(half_c) + 1) / 2;
    if (whole < 16) whole = 16;
    if (whole > 31) whole = 31;
    return static_cast<uint8_t>(31 - whole);
}

cn105_core::HalfDegreesC decodeTemperatureHalfC(uint8_t legacyByte, uint8_t highPrecisionByte) {
    if (highPrecisionByte != 0x00) {
        return static_cast<cn105_core::HalfDegreesC>(highPrecisionByte) - 128;
    }

    const int celsius = lookupInt(TEMP_MAP, TEMP, 16, legacyByte, 25);
    return static_cast<cn105_core::HalfDegreesC>(celsius * 2);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void rememberPacket(const uint8_t* bytes, size_t len) {
    cn105_core::bytesToHex(bytes, len, last_packet_hex, sizeof(last_packet_hex));
    mock_state.lastPacketHex = last_packet_hex;
}

void rememberError(const char* error) {
    std::snprintf(last_error, sizeof(last_error), "%s", error == nullptr ? "" : error);
    mock_state.lastError = last_error;
}

}  // namespace

namespace cn105_core {

uint8_t checksum(const uint8_t* bytes, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += bytes[i];
    }
    return static_cast<uint8_t>((0xFC - sum) & 0xFF);
}

HalfDegreesC celsiusToHalfDegrees(float celsius) {
    return static_cast<HalfDegreesC>(std::lround(celsius * 2.0f));
}

float halfDegreesToCelsius(HalfDegreesC half_c) {
    return static_cast<float>(half_c) / 2.0f;
}

HalfDegreesC fahrenheitSetpointToHalfDegrees(int fahrenheit) {
    return celsiusToHalfDegrees(lookupCelsiusForFahrenheit(fahrenheit));
}

int halfDegreesToFahrenheitSetpoint(HalfDegreesC half_c) {
    return lookupFahrenheitForTargetHalfC(half_c);
}

float halfDegreesToFahrenheit(HalfDegreesC half_c) {
    return halfDegreesToCelsius(half_c) * 1.8f + 32.0f;
}

bool bytesToHex(const uint8_t* bytes, size_t len, char* out, size_t out_len) {
    if (bytes == nullptr || out == nullptr || out_len == 0) {
        return false;
    }

    size_t offset = 0;
    for (size_t i = 0; i < len; ++i) {
        const int written = std::snprintf(out + offset, out_len - offset, "%s%02X", i == 0 ? "" : " ", bytes[i]);
        if (written < 0 || static_cast<size_t>(written) >= out_len - offset) {
            out[out_len - 1] = '\0';
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool parseHex(const char* hex, uint8_t* out, size_t max_len, size_t* out_len, char* error, size_t error_len) {
    if (hex == nullptr || out == nullptr || out_len == nullptr) {
        setError(error, error_len, "missing hex input");
        return false;
    }

    size_t count = 0;
    int high = -1;
    for (const char* cursor = hex; *cursor != '\0'; ++cursor) {
        const char c = *cursor;
        if (c == ' ' || c == ':' || c == ',' || c == '-' || c == '\n' || c == '\r' || c == '\t') {
            continue;
        }

        const int value = hexValue(c);
        if (value < 0) {
            setError(error, error_len, "hex contains a non-hex character");
            return false;
        }

        if (high < 0) {
            high = value;
            continue;
        }

        if (count >= max_len) {
            setError(error, error_len, "too many bytes for one CN105 packet");
            return false;
        }

        out[count++] = static_cast<uint8_t>((high << 4) | value);
        high = -1;
    }

    if (high >= 0) {
        setError(error, error_len, "hex string has an odd number of digits");
        return false;
    }

    *out_len = count;
    return true;
}

bool buildConnectPacket(Packet* packet, char* error, size_t error_len) {
    if (packet == nullptr) {
        setError(error, error_len, "packet output is null");
        return false;
    }

    std::memset(packet, 0, sizeof(Packet));
    std::memcpy(packet->bytes, CONNECT, CONNECT_LEN);
    packet->bytes[CONNECT_LEN - 1] = checksum(packet->bytes, CONNECT_LEN - 1);
    packet->length = CONNECT_LEN;
    return true;
}

bool buildInfoPacket(uint8_t infoCode, Packet* packet, char* error, size_t error_len) {
    if (packet == nullptr) {
        setError(error, error_len, "packet output is null");
        return false;
    }

    std::memset(packet, 0, sizeof(Packet));
    std::memcpy(packet->bytes, INFOHEADER, INFOHEADER_LEN);
    packet->bytes[5] = infoCode;
    packet->bytes[kPacketLen - 1] = checksum(packet->bytes, kPacketLen - 1);
    packet->length = kPacketLen;
    return true;
}

bool buildSetPacket(const SetCommand& command, Packet* packet, char* error, size_t error_len) {
    if (packet == nullptr) {
        setError(error, error_len, "packet output is null");
        return false;
    }

    std::memset(packet, 0, sizeof(Packet));
    std::memcpy(packet->bytes, HEADER, HEADER_LEN);
    packet->length = kPacketLen;

    if (command.hasPower) {
        const int index = lookupIndex(POWER_MAP, 2, command.power);
        if (index < 0) {
            setError(error, error_len, "unknown power value");
            return false;
        }
        packet->bytes[8] = POWER[index];
        packet->bytes[6] |= CONTROL_PACKET_1[0];
    }

    if (command.hasMode) {
        const int index = lookupIndex(MODE_MAP, 5, command.mode);
        if (index < 0) {
            setError(error, error_len, "unknown mode value");
            return false;
        }
        packet->bytes[9] = MODE[index];
        packet->bytes[6] |= CONTROL_PACKET_1[1];
    }

    if (command.hasTargetTemperature) {
        const HalfDegreesC half_c = static_cast<HalfDegreesC>(
            clampInt(command.targetTemperatureHalfC, kMinTargetHalfC, kMaxTargetHalfC));
        packet->bytes[19] = static_cast<uint8_t>(half_c + 128);
        packet->bytes[10] = encodeLegacyTemperatureByte(half_c);
        packet->bytes[6] |= CONTROL_PACKET_1[2];
    }

    if (command.hasFan) {
        const int index = lookupIndex(FAN_MAP, 6, command.fan);
        if (index < 0) {
            setError(error, error_len, "unknown fan value");
            return false;
        }
        packet->bytes[11] = FAN[index];
        packet->bytes[6] |= CONTROL_PACKET_1[3];
    }

    if (command.hasVane) {
        const int index = lookupIndex(VANE_MAP, 7, command.vane);
        if (index < 0) {
            setError(error, error_len, "unknown up/down airflow value");
            return false;
        }
        packet->bytes[12] = VANE[index];
        packet->bytes[6] |= CONTROL_PACKET_1[4];
    }

    if (command.hasWideVane) {
        const int index = lookupIndex(WIDEVANE_MAP, 8, command.wideVane);
        if (index < 0) {
            setError(error, error_len, "unknown left/right airflow value");
            return false;
        }
        packet->bytes[18] = WIDEVANE[index];
        packet->bytes[7] |= CONTROL_PACKET_2[0];
    }

    if (packet->bytes[6] == 0 && packet->bytes[7] == 0) {
        setError(error, error_len, "SET command does not change any field");
        return false;
    }

    packet->bytes[kPacketLen - 1] = checksum(packet->bytes, kPacketLen - 1);
    return true;
}

bool decodePacket(const uint8_t* bytes, size_t len, DecodedPacket* decoded, char* error, size_t error_len) {
    if (bytes == nullptr || decoded == nullptr) {
        setError(error, error_len, "decode input is null");
        return false;
    }
    if (len < 6) {
        setError(error, error_len, "packet is too short");
        return false;
    }
    if (bytes[0] != 0xFC) {
        setError(error, error_len, "packet does not start with 0xFC");
        return false;
    }

    const uint8_t data_len = bytes[4];
    const size_t expected_len = static_cast<size_t>(data_len) + 6;
    if (len != expected_len) {
        setError(error, error_len, "packet length does not match data length byte");
        return false;
    }

    *decoded = DecodedPacket{};
    decoded->command = bytes[1];
    decoded->dataLength = data_len;
    decoded->checksumOk = bytes[len - 1] == checksum(bytes, len - 1);
    if (!decoded->checksumOk) {
        setError(error, error_len, "checksum mismatch");
        return false;
    }

    switch (decoded->command) {
        case 0x41: {
            std::snprintf(decoded->type, sizeof(decoded->type), "set");
            const HalfDegreesC temp_half_c = decodeTemperatureHalfC(bytes[10], bytes[19]);
            std::snprintf(decoded->summary,
                          sizeof(decoded->summary),
                          "SET flags=%02X/%02X power=%s mode=%s temp=%.1fC fan=%s vane=%s wide=%s",
                          bytes[6],
                          bytes[7],
                          lookupString(POWER_MAP, POWER, 2, bytes[8]),
                          lookupString(MODE_MAP, MODE, 5, bytes[9]),
                          static_cast<double>(halfDegreesToCelsius(temp_half_c)),
                          lookupString(FAN_MAP, FAN, 6, bytes[11]),
                          lookupString(VANE_MAP, VANE, 7, bytes[12]),
                          lookupString(WIDEVANE_MAP, WIDEVANE, 8, bytes[18] & 0x0F));
            break;
        }
        case 0x42:
            std::snprintf(decoded->type, sizeof(decoded->type), "info_request");
            decoded->infoCode = bytes[5];
            std::snprintf(decoded->summary, sizeof(decoded->summary), "INFO request code=0x%02X", bytes[5]);
            break;
        case 0x61:
            std::snprintf(decoded->type, sizeof(decoded->type), "set_ack");
            std::snprintf(decoded->summary, sizeof(decoded->summary), "SET acknowledged");
            break;
        case 0x62: {
            std::snprintf(decoded->type, sizeof(decoded->type), "info_response");
            decoded->infoCode = bytes[5];
            const uint8_t* data = &bytes[5];
            if (data[0] == 0x02 && data_len >= 12) {
                std::snprintf(decoded->summary,
                              sizeof(decoded->summary),
                              "SETTINGS power=%s mode=%s temp=%.1fC fan=%s vane=%s wide=%s",
                              lookupString(POWER_MAP, POWER, 2, data[3]),
                              lookupString(MODE_MAP, MODE, 5, data[4] > 0x08 ? data[4] - 0x08 : data[4]),
                              static_cast<double>(halfDegreesToCelsius(decodeTemperatureHalfC(data[5], data[11]))),
                              lookupString(FAN_MAP, FAN, 6, data[6]),
                              lookupString(VANE_MAP, VANE, 7, data[7]),
                              lookupString(WIDEVANE_MAP, WIDEVANE, 8, data[10] & 0x0F));
            } else if (data[0] == 0x03 && data_len >= 7) {
                const HalfDegreesC room_half_c = data[6] != 0
                                                     ? static_cast<HalfDegreesC>(data[6]) - 128
                                                     : static_cast<HalfDegreesC>(roomTempFromByte(data[3]) * 2);
                std::snprintf(decoded->summary,
                              sizeof(decoded->summary),
                              "ROOM temperature=%.1fC",
                              static_cast<double>(halfDegreesToCelsius(room_half_c)));
            } else if (data[0] == 0x06 && data_len >= 7) {
                std::snprintf(decoded->summary,
                              sizeof(decoded->summary),
                              "STATUS compressor=%uHz operating=%s input=%uW",
                              data[3],
                              data[4] != 0 ? "yes" : "no",
                              static_cast<unsigned>((data[5] << 8) | data[6]));
            } else {
                std::snprintf(decoded->summary, sizeof(decoded->summary), "INFO response code=0x%02X", data[0]);
            }
            break;
        }
        case 0x7A:
        case 0x7B:
            std::snprintf(decoded->type, sizeof(decoded->type), "connect_ack");
            std::snprintf(decoded->summary, sizeof(decoded->summary), "CONNECT acknowledged command=0x%02X", decoded->command);
            break;
        default:
            std::snprintf(decoded->type, sizeof(decoded->type), "unknown");
            std::snprintf(decoded->summary, sizeof(decoded->summary), "Unknown command=0x%02X", decoded->command);
            break;
    }

    return true;
}

void initMockState() {
    {
        platform_lock::ScopedLock lock(state_lock);
        mock_state = MockState{};
        mock_state.lastPacketHex = last_packet_hex;
        mock_state.lastError = last_error;
    }
    ESP_LOGI(TAG,
             "CN105 mock initialized: power=%s mode=%s target=%.1fC room=%.1fC transport=mock",
             mock_state.power,
             mock_state.mode,
             static_cast<double>(halfDegreesToCelsius(mock_state.targetTemperatureHalfC)),
             static_cast<double>(halfDegreesToCelsius(mock_state.roomTemperatureHalfC)));
}

MockState getMockState() {
    platform_lock::ScopedLock lock(state_lock);
    mock_state.lastPacketHex = last_packet_hex;
    mock_state.lastError = last_error;
    MockState copy = mock_state;
    return copy;
}

bool applySetPacketToMock(const uint8_t* bytes, size_t len, char* error, size_t error_len) {
    DecodedPacket decoded{};
    if (!decodePacket(bytes, len, &decoded, error, error_len)) {
        platform_lock::ScopedLock lock(state_lock);
        rememberError(error);
        return false;
    }
    if (decoded.command != 0x41) {
        setError(error, error_len, "mock apply expects a SET packet");
        platform_lock::ScopedLock lock(state_lock);
        rememberError(error);
        return false;
    }

    platform_lock::ScopedLock lock(state_lock);
    if ((bytes[6] & CONTROL_PACKET_1[0]) != 0) {
        mock_state.power = lookupString(POWER_MAP, POWER, 2, bytes[8]);
    }
    if ((bytes[6] & CONTROL_PACKET_1[1]) != 0) {
        mock_state.mode = lookupString(MODE_MAP, MODE, 5, bytes[9]);
    }
    if ((bytes[6] & CONTROL_PACKET_1[2]) != 0) {
        mock_state.targetTemperatureHalfC = decodeTemperatureHalfC(bytes[10], bytes[19]);
    }
    if ((bytes[6] & CONTROL_PACKET_1[3]) != 0) {
        mock_state.fan = lookupString(FAN_MAP, FAN, 6, bytes[11]);
    }
    if ((bytes[6] & CONTROL_PACKET_1[4]) != 0) {
        mock_state.vane = lookupString(VANE_MAP, VANE, 7, bytes[12]);
    }
    if ((bytes[7] & CONTROL_PACKET_2[0]) != 0) {
        mock_state.wideVane = lookupString(WIDEVANE_MAP, WIDEVANE, 8, bytes[18] & 0x0F);
    }

    mock_state.operating = equals(mock_state.power, "ON");
    mock_state.compressorFrequencyHz = mock_state.operating ? 42 : 0;
    mock_state.inputPowerW = mock_state.operating ? 650 : 0;
    rememberPacket(bytes, len);
    rememberError("");
    mock_dirty = true;
    return true;
}

bool isMockDirty() {
    platform_lock::ScopedLock lock(state_lock);
    bool dirty = mock_dirty;
    return dirty;
}

void clearMockDirty() {
    platform_lock::ScopedLock lock(state_lock);
    mock_dirty = false;
}

bool applyInfoResponseToState(const uint8_t* bytes, size_t len) {
    if (bytes == nullptr || len < 6 || bytes[0] != 0xFC || bytes[1] != 0x62) {
        return false;
    }
    const uint8_t data_len = bytes[4];
    if (len != static_cast<size_t>(data_len) + 6) {
        return false;
    }
    const uint8_t* data = &bytes[5];

    platform_lock::ScopedLock lock(state_lock);
    if (data[0] == 0x02 && data_len >= 12) {
        mock_state.power = lookupString(POWER_MAP, POWER, 2, data[3]);
        uint8_t mode_byte = data[4] > 0x08 ? data[4] - 0x08 : data[4];
        mock_state.mode = lookupString(MODE_MAP, MODE, 5, mode_byte);
        mock_state.targetTemperatureHalfC = decodeTemperatureHalfC(data[5], data[11]);
        mock_state.fan = lookupString(FAN_MAP, FAN, 6, data[6]);
        mock_state.vane = lookupString(VANE_MAP, VANE, 7, data[7]);
        if (data_len >= 11) {
            mock_state.wideVane = lookupString(WIDEVANE_MAP, WIDEVANE, 8, data[10] & 0x0F);
        }
        mock_dirty = true;
    } else if (data[0] == 0x03 && data_len >= 7) {
        if (data[6] != 0) {
            mock_state.roomTemperatureHalfC = static_cast<HalfDegreesC>(data[6]) - 128;
        } else {
            mock_state.roomTemperatureHalfC = static_cast<HalfDegreesC>(roomTempFromByte(data[3]) * 2);
        }
        mock_dirty = true;
    } else if (data[0] == 0x06 && data_len >= 7) {
        mock_state.compressorFrequencyHz = data[3];
        mock_state.operating = data[4] != 0;
        mock_state.inputPowerW = (data[5] << 8) | data[6];
        mock_dirty = true;
    }
    rememberPacket(bytes, len);
    return true;
}

void setConnected(bool connected) {
    platform_lock::ScopedLock lock(state_lock);
    mock_state.connected = connected;
    mock_dirty = true;
}

bool runSelfTest(char* error, size_t error_len) {
    for (int temp_f = 50; temp_f <= 88; ++temp_f) {
        const HalfDegreesC half_c = fahrenheitSetpointToHalfDegrees(temp_f);
        const int decoded_f = halfDegreesToFahrenheitSetpoint(half_c);
        if (decoded_f != temp_f) {
            std::snprintf(error, error_len, "%dF target roundtrip failed: decoded %dF", temp_f, decoded_f);
            return false;
        }
    }

    for (HalfDegreesC half_c = kMinTargetHalfC; half_c <= kMaxTargetHalfC; ++half_c) {
        const uint8_t encoded = static_cast<uint8_t>(half_c + 128);
        if (decodeTemperatureHalfC(0, encoded) != half_c) {
            std::snprintf(error, error_len, "half-degree target roundtrip failed at %.1fC",
                          static_cast<double>(halfDegreesToCelsius(half_c)));
            return false;
        }
    }

    for (int celsius = 16; celsius <= 31; ++celsius) {
        const uint8_t legacy = static_cast<uint8_t>(31 - celsius);
        if (decodeTemperatureHalfC(legacy, 0) != celsius * 2) {
            std::snprintf(error, error_len, "legacy target decode failed at %dC", celsius);
            return false;
        }
    }

    SetCommand command{};
    command.hasPower = true;
    command.power = "ON";
    command.hasMode = true;
    command.mode = "COOL";
    command.hasTargetTemperature = true;
    command.targetTemperatureHalfC = 53;  // 26.5 C, Encoding B = 0xB5.
    command.hasFan = true;
    command.fan = "AUTO";
    command.hasVane = true;
    command.vane = "AUTO";
    command.hasWideVane = true;
    command.wideVane = "|";

    Packet packet{};
    if (!buildSetPacket(command, &packet, error, error_len)) {
        return false;
    }

    MockState before = {};
    char previous_packet_hex[sizeof(last_packet_hex)] = {};
    char previous_error[sizeof(last_error)] = {};
    {
        platform_lock::ScopedLock lock(state_lock);
        before = mock_state;
        std::snprintf(previous_packet_hex, sizeof(previous_packet_hex), "%s", last_packet_hex);
        std::snprintf(previous_error, sizeof(previous_error), "%s", last_error);
    }
    if (!applySetPacketToMock(packet.bytes, packet.length, error, error_len)) {
        platform_lock::ScopedLock lock(state_lock);
        mock_state = before;
        std::snprintf(last_packet_hex, sizeof(last_packet_hex), "%s", previous_packet_hex);
        std::snprintf(last_error, sizeof(last_error), "%s", previous_error);
        return false;
    }

    HalfDegreesC round_trip_half_c = 0;
    {
        platform_lock::ScopedLock lock(state_lock);
        round_trip_half_c = mock_state.targetTemperatureHalfC;
        mock_state = before;
        std::snprintf(last_packet_hex, sizeof(last_packet_hex), "%s", previous_packet_hex);
        std::snprintf(last_error, sizeof(last_error), "%s", previous_error);
    }
    if (packet.bytes[19] != 0xB5 || round_trip_half_c != 53) {
        setError(error, error_len, "26.5C SET roundtrip failed");
        return false;
    }

    ESP_LOGI(TAG, "CN105 offline self-test passed: half-degree and Fahrenheit target roundtrips");
    return true;
}

}  // namespace cn105_core
