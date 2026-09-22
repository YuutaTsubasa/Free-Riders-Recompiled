#pragma once
#include <cstdint>
#include <string>
namespace sfr {
class GuestMemory;
class NativeSyncObjects;
class GuestSyncObjects {
public:
    GuestSyncObjects(GuestMemory& memory, NativeSyncObjects& native) : memory_(memory), native_(native) {}
    uint32_t create_semaphore(uint32_t output, uint32_t attributes, int32_t initial, int32_t maximum);
    uint32_t create_event(uint32_t output, uint32_t attributes, uint32_t type, uint32_t initial);
private:
    std::string check_creation(uint32_t output, uint32_t attributes) const;
    uint32_t publish(uint32_t output, uint32_t status, uint32_t handle);
    GuestMemory& memory_;
    NativeSyncObjects& native_;
};
}
