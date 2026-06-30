#pragma once

#include "cn105_core.h"

#include <stddef.h>
#include <stdint.h>

namespace lua_vm {

inline constexpr size_t kMaxActions = 4;
inline constexpr size_t kMaxScriptBytes = 12 * 1024;
inline constexpr uint8_t kHookStateChanged = 1 << 0;
inline constexpr uint8_t kHookPowerOn = 1 << 1;
inline constexpr uint8_t kHookPowerOff = 1 << 2;

enum class ActionType {
    kNone,
    kSetPower,
    kSetMode,
    kSetTargetTemperatureF,
    kSetFan,
    kSetVane,
    kSetWideVane,
};

struct Action {
    ActionType type = ActionType::kNone;
    char value[32] = "";
    int intValue = 0;
};

struct RunResult {
    bool ok = false;
    bool scriptLoaded = false;
    bool hookFound = false;
    bool instructionLimitHit = false;
    size_t peakBytes = 0;
    size_t actionCount = 0;
    Action actions[kMaxActions] = {};
    char message[128] = "";
};

struct ProbeResult {
    bool ok;
    size_t peakBytes;
    const char* message;
};

struct ValidationResult {
    bool ok = false;
    bool instructionLimitHit = false;
    uint8_t hookMask = 0;
    size_t peakBytes = 0;
    char message[128] = "";
};

struct RuntimeStatus {
    bool enabled = false;
    bool scriptExists = false;
    size_t scriptSize = 0;
    bool lastSet = false;
    bool lastOk = false;
    bool lastScriptLoaded = false;
    bool lastHookFound = false;
    bool lastInstructionLimitHit = false;
    size_t lastPeakBytes = 0;
    size_t lastActionCount = 0;
    uint32_t runCount = 0;
    char lastHook[32] = "";
    char lastMessage[128] = "";
    Action lastActions[kMaxActions] = {};
};

const char* scriptLogicalPath();
const char* scriptPhysicalPath();
bool isEnabled();
bool setEnabled(bool enabled, char* error, size_t error_len);
ValidationResult validateScript(const char* script, size_t scriptSize);
bool saveScript(const char* script, size_t scriptSize, ValidationResult* validation);
RuntimeStatus getRuntimeStatus();
ProbeResult runProbe();
RunResult runHook(const char* hookName,
                  const cn105_core::MockState& state,
                  const cn105_core::MockState* previousState);

}  // namespace lua_vm
