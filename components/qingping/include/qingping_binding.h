// Kiri Bridge — https://kiri.dkt.moe — GPL-3.0
#pragma once
#include "qingping_parser.h"
#include <cstring>

namespace qingping {
// Pure policy, shared by the firmware worker and host regression tests.
// The owner serializes access. Persist only address, never a reading or its age.
class Binding {
public:
    static constexpr uint64_t kTimeoutMs = 300000;
    void bind(const uint8_t address[6]) {
        std::memcpy(address_, address, 6);
        bound_ = true;
        received_ = false;
    }
    void unbind() { bound_ = false; received_ = false; }
    bool bound() const { return bound_; }
    const uint8_t* address() const { return address_; }
    bool receive(const Reading& reading, uint64_t nowMs) {
        if (!bound_ || std::memcmp(address_, reading.address, 6) != 0) return false;
        reading_ = reading;
        lastSeenMs_ = nowMs;
        received_ = true;
        return true;
    }
    bool received() const { return received_; }
    uint64_t ageMs(uint64_t nowMs) const {
        return received_ && nowMs >= lastSeenMs_ ? nowMs - lastSeenMs_ : 0;
    }
    bool online(uint64_t nowMs) const {
        return bound_ && received_ && nowMs >= lastSeenMs_ && ageMs(nowMs) < kTimeoutMs;
    }
    bool usableForControl(uint64_t nowMs) const {
        // External temperature Encoding A/B range supported by our CN105 builder.
        return online(nowMs) && reading_.temperatureTenthsC >= 85 && reading_.temperatureTenthsC <= 500;
    }
    const Reading& reading() const { return reading_; }
private:
    uint8_t address_[6] = {};
    bool bound_ = false;
    bool received_ = false;
    uint64_t lastSeenMs_ = 0;
    Reading reading_{};
};
}
