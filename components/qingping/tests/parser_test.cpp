#include "qingping_parser.h"
#include "qingping_binding.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

int main() {
    const uint8_t frame[] = {0x08,0x10,0x3f,0x17,0x80,0x34,0x2d,0x58,0x01,0x04,0xe3,0x00,0x90,0x01,0x02,0x01,0x55};
    qingping::Reading r{};
    assert(qingping::parseFdcd(frame, sizeof(frame), &r));
    assert(r.temperatureTenthsC == 227 && r.humidityTenthsPercent == 400 && r.batteryPercent == 85);
    assert(r.address[0] == 0x58 && r.address[5] == 0x3f);
    qingping::Binding binding;
    assert(!binding.online(0));
    binding.bind(r.address);
    assert(!binding.online(0)); // Binding/reboot cannot revive a persisted stale value.
    auto other = r;
    other.address[5] ^= 1;
    assert(!binding.receive(other, 1000));
    assert(binding.receive(r, 1000));
    assert(binding.usableForControl(300999));
    assert(!binding.online(301000)); // Exactly five minutes means offline.
    assert(binding.receive(r, 301001));
    assert(binding.usableForControl(301001));
    auto cold = r;
    cold.temperatureTenthsC = 0;
    assert(binding.receive(cold, 301002));
    assert(binding.online(301002) && !binding.usableForControl(301002));
    binding.unbind();
    assert(!binding.online(301002) && !binding.receive(r, 301003));
    assert(!qingping::parseFdcd(frame, sizeof(frame) - 1, &r));
    const uint8_t badHumidity[] = {0x08,0,0,0,0,0,0,0,0x01,0x04,0,0,0xe9,0x03};
    assert(!qingping::parseFdcd(badHumidity, sizeof(badHumidity), &r));
    std::puts("Qingping FDCD parser and binding/timeout policy tests passed");
}
