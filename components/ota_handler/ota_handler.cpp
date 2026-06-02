// Kiri Bridge — shared OTA HTTP handlers.
//
// Mirrors the original handlers in components/web/web_routes.cpp; that file
// now wraps registerHandlers() so the production firmware and installer
// share one implementation. Only acceptedVariants differs between callers.

#include "ota_handler.h"

#include "board_profile.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ota_handler {

namespace {

constexpr const char* TAG = "ota_handler";

const esp_partition_t* g_pending_partition = nullptr;
AcceptedVariants g_accepted_variants{nullptr, 0};

esp_err_t sendText(httpd_req_t* req, const char* content_type, const char* body) {
    httpd_resp_set_type(req, content_type);
    return httpd_resp_sendstr(req, body);
}

void jsonEscape(const char* src, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) return;
    size_t j = 0;
    for (size_t i = 0; src != nullptr && src[i] != '\0' && j + 1 < out_len; ++i) {
        const char c = src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= out_len) break;
            out[j++] = '\\';
            out[j++] = c;
        } else if (c == '\n') {
            if (j + 2 >= out_len) break;
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= out_len) break;
            out[j++] = '\\';
            out[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 >= out_len) break;
            out[j++] = '\\';
            out[j++] = 't';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

esp_err_t sendJsonError(httpd_req_t* req, const char* error) {
    char escaped[192] = {};
    jsonEscape(error == nullptr ? "unknown error" : error, escaped, sizeof(escaped));
    char body[256] = {};
    std::snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", escaped);
    httpd_resp_set_status(req, "400 Bad Request");
    return sendText(req, "application/json", body);
}

void writePartitionJson(const esp_partition_t* partition, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) return;
    if (partition == nullptr) {
        std::snprintf(out, out_size, "null");
        return;
    }
    char esc_label[32] = {};
    jsonEscape(partition->label, esc_label, sizeof(esc_label));
    std::snprintf(out,
                  out_size,
                  "{\"label\":\"%s\",\"address\":%u,\"size\":%u}",
                  esc_label,
                  static_cast<unsigned>(partition->address),
                  static_cast<unsigned>(partition->size));
}

int monthNumber(const char* month) {
    if (month == nullptr) return 0;
    static const char* kNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; ++i) {
        if (std::strcmp(month, kNames[i]) == 0) return i + 1;
    }
    return 0;
}

uint64_t versionStamp(const char* version) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(version == nullptr ? "" : version,
                    "%4d.%2d%2d.%2d%2d%2d",
                    &year, &month, &day, &hour, &minute, &second) != 6) {
        return 0;
    }
    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return 0;
    }
    return static_cast<uint64_t>(year) * 10000000000ULL +
           static_cast<uint64_t>(month) * 100000000ULL +
           static_cast<uint64_t>(day) * 1000000ULL +
           static_cast<uint64_t>(hour) * 10000ULL +
           static_cast<uint64_t>(minute) * 100ULL +
           static_cast<uint64_t>(second);
}

uint64_t compileStamp(const char* date, const char* time) {
    char month[4] = {};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(date == nullptr ? "" : date, "%3s %d %d", month, &day, &year) != 3 ||
        std::sscanf(time == nullptr ? "" : time, "%d:%d:%d", &hour, &minute, &second) != 3) {
        return 0;
    }
    const int month_num = monthNumber(month);
    if (month_num <= 0) return 0;
    return static_cast<uint64_t>(year) * 10000000000ULL +
           static_cast<uint64_t>(month_num) * 100000000ULL +
           static_cast<uint64_t>(day) * 1000000ULL +
           static_cast<uint64_t>(hour) * 10000ULL +
           static_cast<uint64_t>(minute) * 100ULL +
           static_cast<uint64_t>(second);
}

uint64_t appDescStamp(const esp_app_desc_t& desc) {
    const uint64_t v = versionStamp(desc.version);
    return v > 0 ? v : compileStamp(desc.date, desc.time);
}

void versionFromAppDesc(const esp_app_desc_t& desc, char* out, size_t out_len) {
    if (desc.version[0] != '\0') {
        std::snprintf(out, out_len, "%s", desc.version);
        return;
    }
    char month[4] = {};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(desc.date, "%3s %d %d", month, &day, &year) == 3 &&
        std::sscanf(desc.time, "%d:%d:%d", &hour, &minute, &second) == 3) {
        const int month_num = monthNumber(month);
        if (month_num > 0) {
            std::snprintf(out, out_len, "%04d.%02d%02d.%02d%02d%02d",
                          year, month_num, day, hour, minute, second);
            return;
        }
    }
    std::snprintf(out, out_len, "%s", desc.version);
}

bool variantAccepted(const char* variant) {
    if (variant == nullptr || variant[0] == '\0') return false;
    for (size_t i = 0; i < g_accepted_variants.count; ++i) {
        const char* allowed = g_accepted_variants.values[i];
        if (allowed != nullptr && std::strcmp(variant, allowed) == 0) return true;
    }
    return false;
}

bool readShaHeader(httpd_req_t* req, char* out_lower_hex_65, bool* present) {
    *present = false;
    if (httpd_req_get_hdr_value_str(req, "X-Kiri-Sha256", out_lower_hex_65, 65) != ESP_OK) {
        return true;  // header simply not provided
    }
    *present = true;
    for (size_t i = 0; out_lower_hex_65[i] != '\0'; ++i) {
        const char c = out_lower_hex_65[i];
        const bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!is_hex) return false;
        if (c >= 'A' && c <= 'F') out_lower_hex_65[i] = static_cast<char>(c - 'A' + 'a');
    }
    return std::strlen(out_lower_hex_65) == 64;
}

bool readTargetHeader(httpd_req_t* req, char* out_target, size_t out_len, bool* present) {
    if (out_target == nullptr || out_len == 0 || present == nullptr) return false;
    out_target[0] = '\0';
    *present = false;
    if (httpd_req_get_hdr_value_str(req, "X-Kiri-Target", out_target, out_len) != ESP_OK) {
        return true;
    }
    *present = true;
    return out_target[0] != '\0';
}

esp_err_t otaInfoHandler(httpd_req_t* req) {
    const esp_partition_t* running_partition = esp_ota_get_running_partition();
    const esp_partition_t* boot_partition = esp_ota_get_boot_partition();
    const esp_partition_t* next_partition = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t* ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                           ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                                           nullptr);
    const esp_partition_t* ota1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                           ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                                           nullptr);
    const esp_app_desc_t* app_desc = esp_app_get_description();

    char running_json[96] = {};
    char boot_json[96] = {};
    char next_json[96] = {};
    char ota0_json[96] = {};
    char ota1_json[96] = {};
    writePartitionJson(running_partition, running_json, sizeof(running_json));
    writePartitionJson(boot_partition, boot_json, sizeof(boot_json));
    writePartitionJson(next_partition, next_json, sizeof(next_json));
    writePartitionJson(ota0, ota0_json, sizeof(ota0_json));
    writePartitionJson(ota1, ota1_json, sizeof(ota1_json));

    char project_name[64] = {};
    char version[64] = {};
    jsonEscape(app_desc == nullptr ? "" : app_desc->project_name, project_name, sizeof(project_name));
    jsonEscape(app_desc == nullptr ? "" : app_desc->version, version, sizeof(version));

    // accepted_variants as JSON array
    char variants_json[128] = "[";
    size_t vp = 1;
    for (size_t i = 0; i < g_accepted_variants.count; ++i) {
        const char* v = g_accepted_variants.values[i];
        if (v == nullptr) continue;
        if (i > 0 && vp + 1 < sizeof(variants_json)) variants_json[vp++] = ',';
        char esc[32] = {};
        jsonEscape(v, esc, sizeof(esc));
        const int n = std::snprintf(variants_json + vp, sizeof(variants_json) - vp, "\"%s\"", esc);
        if (n < 0) break;
        vp += static_cast<size_t>(n);
        if (vp + 1 >= sizeof(variants_json)) break;
    }
    if (vp + 1 < sizeof(variants_json)) variants_json[vp++] = ']';
    variants_json[vp] = '\0';

    char body[1152] = {};
    std::snprintf(body,
                  sizeof(body),
                  "{\"ok\":true,"
                  "\"project_name\":\"%s\","
                  "\"version\":\"%s\","
                  "\"target\":\"%s\","
                  "\"board\":{\"id\":\"%s\",\"name\":\"%s\"},"
                  "\"accepted_variants\":%s,"
                  "\"running_partition\":%s,"
                  "\"boot_partition\":%s,"
                  "\"next_partition\":%s,"
                  "\"partitions\":{\"ota_0\":%s,\"ota_1\":%s}}",
                  project_name,
                  version,
                  board_profile::kIdfTarget,
                  board_profile::kBoardId,
                  board_profile::kBoardName,
                  variants_json,
                  running_json,
                  boot_json,
                  next_json,
                  ota0_json,
                  ota1_json);
    return sendText(req, "application/json", body);
}

esp_err_t otaUploadHandler(httpd_req_t* req) {
    if (req->content_len <= 0) {
        return sendJsonError(req, "empty OTA upload");
    }

    char expected_sha_hex[65] = {};
    bool have_expected_sha = false;
    if (!readShaHeader(req, expected_sha_hex, &have_expected_sha)) {
        return sendJsonError(req, "X-Kiri-Sha256 header must be 64 lowercase hex chars");
    }

    char uploaded_target[24] = {};
    bool have_uploaded_target = false;
    if (!readTargetHeader(req, uploaded_target, sizeof(uploaded_target), &have_uploaded_target)) {
        return sendJsonError(req, "X-Kiri-Target header is invalid");
    }
    if (have_uploaded_target && std::strcmp(uploaded_target, board_profile::kIdfTarget) != 0) {
        char message[160] = {};
        std::snprintf(message,
                      sizeof(message),
                      "firmware target '%s' does not match this device target '%s'",
                      uploaded_target,
                      board_profile::kIdfTarget);
        return sendJsonError(req, message);
    }

    const esp_partition_t* running_partition = esp_ota_get_running_partition();
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(nullptr);
    if (update_partition == nullptr) {
        return sendJsonError(req, "no OTA update partition available");
    }
    if (running_partition != nullptr && update_partition->address == running_partition->address) {
        return sendJsonError(req, "OTA update partition matches running partition");
    }
    if (static_cast<size_t>(req->content_len) > update_partition->size) {
        return sendJsonError(req, "uploaded app is larger than OTA partition");
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        char message[128] = {};
        std::snprintf(message, sizeof(message), "esp_ota_begin failed: %s", esp_err_to_name(err));
        return sendJsonError(req, message);
    }

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    uint8_t buffer[1024] = {};
    int remaining = req->content_len;
    size_t written_total = 0;
    while (remaining > 0) {
        const int recv = httpd_req_recv(req, reinterpret_cast<char*>(buffer),
                                        std::min<int>(remaining, sizeof(buffer)));
        if (recv <= 0) {
            esp_ota_abort(ota_handle);
            mbedtls_sha256_free(&sha_ctx);
            return sendJsonError(req, "OTA upload receive failed");
        }
        mbedtls_sha256_update(&sha_ctx, buffer, static_cast<size_t>(recv));
        err = esp_ota_write(ota_handle, buffer, static_cast<size_t>(recv));
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            mbedtls_sha256_free(&sha_ctx);
            char message[128] = {};
            std::snprintf(message, sizeof(message), "esp_ota_write failed: %s", esp_err_to_name(err));
            return sendJsonError(req, message);
        }
        written_total += static_cast<size_t>(recv);
        remaining -= recv;
    }

    uint8_t computed_sha[32] = {};
    mbedtls_sha256_finish(&sha_ctx, computed_sha);
    mbedtls_sha256_free(&sha_ctx);

    if (have_expected_sha) {
        char computed_hex[65] = {};
        for (size_t i = 0; i < sizeof(computed_sha); ++i) {
            std::snprintf(computed_hex + i * 2, 3, "%02x", computed_sha[i]);
        }
        if (std::strcmp(computed_hex, expected_sha_hex) != 0) {
            esp_ota_abort(ota_handle);
            char message[192] = {};
            std::snprintf(message,
                          sizeof(message),
                          "SHA-256 mismatch: computed=%s expected=%s",
                          computed_hex,
                          expected_sha_hex);
            return sendJsonError(req, message);
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        char message[128] = {};
        std::snprintf(message, sizeof(message), "esp_ota_end failed: %s", esp_err_to_name(err));
        return sendJsonError(req, message);
    }

    esp_app_desc_t uploaded_desc = {};
    err = esp_ota_get_partition_description(update_partition, &uploaded_desc);
    if (err != ESP_OK) {
        char message[128] = {};
        std::snprintf(message, sizeof(message), "app description read failed: %s", esp_err_to_name(err));
        return sendJsonError(req, message);
    }

    // Variant gate. The .kiri client side reads manifest.variant out of band,
    // but the running firmware has no access to it; we treat the embedded
    // app_desc->project_name as the variant marker. Project names match the
    // CMake project() label: kiri_bridge for app, kiri_installer for installer.
    const char* uploaded_project = uploaded_desc.project_name;
    const char* uploaded_variant = nullptr;
    if (std::strcmp(uploaded_project, "kiri_bridge") == 0) uploaded_variant = "app";
    else if (std::strcmp(uploaded_project, "kiri_installer") == 0) uploaded_variant = "installer";

    if (uploaded_variant == nullptr || !variantAccepted(uploaded_variant)) {
        char message[224] = {};
        std::snprintf(message,
                      sizeof(message),
                      "uploaded firmware variant '%s' (project_name=%s) is not accepted here",
                      uploaded_variant != nullptr ? uploaded_variant : "unknown",
                      uploaded_project);
        return sendJsonError(req, message);
    }

    char uploaded_version[32] = {};
    char current_version[32] = {};
    versionFromAppDesc(uploaded_desc, uploaded_version, sizeof(uploaded_version));
    const esp_app_desc_t* current_desc = esp_app_get_description();
    if (current_desc != nullptr) {
        versionFromAppDesc(*current_desc, current_version, sizeof(current_version));
    } else {
        std::snprintf(current_version, sizeof(current_version), "unknown");
    }

    const uint64_t uploaded_stamp = appDescStamp(uploaded_desc);
    uint64_t current_stamp = versionStamp(current_version);
    if (current_stamp == 0 && current_desc != nullptr) {
        current_stamp = appDescStamp(*current_desc);
    }
    const bool rollback = uploaded_stamp > 0 && current_stamp > 0 && uploaded_stamp < current_stamp;
    const bool same_or_older = uploaded_stamp > 0 && current_stamp > 0 && uploaded_stamp <= current_stamp;
    g_pending_partition = update_partition;

    char body[768] = {};
    std::snprintf(body,
                  sizeof(body),
                  "{\"ok\":true,"
                  "\"message\":\"OTA image written. Reboot to boot the uploaded firmware.\","
                  "\"bytes\":%u,"
                  "\"partition\":\"%s\","
                  "\"variant\":\"%s\","
                  "\"current_version\":\"%s\","
                  "\"uploaded_version\":\"%s\","
                  "\"rollback\":%s,"
                  "\"same_or_older\":%s,"
                  "\"warning\":\"%s\"}",
                  static_cast<unsigned>(written_total),
                  update_partition->label,
                  uploaded_variant,
                  current_version,
                  uploaded_version,
                  rollback ? "true" : "false",
                  same_or_older ? "true" : "false",
                  same_or_older ? "Uploaded firmware is not newer than the running firmware. Reboot is still allowed." : "");
    return sendText(req, "application/json", body);
}

esp_err_t otaApplyHandler(httpd_req_t* req) {
    if (g_pending_partition == nullptr) {
        return sendJsonError(req, "no pending OTA upload");
    }
    const esp_partition_t* partition = g_pending_partition;
    g_pending_partition = nullptr;

    const esp_err_t err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        char message[128] = {};
        std::snprintf(message, sizeof(message), "set boot partition failed: %s", esp_err_to_name(err));
        return sendJsonError(req, message);
    }

    sendText(req, "application/json", "{\"ok\":true,\"message\":\"rebooting into uploaded OTA firmware\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

}  // namespace

esp_err_t registerHandlers(httpd_handle_t server, AcceptedVariants accepted_variants) {
    if (server == nullptr) return ESP_ERR_INVALID_ARG;
    g_accepted_variants = accepted_variants;

    const httpd_uri_t info = {
        .uri = "/api/ota/info",
        .method = HTTP_GET,
        .handler = otaInfoHandler,
        .user_ctx = nullptr,
    };
    const httpd_uri_t upload = {
        .uri = "/api/ota/upload",
        .method = HTTP_POST,
        .handler = otaUploadHandler,
        .user_ctx = nullptr,
    };
    const httpd_uri_t apply = {
        .uri = "/api/ota/apply",
        .method = HTTP_POST,
        .handler = otaApplyHandler,
        .user_ctx = nullptr,
    };

    esp_err_t err = httpd_register_uri_handler(server, &info);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &upload);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &apply);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA route registration failed: %s", esp_err_to_name(err));
    }
    return err;
}

}  // namespace ota_handler
