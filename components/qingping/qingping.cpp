// Kiri Bridge — https://kiri.dkt.moe — GPL-3.0
#include "qingping.h"
#include "qingping_binding.h"
#include "cn105_transport.h"
#include "device_settings.h"
#include "platform_lock.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern "C" {
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
}
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {
platform_lock::RecursiveMutex dataLock, lifecycleLock;
qingping::Binding binding;
struct Device { bool used = false; qingping::Reading reading{}; uint64_t seen = 0; int rssi = 0; uint8_t model = 0; };
Device devices[16];
bool initialized = false, hostStarted = false;
std::atomic<bool> paused{false}, synced{false};
std::atomic<int> bluetoothError{0};
uint64_t scanUntil = 0;
// Recover from reboot between unbind persistence and delivery of the restore command.
bool restorePending = true;
uint64_t lastSend = 0;
bool sentEnabled = false;
uint32_t pendingRequestId = 0;
bool awaitingAck = false;
char message[96] = "Not bound";
uint64_t nowMs() { return esp_timer_get_time() / 1000; }
void addressText(const uint8_t* a, char* out) {
    std::snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",a[0],a[1],a[2],a[3],a[4],a[5]);
}
int gapEvent(ble_gap_event* event, void*) {
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    const uint8_t* bytes = event->disc.data;
    size_t len = event->disc.length_data;
    for (size_t p = 0; p < len;) {
        size_t n = bytes[p];
        if (!n || n > len - p - 1) break;
        if (n >= 4 && bytes[p+1] == 0x16 && bytes[p+2] == 0xcd && bytes[p+3] == 0xfd) {
            qingping::Reading reading{};
            if (qingping::parseFdcd(bytes+p+4, n-3, &reading)) {
                platform_lock::ScopedLock lock(dataLock);
                const uint64_t now = nowMs();
                binding.receive(reading, now);
                Device* slot = nullptr;
                for (auto& d : devices) if (d.used && !std::memcmp(d.reading.address, reading.address, 6)) { slot = &d; break; }
                if (!slot) for (auto& d : devices) if (!d.used) { slot = &d; break; }
                if (!slot) {
                    // Never evict the bound device when the discovery cache is full.
                    for (auto& d : devices) {
                        if (binding.bound() && !std::memcmp(d.reading.address, binding.address(), 6)) continue;
                        if (!slot || d.seen < slot->seen) slot = &d;
                    }
                }
                if (slot) { slot->used = true; slot->reading = reading; slot->seen = now; slot->rssi = event->disc.rssi; slot->model = bytes[p+5]; }
            }
        }
        p += n + 1;
    }
    return 0;
}
void hostTask(void*) { nimble_port_run(); nimble_port_freertos_deinit(); }
void onReset(int reason) { synced.store(false); bluetoothError.store(reason); }
void onSync() {
    const int rc = ble_hs_util_ensure_addr(0);
    bluetoothError.store(rc);
    synced.store(rc == 0);
}
bool startHost() {
    if (hostStarted) return true;
    const esp_err_t rc = nimble_port_init();
    if (rc != ESP_OK) { bluetoothError.store(rc); return false; }
    // Do not retain provisioning callbacks that refer to its released GATT state.
    ble_hs_cfg.reset_cb = onReset;
    ble_hs_cfg.gatts_register_cb = nullptr;
    ble_hs_cfg.store_status_cb = nullptr;
    ble_hs_cfg.sync_cb = onSync;
    synced.store(false);
    hostStarted = true;
    nimble_port_freertos_init(hostTask);
    return true;
}
bool stopHost() {
    if (!hostStarted) return true;
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    if (nimble_port_stop() != 0) return false;
    if (nimble_port_deinit() != ESP_OK) return false;
    hostStarted = false;
    synced.store(false);
    return true;
}
bool persist(const uint8_t* address) {
    nvs_handle_t h;
    if (nvs_open("qingping", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = address ? nvs_set_blob(h, "address", address, 6) : nvs_erase_key(h,"address");
    if (e == ESP_ERR_NVS_NOT_FOUND && !address) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}
void controlStep() {
        {
            platform_lock::ScopedLock lock(dataLock);
            const uint64_t now = nowMs();
            const bool use = binding.usableForControl(now) && !paused.load();
            if (awaitingAck && cn105_transport::getStatus().remoteTemperatureAckId == pendingRequestId) {
                awaitingAck = false;
                if (!sentEnabled) restorePending = false;
                std::snprintf(message, sizeof(message), "%s", sentEnabled ? "External temperature command acknowledged" : "Internal temperature restore acknowledged");
            }
            // Always restore once after a persisted binding boots, before receiving a fresh reading.
            // Repeat even unchanged readings, but never renew their sensor freshness timestamp.
            if (device_settings::useRealCn105() && (!awaitingAck || now-lastSend >= 5000) &&
                ((use && (!sentEnabled || now-lastSend >= 20000)) ||
                 (!use && (sentEnabled || restorePending) && now-lastSend >= 1000))) {
                const auto half = static_cast<cn105_core::HalfDegreesC>((binding.reading().temperatureTenthsC + 2) / 5);
                uint32_t requestId = 0;
                if (cn105_transport::queueRemoteTemperature(use, half, &requestId)) {
                    sentEnabled = use; lastSend = now; pendingRequestId = requestId; awaitingAck = true;
                    restorePending = !use; // Retain until its ACK arrives; retry restore on loss.
                    std::snprintf(message, sizeof(message), "%s",use ? "Sending external temperature" : "Restoring internal temperature");
                }
            }
        }
}
void radioStep() {
        bool wanted;
        {
            platform_lock::ScopedLock lock(dataLock);
            wanted = binding.bound() || nowMs() < scanUntil;
        }
        {
            platform_lock::ScopedLock lock(lifecycleLock);
            if (!paused.load() && wanted) {
                if (startHost() && synced.load() && !ble_gap_disc_active()) {
                    uint8_t type;
                    if (ble_hs_id_infer_auto(0, &type) == 0) {
                        ble_gap_disc_params params{};
                        params.passive = 1; params.itvl = 160; params.window = 80; params.filter_duplicates = 0;
                        bluetoothError.store(ble_gap_disc(type, 10000, &params, gapEvent, nullptr));
                    }
                }
            } else if (!paused.load()) stopHost();
        }
}
void worker(void*) {
    for (;;) {
        controlStep();
        radioStep();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
}
namespace qingping {
esp_err_t init() {
    if (initialized) return ESP_OK;
    if (!dataLock.init() || !lifecycleLock.init()) return ESP_ERR_NO_MEM;
    nvs_handle_t h;
    if (nvs_open("qingping", NVS_READONLY, &h) == ESP_OK) {
        uint8_t a[6]; size_t n = sizeof(a);
        if (nvs_get_blob(h,"address",a,&n) == ESP_OK && n == 6) { binding.bind(a); restorePending = true; }
        nvs_close(h);
    }
    if (xTaskCreate(worker,"qingping",4096,nullptr,4,nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    initialized = true;
    return ESP_OK;
}
bool refresh() {
    if (!initialized || paused.load()) return false;
    platform_lock::ScopedLock lock(dataLock);
    scanUntil = nowMs() + 30000;
    return true;
}
bool bindAddress(const char* text) {
    if (!initialized || !text || std::strlen(text) != 17) return false;
    uint8_t a[6];
    for (size_t i=0;i<6;++i) {
        auto hex = [](char c)->int { if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; };
        int hi=hex(text[i*3]), lo=hex(text[i*3+1]);
        if(hi<0||lo<0||(i<5&&text[i*3+2]!=':')) return false;
        a[i]=static_cast<uint8_t>((hi<<4)|lo);
    }
    platform_lock::ScopedLock lock(dataLock);
    bool found=false;
    for(const auto& d:devices) if(d.used&&!std::memcmp(a,d.reading.address,6)&&nowMs()-d.seen<30000)found=true;
    if(!found || !persist(a))return false;
    binding.bind(a); // Wait for a post-bind advertisement, rather than reuse discovery data.
    restorePending=true; lastSend=0;
    return true;
}
bool unbind() {
    if (!initialized) return false;
    platform_lock::ScopedLock lock(dataLock);
    if(!persist(nullptr))return false;
    binding.unbind(); restorePending=true; lastSend=0;
    return true;
}
bool pauseForProvisioning() {
    paused.store(true);
    if (!initialized) return true;
    platform_lock::ScopedLock lock(lifecycleLock);
    return stopHost();
}
void resumeAfterProvisioning() { paused.store(false); }
bool activeTemperatureC(float* temperature) {
    if (!initialized || !temperature || paused.load()) return false;
    platform_lock::ScopedLock lock(dataLock);
    if (!binding.usableForControl(nowMs())) return false;
    *temperature = binding.reading().temperatureTenthsC / 10.0f;
    return true;
}
std::string statusJson(bool includeDevices) {
    if(!initialized)return "{\"ok\":false,\"error\":\"Sensor service not initialized\"}";
    platform_lock::ScopedLock lock(dataLock);
    const uint64_t now=nowMs();
    char address[18]="", buf[768];
    if(binding.bound()) addressText(binding.address(),address);
    std::snprintf(buf,sizeof(buf),"{\"ok\":true,\"bound\":%s,\"address\":\"%s\",\"online\":%s,\"received\":%s,\"age_ms\":%llu,\"temperature_c\":%.1f,\"humidity\":%.1f,\"battery\":%d,\"timeout_ms\":300000,\"scanning\":%s,\"paused\":%s,\"control_usable\":%s,\"command_pending\":%s,\"message\":\"%s\",\"devices\":[",
        binding.bound()?"true":"false",address,binding.online(now)?"true":"false",binding.received()?"true":"false",static_cast<unsigned long long>(binding.ageMs(now)),binding.reading().temperatureTenthsC/10.0,binding.reading().humidityTenthsPercent/10.0,binding.reading().batteryPercent,
        now<scanUntil?"true":"false",paused.load()?"true":"false",binding.usableForControl(now)&&!paused.load()?"true":"false",awaitingAck?"true":"false",message);
    std::string json=buf; bool first=true;
    for(const auto& d:devices)if(includeDevices && d.used && now-d.seen<60000){
        addressText(d.reading.address,address);
        std::snprintf(buf,sizeof(buf),"%s{\"address\":\"%s\",\"service_uuid\":\"0000fdcd-0000-1000-8000-00805f9b34fb\",\"model\":%u,\"temperature_c\":%.1f,\"humidity\":%.1f,\"battery\":%d,\"rssi\":%d,\"age_ms\":%llu}",first?"":",",address,d.model,d.reading.temperatureTenthsC/10.0,d.reading.humidityTenthsPercent/10.0,d.reading.batteryPercent,d.rssi,static_cast<unsigned long long>(now-d.seen));
        json+=buf; first=false;
    }
    int rssi = 0;
    if (binding.bound()) for (const auto& d : devices)
        if (d.used && !std::memcmp(d.reading.address, binding.address(), 6)) rssi = d.rssi;
    std::snprintf(buf,sizeof(buf),"],\"rssi\":%d,\"ble_error\":%d}",rssi,bluetoothError.load());
    return json+buf;
}
}
