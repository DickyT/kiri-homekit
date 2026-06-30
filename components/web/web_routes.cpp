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

#include "web_routes.h"

#include "app_config.h"
#include "board_profile.h"
#include "build_info.h"
#include "cn105_core.h"
#include "cn105_transport.h"
#include "cn105_uart.h"
#include "device_settings.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "homekit_bridge.h"
#include "lua_vm.h"
#include "ota_handler.h"
#include "platform_fs.h"
#include "platform_log.h"
#include "platform_maintenance.h"
#include "platform_provisioning.h"
#include "platform_wifi.h"
#include "web_http.h"
#include "web_pages.h"

#include "jsmn.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/time.h>

namespace {


uint64_t uptimeMs() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}


uint64_t currentUnixMs(bool* valid) {
    timeval tv {};
    gettimeofday(&tv, nullptr);
    const uint64_t unix_ms = (static_cast<uint64_t>(tv.tv_sec) * 1000ULL) +
                             static_cast<uint64_t>(tv.tv_usec / 1000);
    const bool looks_valid = tv.tv_sec >= 1704067200;  // 2024-01-01 UTC
    if (valid != nullptr) {
        *valid = looks_valid;
    }
    return unix_ms;
}

bool readBody(httpd_req_t* req, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    int remaining = req->content_len;
    size_t total = 0;
    while (remaining > 0 && total + 1 < out_len) {
        const int received = httpd_req_recv(req, out + total, std::min<int>(remaining, static_cast<int>(out_len - total - 1)));
        if (received <= 0) {
            return false;
        }
        total += static_cast<size_t>(received);
        remaining -= received;
    }
    out[total] = '\0';
    return remaining == 0;
}

char* readBodyAlloc(httpd_req_t* req, size_t max_len, size_t* out_len) {
    if (out_len != nullptr) {
        *out_len = 0;
    }
    const size_t content_len = static_cast<size_t>(req->content_len);
    if (content_len > max_len) {
        return nullptr;
    }
    char* body = static_cast<char*>(std::calloc(content_len + 1, sizeof(char)));
    if (body == nullptr) {
        return nullptr;
    }
    size_t remaining = content_len;
    size_t total = 0;
    while (remaining > 0) {
        const int received = httpd_req_recv(req,
                                            body + total,
                                            static_cast<int>(std::min<size_t>(remaining, 1024)));
        if (received <= 0) {
            std::free(body);
            return nullptr;
        }
        total += static_cast<size_t>(received);
        remaining -= received;
    }
    body[total] = '\0';
    if (out_len != nullptr) {
        *out_len = total;
    }
    return body;
}

bool normalizeHomeKitCodeParam(const char* value, char* out_digits, size_t out_len) {
    if (out_digits == nullptr || out_len < 9) {
        return false;
    }
    size_t count = 0;
    if (value != nullptr) {
        for (size_t i = 0; value[i] != '\0'; ++i) {
            if (value[i] >= '0' && value[i] <= '9') {
                if (count >= 8) {
                    return false;
                }
                out_digits[count++] = value[i];
            } else if (value[i] != '-' && value[i] != ' ' && value[i] != '\t') {
                return false;
            }
        }
    }
    if (count != 8) {
        return false;
    }
    out_digits[count] = '\0';
    return true;
}

void writeMockStateJson(const cn105_core::MockState& state, char* out, size_t out_len) {
    char esc_error[128] = {};
    web_http::jsonEscape(state.lastError, esc_error, sizeof(esc_error));
    std::snprintf(out,
                  out_len,
                  "\"mock_state\":{"
                  "\"connected\":%s,"
                  "\"power\":\"%s\","
                  "\"mode\":\"%s\","
                  "\"target_temperature_f\":%d,"
                  "\"room_temperature_f\":%d,"
                  "\"fan\":\"%s\","
                  "\"vane\":\"%s\","
                  "\"wide_vane\":\"%s\","
                  "\"operating\":%s,"
                  "\"compressor_frequency_hz\":%d,"
                  "\"input_power_w\":%d,"
                  "\"energy_kwh\":%.1f,"
                  "\"last_packet_hex\":\"%s\","
                  "\"last_error\":\"%s\""
                  "}",
                  state.connected ? "true" : "false",
                  state.power,
                  state.mode,
                  state.targetTemperatureF,
                  state.roomTemperatureF,
                  state.fan,
                  state.vane,
                  state.wideVane,
                  state.operating ? "true" : "false",
                  state.compressorFrequencyHz,
                  state.inputPowerW,
                  static_cast<double>(state.energyKwh),
                  state.lastPacketHex,
                  esc_error);
}

void writeHomeKitJson(const homekit_bridge::Status& status, char* out, size_t out_len) {
    char esc_error[128] = {};
    char esc_name[128] = {};
    char esc_model[128] = {};
    char esc_fw[64] = {};
    web_http::jsonEscape(status.lastError, esc_error, sizeof(esc_error));
    web_http::jsonEscape(status.accessoryName, esc_name, sizeof(esc_name));
    web_http::jsonEscape(status.model, esc_model, sizeof(esc_model));
    web_http::jsonEscape(status.firmwareRevision, esc_fw, sizeof(esc_fw));
    std::snprintf(out,
                  out_len,
                  "\"homekit\":{"
                  "\"enabled\":%s,"
                  "\"started\":%s,"
                  "\"paired_controllers\":%d,"
                  "\"accessory_name\":\"%s\","
                  "\"model\":\"%s\","
                  "\"firmware_revision\":\"%s\","
                  "\"setup_code\":\"%s\","
                  "\"setup_id\":\"%s\","
                  "\"setup_payload\":\"%s\","
                  "\"last_event\":\"%s\","
                  "\"last_error\":\"%s\""
                  "}",
                  status.enabled ? "true" : "false",
                  status.started ? "true" : "false",
                  status.pairedControllers,
                  esc_name,
                  esc_model,
                  esc_fw,
                  status.setupCode,
                  status.setupId,
                  status.setupPayload,
                  status.lastEvent,
                  esc_error);
}

std::string quotedJson(const char* value) {
    return "\"" + platform_fs::jsonEscape(value == nullptr ? "" : value) + "\"";
}

bool homeKitDatabaseSettingsChanged(const device_settings::Settings& before, const device_settings::Settings& after) {
    return std::strcmp(before.deviceName, after.deviceName) != 0 ||
           std::strcmp(before.homeKitManufacturer, after.homeKitManufacturer) != 0 ||
           std::strcmp(before.homeKitModel, after.homeKitModel) != 0 ||
           std::strcmp(before.homeKitSerial, after.homeKitSerial) != 0 ||
           std::strcmp(before.homeKitSetupId, after.homeKitSetupId) != 0 ||
           before.homeKitSeparateAirflowTile != after.homeKitSeparateAirflowTile ||
           (before.acCapabilities & device_settings::kAcCapabilityTargetMask) !=
               (after.acCapabilities & device_settings::kAcCapabilityTargetMask);
}

std::string deviceCfgJson() {
    const device_settings::Settings& s = device_settings::get();
    std::string body = "{\n";
    body += "  \"device_name\": " + quotedJson(s.deviceName) + ",\n";
    body += "  \"wifi_ssid\": " + quotedJson(s.wifiSsid) + ",\n";
    body += "  \"wifi_pass\": " + quotedJson(s.wifiPassword) + ",\n";
    body += "  \"hk_code\": " + quotedJson(s.homeKitCode) + ",\n";
    body += "  \"hk_mfr\": " + quotedJson(s.homeKitManufacturer) + ",\n";
    body += "  \"hk_model\": " + quotedJson(s.homeKitModel) + ",\n";
    body += "  \"hk_serial\": " + quotedJson(s.homeKitSerial) + ",\n";
    body += "  \"hk_setupid\": " + quotedJson(s.homeKitSetupId) + ",\n";
    body += "  \"hk_airtile\": " + std::string(s.homeKitSeparateAirflowTile ? "true" : "false") + ",\n";
    body += "  \"ac_caps\": " + std::to_string(s.acCapabilities) + ",\n";
    body += "  \"use_real\": " + std::string(s.useRealCn105 ? "true" : "false") + ",\n";
    body += "  \"led_pin\": " + std::to_string(s.statusLedPin) + ",\n";
    body += "  \"rx_pin\": " + std::to_string(s.cn105RxPin) + ",\n";
    body += "  \"tx_pin\": " + std::to_string(s.cn105TxPin) + ",\n";
    body += "  \"baud\": " + std::to_string(s.cn105BaudRate) + ",\n";
    body += "  \"data_bits\": " + std::to_string(s.cn105DataBits) + ",\n";
    char parity[2] = {s.cn105Parity, '\0'};
    body += "  \"parity\": " + quotedJson(parity) + ",\n";
    body += "  \"stop_bits\": " + std::to_string(s.cn105StopBits) + ",\n";
    body += "  \"rx_pull\": " + std::string(s.cn105RxPullupEnabled ? "true" : "false") + ",\n";
    body += "  \"tx_od\": " + std::string(s.cn105TxOpenDrain ? "true" : "false") + ",\n";
    body += "  \"poll_on\": " + std::to_string(s.pollIntervalActiveMs) + ",\n";
    body += "  \"poll_off\": " + std::to_string(s.pollIntervalOffMs) + ",\n";
    body += "  \"log_level\": " + quotedJson(device_settings::logLevelName()) + "\n";
    body += "}\n";
    return body;
}

bool tokenEquals(const char* json, const jsmntok_t& token, const char* value) {
    if (json == nullptr || value == nullptr || token.start < 0 || token.end < token.start) {
        return false;
    }
    const size_t len = static_cast<size_t>(token.end - token.start);
    return std::strlen(value) == len && std::strncmp(json + token.start, value, len) == 0;
}

bool decodeJsonString(const char* src, const jsmntok_t& token, char* out, size_t out_len) {
    if (src == nullptr || out == nullptr || out_len == 0 || token.type != JSMN_STRING) {
        return false;
    }
    size_t written = 0;
    for (int i = token.start; i < token.end && written + 1 < out_len; ++i) {
        char c = src[i];
        if (c == '\\' && i + 1 < token.end) {
            const char esc = src[++i];
            switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: c = esc; break;
            }
        }
        out[written++] = c;
    }
    out[written] = '\0';
    return true;
}

bool tokenText(const char* json, const jsmntok_t& token, char* out, size_t out_len) {
    if (json == nullptr || out == nullptr || out_len == 0 || token.start < 0 || token.end < token.start) {
        return false;
    }
    if (token.type == JSMN_STRING) {
        return decodeJsonString(json, token, out, out_len);
    }
    const size_t len = std::min(static_cast<size_t>(token.end - token.start), out_len - 1);
    std::memcpy(out, json + token.start, len);
    out[len] = '\0';
    return true;
}

bool parseJsonBoolValue(const char* json, const jsmntok_t& token, bool* out) {
    char value[16] = {};
    if (out == nullptr || !tokenText(json, token, value, sizeof(value))) {
        return false;
    }
    for (char* p = value; *p != '\0'; ++p) {
        *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }
    if (std::strcmp(value, "true") == 0 || std::strcmp(value, "1") == 0 || std::strcmp(value, "on") == 0) {
        *out = true;
        return true;
    }
    if (std::strcmp(value, "false") == 0 || std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool parseJsonIntValue(const char* json, const jsmntok_t& token, int* out) {
    char value[24] = {};
    if (out == nullptr || !tokenText(json, token, value, sizeof(value))) {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || (end != nullptr && *end != '\0')) {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

const char* luaActionTypeName(lua_vm::ActionType type) {
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

std::string luaActionsJson(const lua_vm::Action* actions, size_t count) {
    std::string json = "[";
    for (size_t i = 0; actions != nullptr && i < count; ++i) {
        if (i > 0) {
            json += ",";
        }
        json += "{\"type\":\"";
        json += luaActionTypeName(actions[i].type);
        json += "\",\"value\":\"";
        json += platform_fs::jsonEscape(actions[i].value);
        json += "\",\"int_value\":";
        json += std::to_string(actions[i].intValue);
        json += "}";
    }
    json += "]";
    return json;
}

std::string automationStatusJson() {
    const lua_vm::RuntimeStatus status = lua_vm::getRuntimeStatus();
    std::string body = "{\"ok\":true";
    body += ",\"enabled\":";
    body += status.enabled ? "true" : "false";
    body += ",\"script_exists\":";
    body += status.scriptExists ? "true" : "false";
    body += ",\"script_size\":";
    body += std::to_string(status.scriptSize);
    body += ",\"script_max_bytes\":";
    body += std::to_string(lua_vm::kMaxScriptBytes);
    body += ",\"last_set\":";
    body += status.lastSet ? "true" : "false";
    body += ",\"last_ok\":";
    body += status.lastOk ? "true" : "false";
    body += ",\"last_script_loaded\":";
    body += status.lastScriptLoaded ? "true" : "false";
    body += ",\"last_hook_found\":";
    body += status.lastHookFound ? "true" : "false";
    body += ",\"last_instruction_limit_hit\":";
    body += status.lastInstructionLimitHit ? "true" : "false";
    body += ",\"last_peak_bytes\":";
    body += std::to_string(status.lastPeakBytes);
    body += ",\"last_hook\":\"";
    body += platform_fs::jsonEscape(status.lastHook);
    body += "\",\"last_message\":\"";
    body += platform_fs::jsonEscape(status.lastMessage);
    body += "\",\"run_count\":";
    body += std::to_string(status.runCount);
    body += ",\"last_actions\":";
    body += luaActionsJson(status.lastActions, status.lastActionCount);
    body += "}";
    return body;
}

int skipJsonToken(const jsmntok_t* tokens, int count, int index) {
    if (tokens == nullptr || index < 0 || index >= count) {
        return index + 1;
    }
    const jsmntok_t& token = tokens[index];
    int next = index + 1;
    if (token.type == JSMN_OBJECT) {
        for (int i = 0; i < token.size && next < count; ++i) {
            next = skipJsonToken(tokens, count, next);
            next = skipJsonToken(tokens, count, next);
        }
    } else if (token.type == JSMN_ARRAY) {
        for (int i = 0; i < token.size && next < count; ++i) {
            next = skipJsonToken(tokens, count, next);
        }
    }
    return next;
}

bool applyDeviceCfgJson(const char* json, size_t len, device_settings::Settings* out, char* error, size_t error_len) {
    if (json == nullptr || out == nullptr) {
        std::snprintf(error, error_len, "invalid request");
        return false;
    }

    jsmn_parser parser;
    jsmntok_t tokens[80] = {};
    jsmn_init(&parser);
    const int count = jsmn_parse(&parser, json, len, tokens, sizeof(tokens) / sizeof(tokens[0]));
    if (count < 1 || tokens[0].type != JSMN_OBJECT) {
        std::snprintf(error, error_len, "invalid JSON object");
        return false;
    }

    int i = 1;
    for (int field = 0; field < tokens[0].size && i + 1 < count; ++field) {
        const jsmntok_t& key = tokens[i];
        const jsmntok_t& value = tokens[i + 1];
        const int next = skipJsonToken(tokens, count, i + 1);
        char text[192] = {};
        int number = 0;
        bool boolean = false;

        if (tokenEquals(json, key, "device_name") && tokenText(json, value, out->deviceName, sizeof(out->deviceName))) { i = next; continue; }
        if (tokenEquals(json, key, "wifi_ssid") && tokenText(json, value, out->wifiSsid, sizeof(out->wifiSsid))) { i = next; continue; }
        if (tokenEquals(json, key, "wifi_pass") && tokenText(json, value, out->wifiPassword, sizeof(out->wifiPassword))) { i = next; continue; }
        if (tokenEquals(json, key, "hk_code") && tokenText(json, value, out->homeKitCode, sizeof(out->homeKitCode))) { i = next; continue; }
        if (tokenEquals(json, key, "hk_mfr") && tokenText(json, value, out->homeKitManufacturer, sizeof(out->homeKitManufacturer))) { i = next; continue; }
        if (tokenEquals(json, key, "hk_model") && tokenText(json, value, out->homeKitModel, sizeof(out->homeKitModel))) { i = next; continue; }
        if (tokenEquals(json, key, "hk_serial") && tokenText(json, value, out->homeKitSerial, sizeof(out->homeKitSerial))) { i = next; continue; }
        if (tokenEquals(json, key, "hk_setupid") && tokenText(json, value, out->homeKitSetupId, sizeof(out->homeKitSetupId))) { i = next; continue; }
        if (tokenEquals(json, key, "hk_airtile") && parseJsonBoolValue(json, value, &boolean)) {
            out->homeKitSeparateAirflowTile = boolean;
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "ac_caps") && parseJsonIntValue(json, value, &number)) {
            if (number < 0) {
                std::snprintf(error, error_len, "invalid AC capabilities");
                return false;
            }
            out->acCapabilities = static_cast<uint32_t>(number);
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "hk_hvac") && tokenText(json, value, text, sizeof(text))) {
            uint8_t mask = device_settings::kHomeKitTargetAllMask;
            if (!device_settings::parseHomeKitTargetModeMask(text, &mask)) {
                std::snprintf(error, error_len, "invalid HomeKit target modes");
                return false;
            }
            out->acCapabilities = (out->acCapabilities & ~device_settings::kAcCapabilityTargetMask) | mask;
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "use_real") && parseJsonBoolValue(json, value, &boolean)) {
            out->useRealCn105 = boolean;
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "led_pin") && parseJsonIntValue(json, value, &number)) {
            out->statusLedPin = number;
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "rx_pin") && parseJsonIntValue(json, value, &number)) {
            out->cn105RxPin = number;
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "tx_pin") && parseJsonIntValue(json, value, &number)) {
            out->cn105TxPin = number;
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "baud") && parseJsonIntValue(json, value, &number)) {
            out->cn105BaudRate = number;
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "data_bits") && parseJsonIntValue(json, value, &number)) {
            out->cn105DataBits = number;
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "parity") && tokenText(json, value, text, sizeof(text))) {
            char parity = 'E';
            if (!device_settings::parseCn105Parity(text, &parity)) {
                std::snprintf(error, error_len, "invalid parity");
                return false;
            }
            out->cn105Parity = parity;
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "stop_bits") && parseJsonIntValue(json, value, &number)) {
            out->cn105StopBits = number;
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "rx_pull") && parseJsonBoolValue(json, value, &boolean)) {
            out->cn105RxPullupEnabled = boolean;
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "tx_od") && parseJsonBoolValue(json, value, &boolean)) {
            out->cn105TxOpenDrain = boolean;
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "poll_on") && parseJsonIntValue(json, value, &number)) {
            out->pollIntervalActiveMs = static_cast<uint32_t>(number);
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "poll_off") && parseJsonIntValue(json, value, &number)) {
            out->pollIntervalOffMs = static_cast<uint32_t>(number);
            i = next;
            continue;
        }
        if (tokenEquals(json, key, "log_level") && tokenText(json, value, text, sizeof(text))) {
            esp_log_level_t level = ESP_LOG_INFO;
            if (!device_settings::parseLogLevel(text, &level)) {
                std::snprintf(error, error_len, "invalid log_level");
                return false;
            }
            out->logLevel = level;
            i = next;
            continue;
        }
        i = next;
    }

    return true;
}

esp_err_t rootHandler(httpd_req_t* req) {
    return web_pages::sendRoot(req);
}

esp_err_t logsHandler(httpd_req_t* req) {
    return web_pages::sendLogs(req);
}

const char* downloadFileName(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return "download.bin";
    }
    const char* slash = std::strrchr(path, '/');
    return slash != nullptr && slash[1] != '\0' ? slash + 1 : path;
}

esp_err_t streamFile(httpd_req_t* req, FILE* file, const char* content_type, const char* logical_path) {
    if (file == nullptr) {
        httpd_resp_set_status(req, "404 Not Found");
        return web_http::sendText(req, "text/plain; charset=utf-8", "File not found");
    }

    char disposition[192] = {};
    std::snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", downloadFileName(logical_path));
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    char buffer[app_config::kPersistentLogReadChunkBytes] = {};
    while (true) {
        const size_t read = std::fread(buffer, 1, sizeof(buffer), file);
        if (read > 0) {
            const esp_err_t err = httpd_resp_send_chunk(req, buffer, read);
            if (err != ESP_OK) {
                std::fclose(file);
                return err;
            }
        }
        if (read < sizeof(buffer)) {
            break;
        }
    }
    std::fclose(file);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t healthHandler(httpd_req_t* req) {
    char body[512] = {};
    char esc_device[128] = {};
    web_http::jsonEscape(device_settings::deviceName(), esc_device, sizeof(esc_device));
    std::snprintf(body,
                  sizeof(body),
                  "{\"ok\":true,\"device\":\"%s\",\"phase\":\"%s\",\"uptime_ms\":%llu}",
                  esc_device,
                  app_config::kPhaseName,
                  static_cast<unsigned long long>(uptimeMs()));

    return web_http::sendText(req, "application/json", body);
}

esp_err_t statusHandler(httpd_req_t* req) {
    const platform_wifi::Status wifi = platform_wifi::getStatus();
    const platform_fs::Status fs = platform_fs::getStatus();
    const cn105_uart::Status cn105 = cn105_uart::getStatus();
    const cn105_core::MockState mock = cn105_core::getMockState();
    const homekit_bridge::Status homekit = homekit_bridge::getStatus();
    const cn105_transport::Status transport = cn105_transport::getStatus();
    const platform_log::Status log = platform_log::getStatus();
    const platform_provisioning::Status provisioning = platform_provisioning::getStatus();
    const esp_partition_t* running_partition = esp_ota_get_running_partition();
    const esp_partition_t* boot_partition = esp_ota_get_boot_partition();
    bool server_time_valid = false;
    const uint64_t server_time_unix_ms = currentUnixMs(&server_time_valid);
    const uint64_t boot_time_unix_ms = server_time_valid && server_time_unix_ms >= uptimeMs()
        ? (server_time_unix_ms - uptimeMs())
        : 0;

    char mock_json[768] = {};
    writeMockStateJson(mock, mock_json, sizeof(mock_json));

    char homekit_json[1024] = {};
    writeHomeKitJson(homekit, homekit_json, sizeof(homekit_json));

    const device_settings::Settings& config = device_settings::get();

    char esc_transport_err[128] = {};
    web_http::jsonEscape(transport.lastError, esc_transport_err, sizeof(esc_transport_err));
    char esc_log_path[160] = {};
    web_http::jsonEscape(log.currentPath, esc_log_path, sizeof(esc_log_path));
    char esc_device_name[128] = {};
    char esc_version[64] = {};
    char esc_config_name[128] = {};
    char esc_config_wifi_ssid[96] = {};
    char esc_config_hk_mfr[128] = {};
    char esc_config_hk_model[128] = {};
    char esc_config_hk_serial[128] = {};
    char esc_config_hk_setup_id[16] = {};
    char esc_homekit_code[32] = {};
    char esc_prov_stage[32] = {};
    char esc_prov_result[32] = {};
    char esc_prov_service[64] = {};
    char esc_prov_ssid[96] = {};
    web_http::jsonEscape(device_settings::deviceName(), esc_device_name, sizeof(esc_device_name));
    web_http::jsonEscape(build_info::firmwareVersion(), esc_version, sizeof(esc_version));
    web_http::jsonEscape(config.deviceName, esc_config_name, sizeof(esc_config_name));
    web_http::jsonEscape(config.wifiSsid, esc_config_wifi_ssid, sizeof(esc_config_wifi_ssid));
    web_http::jsonEscape(config.homeKitManufacturer, esc_config_hk_mfr, sizeof(esc_config_hk_mfr));
    web_http::jsonEscape(config.homeKitModel, esc_config_hk_model, sizeof(esc_config_hk_model));
    web_http::jsonEscape(config.homeKitSerial, esc_config_hk_serial, sizeof(esc_config_hk_serial));
    web_http::jsonEscape(config.homeKitSetupId, esc_config_hk_setup_id, sizeof(esc_config_hk_setup_id));
    web_http::jsonEscape(device_settings::homeKitDisplayCode(), esc_homekit_code, sizeof(esc_homekit_code));
    web_http::jsonEscape(provisioning.stage, esc_prov_stage, sizeof(esc_prov_stage));
    web_http::jsonEscape(provisioning.lastResult, esc_prov_result, sizeof(esc_prov_result));
    web_http::jsonEscape(provisioning.serviceName, esc_prov_service, sizeof(esc_prov_service));
    web_http::jsonEscape(provisioning.pendingSsid, esc_prov_ssid, sizeof(esc_prov_ssid));

    constexpr size_t kStatusBodyLen = 10240;
    char* body = static_cast<char*>(std::calloc(kStatusBodyLen, sizeof(char)));
    if (body == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "response allocation failed");
    }

    const char* transport_mode = device_settings::useRealCn105() ? "real" : "mock";

    std::snprintf(body,
                  kStatusBodyLen,
                  "{"
                  "\"ok\":true,"
                  "\"device\":\"%s\","
                  "\"version\":\"%s\","
                  "\"phase\":\"%s\","
                  "\"board\":{\"id\":\"%s\",\"name\":\"%s\",\"target\":\"%s\",\"defaults\":{\"led_pin\":%d,\"cn105_rx_pin\":%d,\"cn105_tx_pin\":%d,\"provisioning_button_gpio\":%d}},"
                  "\"server_time_unix_ms\":%llu,"
                  "\"boot_time_unix_ms\":%llu,"
                  "\"boot_time_valid\":%s,"
                  "\"uptime_ms\":%llu,"
                  "\"config\":{"
                  "\"device_name\":\"%s\","
                  "\"wifi_ssid\":\"%s\","
                  "\"wifi_password_set\":%s,"
                  "\"homekit_code\":\"%s\","
                  "\"homekit_manufacturer\":\"%s\","
                  "\"homekit_model\":\"%s\","
                  "\"homekit_serial\":\"%s\","
                  "\"homekit_setup_id\":\"%s\","
                  "\"homekit_separate_airflow_tile\":%s,"
                  "\"ac_capabilities\":%lu,"
                  "\"homekit_hvac_modes\":\"%s\","
                  "\"led_pin\":%d,"
                  "\"cn105_mode\":\"%s\","
                  "\"cn105_rx_pin\":%d,"
                  "\"cn105_tx_pin\":%d,"
                  "\"cn105_baud\":%d,"
                  "\"cn105_data_bits\":%d,"
                  "\"cn105_parity\":\"%c\","
                  "\"cn105_stop_bits\":%d,"
                  "\"cn105_rx_pullup\":%s,"
                  "\"cn105_tx_open_drain\":%s,"
                  "\"poll_active_ms\":%lu,"
                  "\"poll_off_ms\":%lu,"
                  "\"log_level\":\"%s\"},"
                  "\"wifi\":{\"initialized\":%s,\"connected\":%s,\"mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"channel\":%d,\"mac\":\"%s\",\"bssid\":\"%s\",\"last_event\":\"%s\",\"last_event_age_ms\":%lu},"
                  "\"provisioning\":{\"initialized\":%s,\"active\":%s,\"credentials_received\":%s,\"reboot_pending\":%s,\"remaining_ms\":%lu,\"button_gpio\":%d,\"stage\":\"%s\",\"last_result\":\"%s\",\"service_name\":\"%s\",\"pending_ssid\":\"%s\"},"
                  "%s,"
                  "\"filesystem\":{\"mounted\":%s,\"base_path\":\"%s\",\"total_bytes\":%u,\"used_bytes\":%u,\"free_bytes\":%u},"
                  "\"ota\":{\"running_partition\":\"%s\",\"boot_partition\":\"%s\",\"running_address\":%u,\"boot_address\":%u},"
                  "\"log\":{\"active\":%s,\"current\":\"%s\",\"current_bytes\":%u,\"dropped_lines\":%u,\"level\":\"%s\"},"
                  "\"cn105\":{\"uart_initialized\":%s,\"uart\":%d,\"rx_pin\":%d,\"tx_pin\":%d,\"baud\":%d,\"format\":\"%s\",\"transport\":\"%s\","
                  "\"transport_status\":{\"running\":%s,\"connected\":%s,\"phase\":\"%s\",\"connect_attempts\":%lu,\"poll_cycles\":%lu,\"rx_packets\":%lu,\"rx_errors\":%lu,\"tx_packets\":%lu,\"sets_pending\":%lu,\"last_error\":\"%s\"},"
                  "%s}"
                  "}",
                  esc_device_name,
                  esc_version,
                  app_config::kPhaseName,
                  app_config::kBoardId,
                  app_config::kBoardName,
                  app_config::kIdfTarget,
                  board_profile::kDefaultStatusLedPin,
                  board_profile::kDefaultCn105RxPin,
                  board_profile::kDefaultCn105TxPin,
                  app_config::kProvisioningButtonPin,
                  static_cast<unsigned long long>(server_time_unix_ms),
                  static_cast<unsigned long long>(boot_time_unix_ms),
                  server_time_valid ? "true" : "false",
                  static_cast<unsigned long long>(uptimeMs()),
                  esc_config_name,
                  esc_config_wifi_ssid,
                  config.wifiPassword[0] == '\0' ? "false" : "true",
                  esc_homekit_code,
                  esc_config_hk_mfr,
                  esc_config_hk_model,
                  esc_config_hk_serial,
                  esc_config_hk_setup_id,
                  config.homeKitSeparateAirflowTile ? "true" : "false",
                  static_cast<unsigned long>(config.acCapabilities),
                  device_settings::homeKitTargetModeMaskName(),
                  config.statusLedPin,
                  transport_mode,
                  config.cn105RxPin,
                  config.cn105TxPin,
                  config.cn105BaudRate,
                  config.cn105DataBits,
                  config.cn105Parity,
                  config.cn105StopBits,
                  config.cn105RxPullupEnabled ? "true" : "false",
                  config.cn105TxOpenDrain ? "true" : "false",
                  static_cast<unsigned long>(config.pollIntervalActiveMs),
                  static_cast<unsigned long>(config.pollIntervalOffMs),
                  device_settings::logLevelName(),
                  wifi.initialized ? "true" : "false",
                  wifi.staConnected ? "true" : "false",
                  wifi.mode,
                  wifi.ssid,
                  wifi.ip,
                  wifi.rssi,
                  wifi.channel,
                  wifi.mac,
                  wifi.bssid,
                  wifi.lastEvent,
                  static_cast<unsigned long>(wifi.lastEventAgeMs),
                  provisioning.initialized ? "true" : "false",
                  provisioning.active ? "true" : "false",
                  provisioning.credentialsReceived ? "true" : "false",
                  provisioning.rebootPending ? "true" : "false",
                  static_cast<unsigned long>(provisioning.remainingMs),
                  provisioning.buttonGpio,
                  esc_prov_stage,
                  esc_prov_result,
                  esc_prov_service,
                  esc_prov_ssid,
                  homekit_json,
                  fs.mounted ? "true" : "false",
                  platform_fs::basePath(),
                  static_cast<unsigned>(fs.totalBytes),
                  static_cast<unsigned>(fs.usedBytes),
                  static_cast<unsigned>(fs.freeBytes),
                  running_partition == nullptr ? "" : running_partition->label,
                  boot_partition == nullptr ? "" : boot_partition->label,
                  static_cast<unsigned>(running_partition == nullptr ? 0 : running_partition->address),
                  static_cast<unsigned>(boot_partition == nullptr ? 0 : boot_partition->address),
                  log.active ? "true" : "false",
                  esc_log_path,
                  static_cast<unsigned>(log.currentBytes),
                  static_cast<unsigned>(log.droppedLines),
                  log.levelName,
                  cn105.initialized ? "true" : "false",
                  cn105.uart,
                  cn105.rxPin,
                  cn105.txPin,
                  cn105.baudRate,
                  cn105.format,
                  transport_mode,
                  transport.taskRunning ? "true" : "false",
                  transport.connected ? "true" : "false",
                  transport.phase,
                  static_cast<unsigned long>(transport.connectAttempts),
                  static_cast<unsigned long>(transport.pollCycles),
                  static_cast<unsigned long>(transport.rxPackets),
                  static_cast<unsigned long>(transport.rxErrors),
                  static_cast<unsigned long>(transport.txPackets),
                  static_cast<unsigned long>(transport.setsPending),
                  esc_transport_err,
                  mock_json);

    const esp_err_t err = web_http::sendText(req, "application/json", body);
    std::free(body);
    return err;
}

esp_err_t cn105MockStatusHandler(httpd_req_t* req) {
    const cn105_core::MockState mock = cn105_core::getMockState();
    char mock_json[768] = {};
    writeMockStateJson(mock, mock_json, sizeof(mock_json));

    char body[1024] = {};
    std::snprintf(body, sizeof(body), "{\"ok\":true,\"transport\":\"mock\",%s}", mock_json);
    return web_http::sendText(req, "application/json", body);
}

esp_err_t cn105RefreshHandler(httpd_req_t* req) {
    if (device_settings::useRealCn105()) {
        cn105_transport::RefreshResult result{};
        if (!cn105_transport::requestFullInfoPollAndWait(&result)) {
            char body[192] = {};
            std::snprintf(body,
                          sizeof(body),
                          "{\"ok\":false,\"error\":\"%s\",\"received_mask\":%u}",
                          result.message[0] != '\0' ? result.message : "CN105 refresh failed",
                          static_cast<unsigned>(result.receivedMask));
            return web_http::sendText(req, "application/json", body);
        }
    }
    return statusHandler(req);
}

esp_err_t cn105BuildSetHandler(httpd_req_t* req) {
    const cn105_core::MockState mock = cn105_core::getMockState();
    char query[512] = {};
    web_http::readQuery(req, query, sizeof(query));

    cn105_core::SetCommand command{};
    char power[16] = {};
    bool any = false;

    if (web_http::queryValue(query, "power", power, sizeof(power))) {
        command.hasPower = true;
        command.power = power;
        any = true;
    }

    char mode[16] = {};
    if (web_http::queryValue(query, "mode", mode, sizeof(mode))) {
        command.hasMode = true;
        command.mode = mode;
        any = true;
    }

    char temp[16] = {};
    if (web_http::queryValue(query, "temperature_f", temp, sizeof(temp)) || web_http::queryValue(query, "temp_f", temp, sizeof(temp))) {
        command.hasTemperatureF = true;
        command.temperatureF = std::atoi(temp);
        any = true;
    }

    char fan[16] = {};
    if (web_http::queryValue(query, "fan", fan, sizeof(fan))) {
        command.hasFan = true;
        command.fan = fan;
        any = true;
    }

    char vane[16] = {};
    if (web_http::queryValue(query, "vane", vane, sizeof(vane))) {
        if (!device_settings::supportsUpDownAirflow()) {
            return web_http::sendJsonError(req, "up/down airflow is disabled in AC capabilities");
        }
        command.hasVane = true;
        command.vane = vane;
        any = true;
    }

    char wide_vane[32] = {};
    if (web_http::queryValue(query, "wide_vane", wide_vane, sizeof(wide_vane))) {
        if (!device_settings::supportsLeftRightAirflow()) {
            return web_http::sendJsonError(req, "left/right airflow is disabled in AC capabilities");
        }
        command.hasWideVane = true;
        command.wideVane = wide_vane;
        any = true;
    }

    if (!any) {
        command.hasPower = true;
        command.power = mock.power;
        command.hasMode = true;
        command.mode = mock.mode;
        command.hasTemperatureF = true;
        command.temperatureF = mock.targetTemperatureF;
        command.hasFan = true;
        command.fan = mock.fan;
        if (device_settings::supportsUpDownAirflow()) {
            command.hasVane = true;
            command.vane = mock.vane;
        }
        if (device_settings::supportsLeftRightAirflow()) {
            command.hasWideVane = true;
            command.wideVane = mock.wideVane;
        }
    }

    cn105_core::Packet packet{};
    char error[96] = {};
    if (!cn105_core::buildSetPacket(command, &packet, error, sizeof(error))) {
        return web_http::sendJsonError(req, error);
    }

    cn105_core::DecodedPacket decoded{};
    if (!cn105_core::decodePacket(packet.bytes, packet.length, &decoded, error, sizeof(error))) {
        return web_http::sendJsonError(req, error);
    }

    char apply_value[8] = {};
    const bool apply = req->method == HTTP_POST ||
        (web_http::queryValue(query, "apply", apply_value, sizeof(apply_value)) && std::strcmp(apply_value, "1") == 0);
    if (apply) {
        if (device_settings::useRealCn105()) {
            cn105_transport::ApplyResult result{};
            if (!cn105_transport::queueSetCommandAndConfirm(command, &result)) {
                return web_http::sendJsonError(req, result.message[0] != '\0' ? result.message : "CN105 confirmation failed");
            }
        } else if (!cn105_core::applySetPacketToMock(packet.bytes, packet.length, error, sizeof(error))) {
            return web_http::sendJsonError(req, error);
        }
    }

    char packet_hex[cn105_core::kMaxHexLen] = {};
    cn105_core::bytesToHex(packet.bytes, packet.length, packet_hex, sizeof(packet_hex));

    const cn105_core::MockState state = cn105_core::getMockState();
    char mock_json[768] = {};
    writeMockStateJson(state, mock_json, sizeof(mock_json));

    char body[1536] = {};
    std::snprintf(body,
                  sizeof(body),
                  "{\"ok\":true,\"applied\":%s,\"packet_hex\":\"%s\",\"decoded\":{\"type\":\"%s\",\"summary\":\"%s\"},%s}",
                  apply ? "true" : "false",
                  packet_hex,
                  decoded.type,
                  decoded.summary,
                  mock_json);
    return web_http::sendText(req, "application/json", body);
}

esp_err_t cn105DecodeHandler(httpd_req_t* req) {
    char query[768] = {};
    if (!web_http::readQuery(req, query, sizeof(query))) {
        return web_http::sendJsonError(req, "missing query string");
    }

    char hex[256] = {};
    if (!web_http::queryValue(query, "hex", hex, sizeof(hex))) {
        return web_http::sendJsonError(req, "missing hex parameter");
    }

    uint8_t bytes[cn105_core::kPacketLen] = {};
    size_t len = 0;
    char error[96] = {};
    if (!cn105_core::parseHex(hex, bytes, sizeof(bytes), &len, error, sizeof(error))) {
        return web_http::sendJsonError(req, error);
    }

    cn105_core::DecodedPacket decoded{};
    if (!cn105_core::decodePacket(bytes, len, &decoded, error, sizeof(error))) {
        return web_http::sendJsonError(req, error);
    }

    char normalized_hex[cn105_core::kMaxHexLen] = {};
    cn105_core::bytesToHex(bytes, len, normalized_hex, sizeof(normalized_hex));

    char body[768] = {};
    std::snprintf(body,
                  sizeof(body),
                  "{\"ok\":true,\"packet_hex\":\"%s\",\"decoded\":{\"checksum_ok\":%s,\"command\":%u,\"type\":\"%s\",\"info_code\":%u,\"summary\":\"%s\"}}",
                  normalized_hex,
                  decoded.checksumOk ? "true" : "false",
                  decoded.command,
                  decoded.type,
                  decoded.infoCode,
                  decoded.summary);
    return web_http::sendText(req, "application/json", body);
}

esp_err_t adminHandler(httpd_req_t* req) {
    return web_pages::sendAdmin(req);
}

esp_err_t configSaveHandler(httpd_req_t* req) {
    char body[1536] = {};
    if (!readBody(req, body, sizeof(body))) {
        return web_http::sendJsonError(req, "failed to read request body");
    }

    device_settings::Settings next = device_settings::get();

    char value[192] = {};
    if (web_http::queryValue(body, "device_name", value, sizeof(value))) {
        std::strncpy(next.deviceName, value, sizeof(next.deviceName) - 1);
        next.deviceName[sizeof(next.deviceName) - 1] = '\0';
    }
    if (web_http::queryValue(body, "wifi_ssid", value, sizeof(value))) {
        std::strncpy(next.wifiSsid, value, sizeof(next.wifiSsid) - 1);
        next.wifiSsid[sizeof(next.wifiSsid) - 1] = '\0';
    }
    if (web_http::queryValue(body, "wifi_password", value, sizeof(value))) {
        std::strncpy(next.wifiPassword, value, sizeof(next.wifiPassword) - 1);
        next.wifiPassword[sizeof(next.wifiPassword) - 1] = '\0';
    }
    if (web_http::queryValue(body, "homekit_code", value, sizeof(value))) {
        char digits[sizeof(next.homeKitCode)] = {};
        if (!normalizeHomeKitCodeParam(value, digits, sizeof(digits))) {
            return web_http::sendJsonError(req, "invalid HomeKit setup code");
        }
        std::strncpy(next.homeKitCode, digits, sizeof(next.homeKitCode) - 1);
        next.homeKitCode[sizeof(next.homeKitCode) - 1] = '\0';
    }
    if (web_http::queryValue(body, "homekit_manufacturer", value, sizeof(value))) {
        std::strncpy(next.homeKitManufacturer, value, sizeof(next.homeKitManufacturer) - 1);
        next.homeKitManufacturer[sizeof(next.homeKitManufacturer) - 1] = '\0';
    }
    if (web_http::queryValue(body, "homekit_model", value, sizeof(value))) {
        std::strncpy(next.homeKitModel, value, sizeof(next.homeKitModel) - 1);
        next.homeKitModel[sizeof(next.homeKitModel) - 1] = '\0';
    }
    if (web_http::queryValue(body, "homekit_serial", value, sizeof(value))) {
        std::strncpy(next.homeKitSerial, value, sizeof(next.homeKitSerial) - 1);
        next.homeKitSerial[sizeof(next.homeKitSerial) - 1] = '\0';
    }
    if (web_http::queryValue(body, "homekit_setup_id", value, sizeof(value))) {
        std::strncpy(next.homeKitSetupId, value, sizeof(next.homeKitSetupId) - 1);
        next.homeKitSetupId[sizeof(next.homeKitSetupId) - 1] = '\0';
    }
    if (web_http::queryValue(body, "homekit_separate_airflow_tile", value, sizeof(value))) {
        next.homeKitSeparateAirflowTile = std::strcmp(value, "0") != 0 &&
                                          std::strcmp(value, "false") != 0 &&
                                          std::strcmp(value, "off") != 0;
    }
    if (web_http::queryValue(body, "ac_capabilities", value, sizeof(value))) {
        next.acCapabilities = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    }
    if (web_http::queryValue(body, "homekit_hvac_modes", value, sizeof(value))) {
        uint8_t mask = device_settings::kHomeKitTargetAllMask;
        if (!device_settings::parseHomeKitTargetModeMask(value, &mask)) {
            return web_http::sendJsonError(req, "invalid HomeKit target modes");
        }
        next.acCapabilities = (next.acCapabilities & ~device_settings::kAcCapabilityTargetMask) | mask;
    }
    if (web_http::queryValue(body, "led_pin", value, sizeof(value))) {
        next.statusLedPin = std::atoi(value);
    }
    if (web_http::queryValue(body, "cn105_mode", value, sizeof(value))) {
        next.useRealCn105 = std::strcmp(value, "mock") != 0;
    }
    if (web_http::queryValue(body, "cn105_rx_pin", value, sizeof(value))) {
        next.cn105RxPin = std::atoi(value);
    }
    if (web_http::queryValue(body, "cn105_tx_pin", value, sizeof(value))) {
        next.cn105TxPin = std::atoi(value);
    }
    if (web_http::queryValue(body, "cn105_baud", value, sizeof(value))) {
        next.cn105BaudRate = std::atoi(value);
    }
    if (web_http::queryValue(body, "cn105_data_bits", value, sizeof(value))) {
        next.cn105DataBits = std::atoi(value);
    }
    if (web_http::queryValue(body, "cn105_parity", value, sizeof(value))) {
        char parity = 'E';
        if (!device_settings::parseCn105Parity(value, &parity)) {
            return web_http::sendJsonError(req, "invalid CN105 parity");
        }
        next.cn105Parity = parity;
    }
    if (web_http::queryValue(body, "cn105_stop_bits", value, sizeof(value))) {
        next.cn105StopBits = std::atoi(value);
    }
    if (web_http::queryValue(body, "cn105_rx_pullup", value, sizeof(value))) {
        next.cn105RxPullupEnabled = std::strcmp(value, "0") != 0 &&
                                    std::strcmp(value, "false") != 0 &&
                                    std::strcmp(value, "off") != 0;
    }
    if (web_http::queryValue(body, "cn105_tx_open_drain", value, sizeof(value))) {
        next.cn105TxOpenDrain = std::strcmp(value, "0") != 0 &&
                                std::strcmp(value, "false") != 0 &&
                                std::strcmp(value, "off") != 0;
    }
    if (web_http::queryValue(body, "poll_active_ms", value, sizeof(value))) {
        next.pollIntervalActiveMs = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    }
    if (web_http::queryValue(body, "poll_off_ms", value, sizeof(value))) {
        next.pollIntervalOffMs = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    }
    if (web_http::queryValue(body, "log_level", value, sizeof(value))) {
        esp_log_level_t level = ESP_LOG_INFO;
        if (!device_settings::parseLogLevel(value, &level)) {
            return web_http::sendJsonError(req, "invalid log level");
        }
        next.logLevel = level;
    }

    bool reboot_required = false;
    const bool homekit_database_changed = homeKitDatabaseSettingsChanged(device_settings::get(), next);
    char message[192] = {};
    if (!device_settings::save(next, &reboot_required, message, sizeof(message))) {
        return web_http::sendJsonError(req, message);
    }

    if (homekit_database_changed) {
        homekit_bridge::markDatabaseChanged();
    }
    platform_log::applyConfiguredLogLevel();

    char escaped[256] = {};
    web_http::jsonEscape(message, escaped, sizeof(escaped));
    char response[512] = {};
    std::snprintf(response,
                  sizeof(response),
                  "{\"ok\":true,\"reboot_required\":%s,\"message\":\"%s\"}",
                  reboot_required ? "true" : "false",
                  escaped);
    return web_http::sendText(req, "application/json", response);
}

esp_err_t deviceCfgJsonGetHandler(httpd_req_t* req) {
    const std::string body = deviceCfgJson();
    return web_http::sendText(req, "application/json", body.c_str());
}

esp_err_t deviceCfgJsonPostHandler(httpd_req_t* req) {
    char body[4096] = {};
    if (!readBody(req, body, sizeof(body))) {
        return web_http::sendJsonError(req, "failed to read JSON body");
    }

    device_settings::Settings next = device_settings::get();
    char error[128] = {};
    if (!applyDeviceCfgJson(body, std::strlen(body), &next, error, sizeof(error))) {
        return web_http::sendJsonError(req, error);
    }

    bool reboot_required = false;
    const bool homekit_database_changed = homeKitDatabaseSettingsChanged(device_settings::get(), next);
    char message[192] = {};
    if (!device_settings::save(next, &reboot_required, message, sizeof(message))) {
        return web_http::sendJsonError(req, message);
    }

    if (homekit_database_changed) {
        homekit_bridge::markDatabaseChanged();
    }
    platform_log::applyConfiguredLogLevel();

    char escaped[256] = {};
    web_http::jsonEscape(message, escaped, sizeof(escaped));
    char response[512] = {};
    std::snprintf(response,
                  sizeof(response),
                  "{\"ok\":true,\"reboot_required\":%s,\"message\":\"%s\"}",
                  reboot_required ? "true" : "false",
                  escaped);
    return web_http::sendText(req, "application/json", response);
}

esp_err_t assetHandler(httpd_req_t* req) {
    return web_pages::sendAsset(req);
}

esp_err_t logsListHandler(httpd_req_t* req) {
    const platform_log::Status status = platform_log::getStatus();
    std::string body = "{\"ok\":true,\"active\":";
    body += status.active ? "true" : "false";
    body += ",\"current\":\"";
    body += platform_fs::jsonEscape(status.currentPath);
    body += "\",\"current_bytes\":";
    body += std::to_string(status.currentBytes);
    body += ",\"level\":\"";
    body += platform_fs::jsonEscape(status.levelName);
    body += "\"";
    body += ",\"dropped_lines\":";
    body += std::to_string(status.droppedLines);
    body += ",\"logs\":";
    body += platform_log::logsJson();
    body += "}";
    return web_http::sendText(req, "application/json", body.c_str());
}

esp_err_t logFileHandler(httpd_req_t* req) {
    char query[256] = {};
    web_http::readQuery(req, query, sizeof(query));
    char file_name[160] = {};
    web_http::queryValue(query, "file", file_name, sizeof(file_name));

    FILE* file = nullptr;
    if (!platform_log::openLogFile(file_name, &file)) {
        httpd_resp_set_status(req, "404 Not Found");
        return web_http::sendText(req, "text/plain; charset=utf-8", "Log file not found");
    }

    char normalized[160] = {};
    platform_fs::normalizePath(file_name[0] == '\0' ? platform_log::getStatus().currentPath : file_name, normalized, sizeof(normalized));
    return streamFile(req, file, "text/plain; charset=utf-8", normalized);
}

esp_err_t liveLogHandler(httpd_req_t* req) {
    char query[128] = {};
    web_http::readQuery(req, query, sizeof(query));
    char offset_value[32] = {};
    size_t offset = 0;
    if (web_http::queryValue(query, "offset", offset_value, sizeof(offset_value))) {
        offset = static_cast<size_t>(std::strtoul(offset_value, nullptr, 10));
    }

    char text[app_config::kPersistentLogReadChunkBytes + 1] = {};
    size_t next_offset = 0;
    size_t file_size = 0;
    bool reset = false;
    if (!platform_log::readLiveLog(offset,
                                   app_config::kPersistentLogReadChunkBytes,
                                   text,
                                   sizeof(text),
                                   &next_offset,
                                   &file_size,
                                   &reset)) {
        return web_http::sendJsonError(req, "active log file not found");
    }

    const std::string escaped = platform_fs::jsonEscape(text);
    std::string body = "{\"ok\":true,\"size\":";
    body += std::to_string(file_size);
    body += ",\"nextOffset\":";
    body += std::to_string(next_offset);
    body += ",\"reset\":";
    body += reset ? "true" : "false";
    body += ",\"text\":\"";
    body += escaped;
    body += "\"}";
    return web_http::sendText(req, "application/json", body.c_str());
}

esp_err_t fileDeleteHandler(httpd_req_t* req) {
    char query[256] = {};
    web_http::readQuery(req, query, sizeof(query));
    char path[160] = {};
    if (!web_http::queryValue(query, "path", path, sizeof(path))) {
        return web_http::sendJsonError(req, "missing path");
    }

    char message[96] = {};
    const bool ok = platform_fs::removePath(path, message, sizeof(message));
    char escaped[128] = {};
    web_http::jsonEscape(message, escaped, sizeof(escaped));
    char body[192] = {};
    std::snprintf(body, sizeof(body), "{\"ok\":%s,\"message\":\"%s\"}", ok ? "true" : "false", escaped);
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    return web_http::sendText(req, "application/json", body);
}

esp_err_t automationStatusHandler(httpd_req_t* req) {
    const std::string body = automationStatusJson();
    return web_http::sendText(req, "application/json", body.c_str());
}

esp_err_t automationScriptGetHandler(httpd_req_t* req) {
    if (!platform_fs::exists(lua_vm::scriptLogicalPath())) {
        return web_http::sendText(req, "text/plain; charset=utf-8", "");
    }

    const size_t size = platform_fs::fileSize(lua_vm::scriptLogicalPath());
    if (size > lua_vm::kMaxScriptBytes) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return web_http::sendText(req, "text/plain; charset=utf-8", "automation script is too large");
    }

    FILE* file = platform_fs::openRead(lua_vm::scriptLogicalPath());
    if (file == nullptr) {
        httpd_resp_set_status(req, "404 Not Found");
        return web_http::sendText(req, "text/plain; charset=utf-8", "automation script not found");
    }

    std::string body;
    body.resize(size);
    const size_t read = size > 0 ? std::fread(body.data(), 1, size, file) : 0;
    std::fclose(file);
    if (read != size) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return web_http::sendText(req, "text/plain; charset=utf-8", "automation script read failed");
    }
    return web_http::sendText(req, "text/plain; charset=utf-8", body.c_str());
}

esp_err_t automationScriptPutHandler(httpd_req_t* req) {
    size_t body_len = 0;
    char* body = readBodyAlloc(req, lua_vm::kMaxScriptBytes, &body_len);
    if (body == nullptr) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return web_http::sendText(req, "application/json", "{\"ok\":false,\"error\":\"automation script is too large\"}");
    }

    char message[128] = {};
    platform_fs::createDirectory("/scripts", message, sizeof(message));
    const bool ok = platform_fs::createFile(lua_vm::scriptLogicalPath(), body, body_len, message, sizeof(message));
    std::free(body);
    if (!ok) {
        return web_http::sendJsonError(req, message);
    }

    const std::string status = automationStatusJson();
    return web_http::sendText(req, "application/json", status.c_str());
}

esp_err_t automationScriptDeleteHandler(httpd_req_t* req) {
    char message[128] = {};
    if (platform_fs::exists(lua_vm::scriptLogicalPath()) &&
        !platform_fs::removePath(lua_vm::scriptLogicalPath(), message, sizeof(message))) {
        return web_http::sendJsonError(req, message);
    }
    lua_vm::setEnabled(false, message, sizeof(message));
    const std::string status = automationStatusJson();
    return web_http::sendText(req, "application/json", status.c_str());
}

esp_err_t automationEnabledHandler(httpd_req_t* req) {
    char body[64] = {};
    if (!readBody(req, body, sizeof(body))) {
        return web_http::sendJsonError(req, "failed to read request body");
    }

    char value[16] = {};
    if (!web_http::queryValue(body, "enabled", value, sizeof(value))) {
        return web_http::sendJsonError(req, "missing enabled");
    }
    const bool enabled = std::strcmp(value, "1") == 0 ||
                         std::strcmp(value, "true") == 0 ||
                         std::strcmp(value, "on") == 0;
    char error[96] = {};
    if (!lua_vm::setEnabled(enabled, error, sizeof(error))) {
        return web_http::sendJsonError(req, error);
    }
    const std::string status = automationStatusJson();
    return web_http::sendText(req, "application/json", status.c_str());
}

esp_err_t rebootHandler(httpd_req_t* req) {
    web_http::sendText(req, "application/json", "{\"ok\":true,\"message\":\"rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t sendMaintenanceResult(httpd_req_t* req, const platform_maintenance::Result& result) {
    char escaped_message[640] = {};
    web_http::jsonEscape(result.message, escaped_message, sizeof(escaped_message));

    char body[896] = {};
    std::snprintf(body,
                  sizeof(body),
                  "{\"ok\":%s,\"action\":\"%s\",\"rebooting\":%s,\"message\":\"%s\"}",
                  result.ok ? "true" : "false",
                  result.action,
                  result.rebooting ? "true" : "false",
                  escaped_message);
    if (!result.ok) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    return web_http::sendText(req, "application/json", body);
}

esp_err_t resetHomeKitHandler(httpd_req_t* req) {
    return sendMaintenanceResult(req, platform_maintenance::resetHomeKit());
}

esp_err_t clearLogsHandler(httpd_req_t* req) {
    return sendMaintenanceResult(req, platform_maintenance::clearLogs());
}

esp_err_t clearSpiffsHandler(httpd_req_t* req) {
    return sendMaintenanceResult(req, platform_maintenance::clearSpiffs());
}

esp_err_t clearAllNvsHandler(httpd_req_t* req) {
    return sendMaintenanceResult(req, platform_maintenance::clearAllNvs());
}


const web_http::Route ROUTES[] = {
    { "/", HTTP_GET, rootHandler },
    { "/logs", HTTP_GET, logsHandler },
    { "/admin", HTTP_GET, adminHandler },
    { "/assets/*", HTTP_GET, assetHandler },
    { "/api/health", HTTP_GET, healthHandler },
    { "/api/status", HTTP_GET, statusHandler },
    { "/api/reboot", HTTP_POST, rebootHandler },
    { "/api/config/save", HTTP_POST, configSaveHandler },
    { "/api/config/device-cfg-json", HTTP_GET, deviceCfgJsonGetHandler },
    { "/api/config/device-cfg-json", HTTP_POST, deviceCfgJsonPostHandler },
    { "/api/maintenance/reset-homekit", HTTP_POST, resetHomeKitHandler },
    { "/api/maintenance/clear-logs", HTTP_POST, clearLogsHandler },
    { "/api/maintenance/clear-spiffs", HTTP_POST, clearSpiffsHandler },
    { "/api/maintenance/clear-all-nvs", HTTP_POST, clearAllNvsHandler },
    { "/api/logs", HTTP_GET, logsListHandler },
    { "/api/log/file", HTTP_GET, logFileHandler },
    { "/api/log/live", HTTP_GET, liveLogHandler },
    { "/api/files/delete", HTTP_POST, fileDeleteHandler },
    { "/api/automation/status", HTTP_GET, automationStatusHandler },
    { "/api/automation/script", HTTP_GET, automationScriptGetHandler },
    { "/api/automation/script", HTTP_PUT, automationScriptPutHandler },
    { "/api/automation/script", HTTP_DELETE, automationScriptDeleteHandler },
    { "/api/automation/enabled", HTTP_POST, automationEnabledHandler },
    { "/api/cn105/mock/status", HTTP_GET, cn105MockStatusHandler },
    { "/api/cn105/refresh", HTTP_POST, cn105RefreshHandler },
    { "/api/cn105/mock/build-set", HTTP_GET, cn105BuildSetHandler },
    { "/api/cn105/mock/build-set", HTTP_POST, cn105BuildSetHandler },
    { "/api/cn105/decode", HTTP_GET, cn105DecodeHandler },
};

}  // namespace

namespace web_routes {

esp_err_t registerRoutes(httpd_handle_t server) {
    const esp_err_t err = web_http::registerRoutes(server, ROUTES, sizeof(ROUTES) / sizeof(ROUTES[0]));
    if (err != ESP_OK) return err;
    static constexpr const char* kAcceptedVariants[] = {"app"};
    return ota_handler::registerHandlers(server,
                                          {kAcceptedVariants,
                                           sizeof(kAcceptedVariants) / sizeof(kAcceptedVariants[0])});
}

}  // namespace web_routes
