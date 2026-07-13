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

#include <cstddef>
#include <cstdint>

namespace cn105_core {

inline constexpr size_t kPacketLen = 22;
inline constexpr size_t kConnectLen = 8;
inline constexpr size_t kMaxHexLen = (kPacketLen * 3);
using HalfDegreesC = int16_t;
inline constexpr HalfDegreesC kMinTargetHalfC = 20;  // 10.0 C
inline constexpr HalfDegreesC kMaxTargetHalfC = 62;  // 31.0 C

struct Packet {
    uint8_t bytes[kPacketLen] = {};
    size_t length = 0;
};

struct SetCommand {
    bool hasPower = false;
    const char* power = nullptr;

    bool hasMode = false;
    const char* mode = nullptr;

    bool hasTargetTemperature = false;
    HalfDegreesC targetTemperatureHalfC = 50;

    bool hasFan = false;
    const char* fan = nullptr;

    bool hasVane = false;
    const char* vane = nullptr;

    bool hasWideVane = false;
    const char* wideVane = nullptr;
};

struct MockState {
    bool connected = true;
    const char* power = "OFF";
    const char* mode = "COOL";
    HalfDegreesC targetTemperatureHalfC = 50;
    HalfDegreesC roomTemperatureHalfC = 48;
    const char* fan = "AUTO";
    const char* vane = "AUTO";
    const char* wideVane = "|";
    bool operating = false;
    int compressorFrequencyHz = 0;
    int inputPowerW = 0;
    float energyKwh = 0.0f;
    const char* lastPacketHex = "";
    const char* lastError = "";
};

struct DecodedPacket {
    bool checksumOk = false;
    uint8_t command = 0;
    uint8_t dataLength = 0;
    uint8_t infoCode = 0;
    char type[24] = "unknown";
    char summary[192] = "";
};

uint8_t checksum(const uint8_t* bytes, size_t len);
HalfDegreesC celsiusToHalfDegrees(float celsius);
float halfDegreesToCelsius(HalfDegreesC half_c);
HalfDegreesC fahrenheitSetpointToHalfDegrees(int fahrenheit);
int halfDegreesToFahrenheitSetpoint(HalfDegreesC half_c);
float halfDegreesToFahrenheit(HalfDegreesC half_c);
bool bytesToHex(const uint8_t* bytes, size_t len, char* out, size_t out_len);
bool parseHex(const char* hex, uint8_t* out, size_t max_len, size_t* out_len, char* error, size_t error_len);

bool buildConnectPacket(Packet* packet, char* error, size_t error_len);
bool buildInfoPacket(uint8_t infoCode, Packet* packet, char* error, size_t error_len);
bool buildSetPacket(const SetCommand& command, Packet* packet, char* error, size_t error_len);
bool decodePacket(const uint8_t* bytes, size_t len, DecodedPacket* decoded, char* error, size_t error_len);

void initMockState();
MockState getMockState();
bool applySetPacketToMock(const uint8_t* bytes, size_t len, char* error, size_t error_len);
bool applyInfoResponseToState(const uint8_t* bytes, size_t len);
void setConnected(bool connected);
bool runSelfTest(char* error, size_t error_len);
bool isMockDirty();
void clearMockDirty();

}  // namespace cn105_core
