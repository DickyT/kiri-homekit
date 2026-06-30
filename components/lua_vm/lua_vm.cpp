#include "lua_vm.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "device_settings.h"
#include "nvs.h"
#include "platform_lock.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace {

constexpr const char* TAG = "lua_vm";
constexpr const char* kScriptLogicalPath = "/scripts/automation.lua";
constexpr const char* kScriptPhysicalPath = "/spiffs/scripts/automation.lua";
constexpr const char* kScriptTempPhysicalPath = "/spiffs/scripts/automation.tmp";
constexpr const char* kScriptBackupPhysicalPath = "/spiffs/scripts/automation.last-good.lua";
constexpr const char* kConfigNamespace = "lua_cfg";
constexpr const char* kEnabledKey = "enabled";
constexpr const char* kKvNamespace = "lua_kv";
constexpr size_t kArenaSize = 32 * 1024;
constexpr int kInstructionLimit = 8000;
constexpr int kApiVersion = 1;
constexpr size_t kKvMaxEntries = 16;
constexpr size_t kKvMaxValueLength = 95;
constexpr size_t kKvMaxTotalBytes = 1024;

platform_lock::RecursiveMutex status_lock;
lua_vm::RuntimeStatus runtime_status{};

struct Arena {
    alignas(max_align_t) unsigned char bytes[kArenaSize];
    size_t used = 0;
    size_t peak = 0;
};

struct BlockHeader {
    size_t size;
};

struct ScriptContext {
    lua_vm::RunResult* result = nullptr;
    int instructionsLeft = kInstructionLimit;
    bool validationOnly = false;
};

void destroyLua(lua_State* L, Arena* arena);

void copyString(char* out, size_t out_len, const char* value) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    std::snprintf(out, out_len, "%s", value == nullptr ? "" : value);
}

void setMessage(lua_vm::RunResult* result, const char* message) {
    if (result != nullptr) {
        copyString(result->message, sizeof(result->message), message);
    }
}

void setErrorMessage(char* error, size_t error_len, const char* message) {
    if (error != nullptr && error_len > 0) {
        std::snprintf(error, error_len, "%s", message == nullptr ? "" : message);
    }
}

void scriptInfo(bool* exists, size_t* size) {
    if (exists != nullptr) {
        *exists = false;
    }
    if (size != nullptr) {
        *size = 0;
    }

    FILE* file = std::fopen(kScriptPhysicalPath, "rb");
    if (file == nullptr) {
        return;
    }
    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long end = std::ftell(file);
        if (end > 0 && size != nullptr) {
            *size = static_cast<size_t>(end);
        }
    }
    std::fclose(file);
    if (exists != nullptr) {
        *exists = true;
    }
}

void refreshScriptInfoLocked(lua_vm::RuntimeStatus* status) {
    if (status == nullptr) {
        return;
    }
    scriptInfo(&status->scriptExists, &status->scriptSize);
}

void recordRun(const char* hookName, const lua_vm::RunResult& result) {
    platform_lock::ScopedLock lock(status_lock);
    refreshScriptInfoLocked(&runtime_status);
    runtime_status.enabled = lua_vm::isEnabled();
    runtime_status.lastSet = true;
    runtime_status.lastOk = result.ok;
    runtime_status.lastScriptLoaded = result.scriptLoaded;
    runtime_status.lastHookFound = result.hookFound;
    runtime_status.lastInstructionLimitHit = result.instructionLimitHit;
    runtime_status.lastPeakBytes = result.peakBytes;
    runtime_status.lastActionCount = std::min(result.actionCount, lua_vm::kMaxActions);
    runtime_status.runCount++;
    copyString(runtime_status.lastHook, sizeof(runtime_status.lastHook), hookName);
    copyString(runtime_status.lastMessage, sizeof(runtime_status.lastMessage), result.message);
    for (size_t i = 0; i < lua_vm::kMaxActions; ++i) {
        runtime_status.lastActions[i] = i < runtime_status.lastActionCount ? result.actions[i] : lua_vm::Action{};
    }
}

void* arenaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    (void)osize;
    auto* arena = static_cast<Arena*>(ud);
    if (nsize == 0) {
        return nullptr;
    }
    if (ptr != nullptr) {
        auto* old_header = reinterpret_cast<BlockHeader*>(ptr) - 1;
        const size_t old_size = old_header->size;
        void* next = arenaAlloc(ud, nullptr, 0, nsize);
        if (next != nullptr) {
            std::memcpy(next, ptr, std::min(old_size, nsize));
        }
        return next;
    }

    const size_t align = alignof(max_align_t);
    const size_t header_size = sizeof(BlockHeader);
    size_t offset = (arena->used + align - 1) & ~(align - 1);
    const size_t total = header_size + nsize;
    if (offset + total > kArenaSize) {
        return nullptr;
    }

    auto* header = reinterpret_cast<BlockHeader*>(arena->bytes + offset);
    header->size = nsize;
    arena->used = offset + total;
    arena->peak = std::max(arena->peak, arena->used);
    return header + 1;
}

ScriptContext* context(lua_State* L) {
    return static_cast<ScriptContext*>(lua_touserdata(L, lua_upvalueindex(1)));
}

void instructionHook(lua_State* L, lua_Debug*) {
    lua_getfield(L, LUA_REGISTRYINDEX, "kiri.context");
    ScriptContext* ctx = static_cast<ScriptContext*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (ctx == nullptr) {
        luaL_error(L, "missing script context");
        return;
    }
    ctx->instructionsLeft -= 100;
    if (ctx->instructionsLeft <= 0) {
        if (ctx->result != nullptr) {
            ctx->result->instructionLimitHit = true;
        }
        luaL_error(L, "instruction limit exceeded");
    }
}

void openSafeLibs(lua_State* L) {
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pushnil(L);
    lua_setfield(L, -2, "dofile");
    lua_pushnil(L);
    lua_setfield(L, -2, "loadfile");
    lua_pushnil(L);
    lua_setfield(L, -2, "collectgarbage");
    lua_pop(L, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);
}

void pushState(lua_State* L, const cn105_core::MockState& state) {
    lua_createtable(L, 0, 14);

    lua_pushboolean(L, state.power != nullptr && std::strcmp(state.power, "ON") == 0);
    lua_setfield(L, -2, "power");
    lua_pushstring(L, state.power);
    lua_setfield(L, -2, "power_raw");
    lua_pushstring(L, state.mode);
    lua_setfield(L, -2, "mode");
    lua_pushinteger(L, state.targetTemperatureF);
    lua_setfield(L, -2, "target_temp_f");
    lua_pushinteger(L, state.roomTemperatureF);
    lua_setfield(L, -2, "room_temp_f");
    lua_pushnumber(L, (static_cast<double>(state.roomTemperatureF) - 32.0) / 1.8);
    lua_setfield(L, -2, "room_temp_c");
    lua_pushstring(L, state.fan);
    lua_setfield(L, -2, "fan");
    lua_pushstring(L, state.vane);
    lua_setfield(L, -2, "vane");
    lua_pushstring(L, state.vane);
    lua_setfield(L, -2, "up_down_airflow");
    lua_pushstring(L, state.wideVane);
    lua_setfield(L, -2, "wide_vane");
    lua_pushstring(L, state.wideVane);
    lua_setfield(L, -2, "left_right_airflow");
    lua_pushboolean(L, state.operating);
    lua_setfield(L, -2, "operating");
    lua_pushinteger(L, state.compressorFrequencyHz);
    lua_setfield(L, -2, "compressor_hz");
    lua_pushinteger(L, state.inputPowerW);
    lua_setfield(L, -2, "input_power_w");
    lua_pushboolean(L, state.connected);
    lua_setfield(L, -2, "connected");
}

bool appendAction(lua_State* L, lua_vm::ActionType type, const char* value, int int_value) {
    ScriptContext* ctx = context(L);
    if (ctx == nullptr || ctx->result == nullptr) {
        return false;
    }
    if (ctx->result->actionCount >= lua_vm::kMaxActions) {
        return luaL_error(L, "too many ac actions; max is %u", static_cast<unsigned>(lua_vm::kMaxActions)) == 0;
    }
    lua_vm::Action& action = ctx->result->actions[ctx->result->actionCount++];
    action.type = type;
    action.intValue = int_value;
    copyString(action.value, sizeof(action.value), value);
    return true;
}

int lStateCurrent(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "kiri.current_state");
    return 1;
}

int lAcSetPower(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    const bool on = lua_toboolean(L, 1);
    appendAction(L, lua_vm::ActionType::kSetPower, on ? "ON" : "OFF", on ? 1 : 0);
    return 0;
}

int lAcSetMode(lua_State* L) {
    const char* mode = luaL_checkstring(L, 1);
    const bool known = std::strcmp(mode, "AUTO") == 0 || std::strcmp(mode, "HEAT") == 0 ||
        std::strcmp(mode, "COOL") == 0 || std::strcmp(mode, "DRY") == 0 ||
        std::strcmp(mode, "FAN") == 0;
    if (!known) {
        return luaL_error(L, "ac.set_mode: expected AUTO, HEAT, COOL, DRY, or FAN");
    }
    const uint32_t capabilities = device_settings::acCapabilities();
    if ((std::strcmp(mode, "AUTO") == 0 && (capabilities & device_settings::kAcCapabilityTargetAuto) == 0) ||
        (std::strcmp(mode, "HEAT") == 0 && (capabilities & device_settings::kAcCapabilityTargetHeat) == 0) ||
        (std::strcmp(mode, "COOL") == 0 && (capabilities & device_settings::kAcCapabilityTargetCool) == 0)) {
        return luaL_error(L, "ac.set_mode: %s is disabled in AC Capabilities", mode);
    }
    appendAction(L, lua_vm::ActionType::kSetMode, mode, 0);
    return 0;
}

int lAcSetTargetTemp(lua_State* L) {
    const int temp_f = static_cast<int>(luaL_checkinteger(L, 1));
    if (temp_f < 50 || temp_f > 88) {
        return luaL_error(L, "ac.set_target_temp_f: expected 50..88 F");
    }
    appendAction(L, lua_vm::ActionType::kSetTargetTemperatureF, nullptr, temp_f);
    return 0;
}

int lAcSetFan(lua_State* L) {
    const char* value = nullptr;
    char fan[8] = {};
    if (lua_isinteger(L, 1)) {
        const int level = static_cast<int>(lua_tointeger(L, 1));
        if (level < 1 || level > 4) {
            return luaL_error(L, "ac.set_fan: numeric level must be 1..4");
        }
        std::snprintf(fan, sizeof(fan), "%d", level);
        value = fan;
    } else {
        value = luaL_checkstring(L, 1);
        if (std::strcmp(value, "AUTO") != 0 && std::strcmp(value, "QUIET") != 0 &&
            std::strcmp(value, "1") != 0 && std::strcmp(value, "2") != 0 &&
            std::strcmp(value, "3") != 0 && std::strcmp(value, "4") != 0) {
            return luaL_error(L, "ac.set_fan: expected AUTO, QUIET, or 1..4");
        }
    }
    appendAction(L, lua_vm::ActionType::kSetFan, value, 0);
    return 0;
}

int lAcSetVane(lua_State* L) {
    if (!device_settings::supportsUpDownAirflow()) {
        return luaL_error(L, "ac.set_up_down_airflow: disabled in AC Capabilities");
    }
    const char* value = luaL_checkstring(L, 1);
    const bool valid = std::strcmp(value, "AUTO") == 0 || std::strcmp(value, "SWING") == 0 ||
        (value[0] >= '1' && value[0] <= '5' && value[1] == '\0');
    if (!valid) {
        return luaL_error(L, "ac.set_up_down_airflow: expected AUTO, SWING, or 1..5");
    }
    appendAction(L, lua_vm::ActionType::kSetVane, value, 0);
    return 0;
}

int lAcSetWideVane(lua_State* L) {
    if (!device_settings::supportsLeftRightAirflow()) {
        return luaL_error(L, "ac.set_left_right_airflow: disabled in AC Capabilities");
    }
    const char* value = luaL_checkstring(L, 1);
    const bool valid = std::strcmp(value, "<<") == 0 || std::strcmp(value, "<") == 0 ||
        std::strcmp(value, "|") == 0 || std::strcmp(value, ">") == 0 ||
        std::strcmp(value, ">>") == 0 || std::strcmp(value, "<>") == 0 ||
        std::strcmp(value, "SWING") == 0 || std::strcmp(value, "AIRFLOW CONTROL") == 0;
    if (!valid) {
        return luaL_error(L, "ac.set_left_right_airflow: expected <<, <, |, >, >>, <>, SWING, or AIRFLOW CONTROL");
    }
    appendAction(L, lua_vm::ActionType::kSetWideVane, value, 0);
    return 0;
}

int lAcSetSwing(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    const bool swing = lua_toboolean(L, 1);
    const bool up_down = device_settings::supportsUpDownAirflow();
    const bool left_right = device_settings::supportsLeftRightAirflow();
    if (!up_down && !left_right) {
        return luaL_error(L, "ac.set_swing: no airflow axes are enabled in AC Capabilities");
    }
    if (up_down) {
        appendAction(L, lua_vm::ActionType::kSetVane, swing ? "SWING" : "AUTO", 0);
    }
    if (left_right) {
        appendAction(L, lua_vm::ActionType::kSetWideVane, swing ? "SWING" : "|", 0);
    }
    return 0;
}

bool validKvKey(const char* key) {
    if (key == nullptr) {
        return false;
    }
    const size_t len = std::strlen(key);
    if (len == 0 || len > 15) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(key[i]);
        if (!std::isalnum(c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

bool kvUsage(nvs_handle_t handle, size_t* entries, size_t* total_bytes) {
    *entries = 0;
    *total_bytes = 0;
    nvs_iterator_t iterator = nullptr;
    esp_err_t err = nvs_entry_find_in_handle(handle, NVS_TYPE_STR, &iterator);
    while (err == ESP_OK && iterator != nullptr) {
        nvs_entry_info_t info{};
        if (nvs_entry_info(iterator, &info) != ESP_OK) {
            nvs_release_iterator(iterator);
            return false;
        }
        size_t value_len = 0;
        if (nvs_get_str(handle, info.key, nullptr, &value_len) != ESP_OK) {
            nvs_release_iterator(iterator);
            return false;
        }
        ++(*entries);
        *total_bytes += std::strlen(info.key) + value_len;
        err = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    return err == ESP_ERR_NVS_NOT_FOUND;
}

int lKvGet(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    if (!validKvKey(key)) {
        return luaL_error(L, "invalid kv key");
    }
    nvs_handle_t handle = 0;
    if (nvs_open(kKvNamespace, NVS_READONLY, &handle) != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }
    char value[96] = {};
    size_t value_len = sizeof(value);
    const esp_err_t err = nvs_get_str(handle, key, value, &value_len);
    nvs_close(handle);
    if (err != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, value);
    return 1;
}

int lKvSet(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    if (!validKvKey(key)) {
        return luaL_error(L, "invalid kv key");
    }

    char value[96] = {};
    if (lua_isboolean(L, 2)) {
        copyString(value, sizeof(value), lua_toboolean(L, 2) ? "true" : "false");
    } else if (lua_isinteger(L, 2)) {
        std::snprintf(value, sizeof(value), "%lld", static_cast<long long>(lua_tointeger(L, 2)));
    } else if (lua_isnumber(L, 2)) {
        std::snprintf(value, sizeof(value), "%.6g", lua_tonumber(L, 2));
    } else {
        const char* string_value = luaL_checkstring(L, 2);
        if (std::strlen(string_value) > kKvMaxValueLength) {
            return luaL_error(L, "kv.set: value exceeds %u bytes", static_cast<unsigned>(kKvMaxValueLength));
        }
        copyString(value, sizeof(value), string_value);
    }
    if (std::strlen(value) > kKvMaxValueLength) {
        return luaL_error(L, "kv.set: value exceeds %u bytes", static_cast<unsigned>(kKvMaxValueLength));
    }

    ScriptContext* ctx = context(L);
    if (ctx != nullptr && ctx->validationOnly) {
        lua_pushboolean(L, true);
        return 1;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kKvNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        char previous[96] = {};
        size_t previous_len = sizeof(previous);
        const esp_err_t previous_err = nvs_get_str(handle, key, previous, &previous_len);
        if (previous_err != ESP_OK && previous_err != ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(handle);
            return luaL_error(L, "kv.set: failed to read existing value");
        }
        if (previous_err == ESP_OK && std::strcmp(previous, value) == 0) {
            nvs_close(handle);
            lua_pushboolean(L, true);
            return 1;
        }

        size_t entries = 0;
        size_t total_bytes = 0;
        if (!kvUsage(handle, &entries, &total_bytes)) {
            nvs_close(handle);
            return luaL_error(L, "kv.set: failed to inspect storage");
        }
        if (previous_err == ESP_ERR_NVS_NOT_FOUND && entries >= kKvMaxEntries) {
            nvs_close(handle);
            return luaL_error(L, "kv.set: storage is limited to %u keys", static_cast<unsigned>(kKvMaxEntries));
        }
        if (previous_err == ESP_OK) {
            total_bytes -= std::strlen(key) + previous_len;
        }
        const size_t new_total = total_bytes + std::strlen(key) + std::strlen(value) + 1;
        if (new_total > kKvMaxTotalBytes) {
            nvs_close(handle);
            return luaL_error(L, "kv.set: storage exceeds %u bytes", static_cast<unsigned>(kKvMaxTotalBytes));
        }

        err = nvs_set_str(handle, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    lua_pushboolean(L, err == ESP_OK);
    return 1;
}

int lKvDelete(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    if (!validKvKey(key)) {
        return luaL_error(L, "invalid kv key");
    }
    ScriptContext* ctx = context(L);
    if (ctx != nullptr && ctx->validationOnly) {
        lua_pushboolean(L, true);
        return 1;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kKvNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        } else if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    lua_pushboolean(L, err == ESP_OK);
    return 1;
}

int lLogInfo(lua_State* L) {
    ScriptContext* ctx = context(L);
    if (ctx != nullptr && ctx->validationOnly) {
        return 0;
    }
    ESP_LOGI(TAG, "script: %s", luaL_tolstring(L, 1, nullptr));
    lua_pop(L, 1);
    return 0;
}

int lLogWarn(lua_State* L) {
    ScriptContext* ctx = context(L);
    if (ctx != nullptr && ctx->validationOnly) {
        return 0;
    }
    ESP_LOGW(TAG, "script: %s", luaL_tolstring(L, 1, nullptr));
    lua_pop(L, 1);
    return 0;
}

int lPrint(lua_State* L) {
    ScriptContext* ctx = context(L);
    if (ctx != nullptr && ctx->validationOnly) {
        return 0;
    }
    const int n = lua_gettop(L);
    char line[160] = {};
    size_t offset = 0;
    for (int i = 1; i <= n; ++i) {
        const char* text = luaL_tolstring(L, i, nullptr);
        const int written = std::snprintf(line + offset, sizeof(line) - offset, "%s%s",
                                          i == 1 ? "" : "\t", text == nullptr ? "" : text);
        lua_pop(L, 1);
        if (written < 0 || static_cast<size_t>(written) >= sizeof(line) - offset) {
            break;
        }
        offset += static_cast<size_t>(written);
    }
    ESP_LOGI(TAG, "script: %s", line);
    return 0;
}

void setFunction(lua_State* L, const char* name, lua_CFunction fn, ScriptContext* ctx) {
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, -2, name);
}

void registerApi(lua_State* L, ScriptContext* ctx, const cn105_core::MockState& state) {
    pushState(L, state);
    lua_setfield(L, LUA_REGISTRYINDEX, "kiri.current_state");

    lua_createtable(L, 0, 1);
    setFunction(L, "current", lStateCurrent, ctx);
    lua_setglobal(L, "state");

    lua_createtable(L, 0, 2);
    lua_pushinteger(L, kApiVersion);
    lua_setfield(L, -2, "api_version");
    lua_createtable(L, 0, 5);
    const uint32_t capabilities = device_settings::acCapabilities();
    lua_pushboolean(L, (capabilities & device_settings::kAcCapabilityTargetAuto) != 0);
    lua_setfield(L, -2, "auto_mode");
    lua_pushboolean(L, (capabilities & device_settings::kAcCapabilityTargetHeat) != 0);
    lua_setfield(L, -2, "heat_mode");
    lua_pushboolean(L, (capabilities & device_settings::kAcCapabilityTargetCool) != 0);
    lua_setfield(L, -2, "cool_mode");
    lua_pushboolean(L, device_settings::supportsUpDownAirflow());
    lua_setfield(L, -2, "up_down_airflow");
    lua_pushboolean(L, device_settings::supportsLeftRightAirflow());
    lua_setfield(L, -2, "left_right_airflow");
    lua_setfield(L, -2, "capabilities");
    lua_setglobal(L, "kiri");

    lua_createtable(L, 0, 8);
    setFunction(L, "set_power", lAcSetPower, ctx);
    setFunction(L, "set_mode", lAcSetMode, ctx);
    setFunction(L, "set_target_temp", lAcSetTargetTemp, ctx);
    setFunction(L, "set_target_temp_f", lAcSetTargetTemp, ctx);
    setFunction(L, "set_fan", lAcSetFan, ctx);
    setFunction(L, "set_vane", lAcSetVane, ctx);
    setFunction(L, "set_up_down_airflow", lAcSetVane, ctx);
    setFunction(L, "set_wide_vane", lAcSetWideVane, ctx);
    setFunction(L, "set_left_right_airflow", lAcSetWideVane, ctx);
    setFunction(L, "set_swing", lAcSetSwing, ctx);
    lua_setglobal(L, "ac");

    lua_createtable(L, 0, 3);
    setFunction(L, "get", lKvGet, ctx);
    setFunction(L, "set", lKvSet, ctx);
    setFunction(L, "delete", lKvDelete, ctx);
    lua_setglobal(L, "kv");

    lua_createtable(L, 0, 2);
    setFunction(L, "info", lLogInfo, ctx);
    setFunction(L, "warn", lLogWarn, ctx);
    lua_setglobal(L, "log");

    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, lPrint, 1);
    lua_setglobal(L, "print");
}

bool fileExists(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    std::fclose(file);
    return true;
}

bool createLua(lua_State** out, Arena** arena_out, ScriptContext* ctx) {
    auto* arena = static_cast<Arena*>(heap_caps_calloc(1, sizeof(Arena), MALLOC_CAP_8BIT));
    if (arena == nullptr) {
        return false;
    }
    lua_State* L = lua_newstate(arenaAlloc, arena);
    if (L == nullptr) {
        heap_caps_free(arena);
        return false;
    }
    openSafeLibs(L);
    lua_pushlightuserdata(L, ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, "kiri.context");
    lua_sethook(L, instructionHook, LUA_MASKCOUNT, 100);
    *out = L;
    *arena_out = arena;
    return true;
}

uint8_t discoverHooks(lua_State* L) {
    struct Hook {
        const char* name;
        uint8_t bit;
    };
    constexpr Hook hooks[] = {
        {"on_state_changed", lua_vm::kHookStateChanged},
        {"on_power_on", lua_vm::kHookPowerOn},
        {"on_power_off", lua_vm::kHookPowerOff},
    };

    uint8_t mask = 0;
    for (const Hook& hook : hooks) {
        lua_getglobal(L, hook.name);
        if (lua_isfunction(L, -1)) {
            mask |= hook.bit;
        }
        lua_pop(L, 1);
    }
    return mask;
}

lua_vm::ValidationResult validateScriptBuffer(const char* script, size_t script_size) {
    lua_vm::ValidationResult validation{};
    if (script == nullptr || script_size == 0) {
        copyString(validation.message, sizeof(validation.message), "script is empty");
        return validation;
    }
    if (script_size > lua_vm::kMaxScriptBytes) {
        copyString(validation.message, sizeof(validation.message), "script is too large");
        return validation;
    }

    lua_vm::RunResult run{};
    ScriptContext ctx{.result = &run, .instructionsLeft = kInstructionLimit, .validationOnly = true};
    Arena* arena = nullptr;
    lua_State* L = nullptr;
    if (!createLua(&L, &arena, &ctx)) {
        copyString(validation.message, sizeof(validation.message), "lua_newstate failed");
        return validation;
    }

    registerApi(L, &ctx, cn105_core::MockState{});
    int rc = luaL_loadbuffer(L, script, script_size, "automation.lua");
    if (rc == LUA_OK) {
        rc = lua_pcall(L, 0, 0, 0);
    }
    validation.instructionLimitHit = run.instructionLimitHit;
    validation.peakBytes = arena != nullptr ? arena->peak : 0;
    if (rc != LUA_OK) {
        copyString(validation.message, sizeof(validation.message), lua_tostring(L, -1));
        destroyLua(L, arena);
        return validation;
    }

    validation.hookMask = discoverHooks(L);
    if (validation.hookMask == 0) {
        copyString(validation.message, sizeof(validation.message),
                   "define on_state_changed, on_power_on, or on_power_off");
        destroyLua(L, arena);
        return validation;
    }

    validation.ok = true;
    copyString(validation.message, sizeof(validation.message), "ok");
    destroyLua(L, arena);
    return validation;
}

bool readFile(const char* path, std::string* content) {
    if (path == nullptr || content == nullptr) {
        return false;
    }
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long end = std::ftell(file);
    if (end <= 0 || static_cast<size_t>(end) > lua_vm::kMaxScriptBytes ||
        std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    content->resize(static_cast<size_t>(end));
    const size_t read = std::fread(content->data(), 1, content->size(), file);
    std::fclose(file);
    return read == content->size();
}

lua_vm::ValidationResult validateScriptFile(const char* path) {
    std::string script;
    if (!readFile(path, &script)) {
        lua_vm::ValidationResult result{};
        copyString(result.message, sizeof(result.message), "script not found or unreadable");
        return result;
    }
    return validateScriptBuffer(script.data(), script.size());
}

bool writeFileSynced(const char* path, const char* content, size_t content_size) {
    FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        return false;
    }
    const size_t written = std::fwrite(content, 1, content_size, file);
    const bool flushed = std::fflush(file) == 0;
    const bool synced = flushed && fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    return written == content_size && flushed && synced && closed;
}

bool copyFileSynced(const char* source, const char* destination) {
    std::string content;
    return readFile(source, &content) && writeFileSynced(destination, content.data(), content.size());
}

bool restoreBackupIfValid(char* message, size_t message_len) {
    const lua_vm::ValidationResult backup = validateScriptFile(kScriptBackupPhysicalPath);
    if (!backup.ok || !copyFileSynced(kScriptBackupPhysicalPath, kScriptTempPhysicalPath)) {
        return false;
    }
    std::remove(kScriptPhysicalPath);
    if (std::rename(kScriptTempPhysicalPath, kScriptPhysicalPath) != 0) {
        std::remove(kScriptTempPhysicalPath);
        return false;
    }
    setErrorMessage(message, message_len, "restored last-known-good script");
    return true;
}

void destroyLua(lua_State* L, Arena* arena) {
    if (L != nullptr) {
        lua_close(L);
    }
    if (arena != nullptr) {
        heap_caps_free(arena);
    }
}

const char* actionName(lua_vm::ActionType type) {
    switch (type) {
        case lua_vm::ActionType::kSetPower: return "set_power";
        case lua_vm::ActionType::kSetMode: return "set_mode";
        case lua_vm::ActionType::kSetTargetTemperatureF: return "set_target_temp_f";
        case lua_vm::ActionType::kSetFan: return "set_fan";
        case lua_vm::ActionType::kSetVane: return "set_vane";
        case lua_vm::ActionType::kSetWideVane: return "set_wide_vane";
        case lua_vm::ActionType::kNone:
        default: return "none";
    }
}

}  // namespace

namespace lua_vm {

const char* scriptLogicalPath() {
    return kScriptLogicalPath;
}

const char* scriptPhysicalPath() {
    return kScriptPhysicalPath;
}

bool isEnabled() {
    nvs_handle_t handle = 0;
    uint8_t enabled = 0;
    if (nvs_open(kConfigNamespace, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, kEnabledKey, &enabled);
        nvs_close(handle);
    }
    return enabled != 0;
}

bool setEnabled(bool enabled, char* error, size_t error_len) {
    if (enabled) {
        lua_vm::ValidationResult validation = validateScriptFile(kScriptPhysicalPath);
        if (!validation.ok) {
            char recovery[96] = {};
            if (!restoreBackupIfValid(recovery, sizeof(recovery))) {
                char message[160] = {};
                std::snprintf(message, sizeof(message), "script validation failed: %s", validation.message);
                setErrorMessage(error, error_len, message);
                return false;
            }
            validation = validateScriptFile(kScriptPhysicalPath);
            if (!validation.ok) {
                setErrorMessage(error, error_len, "last-known-good script validation failed");
                return false;
            }
        }
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kConfigNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, kEnabledKey, enabled ? 1 : 0);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        setErrorMessage(error, error_len, esp_err_to_name(err));
        return false;
    }

    platform_lock::ScopedLock lock(status_lock);
    runtime_status.enabled = enabled;
    refreshScriptInfoLocked(&runtime_status);
    setErrorMessage(error, error_len, "ok");
    return true;
}

ValidationResult validateScript(const char* script, size_t scriptSize) {
    return validateScriptBuffer(script, scriptSize);
}

bool saveScript(const char* script, size_t scriptSize, ValidationResult* validation) {
    ValidationResult checked = validateScriptBuffer(script, scriptSize);
    if (validation != nullptr) {
        *validation = checked;
    }
    if (!checked.ok || !writeFileSynced(kScriptTempPhysicalPath, script, scriptSize)) {
        if (checked.ok && validation != nullptr) {
            validation->ok = false;
            copyString(validation->message, sizeof(validation->message), "temporary script write failed");
        }
        std::remove(kScriptTempPhysicalPath);
        return false;
    }

    const ValidationResult current = validateScriptFile(kScriptPhysicalPath);
    if (current.ok) {
        std::remove(kScriptBackupPhysicalPath);
        if (std::rename(kScriptPhysicalPath, kScriptBackupPhysicalPath) != 0) {
            std::remove(kScriptTempPhysicalPath);
            if (validation != nullptr) {
                validation->ok = false;
                copyString(validation->message, sizeof(validation->message), "failed to preserve current script");
            }
            return false;
        }
    } else {
        std::remove(kScriptPhysicalPath);
    }

    if (std::rename(kScriptTempPhysicalPath, kScriptPhysicalPath) != 0) {
        std::remove(kScriptTempPhysicalPath);
        if (current.ok) {
            std::rename(kScriptBackupPhysicalPath, kScriptPhysicalPath);
        }
        if (validation != nullptr) {
            validation->ok = false;
            copyString(validation->message, sizeof(validation->message), "failed to activate validated script");
        }
        return false;
    }

    platform_lock::ScopedLock lock(status_lock);
    refreshScriptInfoLocked(&runtime_status);
    return true;
}

RuntimeStatus getRuntimeStatus() {
    platform_lock::ScopedLock lock(status_lock);
    RuntimeStatus status = runtime_status;
    status.enabled = isEnabled();
    refreshScriptInfoLocked(&status);
    return status;
}

void recordActionQueued() {
    platform_lock::ScopedLock lock(status_lock);
    runtime_status.actionSequence++;
    runtime_status.actionPending = true;
    runtime_status.lastActionConfirmed = false;
    runtime_status.lastActionAttempts = 0;
    copyString(runtime_status.lastActionMessage,
               sizeof(runtime_status.lastActionMessage),
               "queued for CN105 confirmation");
}

void recordActionResult(bool confirmed, uint8_t attempts, const char* message) {
    platform_lock::ScopedLock lock(status_lock);
    runtime_status.actionPending = false;
    runtime_status.lastActionConfirmed = confirmed;
    runtime_status.lastActionAttempts = attempts;
    copyString(runtime_status.lastActionMessage,
               sizeof(runtime_status.lastActionMessage),
               message == nullptr ? (confirmed ? "CN105 state confirmed" : "CN105 action failed") : message);
}

ProbeResult runProbe() {
    RunResult result{};
    ScriptContext ctx{.result = &result};
    Arena* arena = nullptr;
    lua_State* L = nullptr;
    if (!createLua(&L, &arena, &ctx)) {
        return {false, 0, "lua_newstate failed"};
    }

    constexpr const char* script =
        "local x = 0\n"
        "for i = 1, 64 do x = x + i end\n"
        "return x == 2080\n";
    int rc = luaL_loadstring(L, script);
    if (rc == LUA_OK) {
        rc = lua_pcall(L, 0, 1, 0);
    }

    bool ok = false;
    const char* message = "ok";
    if (rc == LUA_OK) {
        ok = lua_toboolean(L, -1);
        message = ok ? "ok" : "unexpected result";
    } else {
        message = lua_tostring(L, -1);
    }

    const size_t peak = arena != nullptr ? arena->peak : 0;
    ESP_LOGI(TAG, "Lua probe %s peak=%u bytes", message ? message : "unknown",
             static_cast<unsigned>(peak));
    destroyLua(L, arena);
    return {ok, peak, message ? message : "unknown"};
}

RunResult runHook(const char* hookName,
                  const cn105_core::MockState& state,
                  const cn105_core::MockState* previousState) {
    RunResult result{};
    if (!isEnabled()) {
        setMessage(&result, "automation disabled");
        return result;
    }
    if (hookName == nullptr || hookName[0] == '\0') {
        setMessage(&result, "missing hook name");
        return result;
    }
    if (!fileExists(kScriptPhysicalPath)) {
        setMessage(&result, "automation script not found");
        return result;
    }

    ScriptContext ctx{.result = &result};
    Arena* arena = nullptr;
    lua_State* L = nullptr;
    if (!createLua(&L, &arena, &ctx)) {
        setMessage(&result, "lua_newstate failed");
        return result;
    }

    registerApi(L, &ctx, state);
    result.scriptLoaded = true;

    int rc = luaL_loadfile(L, kScriptPhysicalPath);
    if (rc == LUA_OK) {
        rc = lua_pcall(L, 0, 0, 0);
    }
    if (rc != LUA_OK) {
        setMessage(&result, lua_tostring(L, -1));
        result.peakBytes = arena != nullptr ? arena->peak : 0;
        recordRun(hookName, result);
        destroyLua(L, arena);
        return result;
    }

    lua_getglobal(L, hookName);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        result.ok = true;
        result.hookFound = false;
        setMessage(&result, "hook not defined");
        result.peakBytes = arena != nullptr ? arena->peak : 0;
        recordRun(hookName, result);
        destroyLua(L, arena);
        return result;
    }

    result.hookFound = true;
    pushState(L, state);
    if (previousState != nullptr) {
        pushState(L, *previousState);
    } else {
        lua_pushnil(L);
    }
    rc = lua_pcall(L, 2, 0, 0);
    result.peakBytes = arena != nullptr ? arena->peak : 0;
    if (rc != LUA_OK) {
        setMessage(&result, lua_tostring(L, -1));
        recordRun(hookName, result);
        destroyLua(L, arena);
        return result;
    }

    result.ok = true;
    setMessage(&result, "ok");
    for (size_t i = 0; i < result.actionCount; ++i) {
        ESP_LOGI(TAG, "Lua action[%u]: %s value=%s int=%d",
                 static_cast<unsigned>(i),
                 actionName(result.actions[i].type),
                 result.actions[i].value,
                 result.actions[i].intValue);
    }
    recordRun(hookName, result);
    destroyLua(L, arena);
    return result;
}

}  // namespace lua_vm
