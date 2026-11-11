// Kiri Bridge — https://kiri.dkt.moe — GPL-3.0
#pragma once
#include "esp_err.h"
#include <string>
namespace qingping {
esp_err_t init();
bool refresh();
bool bindAddress(const char* address);
bool unbind();
std::string statusJson(bool includeDevices = true);
// Returns true only for a fresh, bound, control-eligible sensor reading.
bool activeTemperatureC(float* temperature);
// Exclusive Bluetooth ownership handoff to the WiFi provisioning manager.
bool pauseForProvisioning();
void resumeAfterProvisioning();
}
