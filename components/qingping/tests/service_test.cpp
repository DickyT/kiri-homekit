#include "test_platform.h"
#include <cassert>
#include <cmath>
#include <cstdio>
// Exercise the production worker steps, not a duplicated model of them.
#include "../qingping.cpp"
int main() {
    using namespace test_platform;
    assert(qingping::init()==ESP_OK);
    timeMs=1100; controlStep();
    assert(commands.size()==1&&!commands.back().enabled);
    ack=commands.back().id; controlStep();
    assert(!awaitingAck&&!restorePending);
    radioStep(); assert(starts==0); // Unbound and no refresh: no radio activity.
    assert(qingping::refresh()); radioStep();
    assert(starts==1&&scans==1);
    const uint8_t frame[]={20,0x16,0xcd,0xfd,0x08,0x10,0x3f,0x17,0x80,0x34,0x2d,0x58,0x01,0x04,0xe3,0x00,0x90,0x01,0x02,0x01,0x55};
    ble_gap_event event{};event.type=BLE_GAP_EVENT_DISC;event.disc={frame,sizeof(frame),-67};
    gapEvent(&event,nullptr);
    float temperature=0;
    assert(!qingping::activeTemperatureC(&temperature)); // Discovery alone never enables control.
    assert(!qingping::bindAddress("58:2D:34:80:17:40"));
    assert(qingping::bindAddress("58:2D:34:80:17:3F"));
    assert(!qingping::activeTemperatureC(&temperature));
    assert(stored);
    // Re-run the actual cold-start NVS load, without persisting any sensor value.
    initialized=false; binding.unbind();
    assert(qingping::init()==ESP_OK&&binding.bound()&&!binding.received());
    assert(!qingping::activeTemperatureC(&temperature));
    timeMs=2000;gapEvent(&event,nullptr);
    assert(qingping::activeTemperatureC(&temperature)&&std::fabs(temperature-22.7f)<0.001f);
    controlStep(); assert(commands.back().enabled&&commands.back().half==45);
    ack=commands.back().id+100;controlStep();assert(awaitingAck); // Unrelated ACK is ignored.
    ack=commands.back().id;controlStep();assert(!awaitingAck);
    const size_t beforeKeepalive=commands.size();
    timeMs=22000;controlStep();assert(commands.size()==beforeKeepalive+1&&commands.back().enabled);
    ack=commands.back().id;controlStep();
    timeMs=301999;assert(qingping::activeTemperatureC(&temperature));
    timeMs=302000;assert(!qingping::activeTemperatureC(&temperature));
    queueAvailable=false;controlStep();assert(sentEnabled); // Queue-full must not lose restore intent.
    queueAvailable=true;controlStep();assert(!commands.back().enabled&&restorePending);
    const size_t lostAckCount=commands.size();
    timeMs+=5000;controlStep();assert(commands.size()==lostAckCount+1&&!commands.back().enabled);
    ack=commands.back().id;controlStep();assert(!restorePending);
    gapEvent(&event,nullptr);controlStep();assert(commands.back().enabled);
    ack=commands.back().id;controlStep();
    assert(qingping::pauseForProvisioning());assert(stops==1&&!qingping::activeTemperatureC(&temperature));
    assert(!qingping::refresh());radioStep();assert(starts==1);
    timeMs+=1000;controlStep();assert(!commands.back().enabled);
    ack=commands.back().id;controlStep();
    qingping::resumeAfterProvisioning();radioStep();assert(starts==2);
    failCommit=true;assert(!qingping::unbind());assert(binding.bound()&&stored);
    failCommit=false;assert(qingping::unbind());assert(!binding.bound());
    assert(!stored);
    assert(!qingping::activeTemperatureC(&temperature));
    timeMs+=1000;controlStep();assert(!commands.back().enabled);
    ack=commands.back().id;controlStep();assert(!restorePending);
    assert(qingping::statusJson().find("\"bound\":false")!=std::string::npos);
    std::puts("Qingping production worker: discovery/binding/freshness/HomeKit source/keepalive/ACK/timeout/retry/provisioning/unbind PASS");
}
