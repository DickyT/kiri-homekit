#pragma once

#include "cn105_core.h"

#include <stddef.h>
#include <stdint.h>

namespace lua_vm {

inline constexpr size_t kMaxActions = 4;
inline constexpr size_t kRunHistorySize = 12;
inline constexpr size_t kMaxScriptBytes = 12 * 1024;
inline constexpr uint8_t kApiVersion = 2;
inline constexpr uint8_t kHookStateChanged = 1 << 0;
inline constexpr uint8_t kHookPowerOn = 1 << 1;
inline constexpr uint8_t kHookPowerOff = 1 << 2;

enum class ActionType {
    kNone,
    kSetPower,
    kSetMode,
    kSetTargetTemperatureF,
    kSetTargetTemperatureC,
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
    bool ok = false;
    size_t peakBytes = 0;
    uint8_t checksPassed = 0;
    uint8_t checksTotal = 0;
    char message[128] = "";
};

struct ValidationResult {
    bool ok = false;
    bool instructionLimitHit = false;
    uint8_t hookMask = 0;
    size_t peakBytes = 0;
    char message[128] = "";
};

struct RunHistoryEntry {
    uint32_t sequence = 0;
    uint32_t uptimeMs = 0;
    uint32_t durationMs = 0;
    bool ok = false;
    bool hookFound = false;
    bool instructionLimitHit = false;
    size_t peakBytes = 0;
    size_t actionCount = 0;
    char hook[32] = "";
    char message[96] = "";
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
    uint32_t actionSequence = 0;
    bool actionPending = false;
    bool lastActionConfirmed = false;
    uint8_t lastActionAttempts = 0;
    char lastHook[32] = "";
    char lastMessage[128] = "";
    char lastActionMessage[128] = "";
    Action lastActions[kMaxActions] = {};
    size_t historyCount = 0;
    RunHistoryEntry history[kRunHistorySize] = {};
};

const char* scriptLogicalPath();
const char* scriptPhysicalPath();
bool isEnabled();
bool setEnabled(bool enabled, char* error, size_t error_len);
ValidationResult validateScript(const char* script, size_t scriptSize);
bool saveScript(const char* script, size_t scriptSize, ValidationResult* validation);
RuntimeStatus getRuntimeStatus();
void recordActionQueued();
void recordActionResult(bool confirmed, uint8_t attempts, const char* message);
ProbeResult runProbe();
RunResult runHook(const char* hookName,
                  const cn105_core::MockState& state,
                  const cn105_core::MockState* previousState);

}  // namespace lua_vm
