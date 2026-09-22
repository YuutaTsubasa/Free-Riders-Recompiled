#include "guest_memory.h"
#include "native_notifications.h"
#include "native_sync_objects.h"

#include <cstdint>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
constexpr uint32_t success = 0;
constexpr uint32_t timeout = 0x102;
constexpr uint32_t invalid_handle = 0xc0000008u;
constexpr uint32_t type_mismatch = 0xc0000024u;
constexpr uint32_t system_ui = 0x00000009;
constexpr uint32_t nui_pause = 0x0006001a;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template <typename F> void rejects(F&& operation, const char* message) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error(message);
}

struct Fixture {
    sfr::GuestMemory memory;
    sfr::NativeSyncObjects sync;
    sfr::NativeNotifications notifications{memory, sync};
    Fixture() { memory.map(0x100000, 0x1000); }
};

void listener_is_a_real_initially_empty_waitable_event() {
    Fixture f;
    const uint32_t handle = f.notifications.create(1, 6);
    require(f.notifications.owns(handle) && f.sync.owns(handle) && f.sync.wait(handle, 0) == timeout,
            "new listener is an owned initially nonsignaled waitable notification event");
    f.notifications.publish(system_ui, 7);
    require(f.sync.wait(handle, 0) == success,
            "matching native publication signals the listener event");
    require(f.memory.load<uint32_t>(0x100000) == 0,
            "listener creation and producer publication never write guest outputs");
}

void fifo_matching_and_event_reset_follow_remaining_queue() {
    Fixture f;
    const uint32_t handle = f.notifications.create(1, 6);
    f.notifications.publish(system_ui, 11);
    f.notifications.publish(nui_pause, 22);
    require(!f.notifications.get_next(handle, 0x0000000a, 0x100000, 0x100004) &&
                f.memory.load<uint32_t>(0x100000) == 0 && f.memory.load<uint32_t>(0x100004) == 0 &&
                f.sync.wait(handle, 0) == success,
            "no matching item clears outputs but leaves other queued items and signal intact");
    require(f.notifications.get_next(handle, nui_pause, 0x100000, 0x100004) &&
                f.memory.load<uint32_t>(0x100000) == nui_pause &&
                f.memory.load<uint32_t>(0x100004) == 22 && f.sync.wait(handle, 0) == success,
            "specific match removes its first item while earlier FIFO item keeps event signaled");
    require(f.notifications.get_next(handle, 0, 0x100000, 0) &&
                f.memory.load<uint32_t>(0x100000) == system_ui && f.sync.wait(handle, 0) == timeout,
            "zero match dequeues FIFO, optional null parameter works, and empty queue resets event");
}

void filters_full_mask_category_version_and_never_injects_startup_items() {
    Fixture f;
    const uint32_t low = f.notifications.create(1, 6);
    const uint32_t high = f.notifications.create(uint64_t{1} << 40, 10);
    require(!f.notifications.get_next(low, 0, 0x100000, 0x100004),
            "listener creation injects no heuristic startup notifications");
    f.notifications.publish((40u << 25) | (10u << 16) | 3u, 0xabcdef01u);
    f.notifications.publish((40u << 25) | (11u << 16) | 4u, 9);
    require(f.sync.wait(low, 0) == timeout && f.sync.wait(high, 0) == success &&
                f.notifications.get_next(high, 0, 0x100000, 0x100004) &&
                f.memory.load<uint32_t>(0x100004) == 0xabcdef01u &&
                !f.notifications.get_next(high, 0, 0x100000, 0x100004),
            "full uint64 mask and packed category/version filter exactly without version-11 delivery");
    rejects([&] { (void)f.notifications.create(1, 11); },
            "unsupported maximum notification version fails explicitly");
}

void output_preflight_aliasing_and_capacity_preserve_items() {
    Fixture f;
    const uint32_t handle = f.notifications.create(1, 6);
    f.notifications.publish(system_ui, 0x12345678);
    f.memory.add_import_variable(0x100004, "NotificationParameterGuard");
    rejects([&] { (void)f.notifications.get_next(handle, 0, 0x100000, 0x100004); },
            "both complete output spans are checked before dequeue");
    require(f.notifications.get_next(handle, 0, 0x100008, 0x100008) &&
                f.memory.load<uint32_t>(0x100008) == 0x12345678,
            "failed output loses no item and aliased stores deterministically leave parameter last");
    rejects([&] { (void)f.notifications.get_next(handle, 0, 0, 0x100008); },
            "notification ID output is mandatory");

    for (uint32_t i = 0; i < 4096; ++i) f.notifications.publish(system_ui, i);
    rejects([&] { f.notifications.publish(system_ui, 4096); },
            "per-listener queue capacity rejects overflow without dropping an old item");
    require(f.notifications.get_next(handle, 0, 0x100008, 0x10000c) &&
                f.memory.load<uint32_t>(0x10000c) == 0,
            "queue overflow preserves the oldest FIFO item");
}

void listener_capacity_is_bounded_without_partial_event_publication() {
    Fixture f;
    for (uint32_t i = 0; i < 1024; ++i)
        require(f.notifications.owns(f.notifications.create(1, 6)),
                "each listener through the documented capacity publishes one typed handle");
    const auto native_count = f.sync.open_count();
    rejects([&] { (void)f.notifications.create(1, 6); },
            "listener capacity rejects the 1025th listener");
    require(f.sync.open_count() == native_count,
            "listener capacity failure publishes no extra native event");
}

void typed_handles_close_and_retained_wait_lifetimes_are_strict() {
    Fixture f;
    const auto ordinary = f.sync.create_event(true, false);
    rejects([&] { (void)f.notifications.get_next(ordinary.handle, 0, 0x100000, 0); },
            "ordinary events are not notification listeners");
    require(f.notifications.close(ordinary.handle) == invalid_handle && f.sync.owns(ordinary.handle),
            "listener close never consumes an ordinary event");

    const uint32_t listener = f.notifications.create(1, 6);
    require(f.sync.set_event(listener) == type_mismatch &&
                f.sync.reset_event(listener) == type_mismatch && f.sync.wait(listener, 0) == timeout,
            "ordinary set/reset reject notification type without changing event state");
    rejects([&] { (void)f.sync.retain_event(listener); },
            "ordinary retained event capability cannot mutate notification events");
    f.notifications.publish(system_ui, 1);
    auto retained = f.sync.retain_wait(listener);
    require(retained != nullptr && f.notifications.close(listener) == success &&
                !f.notifications.owns(listener) && !f.sync.owns(listener),
            "listener close removes both queue identity and guest event handle once");
    require(retained->wait(0).status == success,
            "retained native wait survives listener close and observes prior signal");
    require(f.notifications.close(listener) == invalid_handle,
            "stale listener close returns invalid handle without effects");
    rejects([&] { (void)f.notifications.get_next(listener, 0, 0x100000, 0); },
            "stale listener cannot masquerade as an empty live queue");
    f.sync.close(ordinary.handle);
}

void producer_thread_only_queues_and_signals_retained_native_state() {
    Fixture f;
    const uint32_t listener = f.notifications.create(1, 6);
    std::promise<void> start;
    auto ready = start.get_future();
    std::jthread producer([&] {
        ready.wait();
        f.notifications.publish(system_ui, 0xfeedbeefu);
    });
    start.set_value();
    require(f.sync.wait(listener, 2000) == success,
            "real producer thread signals listener without guest-memory publication");
    producer.join();
    require(f.notifications.get_next(listener, 0, 0x100000, 0x100004) &&
                f.memory.load<uint32_t>(0x100004) == 0xfeedbeefu,
            "guest thread drains producer item only after native wake");

    const auto baseline = f.sync.open_count();
    {
        sfr::NativeNotifications temporary(f.memory, f.sync);
        (void)temporary.create(1, 6);
        (void)temporary.create(uint64_t{1} << 40, 10);
        require(f.sync.open_count() == baseline + 2,
                "temporary listener owner publishes two native handles");
    }
    require(f.sync.open_count() == baseline,
            "notification owner destructor closes every remaining guest listener handle");
}
}

// A system UI opens and closes; the close is held back so the title cannot
// read both in one pump and miss the "showing" state in between.
void a_drained_publication_waits_for_the_listener_to_report_empty() {
    Fixture f;
    const uint32_t handle = f.notifications.create(1, 6);
    f.notifications.publish(system_ui, 1);
    f.notifications.publish_when_drained(system_ui, 0);
    require(f.notifications.get_next(handle, 0, 0x100000, 0x100004) &&
                f.memory.load<uint32_t>(0x100000) == system_ui && f.memory.load<uint32_t>(0x100004) == 1,
            "the open arrives first");
    require(!f.notifications.get_next(handle, 0, 0x100000, 0x100004) &&
                f.memory.load<uint32_t>(0x100000) == 0,
            "the same pump reads nothing more, so the title sees the UI showing");
    require(f.notifications.get_next(handle, 0, 0x100000, 0x100004) &&
                f.memory.load<uint32_t>(0x100000) == system_ui && f.memory.load<uint32_t>(0x100004) == 0,
            "the next pump reads the close");
    require(!f.notifications.get_next(handle, 0, 0x100000, 0x100004), "and then nothing");
}

int main() {
    try {
        a_drained_publication_waits_for_the_listener_to_report_empty();
        listener_is_a_real_initially_empty_waitable_event();
        fifo_matching_and_event_reset_follow_remaining_queue();
        filters_full_mask_category_version_and_never_injects_startup_items();
        output_preflight_aliasing_and_capacity_preserve_items();
        listener_capacity_is_bounded_without_partial_event_publication();
        typed_handles_close_and_retained_wait_lifetimes_are_strict();
        producer_thread_only_queues_and_signals_retained_native_state();
        std::cout << "Native notification checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
