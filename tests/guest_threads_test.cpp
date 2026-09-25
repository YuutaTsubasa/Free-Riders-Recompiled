#include "guest_threads.h"
#include "guest_memory.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <future>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("unsupported thread operation did not stop");
}
bool target(uint32_t address) { return address == 0x82001000 || address == 0x82002000; }
sfr::GuestThreads::Request request() {
    return {0x10000000, 0x1000, 0x10000004, 0x82001000, 0x82002000, 0x10000080, 1, 0};
}
void prepare(sfr::GuestMemory& memory) {
    memory.map(0x10000000, 0x100);
    memory.store<uint32_t>(0x10000040, 0x12345678);
}
void independent_state_and_owned_suspension() {
    sfr::GuestMemory memory;
    prepare(memory);
    std::atomic<uint32_t> calls = 0;
    uint32_t factories = 0;
    {
        sfr::GuestThreads threads(memory, {4, 0x10000040, 8, 4}, 0x40000, target,
            [&](const sfr::GuestThreads::State& state) {
                ++factories;
                require(state.startup == 0x82001000 && state.worker == 0x82002000 && state.argument == 0x10000080,
                        "factory preserves original startup, worker and argument");
                return [&](std::stop_token) { ++calls; return 0u; };
            });
        require(threads.create(request()) == 0, "actual suspended worker creation succeeds");
        const auto first = threads.snapshot(memory.load<uint32_t>(0x10000000));
        const auto& a = first.state;
        require(a.handle == 0x72200004 && a.id == 2 && memory.load<uint32_t>(0x10000004) == 2,
                "opaque handle and independent guest ID are published big-endian");
        require(memory.load<uint8_t>(0x10000000) == 0x72 && memory.load<uint8_t>(0x10000001) == 0x20 &&
                memory.load<uint8_t>(0x10000003) == 4, "output handle byte order");
        require(first.native_id != 0 && first.suspended && !first.entry_started && calls == 0,
                "real native worker remains suspended before original entry");
        require(a.pcr == 0x73000000 && a.thread_object == 0x73001000 &&
                a.stack_limit == 0x73021000 && a.stack_base == 0x73025000,
                "observed stack request rounds up to the documented minimum");
        require(memory.load<uint32_t>(a.pcr) == a.tls_static &&
                memory.load<uint32_t>(a.pcr + 0x30) == a.pcr &&
                memory.load<uint32_t>(a.pcr + 0x70) == a.stack_base &&
                memory.load<uint32_t>(a.pcr + 0x74) == a.stack_limit &&
                memory.load<uint32_t>(a.pcr + 0x100) == a.thread_object,
                "PCR points to this thread's own storage and stack");
        require(memory.load<uint8_t>(a.thread_object) == 6 && memory.load<uint8_t>(a.thread_object + 0xBC) == 1 &&
                memory.load<uint32_t>(a.thread_object + 0x68) == a.tls_static &&
                memory.load<uint32_t>(a.thread_object + 0x14C) == a.id &&
                memory.load<uint32_t>(a.thread_object + 0x150) == a.worker &&
                memory.load<uint32_t>(a.thread_object + 0x16C) == 1,
                "known thread fields preserve suspended state, TLS, identity and original entry");
        require(memory.load<uint32_t>(a.tls_static) == 0x12345678 &&
                memory.load<uint32_t>(a.tls_static + 4) == 0 && memory.load<uint32_t>(a.tls_dynamic) == 0,
                "fresh static template and zero dynamic TLS values");
        rejects([&] { memory.check(a.stack_limit - 1, 1); });
        rejects([&] { memory.check(a.stack_base, 1); });
        rejects([&] { (void)memory.load<uint32_t>(a.thread_object + 0x84); });
        memory.store<uint32_t>(a.tls_static, 0xAABBCCDD);
        auto next = request(); next.stack_size = 0; next.parent_cpu = 2;
        require(threads.create(next) == 0, "second suspended worker creation succeeds");
        const auto second = threads.snapshot(memory.load<uint32_t>(next.handle_output));
        const auto& b = second.state;
        require(b.handle == a.handle + 4 && b.id == a.id + 1 && b.pcr != a.pcr && first.native_id != second.native_id,
                "guest and native worker identities are independent");
        require(b.stack_base - b.stack_limit == 0x40000 && memory.load<uint8_t>(b.pcr + 0x10C) == 2,
                "zero stack request uses executable default and CPU identity is inherited");
        require(memory.load<uint32_t>(b.tls_static) == 0x12345678 && memory.load<uint32_t>(a.tls_static) == 0xAABBCCDD,
                "new TLS uses the template, not another thread's mutated values");
        require(threads.size() == 2 && factories == 2 && calls == 0, "both workers are prepared but never executed");
    }
    require(calls == 0, "manager cleanup cancels parked native workers without invoking guest entry");
}
void creation_affinity_selects_guest_and_native_processor_while_parked() {
    sfr::GuestMemory memory;
    prepare(memory);
    std::atomic<unsigned> calls = 0;
    unsigned factories = 0;
    auto expected = request();
    expected.startup = 0x824D2A10;
    expected.worker = 0x824D4388;
    expected.argument = 0x82B4FFC8;
    uint32_t expected_cpu = 0;
    {
        sfr::GuestThreads threads(memory, {4, 0x10000040, 8, 4}, 0x40000,
            [](uint32_t address) { return address == 0x824D2A10 || address == 0x824D4388; },
            [&](const sfr::GuestThreads::State& state) {
                ++factories;
                require(state.startup == expected.startup && state.worker == expected.worker &&
                        state.argument == expected.argument,
                        "creation affinity preserves the observed original startup, worker and argument");
                require(memory.load<uint8_t>(state.pcr + 0x10C) == expected_cpu &&
                        memory.load<uint8_t>(state.thread_object + 0xBF) == expected_cpu,
                        "both processor fields are initialized before the native entry factory");
                require(memory.load<uint32_t>(expected.handle_output) == 0x11223344 &&
                        memory.load<uint32_t>(expected.id_output) == 0x55667788,
                        "creation prepares state before publishing either output");
                return [&](std::stop_token) { ++calls; return 0u; };
            });
        const auto create = [&] {
            memory.store<uint32_t>(expected.handle_output, 0x11223344);
            memory.store<uint32_t>(expected.id_output, 0x55667788);
            require(threads.create(expected) == 0, "suspended creation accepts a supported creation affinity");
            const auto result = threads.snapshot(memory.load<uint32_t>(expected.handle_output));
            const auto& state = result.state;
            require(result.native_id != 0 && result.suspended && !result.entry_started && calls == 0 &&
                    memory.load<uint8_t>(state.thread_object + 0xBC) == 1,
                    "creation affinity leaves the real native worker parked without entering guest code");
            require(memory.load<uint8_t>(state.pcr + 0x10C) == expected_cpu &&
                    memory.load<uint8_t>(state.thread_object + 0xBF) == expected_cpu &&
                    memory.base()[state.pcr + 0x10C] == expected_cpu &&
                    memory.base()[state.thread_object + 0xBF] == expected_cpu,
                    "guest loads and direct generated-code reads see the selected processor");
            require(memory.load<uint32_t>(state.thread_object + 0x16C) == expected.flags,
                    "KTHREAD retains the full original creation flags");
            for (unsigned byte = 0; byte < 4; ++byte)
                require(memory.base()[state.thread_object + 0x16C + byte] ==
                        static_cast<uint8_t>(expected.flags >> ((3 - byte) * 8)),
                        "original creation flags retain big-endian bytes");
            require(memory.load<uint32_t>(expected.id_output) == state.id,
                    "successful affinity creation publishes the independent guest ID");
            return state.thread_object;
        };
        const auto reference_object = create();
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
            const uint32_t mask = uint32_t{1} << cpu;
            require(threads.set_affinity(reference_object, mask, 0) == 0,
                    "existing affinity route selects each guest processor");
            const auto expected_host = threads.host_affinity(reference_object);
            require(expected_host && !(expected_host & (expected_host - 1)),
                    "reference affinity binds one allowed native processor");
            expected_cpu = cpu;
            for (const bool explicit_affinity : {false, true}) {
                expected.flags = explicit_affinity ? (mask << 24) | 1u : 1u;
                expected.parent_cpu = explicit_affinity ? (cpu + 1) % 6 : cpu;
                const auto object = create();
                require(threads.host_affinity(object) == expected_host,
                        "inherited and explicit creation affinity use the existing native processor mapping");
            }
        }
        require(factories == 13 && threads.size() == 13 && calls == 0,
                "all six inherited CPUs and all six explicit masks remain parked");
    }
    require(calls == 0, "affinity-selected workers are cancelled without running their original entry");
}
void preflight_rejects_without_publishing_or_allocating() {
    sfr::GuestMemory memory;
    prepare(memory);
    uint32_t factories = 0;
    sfr::GuestThreads threads(memory, {4, 0x10000040, 8, 4}, 0x40000, target,
        [&](const auto&) { ++factories; return [](std::stop_token) { return 0u; }; });
    memory.store<uint32_t>(0x10000000, 0x11223344);
    memory.store<uint32_t>(0x10000004, 0x55667788);
    const auto reject_request = [&](auto changed) {
        const auto before = memory.usage();
        rejects([&] { threads.create(changed); });
        const auto after = memory.usage();
        require(threads.size() == 0 && factories == 0 && memory.available(0x73000000, 0x200000),
                "invalid request allocates no guest state or native entry");
        require(before.reserved_bytes == after.reserved_bytes && before.committed_bytes == after.committed_bytes,
                "invalid request changes neither reserved nor committed guest memory");
        require(memory.load<uint32_t>(0x10000000) == 0x11223344 && memory.load<uint32_t>(0x10000004) == 0x55667788,
                "invalid request preserves both outputs");
    };
    for (const uint32_t flags : {0u, 2u, 3u, 0x81u, 0x00010001u, 0x00800001u,
                                 0x01000000u, 0x08000000u, 0x08000003u, 0x08000081u,
                                 0x03000001u, 0x21000001u, 0x40000001u, 0x80000001u, 0xFF000001u}) {
        auto invalid = request(); invalid.flags = flags; reject_request(invalid);
    }
    auto r = request();
    r = request(); r.handle_output = 0; reject_request(r);
    r = request(); r.handle_output += 1; reject_request(r);
    r = request(); r.id_output = r.handle_output; reject_request(r);
    r = request(); r.id_output = 0x20000000; reject_request(r);
    r = request(); r.startup = 0; reject_request(r);
    r = request(); r.worker = 0x82003000; reject_request(r);
    r = request(); r.worker += 1; reject_request(r);
    r = request(); r.stack_size = 0xFFFFFFFF; reject_request(r);
    r = request(); r.stack_size = 0x100001; reject_request(r);
    r = request(); r.parent_cpu = 6; reject_request(r);
    r = request(); r.flags = 0x08000001; r.parent_cpu = 6; reject_request(r);
    memory.add_read_only_word(0x10000020, [] { return 0x12345678u; });
    r = request(); r.id_output = 0x10000020; reject_request(r);
    memory.map(0x30000000, 2);
    r = request(); r.handle_output = 0x30000000; reject_request(r);
    memory.reserve(0x73000000, 0x200000);
    rejects([&] { threads.create(request()); });
    require(threads.size() == 0 && factories == 0, "preexisting thread-slot reservation is never overwritten");
}
void creation_failures_retire_slots_without_publishing() {
    sfr::GuestMemory memory;
    prepare(memory);
    memory.store<uint32_t>(0x10000000, 0x11223344);
    memory.store<uint32_t>(0x10000004, 0x55667788);
    unsigned attempt = 0;
    std::atomic<unsigned> calls = 0;
    sfr::GuestThreads threads(memory, {4, 0x10000040, 8, 4}, 0x40000, target,
        [&](const auto&) -> sfr::NativeThread::Entry {
            ++attempt;
            if (attempt == 1) throw sfr::RuntimeStop("test-factory", 0, "injected factory failure");
            if (attempt == 2)
                memory.add_read_only_word(0x10000004, [] { return 0x55667788u; });
            return [&](std::stop_token) { ++calls; return 0u; };
        });
    rejects([&] { threads.create(request()); });
    require(!memory.available(0x73000000, 0x200000) && threads.size() == 0,
            "factory failure retires reserved slot and exposes no successful thread");
    rejects([&] { threads.create(request()); });
    require(!memory.available(0x73200000, 0x200000) && threads.size() == 0 && calls == 0,
            "publication failure cancels parked native thread before callback");
    require(memory.load<uint32_t>(0x10000000) == 0x11223344 && memory.load<uint32_t>(0x10000004) == 0x55667788,
            "rechecking both outputs prevents partial publication");
    auto next = request(); next.id_output = 0;
    threads.create(next);
    const auto result = threads.snapshot(memory.load<uint32_t>(next.handle_output));
    require(result.state.handle == 0x72200004 && result.state.id == 2 && result.state.pcr == 0x73400000,
            "only successful creation consumes public identity, failed slots stay retired");
    rejects([&] { threads.snapshot(0x72200008); });
}
void invalid_templates_reject_before_allocation() {
    sfr::GuestMemory memory;
    prepare(memory);
    const sfr::GuestThreads::EntryFactory factory = [](const auto&) { return [](std::stop_token) { return 0u; }; };
    for (const auto tls : {sfr::GuestThreads::TlsTemplate{0, 0x10000040, 8, 4},
                          {2049, 0x10000040, 8, 4}, {4, 0x10000040, 65537, 4},
                          {4, 0x10000040, 8, 9}, {4, 0x20000000, 8, 4}})
        rejects([&] { sfr::GuestThreads threads(memory, tls, 0x40000, target, factory); });
    for (const uint32_t stack : {0u, 0x100001u})
        rejects([&] { sfr::GuestThreads threads(memory, {4, 0x10000040, 8, 4}, stack, target, factory); });
    require(memory.available(0x73000000, 0x200000), "invalid templates reserve no thread slots");
}
void object_references_and_native_configuration() {
    sfr::GuestMemory memory;
    prepare(memory);
    std::atomic<unsigned> calls = 0;
    sfr::GuestThreads threads(memory, {4, 0x10000040, 8, 4}, 0x40000, target,
        [&](const auto&) { return [&](std::stop_token) { ++calls; return 0u; }; });
    threads.create(request());
    const auto handle = memory.load<uint32_t>(0x10000000);
    const auto snapshot = threads.snapshot(handle);
    const auto object = snapshot.state.thread_object;
    constexpr uint32_t output = 0x10000010;
    memory.store<uint32_t>(output, 0x12345678);
    require(threads.reference(handle, sfr::GuestThreads::object_type, output) == 0, "reference resolves real thread");
    require(memory.load<uint32_t>(output) == object && threads.references(object) == 1, "reference publishes actual KTHREAD");
    require(threads.reference(handle, 0, output) == 0 && threads.references(object) == 2, "null type accepts owned thread");
    require(threads.reference(handle, 0x72301000, output) == 0xC0000024 && threads.references(object) == 2,
            "wrong type does not acquire reference");
    require(threads.reference(handle + 4, sfr::GuestThreads::object_type, output) == 0xC0000008,
            "unknown handle rejected");
    rejects([&] { threads.reference(handle, 0, 0x20000000); });
    rejects([&] { threads.reference(handle, 0, output + 1); });
    require(threads.references(object) == 2, "bad reference output has no side effects");
    rejects([&] { memory.check(sfr::GuestThreads::object_type, 1); });
    require(!memory.available(sfr::GuestThreads::object_type, 0x1000), "type identity is owned but descriptor remains guarded");
    const auto initial = threads.priority(object);
    require(threads.set_priority(object, 35) == initial && threads.priority(object) == 2, "high priority uses real host setting");
    require(threads.set_priority(object, -35) == 2 && threads.priority(object) == -2, "low priority returns actual previous setting");
    for (const int increment : {-17, 0, 17}) {
        threads.set_priority(object, increment);
        require(threads.priority(object) == 0, "normal priority reference mapping");
    }
    threads.set_priority(object, 18); require(threads.priority(object) == 1, "above-normal mapping");
    threads.set_priority(object, -18); require(threads.priority(object) == -1, "below-normal mapping");
    require(threads.set_affinity(object, 4, output) == 0 && memory.load<uint32_t>(output) == 1,
            "affinity returns previous guest processor mask");
    require(memory.load<uint8_t>(snapshot.state.pcr + 0x10C) == 2 && memory.load<uint8_t>(object + 0xBF) == 2,
            "affinity updates both guest processor fields");
    const auto host_mask = threads.host_affinity(object);
    require(host_mask && !(host_mask & (host_mask - 1)), "host thread is bound to one real processor");
    rejects([&] { threads.set_affinity(object, 8, 0x20000000); });
    rejects([&] { threads.set_affinity(object, 8, snapshot.state.pcr + 0x10C); });
    rejects([&] { threads.set_affinity(object, 8, object + 0xBC); });
    rejects([&] { threads.set_affinity(object, 3, output); });
    rejects([&] { threads.set_affinity(object, 0x40, output); });
    require(threads.set_affinity(object, 0, output) == 0xC000000D, "zero affinity is invalid");
    require(threads.host_affinity(object) == host_mask && memory.load<uint32_t>(output) == 1 &&
            memory.load<uint8_t>(snapshot.state.pcr + 0x10C) == 2, "rejected affinity changes no state or output");
    require(threads.close(handle) == 0 && threads.close(handle) == 0xC0000008, "close invalidates guest handle once");
    require(threads.reference(handle, 0, output) == 0xC0000008, "closed handle cannot acquire references");
    threads.dereference(object); threads.dereference(object);
    require(threads.references(object) == 0 && threads.size() == 1 && calls == 0,
            "live parked execution remains owned after last handle and explicit reference close");
    rejects([&] { threads.dereference(object); });
    require(threads.priority(object) == -1, "native execution object remains valid after handle close");
    require(threads.snapshot(handle).suspended && !threads.snapshot(handle).entry_started,
            "configuration and handle close do not resume or cancel the live worker");
}
void actual_resume_and_owned_shutdown() {
    sfr::GuestMemory memory;
    prepare(memory);
    std::atomic<unsigned> calls = 0;
    std::atomic<bool> exited = false;
    sfr::GuestThreads threads(memory, {4, 0x10000040, 8, 4}, 0x40000, target,
        [&](const auto&) { return [&](std::stop_token stop) {
            ++calls;
            while (!stop.stop_requested()) std::this_thread::yield();
            exited = true;
            return 0u;
        }; });
    threads.create(request());
    const auto handle = memory.load<uint32_t>(0x10000000);
    const auto object = threads.snapshot(handle).state.thread_object;
    rejects([&] { threads.resume(handle, 0x20000000); });
    rejects([&] { threads.resume(handle, object + 0xBC); });
    require(threads.snapshot(handle).suspended && !threads.snapshot(handle).entry_started,
            "invalid resume outputs do not start native execution");
    const auto result = threads.resume(handle, 0x10000010);
    require(result.status == 0 && result.previous == 1 && result.id == 2 &&
            memory.load<uint32_t>(0x10000010) == 1 && memory.load<uint8_t>(object + 0xBC) == 0,
            "actual resume count and guest suspend state are published");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!calls && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    require(calls == 1 && threads.snapshot(handle).entry_started, "original callback admitted on native worker");
    require(threads.resume(handle, 0).previous == 0, "repeated resume preserves real Windows count");
    threads.shutdown();
    require(exited && calls == 1, "shutdown joins live workers before dependent state can be destroyed");
    threads.shutdown();
}
void completion_before_self_suspend_does_not_lose_an_early_resume() {
    sfr::GuestMemory memory;
    prepare(memory);
    sfr::GuestThreads threads(memory, {4, 0x10000040, 8, 4}, 0x40000, target,
        [](const auto&) { return [](std::stop_token stop) {
            while (!stop.stop_requested()) std::this_thread::yield();
            return 0u;
        }; });
    threads.create(request());
    const auto handle = memory.load<uint32_t>(0x10000000);
    threads.resume(handle, 0);

    // Worker announces completion; main consumes it and resumes the next job
    // before the worker reaches its following NtSuspendThread call.
    require(threads.prepare_self_suspend(handle).status == 0, "prepare completion handoff");
    require(threads.resume(handle, 0).previous == 1, "early resume consumes the announced suspension");
    require(threads.suspend_self(handle, 0x10000010).previous == 0 &&
            memory.load<uint32_t>(0x10000010) == 0 && threads.guest_suspends(handle) == 0,
            "the following self-suspend must not suspend a second time");

    // An ordinary self suspension still blocks, and a late resume releases it.
    require(threads.suspend_self(handle, 0).previous == 0 && threads.guest_suspends(handle) == 1,
            "ordinary self suspension is unchanged");
    auto waiter = threads.suspension_waiter(handle);
    std::stop_source cancel;
    auto waiting = std::async(std::launch::async, [&] { waiter(cancel.get_token()); });
    const bool blocked = waiting.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    threads.resume(handle, 0);
    const bool woke = waiting.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    cancel.request_stop();
    waiting.get();
    require(blocked && woke, "a retained suspension waiter observes a later resume");

    // Preparation consumes only its own suspension, not another caller's.
    threads.prepare_self_suspend(handle);
    rejects([&] { threads.prepare_self_suspend(handle); });
    threads.suspend(handle, 0);
    threads.resume(handle, 0);
    rejects([&] { threads.suspend_self(handle, 0x20000000); });
    threads.suspend_self(handle, 0);
    require(threads.guest_suspends(handle) == 1, "foreign nested suspension remains outstanding");
    auto cancelled_wait = threads.suspension_waiter(handle);
    cancelled_wait(cancel.get_token());
    require(threads.guest_suspends(handle) == 1, "cancellation does not fabricate a resume");
    threads.resume(handle, 0);
}
}
int main() { try { independent_state_and_owned_suspension();
    creation_affinity_selects_guest_and_native_processor_while_parked();
    preflight_rejects_without_publishing_or_allocating();
    creation_failures_retire_slots_without_publishing(); invalid_templates_reject_before_allocation();
    object_references_and_native_configuration(); actual_resume_and_owned_shutdown();
    completion_before_self_suspend_does_not_lose_an_early_resume(); return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
