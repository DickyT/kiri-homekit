// Host-only fakes for deterministic tests of the real qingping.cpp worker.
#pragma once
#include "cn105_core.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_NO_MEM=1, ESP_ERR_NVS_NOT_FOUND=2;
namespace test_platform {
inline uint64_t timeMs=0;
inline bool queueAvailable=true, failCommit=false, stored=false;
inline uint8_t storedAddress[6]={};
inline bool staged=false, stagedPresent=false;
inline uint8_t stagedAddress[6]={};
inline unsigned starts=0, stops=0, scans=0;
inline bool scanning=false;
struct Command { bool enabled; int half; uint32_t id; };
inline std::vector<Command> commands;
inline uint32_t ack=0, sequence=0;
}
inline int64_t esp_timer_get_time(){return test_platform::timeMs*1000;}
using nvs_handle_t=int;
constexpr int NVS_READONLY=0, NVS_READWRITE=1;
inline int nvs_open(const char*,int,nvs_handle_t* h){*h=1;return 0;}
inline int nvs_get_blob(int,const char*,void* data,size_t* n){if(!test_platform::stored)return 2;std::memcpy(data,test_platform::storedAddress,6);*n=6;return 0;}
inline int nvs_set_blob(int,const char*,const void* data,size_t n){if(n!=6)return 1;std::memcpy(test_platform::stagedAddress,data,6);test_platform::staged=true;test_platform::stagedPresent=true;return 0;}
inline int nvs_erase_key(int,const char*){test_platform::staged=true;test_platform::stagedPresent=false;return 0;}
inline int nvs_commit(int){
    if(test_platform::failCommit)return 1;
    if(test_platform::staged){test_platform::stored=test_platform::stagedPresent;std::memcpy(test_platform::storedAddress,test_platform::stagedAddress,6);}
    return 0;
}
inline void nvs_close(int){test_platform::staged=false;}
constexpr int pdPASS=1;
#define pdMS_TO_TICKS(ms) (ms)
inline int xTaskCreate(void(*)(void*),const char*,int,void*,int,void*){return pdPASS;}
inline void vTaskDelay(int){}
struct ble_gap_disc_params {int passive=0,itvl=0,window=0,filter_duplicates=0;};
struct ble_gap_event {int type=0; struct {const uint8_t* data=nullptr;size_t length_data=0;int rssi=0;} disc;};
constexpr int BLE_GAP_EVENT_DISC=1;
struct TestHostCfg {void(*sync_cb)()=nullptr;void(*reset_cb)(int)=nullptr;void* gatts_register_cb=nullptr;void* store_status_cb=nullptr;};
inline TestHostCfg ble_hs_cfg;
inline int nimble_port_init(){test_platform::starts++;return 0;}
inline int nimble_port_stop(){test_platform::stops++;return 0;}
inline int nimble_port_deinit(){return 0;}
inline void nimble_port_run(){}
inline void nimble_port_freertos_init(void(*)(void*)){if(ble_hs_cfg.sync_cb)ble_hs_cfg.sync_cb();}
inline void nimble_port_freertos_deinit(){}
inline int ble_hs_util_ensure_addr(int){return 0;}
inline int ble_hs_id_infer_auto(int,uint8_t* type){*type=0;return 0;}
inline int ble_gap_disc_active(){return test_platform::scanning;}
inline int ble_gap_disc_cancel(){test_platform::scanning=false;return 0;}
inline int ble_gap_disc(uint8_t,int,ble_gap_disc_params*,int(*)(ble_gap_event*,void*),void*){test_platform::scans++;test_platform::scanning=true;return 0;}
namespace device_settings {inline bool useRealCn105(){return true;}}
namespace cn105_transport {
struct Status {uint32_t remoteTemperatureAckId=0;};
inline Status getStatus(){return {test_platform::ack};}
inline bool queueRemoteTemperature(bool enabled,cn105_core::HalfDegreesC half,uint32_t* id){
    if(!test_platform::queueAvailable)return false;
    *id=++test_platform::sequence;
    test_platform::commands.push_back({enabled,half,*id});
    return true;
}
}
