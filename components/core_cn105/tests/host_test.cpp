#include "cn105_core.h"
#include <cassert>
#include <cstdio>
int main() {
    cn105_core::initMockState();
    char error[128] = {};
    assert(cn105_core::runSelfTest(error, sizeof(error)));
    cn105_core::Packet packet{};
    for (cn105_core::HalfDegreesC half = 17; half <= 100; ++half) {
        assert(cn105_core::buildRemoteTemperaturePacket(true,half,&packet,error,sizeof(error)));
        assert(packet.length == 22 && packet.bytes[0] == 0xfc && packet.bytes[1] == 0x41);
        assert(packet.bytes[5] == 7 && packet.bytes[6] == 1);
        assert(packet.bytes[7] == half - 17 && packet.bytes[8] == half + 128);
        assert(packet.bytes[21] == cn105_core::checksum(packet.bytes,21));
    }
    assert(!cn105_core::buildRemoteTemperaturePacket(true,16,&packet,error,sizeof(error)));
    assert(!cn105_core::buildRemoteTemperaturePacket(true,101,&packet,error,sizeof(error)));
    assert(!cn105_core::buildRemoteTemperaturePacket(true,45,nullptr,error,sizeof(error)));
    assert(cn105_core::buildRemoteTemperaturePacket(false,0,&packet,error,sizeof(error)));
    assert(packet.bytes[5] == 7 && packet.bytes[6] == 0 && packet.bytes[7] == 0 && packet.bytes[8] == 128);
    const auto state = cn105_core::getMockState();
    assert(state.targetTemperatureHalfC == 50); // Reference packets never change setpoints.
    std::puts("CN105 real-source self-test + external temperature encoding/range/restore: PASS");
}
