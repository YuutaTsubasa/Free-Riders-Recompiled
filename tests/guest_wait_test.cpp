#include "guest_wait.h"
#include "guest_memory.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("unsupported wait must stop");
}
void checked_relative_timeouts_and_retained_wait() {
    sfr::GuestMemory memory;
    memory.map(0x10000000, 8);
    sfr::NativeSyncObjects objects;
    const auto event = objects.create_event(false, true).handle;
    memory.store<uint64_t>(0x10000000, 0xFFFFFFFFFFFEC780ull);
    auto wait = sfr::GuestWait::prepare(memory, objects, event, 1, 0, 0x10000000);
    require(wait.status == 0 && !wait.infinite && wait.milliseconds == 8 && bool(wait.target),
            "actual guest relative timeout becomes eight milliseconds");
    objects.close(event);
    require(wait.wait({}).status == 0, "prepared wait owns the native event after original guest close");
    require(wait.wait({}).status == 0x102, "auto-reset event signal is consumed only once");
    const auto ready = objects.create_event(true, true).handle;
    for (const auto bits : {0ull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFD8F0ull,
                           0xFFFFFFFFFFFFD8EFull, 0x8000000000000000ull}) {
        memory.store<uint64_t>(0x10000000, bits);
        auto parsed = sfr::GuestWait::prepare(memory, objects, ready, 0, 0, 0x10000000);
        const auto magnitude = uint64_t(0) - bits;
        require(parsed.milliseconds == magnitude / 10000 + (magnitude % 10000 != 0),
                "relative ticks round upward without signed overflow, including INT64_MIN");
        require(parsed.wait({}).status == 0, "even a large finite wait can complete on an actual signaled event");
    }
    auto infinite = sfr::GuestWait::prepare(memory, objects, ready, 1, 0, 0);
    require(infinite.infinite && infinite.wait({}).status == 0, "null timeout waits indefinitely for actual signal");
    std::stop_source cancelled;
    cancelled.request_stop();
    require(infinite.wait(cancelled.get_token()).cancelled, "cancellation remains distinct from guest status");
    auto invalid = sfr::GuestWait::prepare(memory, objects, 0x721000F0, 1, 0, 0);
    require(invalid.status == 0xC0000008 && !invalid.target, "invalid sync handle has no retained wait");
    rejects([&] { sfr::GuestWait::prepare(memory, objects, ready, 2, 0, 0); });
    rejects([&] { sfr::GuestWait::prepare(memory, objects, ready, 1, 1, 0); });
    rejects([&] { sfr::GuestWait::prepare(memory, objects, 0x72200004, 1, 0, 0); });
    rejects([&] { sfr::GuestWait::prepare(memory, objects, ready, 1, 0, 0x20000000); });
    memory.store<uint64_t>(0x10000000, 1);
    rejects([&] { sfr::GuestWait::prepare(memory, objects, ready, 1, 0, 0x10000000); });
}
}
int main() {
    try { checked_relative_timeouts_and_retained_wait(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
