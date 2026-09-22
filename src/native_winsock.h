#pragma once
#include <cstdint>

namespace sfr {
class GuestMemory;
class NativeWinsock {
public:
    explicit NativeWinsock(GuestMemory& memory) : memory_(memory) {}
    ~NativeWinsock();
    NativeWinsock(const NativeWinsock&) = delete;
    NativeWinsock& operator=(const NativeWinsock&) = delete;
    int startup(uint32_t caller, uint16_t version, uint32_t output);
    int cleanup(uint32_t caller);
    uint32_t acquisitions() const { return acquisitions_; }
private:
    GuestMemory& memory_;
    uint32_t acquisitions_ = 0;
};
}
