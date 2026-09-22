#include "guest_sync_objects.h"
#include "guest_memory.h"
#include "native_sync_objects.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("unsupported semaphore request did not stop");
}
void run() {
    sfr::GuestMemory memory;
    memory.map(0x10000000, 64);
    sfr::NativeSyncObjects native;
    sfr::GuestSyncObjects guest(memory, native);
    constexpr uint32_t output = 0x10000000;
    require(guest.create_semaphore(output, 0, 1, 1) == 0, "observed unnamed semaphore creation succeeds");
    const auto handle = memory.load<uint32_t>(output);
    require(handle == 0x72100004 && native.owns(handle) && native.open_count() == 1,
            "published handle owns an actual native semaphore");
    require(memory.load<uint8_t>(output) == 0x72 && memory.load<uint8_t>(output + 1) == 0x10 &&
            memory.load<uint8_t>(output + 2) == 0 && memory.load<uint8_t>(output + 3) == 4,
            "guest handle is stored in big-endian order");
    require(native.wait(handle, 0) == 0 && native.wait(handle, 0) == 0x102,
            "original initial count is real and consumed by native wait");
    require(native.close(handle) == 0 && !native.owns(handle), "real semaphore closes");
    for (const auto initial : {-1, 2}) {
        memory.store<uint32_t>(output, 0xAABBCCDD);
        require(guest.create_semaphore(output, 0, initial, 1) == 0xC000000D &&
                memory.load<uint32_t>(output) == 0 && native.open_count() == 0,
                "invalid counts return invalid-parameter and clear guest output");
    }
    memory.store<uint32_t>(output, 0x11223344);
    rejects([&] { guest.create_semaphore(output, 0x10000010, 1, 1); });
    require(memory.load<uint32_t>(output) == 0x11223344 && native.open_count() == 0,
            "unsupported object attributes do not create or publish a semaphore");
    rejects([&] { guest.create_semaphore(0, 0, 1, 1); });
    rejects([&] { guest.create_semaphore(output + 1, 0, 1, 1); });
    rejects([&] { guest.create_semaphore(0x20000000, 0, 1, 1); });
    rejects([&] { guest.create_semaphore(0x10000040, 0, 1, 1); });
    memory.map(0x30000000, 2);
    rejects([&] { guest.create_semaphore(0x30000000, 0, 1, 1); });
    memory.add_read_only_word(output + 16, [] { return 0x22334455u; });
    rejects([&] { guest.create_semaphore(output + 16, 0, 1, 1); });
    memory.add_import_variable(output + 24, "ProtectedSemaphoreOutput");
    rejects([&] { guest.create_semaphore(output + 24, 0, 1, 1); });
    require(native.open_count() == 0 && memory.load<uint32_t>(output) == 0x11223344,
            "all output preflight failures preserve state and allocate no native object");
    require(guest.create_semaphore(output, 0, 0, 1) == 0, "zero initial count is valid");
    const auto second = memory.load<uint32_t>(output);
    require(second == 0x72100008 && native.wait(second, 0) == 0x102,
            "failed preflights do not consume IDs and zero-count semaphore is unsignaled");
    require(guest.create_event(output, 0, 1, 0) == 0, "observed auto-reset event creation succeeds");
    const auto event = memory.load<uint32_t>(output);
    require(event == 0x7210000C && native.owns(event) && native.wait(event, 0) == 0x102,
            "event shares collision-free IDs and starts unsignaled");
    require(native.set_event(event) == 0 && native.wait(event, 0) == 0 && native.wait(event, 0) == 0x102,
            "guest synchronization event resets after one native wait");
    require(guest.create_event(output, 0, 0, 1) == 0, "notification event creation succeeds");
    const auto notification = memory.load<uint32_t>(output);
    require(native.wait(notification, 0) == 0 && native.wait(notification, 0) == 0,
            "guest type zero maps to a manual-reset event");
    const auto live = native.open_count();
    memory.store<uint32_t>(output, 0x55667788);
    rejects([&] { guest.create_event(output, 0, 2, 0); });
    rejects([&] { guest.create_event(output, output + 8, 1, 0); });
    rejects([&] { guest.create_event(0, 0, 1, 0); });
    rejects([&] { guest.create_event(output + 16, 0, 1, 0); });
    require(native.open_count() == live && memory.load<uint32_t>(output) == 0x55667788,
            "unsupported event profiles and protected output do not mutate or allocate");
}
}
int main() { try { run(); return 0; } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
