#include "guest_sync_objects.h"
#include "guest_memory.h"
#include "native_sync_objects.h"
namespace sfr {
std::string GuestSyncObjects::check_creation(uint32_t output, uint32_t attributes) const {
    if (!output || output % 4)
        throw RuntimeStop("sync-output", output, "requires a nonnull aligned guest handle output");
    // Preflight before creating a real kernel object or publishing a guest ID.
    memory_.check_write(output, 4);
    if (!attributes) return {};
    if (attributes % 4)
        throw RuntimeStop("sync-attributes", attributes, "unaligned object attributes");
    memory_.check(attributes, 12);
    const auto root = memory_.load<uint32_t>(attributes);
    const auto descriptor = memory_.load<uint32_t>(uint64_t(attributes) + 4);
    const auto flags = memory_.load<uint32_t>(uint64_t(attributes) + 8);
    if (root != 0xFFFFFFFC || flags != 0x80)
        throw RuntimeStop("sync-attributes", attributes, "unsupported synchronization root or flags");
    if (!descriptor || descriptor % 4)
        throw RuntimeStop("sync-name", descriptor, "requires a nonnull aligned ANSI descriptor");
    memory_.check(descriptor, 8);
    const auto length = memory_.load<uint16_t>(descriptor);
    const auto maximum = memory_.load<uint16_t>(uint64_t(descriptor) + 2);
    const auto buffer = memory_.load<uint32_t>(uint64_t(descriptor) + 4);
    if (!length || length > maximum || !buffer)
        throw RuntimeStop("sync-name", descriptor, "invalid counted synchronization name");
    memory_.check(buffer, length);
    // Snapshot the complete counted input before publishing any handle. The
    // output may overlap these inputs; MaximumLength does not extend the read.
    std::string name;
    name.reserve(length);
    for (uint32_t i = 0; i < length; ++i) {
        const auto byte = memory_.load<uint8_t>(uint64_t(buffer) + i);
        if (byte < 0x20 || byte > 0x7E || byte == '/' || byte == '\\')
            throw RuntimeStop("sync-name", uint64_t(buffer) + i, "unsupported flat ASCII synchronization name");
        name.push_back(static_cast<char>(byte));
    }
    return name;
}
uint32_t GuestSyncObjects::publish(uint32_t output, uint32_t status, uint32_t handle) {
    try {
        // The verified import clears output when count validation fails.
        memory_.store<uint32_t>(output, handle);
    } catch (...) {
        if (handle) native_.close(handle);
        throw;
    }
    return status;
}
uint32_t GuestSyncObjects::create_semaphore(uint32_t output, uint32_t attributes, int32_t initial, int32_t maximum) {
    const auto name = check_creation(output, attributes);
    const auto result = native_.create_semaphore(initial, maximum, name);
    return publish(output, result.status, result.handle);
}
uint32_t GuestSyncObjects::create_event(uint32_t output, uint32_t attributes, uint32_t type, uint32_t initial) {
    if (type > 1)
        throw RuntimeStop("event-type", type, "requires notification(0) or synchronization(1) event type");
    const auto name = check_creation(output, attributes);
    const auto result = native_.create_event(type == 0, initial != 0, name);
    return publish(output, result.status, result.handle);
}
}
