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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "board_profile.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ota_handler.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

namespace {

const char* TAG = "installer_probe";

constexpr uart_port_t kCn105Uart = UART_NUM_1;
constexpr int kCn105RxBufferBytes = 256;
constexpr int kCn105TxBufferBytes = 256;
constexpr int kBaudOptions[] = {2400, 4800, 9600};
constexpr int kStableCn105DataBits = 8;
constexpr char kStableCn105Parity = 'E';
constexpr int kStableCn105StopBits = 1;
constexpr bool kStableCn105RxPull = true;
constexpr bool kStableCn105TxOpenDrain = false;
constexpr size_t kPacketLen = 22;
constexpr TickType_t kProbeWindowTicks = pdMS_TO_TICKS(900);
constexpr TickType_t kProbeSettleTicks = pdMS_TO_TICKS(120);
constexpr TickType_t kLoopDelayTicks = pdMS_TO_TICKS(10);
constexpr uint32_t kRxByteTimeoutMs = 140;
constexpr int WIFI_CONNECTED_BIT = BIT0;
constexpr const char* kDeviceCfgNamespace = "device_cfg";
constexpr const char* kProvisioningPop = "abcd1234";
constexpr uint16_t kHttpPrimaryPort = 8080;
constexpr char kApIp[] = "192.168.4.1";

EventGroupHandle_t wifi_event_group = nullptr;
esp_netif_t* ap_netif = nullptr;
httpd_handle_t web_server = nullptr;
char wifi_ip[16] = "0.0.0.0";
char wifi_ssid[33] = "";
char wifi_password[65] = "";
bool wifi_connected = false;
led_strip_handle_t test_led = nullptr;
int test_led_pin = -1;
char provisioning_service_name[40] = "";

enum class InstallerLedMode : uint8_t {
    Pairing,
    Provisioned,
};

volatile InstallerLedMode installer_led_mode = InstallerLedMode::Pairing;
TaskHandle_t installer_led_task = nullptr;

struct InstallerSettings {
    char device_name[64] = "Kiri Bridge";
    char wifi_ssid[33] = "YOUR_WIFI_SSID";
    char wifi_pass[65] = "YOUR_WIFI_PASSWORD";
    char hk_code[9] = "11112233";
    char hk_mfr[64] = "dkt smart home";
    char hk_model[64] = "Kiri Bridge";
    char hk_serial[64] = "KIRI-BRIDGE";
    char hk_setupid[5] = "DKT1";
    bool use_real = true;
    int led_pin = board_profile::kDefaultStatusLedPin;
    int rx_pin = board_profile::kDefaultCn105RxPin;
    int tx_pin = board_profile::kDefaultCn105TxPin;
    int baud = 2400;
    int data_bits = kStableCn105DataBits;
    char parity = kStableCn105Parity;
    int stop_bits = kStableCn105StopBits;
    bool rx_pull = kStableCn105RxPull;
    bool tx_od = kStableCn105TxOpenDrain;
    uint32_t poll_on = 15000;
    uint32_t poll_off = 60000;
    uint8_t log_level = ESP_LOG_ERROR;
};

struct ProbeResult {
    bool ran = false;
    bool found = false;
    int rx_pin = board_profile::kDefaultCn105RxPin;
    int tx_pin = board_profile::kDefaultCn105TxPin;
    int baud = 2400;
    uint8_t profile = 1;
    uint8_t connect = 0x5A;
    uint32_t rx_bytes = 0;
    uint32_t rx_packets = 0;
    uint32_t rx_errors = 0;
    char summary[192] = "Not run yet";
};

struct ProbeRuntime {
    uint8_t rx_buf[kPacketLen] = {};
    size_t rx_index = 0;
    size_t rx_expected = 0;
    uint32_t last_rx_byte_ms = 0;
    uint32_t rx_bytes = 0;
    uint32_t rx_packets = 0;
    uint32_t rx_errors = 0;
    bool legal_response = false;
};

ProbeResult last_probe;

uint8_t checksum(const uint8_t* bytes, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += bytes[i];
    }
    return static_cast<uint8_t>((0xFC - sum) & 0xFF);
}

bool falseLike(const char* value) {
    return value != nullptr &&
           (std::strcmp(value, "0") == 0 ||
            std::strcmp(value, "false") == 0 ||
            std::strcmp(value, "off") == 0);
}

void copyString(char* out, size_t out_len, const char* value) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    std::snprintf(out, out_len, "%s", value == nullptr ? "" : value);
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void urlDecodeInPlace(char* value, size_t value_len) {
    if (value == nullptr || value_len == 0) {
        return;
    }
    size_t out = 0;
    for (size_t in = 0; value[in] != '\0' && out < value_len - 1; ++in) {
        if (value[in] == '%' && value[in + 1] != '\0' && value[in + 2] != '\0') {
            const int high = hexNibble(value[in + 1]);
            const int low = hexNibble(value[in + 2]);
            if (high >= 0 && low >= 0) {
                value[out++] = static_cast<char>((high << 4) | low);
                in += 2;
                continue;
            }
        }
        value[out++] = value[in] == '+' ? ' ' : value[in];
    }
    value[out] = '\0';
}

bool formValue(const char* body, const char* key, char* value, size_t value_len) {
    if (body == nullptr || key == nullptr || value == nullptr || value_len == 0) {
        return false;
    }
    if (httpd_query_key_value(body, key, value, value_len) != ESP_OK) {
        return false;
    }
    urlDecodeInPlace(value, value_len);
    return true;
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

std::string jsonEscape(const char* src) {
    std::string out;
    if (src == nullptr) {
        return out;
    }
    for (size_t i = 0; src[i] != '\0'; ++i) {
        const char c = src[i];
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void buildProvisioningServiceName(char* out, size_t out_len) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(out,
                  out_len,
                  "PROV_KIRI_%02X",
                  mac[0]);
}

esp_err_t sendText(httpd_req_t* req, const char* type, const char* body) {
    httpd_resp_set_type(req, type);
    return httpd_resp_sendstr(req, body);
}

esp_err_t sendJsonError(httpd_req_t* req, const char* error) {
    std::string body = "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}";
    httpd_resp_set_status(req, "400 Bad Request");
    return sendText(req, "application/json", body.c_str());
}

bool validGpio(int value) {
    return board_profile::validGpio(value);
}

bool validOutputGpio(int value) {
    return board_profile::validOutputGpio(value);
}

bool validBaud(int value) {
    return value == 2400 || value == 4800 || value == 9600;
}

bool validSetupId(const char* value) {
    if (value == nullptr || std::strlen(value) != 4) {
        return false;
    }
    for (size_t i = 0; i < 4; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
            return false;
        }
    }
    return true;
}

bool normalizeHomeKitCode(const char* value, char* out, size_t out_len) {
    if (out == nullptr || out_len < 9) {
        return false;
    }
    size_t count = 0;
    if (value != nullptr) {
        for (size_t i = 0; value[i] != '\0'; ++i) {
            if (value[i] >= '0' && value[i] <= '9') {
                if (count >= 8) return false;
                out[count++] = value[i];
            } else if (value[i] != '-' && value[i] != ' ' && value[i] != '\t') {
                return false;
            }
        }
    }
    if (count != 8) {
        return false;
    }
    out[count] = '\0';
    return true;
}

uart_parity_t parityFromChar(char parity) {
    if (parity == 'N') return UART_PARITY_DISABLE;
    if (parity == 'O') return UART_PARITY_ODD;
    return UART_PARITY_EVEN;
}

uart_stop_bits_t stopBitsFromInt(int stop_bits) {
    return stop_bits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

const char* profileName(uint8_t profile) {
    switch (profile) {
        case 0: return "push-pull";
        case 1: return "push-pull+rx-pullup";
        default: return "unknown";
    }
}

void configureCn105RxPullup(int rx_pin, uint8_t profile) {
    const gpio_num_t rx = static_cast<gpio_num_t>(rx_pin);
    const bool rx_pullup = profile >= 1;

    if (rx_pullup) {
        gpio_pullup_en(rx);
    } else {
        gpio_pullup_dis(rx);
    }
}

esp_err_t reopenCn105Uart(int rx_pin, int tx_pin, int baud, uint8_t profile, char parity = 'E', int stop_bits = 1) {
    uart_driver_delete(kCn105Uart);

    uart_config_t config = {};
    config.baud_rate = baud;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = parityFromChar(parity);
    config.stop_bits = stopBitsFromInt(stop_bits);
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.rx_flow_ctrl_thresh = 0;
    config.source_clk = UART_SCLK_DEFAULT;

    ESP_RETURN_ON_ERROR(uart_param_config(kCn105Uart, &config), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(kCn105Uart,
                                     static_cast<gpio_num_t>(tx_pin),
                                     static_cast<gpio_num_t>(rx_pin),
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG,
                        "uart_set_pin failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(kCn105Uart, kCn105RxBufferBytes, kCn105TxBufferBytes, 0, nullptr, 0),
                        TAG,
                        "uart_driver_install failed");
    configureCn105RxPullup(rx_pin, profile);
    uart_flush_input(kCn105Uart);
    return ESP_OK;
}

void resetProbeRx(ProbeRuntime& runtime) {
    runtime.rx_index = 0;
    runtime.rx_expected = 0;
}

void processProbePacket(ProbeRuntime& runtime, const uint8_t* bytes, size_t len) {
    runtime.rx_packets++;
    if (len < 2) {
        runtime.rx_errors++;
        return;
    }
    const uint8_t expected = checksum(bytes, len - 1);
    if (bytes[len - 1] != expected) {
        runtime.rx_errors++;
        return;
    }
    const uint8_t command = bytes[1];
    if (command == 0x7A || command == 0x7B || command == 0x61 || command == 0x62) {
        runtime.legal_response = true;
    }
}

void processProbeByte(ProbeRuntime& runtime, uint8_t byte) {
    if (runtime.rx_index == 0 && byte != 0xFC) {
        return;
    }
    runtime.rx_buf[runtime.rx_index++] = byte;
    runtime.last_rx_byte_ms = esp_log_timestamp();

    if (runtime.rx_index == 5 && runtime.rx_expected == 0) {
        runtime.rx_expected = static_cast<size_t>(runtime.rx_buf[4]) + 6;
        if (runtime.rx_expected > kPacketLen) {
            runtime.rx_errors++;
            resetProbeRx(runtime);
            return;
        }
    }
    if (runtime.rx_expected > 0 && runtime.rx_index >= runtime.rx_expected) {
        processProbePacket(runtime, runtime.rx_buf, runtime.rx_index);
        resetProbeRx(runtime);
    }
    if (runtime.rx_index >= kPacketLen) {
        runtime.rx_errors++;
        resetProbeRx(runtime);
    }
}

void drainProbeRx(ProbeRuntime& runtime) {
    uint8_t byte = 0;
    while (uart_read_bytes(kCn105Uart, &byte, 1, 0) == 1) {
        runtime.rx_bytes++;
        processProbeByte(runtime, byte);
    }
}

void checkProbeTimeout(ProbeRuntime& runtime) {
    if (runtime.rx_index == 0) {
        return;
    }
    if ((esp_log_timestamp() - runtime.last_rx_byte_ms) >= kRxByteTimeoutMs) {
        runtime.rx_errors++;
        resetProbeRx(runtime);
    }
}

void probeWait(ProbeRuntime& runtime, TickType_t ticks) {
    const TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < ticks) {
        drainProbeRx(runtime);
        checkProbeTimeout(runtime);
        vTaskDelay(kLoopDelayTicks);
    }
}

void sendConnect(uint8_t command) {
    uint8_t packet[8] = {0xFC, command, 0x01, 0x30, 0x02, 0xCA, 0x01, 0x00};
    packet[7] = checksum(packet, 7);
    uart_write_bytes(kCn105Uart, packet, sizeof(packet));
    uart_wait_tx_done(kCn105Uart, pdMS_TO_TICKS(200));
}

void sendInfo(uint8_t info_code) {
    uint8_t packet[kPacketLen] = {};
    packet[0] = 0xFC;
    packet[1] = 0x42;
    packet[2] = 0x01;
    packet[3] = 0x30;
    packet[4] = 0x10;
    packet[5] = info_code;
    packet[kPacketLen - 1] = checksum(packet, kPacketLen - 1);
    uart_write_bytes(kCn105Uart, packet, sizeof(packet));
    uart_wait_tx_done(kCn105Uart, pdMS_TO_TICKS(200));
}

ProbeResult runProbe(int rx_pin, int tx_pin) {
    ProbeResult best{};
    best.ran = true;
    best.rx_pin = rx_pin;
    best.tx_pin = tx_pin;
    copyString(best.summary, sizeof(best.summary), "No legal CN105 response detected");

    uint32_t best_score = 0;
    static constexpr uint8_t profiles[] = {0, 1};
    static constexpr uint8_t connects[] = {0x5A, 0x5B};

    for (int baud : kBaudOptions) {
        for (uint8_t profile : profiles) {
            if (reopenCn105Uart(rx_pin, tx_pin, baud, profile, kStableCn105Parity, kStableCn105StopBits) != ESP_OK) {
                continue;
            }
            ProbeRuntime settle_runtime{};
            probeWait(settle_runtime, pdMS_TO_TICKS(1));
            vTaskDelay(kProbeSettleTicks);

            for (uint8_t connect : connects) {
                ProbeRuntime runtime{};
                sendConnect(connect);
                probeWait(runtime, kProbeWindowTicks);
                if (!runtime.legal_response) {
                    sendInfo(0x02);
                    probeWait(runtime, kProbeWindowTicks);
                }

                const uint32_t score = (runtime.legal_response ? 1000000u : 0u) +
                                       runtime.rx_packets * 1000u +
                                       runtime.rx_bytes -
                                       runtime.rx_errors * 100u;
                if (score > best_score) {
                    best_score = score;
                    best.found = runtime.legal_response;
                    best.rx_pin = rx_pin;
                    best.tx_pin = tx_pin;
                    best.baud = baud;
                    best.profile = profile;
                    best.connect = connect;
                    best.rx_bytes = runtime.rx_bytes;
                    best.rx_packets = runtime.rx_packets;
                    best.rx_errors = runtime.rx_errors;
                    std::snprintf(best.summary,
                                  sizeof(best.summary),
                                  "%s: baud=%d profile=%s connect=0x%02X rxBytes=%lu rxPackets=%lu rxErrors=%lu",
                                  best.found ? "Legal CN105 response detected" : "Only non-legal/echo bytes detected",
                                  baud,
                                  profileName(profile),
                                  connect,
                                  static_cast<unsigned long>(runtime.rx_bytes),
                                  static_cast<unsigned long>(runtime.rx_packets),
                                  static_cast<unsigned long>(runtime.rx_errors));
                }
                uart_flush_input(kCn105Uart);
            }
        }
    }

    if (best_score == 0) {
        best.baud = 2400;
        best.profile = 1;
        best.connect = 0x5A;
    }
    reopenCn105Uart(best.rx_pin, best.tx_pin, best.baud, best.profile, kStableCn105Parity, kStableCn105StopBits);
    last_probe = best;
    return best;
}

void writeString(nvs_handle_t handle, const char* key, const char* value) {
    ESP_ERROR_CHECK(nvs_set_str(handle, key, value == nullptr ? "" : value));
}

void writeSettingsToNvs(const InstallerSettings& s) {
    nvs_handle_t handle = 0;
    ESP_ERROR_CHECK(nvs_open(kDeviceCfgNamespace, NVS_READWRITE, &handle));
    writeString(handle, "device_name", s.device_name);
    writeString(handle, "wifi_ssid", s.wifi_ssid);
    writeString(handle, "wifi_pass", s.wifi_pass);
    writeString(handle, "hk_code", s.hk_code);
    writeString(handle, "hk_mfr", s.hk_mfr);
    writeString(handle, "hk_model", s.hk_model);
    writeString(handle, "hk_serial", s.hk_serial);
    writeString(handle, "hk_setupid", s.hk_setupid);
    ESP_ERROR_CHECK(nvs_set_u8(handle, "use_real", s.use_real ? 1 : 0));
    ESP_ERROR_CHECK(nvs_set_i32(handle, "led_pin", s.led_pin));
    ESP_ERROR_CHECK(nvs_set_i32(handle, "rx_pin", s.rx_pin));
    ESP_ERROR_CHECK(nvs_set_i32(handle, "tx_pin", s.tx_pin));
    ESP_ERROR_CHECK(nvs_set_i32(handle, "baud", s.baud));
    ESP_ERROR_CHECK(nvs_set_i32(handle, "data_bits", s.data_bits));
    char parity_value[2] = {s.parity, '\0'};
    writeString(handle, "parity", parity_value);
    ESP_ERROR_CHECK(nvs_set_i32(handle, "stop_bits", s.stop_bits));
    ESP_ERROR_CHECK(nvs_set_u8(handle, "rx_pull", s.rx_pull ? 1 : 0));
    ESP_ERROR_CHECK(nvs_set_u8(handle, "tx_od", s.tx_od ? 1 : 0));
    ESP_ERROR_CHECK(nvs_set_u32(handle, "poll_on", s.poll_on));
    ESP_ERROR_CHECK(nvs_set_u32(handle, "poll_off", s.poll_off));
    ESP_ERROR_CHECK(nvs_set_u8(handle, "log_level", s.log_level));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
}

const char* logLevelName(uint8_t level) {
    switch (level) {
        case ESP_LOG_ERROR:
            return "error";
        case ESP_LOG_WARN:
            return "warn";
        case ESP_LOG_INFO:
            return "info";
        case ESP_LOG_DEBUG:
            return "debug";
        case ESP_LOG_VERBOSE:
            return "verbose";
        default:
            return "none";
    }
}

bool parseLogLevel(const char* value, uint8_t* out) {
    if (std::strcmp(value, "none") == 0 || std::strcmp(value, "0") == 0) {
        *out = ESP_LOG_NONE;
        return true;
    }
    if (std::strcmp(value, "error") == 0 || std::strcmp(value, "1") == 0) {
        *out = ESP_LOG_ERROR;
        return true;
    }
    if (std::strcmp(value, "warn") == 0 || std::strcmp(value, "warning") == 0 || std::strcmp(value, "2") == 0) {
        *out = ESP_LOG_WARN;
        return true;
    }
    if (std::strcmp(value, "info") == 0 || std::strcmp(value, "3") == 0) {
        *out = ESP_LOG_INFO;
        return true;
    }
    if (std::strcmp(value, "debug") == 0 || std::strcmp(value, "4") == 0) {
        *out = ESP_LOG_DEBUG;
        return true;
    }
    if (std::strcmp(value, "verbose") == 0 || std::strcmp(value, "5") == 0) {
        *out = ESP_LOG_VERBOSE;
        return true;
    }
    return false;
}

bool parseSettingsFromBody(const char* body, InstallerSettings& s, char* error, size_t error_len) {
    char value[192] = {};
    if (formValue(body, "device_name", value, sizeof(value))) copyString(s.device_name, sizeof(s.device_name), value);
    if (formValue(body, "wifi_ssid", value, sizeof(value))) copyString(s.wifi_ssid, sizeof(s.wifi_ssid), value);
    if (formValue(body, "wifi_pass", value, sizeof(value))) copyString(s.wifi_pass, sizeof(s.wifi_pass), value);
    if (formValue(body, "hk_code", value, sizeof(value))) {
        if (!normalizeHomeKitCode(value, s.hk_code, sizeof(s.hk_code))) {
            copyString(error, error_len, "invalid HomeKit setup code");
            return false;
        }
    }
    if (formValue(body, "hk_mfr", value, sizeof(value))) copyString(s.hk_mfr, sizeof(s.hk_mfr), value);
    if (formValue(body, "hk_model", value, sizeof(value))) copyString(s.hk_model, sizeof(s.hk_model), value);
    if (formValue(body, "hk_serial", value, sizeof(value))) copyString(s.hk_serial, sizeof(s.hk_serial), value);
    if (formValue(body, "hk_setupid", value, sizeof(value))) copyString(s.hk_setupid, sizeof(s.hk_setupid), value);
    if (formValue(body, "use_real", value, sizeof(value))) s.use_real = !falseLike(value);
    if (formValue(body, "led_pin", value, sizeof(value))) s.led_pin = std::atoi(value);
    if (formValue(body, "rx_pin", value, sizeof(value))) s.rx_pin = std::atoi(value);
    if (formValue(body, "tx_pin", value, sizeof(value))) s.tx_pin = std::atoi(value);
    if (formValue(body, "baud", value, sizeof(value))) s.baud = std::atoi(value);
    if (formValue(body, "data_bits", value, sizeof(value))) s.data_bits = std::atoi(value);
    if (formValue(body, "parity", value, sizeof(value)) && value[0] != '\0') s.parity = value[0];
    if (formValue(body, "stop_bits", value, sizeof(value))) s.stop_bits = std::atoi(value);
    if (formValue(body, "rx_pull", value, sizeof(value))) s.rx_pull = !falseLike(value);
    if (formValue(body, "tx_od", value, sizeof(value))) s.tx_od = !falseLike(value);
    if (formValue(body, "poll_on", value, sizeof(value))) s.poll_on = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    if (formValue(body, "poll_off", value, sizeof(value))) s.poll_off = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    if (formValue(body, "log_level", value, sizeof(value)) && !parseLogLevel(value, &s.log_level)) {
        copyString(error, error_len, "invalid log level");
        return false;
    }

    if (s.device_name[0] == '\0') {
        copyString(error, error_len, "device name is required");
        return false;
    }
    if (!validSetupId(s.hk_setupid)) {
        copyString(error, error_len, "invalid HomeKit setup id");
        return false;
    }
    if (!validOutputGpio(s.led_pin) || !validGpio(s.rx_pin) || !validOutputGpio(s.tx_pin)) {
        copyString(error, error_len, "invalid GPIO pin");
        return false;
    }
    if (!validBaud(s.baud)) {
        copyString(error, error_len, "invalid CN105 baud rate");
        return false;
    }
    if (s.data_bits != 8 || (s.parity != 'E' && s.parity != 'N' && s.parity != 'O') ||
        (s.stop_bits != 1 && s.stop_bits != 2)) {
        copyString(error, error_len, "invalid CN105 serial format");
        return false;
    }
    if (s.poll_on < 1000 || s.poll_off < 5000) {
        copyString(error, error_len, "poll intervals are too small");
        return false;
    }
    if (s.log_level > ESP_LOG_VERBOSE) {
        copyString(error, error_len, "invalid log level");
        return false;
    }
    return true;
}

std::string settingsJson(const InstallerSettings& s) {
    char body[1600] = {};
    std::snprintf(body,
                  sizeof(body),
                  "{\"device_name\":\"%s\",\"wifi_ssid\":\"%s\",\"wifi_pass\":\"%s\","
                  "\"hk_code\":\"%c%c%c%c-%c%c%c%c\",\"hk_mfr\":\"%s\",\"hk_model\":\"%s\","
                  "\"hk_serial\":\"%s\",\"hk_setupid\":\"%s\",\"use_real\":%s,"
                  "\"led_pin\":%d,\"rx_pin\":%d,\"tx_pin\":%d,\"baud\":%d,\"data_bits\":%d,"
                  "\"parity\":\"%c\",\"stop_bits\":%d,\"rx_pull\":%s,\"tx_od\":%s,"
                  "\"poll_on\":%lu,\"poll_off\":%lu,\"log_level\":\"%s\"}",
                  jsonEscape(s.device_name).c_str(),
                  jsonEscape(s.wifi_ssid).c_str(),
                  jsonEscape(s.wifi_pass).c_str(),
                  s.hk_code[0], s.hk_code[1], s.hk_code[2], s.hk_code[3],
                  s.hk_code[4], s.hk_code[5], s.hk_code[6], s.hk_code[7],
                  jsonEscape(s.hk_mfr).c_str(),
                  jsonEscape(s.hk_model).c_str(),
                  jsonEscape(s.hk_serial).c_str(),
                  jsonEscape(s.hk_setupid).c_str(),
                  s.use_real ? "true" : "false",
                  s.led_pin,
                  s.rx_pin,
                  s.tx_pin,
                  s.baud,
                  s.data_bits,
                  s.parity,
                  s.stop_bits,
                  s.rx_pull ? "true" : "false",
                  s.tx_od ? "true" : "false",
                  static_cast<unsigned long>(s.poll_on),
                  static_cast<unsigned long>(s.poll_off),
                  logLevelName(s.log_level));
    return body;
}

InstallerSettings defaultSettings() {
    InstallerSettings s{};
    // Installer is a recovery path: do not inherit potentially broken app NVS.
    if (wifi_ssid[0] != '\0') {
        copyString(s.wifi_ssid, sizeof(s.wifi_ssid), wifi_ssid);
    }
    if (wifi_password[0] != '\0') {
        copyString(s.wifi_pass, sizeof(s.wifi_pass), wifi_password);
    }
    if (last_probe.found) {
        s.rx_pin = last_probe.rx_pin;
        s.tx_pin = last_probe.tx_pin;
        s.baud = last_probe.baud;
    }
    return s;
}

void setLedColor(int pin, uint8_t red, uint8_t green, uint8_t blue) {
    if (test_led != nullptr && test_led_pin != pin) {
        led_strip_del(test_led);
        test_led = nullptr;
    }
    if (test_led == nullptr) {
        led_strip_config_t strip_config = {};
        strip_config.strip_gpio_num = pin;
        strip_config.max_leds = 1;
        strip_config.led_pixel_format = LED_PIXEL_FORMAT_GRB;
        strip_config.led_model = LED_MODEL_WS2812;
        led_strip_rmt_config_t rmt_config = {};
        rmt_config.resolution_hz = 10 * 1000 * 1000;
        if (led_strip_new_rmt_device(&strip_config, &rmt_config, &test_led) != ESP_OK) {
            return;
        }
        test_led_pin = pin;
    }
    led_strip_set_pixel(test_led, 0, red, green, blue);
    led_strip_refresh(test_led);
}

void ledTestTask(void* arg) {
    const int pin = reinterpret_cast<intptr_t>(arg);
    setLedColor(pin, 255, 0, 80);
    vTaskDelay(pdMS_TO_TICKS(350));
    setLedColor(pin, 0, 255, 255);
    vTaskDelay(pdMS_TO_TICKS(350));
    setLedColor(pin, 255, 180, 0);
    vTaskDelay(pdMS_TO_TICKS(350));
    setLedColor(pin, 0, 0, 0);
    vTaskDelete(nullptr);
}

void setInstallerLedMode(InstallerLedMode mode) {
    installer_led_mode = mode;
}

void installerLedTask(void*) {
    bool blink_on = false;
    while (true) {
        if (installer_led_mode == InstallerLedMode::Provisioned) {
            setLedColor(board_profile::kDefaultStatusLedPin, 0, 255, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        blink_on = !blink_on;
        setLedColor(board_profile::kDefaultStatusLedPin, 0, 0, blink_on ? 255 : 0);
        vTaskDelay(pdMS_TO_TICKS(450));
    }
}

void startInstallerStatusLed() {
    if (installer_led_task != nullptr) {
        return;
    }
    setInstallerLedMode(InstallerLedMode::Pairing);
    xTaskCreate(installerLedTask, "installer_led", 3072, nullptr, 3, &installer_led_task);
}

extern const uint8_t kIndexHtmlStart[] asm("_binary_installer_html_start");
extern const uint8_t kIndexHtmlEnd[] asm("_binary_installer_html_end");

esp_err_t indexHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req,
                           reinterpret_cast<const char*>(kIndexHtmlStart),
                           kIndexHtmlEnd - kIndexHtmlStart);
}

esp_err_t statusHandler(httpd_req_t* req) {
    InstallerSettings settings = defaultSettings();
    std::string body = "{\"ok\":true,\"wifi\":{\"connected\":";
    body += wifi_connected ? "true" : "false";
    body += ",\"ssid\":\"" + jsonEscape(wifi_ssid) + "\",\"ip\":\"" + jsonEscape(wifi_ip) + "\"";
    body += ",\"ap_ssid\":\"" + jsonEscape(provisioning_service_name) + "\",\"ap_ip\":\"" + jsonEscape(kApIp) + "\"}";
    body += ",\"provisioning\":{\"service_name\":\"";
    body += provisioning_service_name;
    body += "\",\"security\":\"Security 1\",\"pop\":\"";
    body += kProvisioningPop;
    body += "\"},\"version\":\"";
    body += jsonEscape(esp_app_get_description()->version);
    body += "\",\"board\":{\"id\":\"";
    body += board_profile::kBoardId;
    body += "\",\"name\":\"";
    body += board_profile::kBoardName;
    body += "\",\"target\":\"";
    body += board_profile::kIdfTarget;
    body += "\"";
    body += ",\"defaults\":{\"led_pin\":" + std::to_string(board_profile::kDefaultStatusLedPin);
    body += ",\"rx_pin\":" + std::to_string(board_profile::kDefaultCn105RxPin);
    body += ",\"tx_pin\":" + std::to_string(board_profile::kDefaultCn105TxPin);
    body += "}}";
    body += ",\"defaults\":";
    body += settingsJson(settings);
    body += ",\"probe\":{\"ran\":";
    body += last_probe.ran ? "true" : "false";
    body += ",\"found\":";
    body += last_probe.found ? "true" : "false";
    body += ",\"rx_pin\":" + std::to_string(last_probe.rx_pin);
    body += ",\"tx_pin\":" + std::to_string(last_probe.tx_pin);
    body += ",\"baud\":" + std::to_string(last_probe.baud);
    body += ",\"profile\":" + std::to_string(last_probe.profile);
    body += ",\"summary\":\"" + jsonEscape(last_probe.summary) + "\"}}";
    return sendText(req, "application/json", body.c_str());
}

esp_err_t probeHandler(httpd_req_t* req) {
    char body[256] = {};
    if (!readBody(req, body, sizeof(body))) {
        return sendJsonError(req, "failed to read body");
    }
    char value[32] = {};
    int rx_pin = board_profile::kDefaultCn105RxPin;
    int tx_pin = board_profile::kDefaultCn105TxPin;
    if (formValue(body, "rx_pin", value, sizeof(value))) rx_pin = std::atoi(value);
    if (formValue(body, "tx_pin", value, sizeof(value))) tx_pin = std::atoi(value);
    if (!validGpio(rx_pin) || !validOutputGpio(tx_pin)) {
        return sendJsonError(req, "invalid GPIO pin");
    }
    ProbeResult result = runProbe(rx_pin, tx_pin);
    char out[512] = {};
    std::snprintf(out,
                  sizeof(out),
                  "{\"ok\":true,\"result\":{\"found\":%s,\"rx_pin\":%d,\"tx_pin\":%d,\"baud\":%d,\"profile\":%u,\"profile_name\":\"%s\",\"connect\":\"0x%02X\",\"rx_bytes\":%lu,\"rx_packets\":%lu,\"rx_errors\":%lu,\"summary\":\"%s\"}}",
                  result.found ? "true" : "false",
                  result.rx_pin,
                  result.tx_pin,
                  result.baud,
                  static_cast<unsigned>(result.profile),
                  profileName(result.profile),
                  result.connect,
                  static_cast<unsigned long>(result.rx_bytes),
                  static_cast<unsigned long>(result.rx_packets),
                  static_cast<unsigned long>(result.rx_errors),
                  jsonEscape(result.summary).c_str());
    return sendText(req, "application/json", out);
}

esp_err_t writeSettingsHandler(httpd_req_t* req) {
    char body[2048] = {};
    if (!readBody(req, body, sizeof(body))) {
        return sendJsonError(req, "failed to read body");
    }
    InstallerSettings settings{};
    char error[128] = {};
    if (!parseSettingsFromBody(body, settings, error, sizeof(error))) {
        return sendJsonError(req, error);
    }
    writeSettingsToNvs(settings);
    std::string response = "{\"ok\":true,\"message\":\"device_cfg NVS saved\",\"settings\":";
    response += settingsJson(settings);
    response += "}";
    return sendText(req, "application/json", response.c_str());
}

esp_err_t ledTestHandler(httpd_req_t* req) {
    char body[128] = {};
    if (!readBody(req, body, sizeof(body))) {
        return sendJsonError(req, "failed to read body");
    }
    char value[32] = {};
    int pin = board_profile::kDefaultStatusLedPin;
    if (formValue(body, "led_pin", value, sizeof(value))) {
        pin = std::atoi(value);
    }
    if (!validOutputGpio(pin)) {
        return sendJsonError(req, "invalid LED GPIO pin");
    }
    xTaskCreate(ledTestTask, "led_test", 3072, reinterpret_cast<void*>(static_cast<intptr_t>(pin)), 3, nullptr);
    return sendText(req, "application/json", "{\"ok\":true,\"message\":\"LED test started\"}");
}


void registerRoutes(httpd_handle_t handle) {
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = nullptr},
        {.uri = "/api/status", .method = HTTP_GET, .handler = statusHandler, .user_ctx = nullptr},
        {.uri = "/api/probe", .method = HTTP_POST, .handler = probeHandler, .user_ctx = nullptr},
        {.uri = "/api/write-settings", .method = HTTP_POST, .handler = writeSettingsHandler, .user_ctx = nullptr},
        {.uri = "/api/led-test", .method = HTTP_POST, .handler = ledTestHandler, .user_ctx = nullptr},
    };
    for (const auto& route : routes) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &route));
    }

    static constexpr const char* kAcceptedVariants[] = {"app", "installer"};
    ESP_ERROR_CHECK(ota_handler::registerHandlers(
        handle,
        {kAcceptedVariants, sizeof(kAcceptedVariants) / sizeof(kAcceptedVariants[0])}));

    // Serve the installer shell for browser refreshes and client-side routes.
    // Register this last so the explicit /api/* routes win.
    const httpd_uri_t fallback = {.uri = "/*", .method = HTTP_GET, .handler = indexHandler, .user_ctx = nullptr};
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &fallback));
}

void startWebServer() {
    if (web_server != nullptr) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kHttpPrimaryPort;
    config.stack_size = 12288;
    config.max_uri_handlers = 16;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&web_server, &config));
    registerRoutes(web_server);
    ESP_LOGI(TAG,
             "Installer WebUI ready on port %u: AP=http://%s:%u/ STA=http://%s:%u/",
             static_cast<unsigned>(config.server_port),
             kApIp,
             static_cast<unsigned>(config.server_port),
             wifi_ip,
             static_cast<unsigned>(config.server_port));
}

void configureSoftAp() {
    wifi_config_t ap_config = {};
    const size_t ssid_len = std::min(std::strlen(provisioning_service_name), sizeof(ap_config.ap.ssid));
    memcpy(ap_config.ap.ssid, provisioning_service_name, ssid_len);
    ap_config.ap.ssid_len = ssid_len;
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
}

void eventHandler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_PROV_EVENT) {
        if (event_id == WIFI_PROV_CRED_RECV) {
            auto* cfg = static_cast<wifi_sta_config_t*>(event_data);
            copyString(wifi_ssid, sizeof(wifi_ssid), reinterpret_cast<const char*>(cfg->ssid));
            copyString(wifi_password, sizeof(wifi_password), reinterpret_cast<const char*>(cfg->password));
            setInstallerLedMode(InstallerLedMode::Pairing);
            ESP_LOGI(TAG, "Provisioning received SSID=%s", wifi_ssid);
        } else if (event_id == WIFI_PROV_CRED_SUCCESS) {
            setInstallerLedMode(InstallerLedMode::Provisioned);
            ESP_LOGI(TAG, "Provisioning successful");
        } else if (event_id == WIFI_PROV_CRED_FAIL) {
            setInstallerLedMode(InstallerLedMode::Pairing);
            ESP_LOGW(TAG, "Provisioning failed");
        } else if (event_id == WIFI_PROV_END) {
            ESP_LOGI(TAG, "Provisioning end event ignored; BLE provisioning remains available");
        }
        return;
    }
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_AP_START) {
            ESP_LOGI(TAG, "Installer SoftAP started: ssid=%s ip=%s", provisioning_service_name, kApIp);
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            auto* event = static_cast<wifi_event_ap_staconnected_t*>(event_data);
            ESP_LOGI(TAG, "Installer SoftAP client joined: " MACSTR, MAC2STR(event->mac));
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_connected = false;
            copyString(wifi_ip, sizeof(wifi_ip), "0.0.0.0");
            ESP_LOGW(TAG, "WiFi disconnected; reconnecting");
            esp_wifi_connect();
        }
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        std::snprintf(wifi_ip, sizeof(wifi_ip), IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
        wifi_config_t cfg = {};
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            copyString(wifi_ssid, sizeof(wifi_ssid), reinterpret_cast<const char*>(cfg.sta.ssid));
            copyString(wifi_password, sizeof(wifi_password), reinterpret_cast<const char*>(cfg.sta.password));
        }
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        setInstallerLedMode(InstallerLedMode::Provisioned);
        ESP_LOGI(TAG, "WiFi connected: ssid=%s ip=%s", wifi_ssid, wifi_ip);
        startWebServer();
    }
}

void startWifiProvisioning() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, eventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, eventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, eventHandler, nullptr));

    esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    buildProvisioningServiceName(provisioning_service_name, sizeof(provisioning_service_name));

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    configureSoftAp();

    wifi_prov_mgr_config_t prov_config = {};
    wifi_prov_scheme_t ble_apsta_scheme = wifi_prov_scheme_ble;
    ble_apsta_scheme.wifi_mode = WIFI_MODE_APSTA;
    prov_config.scheme = ble_apsta_scheme;
    prov_config.scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;
    prov_config.app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;
    ESP_ERROR_CHECK(wifi_prov_mgr_init(prov_config));
    ESP_ERROR_CHECK(wifi_prov_mgr_disable_auto_stop(1000));

    // Installer firmware should always be discoverable from Espressif's BLE provisioning app,
    // even if this board has stale Wi-Fi provisioning data from an older test.
    wifi_prov_mgr_reset_provisioning();

    const wifi_prov_security1_params_t* security_params = kProvisioningPop;
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, security_params, provisioning_service_name, nullptr));
    configureSoftAp();
    startWebServer();
    ESP_LOGI(TAG,
             "BLE provisioning started. Use Espressif app, service name: %s security=1 pop=%s",
             provisioning_service_name,
             kProvisioningPop);
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Kiri Installer starting");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    startInstallerStatusLed();
    startWifiProvisioning();
    ESP_LOGI(TAG, "Installer WebUI is available on SoftAP %s at http://%s:%u/",
             provisioning_service_name,
             kApIp,
             static_cast<unsigned>(kHttpPrimaryPort));
}
