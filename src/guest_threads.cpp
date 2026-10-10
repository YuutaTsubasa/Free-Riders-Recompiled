#include "guest_threads.h"
#include <shared_mutex>
#include "guest_memory.h"
#include "system_time.h"
#include "host_timing.h"
#include <algorithm>
#include <cstdlib>
#include <bit>
#include <limits>
#include <utility>
#include <vector>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace sfr {
namespace {
void place_guest_worker(NativeThread& thread, uint32_t cpu) {
#ifdef _WIN32
    const char* setting = std::getenv("SFR_WORKER_AFFINITY");
    thread.set_guest_processor(cpu, !setting || *setting != '0');
#else
    thread.set_guest_processor(cpu);
#endif
}

// 96 slots fill 0x73000000..0x7EFFFFFF, below the XMA registers at 0x7FEA0000.
constexpr uint32_t slots_begin = 0x73000000, slot_stride = 0x200000, slot_count = 96;
constexpr uint32_t stack_max = 0x100000;
void check_outputs(GuestMemory& memory, const GuestThreads::Request& r) {
    if (!r.handle_output || (r.handle_output & 3) || (r.id_output & 3))
        throw RuntimeStop("thread-output", r.handle_output, "thread outputs require aligned writable words");
    if (r.id_output == r.handle_output)
        throw RuntimeStop("thread-output", r.id_output, "thread handle and ID outputs overlap");
    memory.check_write(r.handle_output, 4);
    if (r.id_output) memory.check_write(r.id_output, 4);
}
}
struct GuestThreads::Impl {
    struct Record {
        State state;
        std::unique_ptr<NativeThread> native;
        uint32_t references = 0;
        // Guest-only suspensions of a running thread (see GuestThreads::suspend).
        std::atomic<uint32_t> guest_suspends{0};
        // The registry is mutated under guest execution, but a retained waiter
        // runs without that permit. Serialize its predicate check with count
        // publication so a resume cannot be lost between checking and parking.
        std::mutex suspension_mutex;
        std::condition_variable_any suspension_changed;
        bool prepared_self_suspend = false;
        uint32_t prepared_previous = 0;
        bool handle_open = true;
        bool owns_storage = true;
        // NtDuplicateObject's further handles to this thread, each closed on its own.
        std::vector<uint32_t> duplicates;
    };
    GuestMemory& memory;
    TlsTemplate tls;
    uint32_t default_stack;
    TargetValidator validator;
    EntryFactory factory;
    std::vector<std::unique_ptr<Record>> records;
    // The registry is changed under the global execution permit; this lets
    // a lookup that runs without it (a detached guest waiting on a thread
    // handle) see a whole change. Taken only by create, close and host_handle.
    mutable std::shared_mutex registry_mutex;
    uint32_t next_slot = 0, next_handle = 0x72200004, next_id = 2;
    const bool suspend_notify = [] {
        const char* setting = std::getenv("SFR_SUSPEND_NOTIFY");
        return !setting || *setting != '0';
    }();
    Impl(GuestMemory& m, TlsTemplate t, uint32_t stack, TargetValidator v, EntryFactory f)
        : memory(m), tls(t), default_stack(stack), validator(std::move(v)), factory(std::move(f)) {
        if (!validator || !factory || !tls.slots || tls.slots > 2048 || tls.data_size > 65536 ||
            tls.raw_size > tls.data_size || !default_stack || default_stack > stack_max)
            throw RuntimeStop("thread-profile", 0, "unsupported thread template or stack limits");
        if (tls.raw_size) memory.check(tls.raw_address, tls.raw_size);
        records.reserve(slot_count);
        if (!memory.available(object_type, 0x1000))
            throw RuntimeStop("thread-type", object_type, "thread type identity already reserved");
        memory.reserve(object_type, 0x1000);
    }
    Record* find_handle(uint32_t handle) const {
        for (const auto& record : records)
            if ((record->handle_open && record->state.handle == handle) ||
                std::find(record->duplicates.begin(), record->duplicates.end(), handle) != record->duplicates.end())
                return record.get();
        return nullptr;
    }
    Record& find_object(uint32_t object) const {
        for (const auto& record : records)
            if (record->owns_storage && record->state.thread_object == object) return *record;
        throw RuntimeStop("thread-object", object, "unknown thread object");
    }
    void initialize(const State& s, const Request& r, uint32_t cpu, bool reused) {
        memory.commit(s.pcr, 0x2D8);
        memory.commit(s.thread_object, 0xAB0);
        memory.commit(s.tls_static, tls.data_size + tls.slots * 4);
        memory.commit(s.stack_limit, s.stack_base - s.stack_limit);
        if (reused) {
            const std::vector<uint8_t> zeros(std::max<uint32_t>(0xAB0, s.stack_base - s.stack_limit));
            const auto clear = [&](uint32_t address, uint32_t size) {
                memory.write_bytes(address, std::span(zeros).first(size));
            };
            clear(s.pcr, 0x2D8);
            // This guarded import word remains registered for the slot's lifetime.
            clear(s.thread_object, 0x84);
            clear(s.thread_object + 0x88, 0xAB0 - 0x88);
            clear(s.stack_limit, s.stack_base - s.stack_limit);
        }
        for (uint32_t i = 0; i < tls.data_size + tls.slots * 4; ++i)
            memory.store<uint8_t>(s.tls_static + i,
                i < tls.raw_size ? memory.load<uint8_t>(uint64_t(tls.raw_address) + i) : 0);
        const auto p = s.pcr, k = s.thread_object;
        memory.store<uint32_t>(p, s.tls_static);
        memory.store<uint32_t>(p + 0x30, p);
        memory.store<uint32_t>(p + 0x70, s.stack_base);
        memory.store<uint32_t>(p + 0x74, s.stack_limit);
        memory.store<uint32_t>(p + 0x100, k);
        memory.store<uint8_t>(p + 0x10C, static_cast<uint8_t>(cpu));
        memory.store<uint8_t>(k, 6);
        memory.store<uint8_t>(k + 0xBC, 1);
        memory.store<uint8_t>(k + 0xBF, static_cast<uint8_t>(cpu));
        memory.store<uint32_t>(k + 0x10, k + 0x10);
        memory.store<uint32_t>(k + 0x14, k + 0x10);
        memory.store<uint32_t>(k + 0x40, k + 0x20);
        memory.store<uint32_t>(k + 0x44, k + 0x20);
        memory.store<uint32_t>(k + 0x48, k);
        memory.store<uint32_t>(k + 0x4C, k + 0x18);
        memory.store<uint16_t>(k + 0x54, 0x102);
        memory.store<uint16_t>(k + 0x56, 1);
        memory.store<uint32_t>(k + 0x5C, s.stack_base);
        memory.store<uint32_t>(k + 0x60, s.stack_limit);
        memory.store<uint32_t>(k + 0x68, s.tls_static);
        memory.store<uint32_t>(k + 0x74, k + 0x74);
        memory.store<uint32_t>(k + 0x78, k + 0x74);
        memory.store<uint32_t>(k + 0x7C, k + 0x7C);
        memory.store<uint32_t>(k + 0x80, k + 0x7C);
        if (!reused)
            memory.add_import_variable(k + 0x84, "unsupported thread process-information pointer");
        memory.store<uint8_t>(k + 0x8B, 1);
        memory.store<uint32_t>(k + 0x9C, 0xFDFFD7FF);
        memory.store<uint32_t>(k + 0xD0, s.stack_base);
        query_system_time(memory, k + 0x130);
        memory.store<uint32_t>(k + 0x144, k + 0x144);
        memory.store<uint32_t>(k + 0x148, k + 0x144);
        memory.store<uint32_t>(k + 0x14C, s.id);
        memory.store<uint32_t>(k + 0x150, s.worker);
        memory.store<uint32_t>(k + 0x154, k + 0x154);
        memory.store<uint32_t>(k + 0x158, k + 0x154);
        memory.store<uint32_t>(k + 0x16C, r.flags);
        memory.store<uint32_t>(k + 0x17C, 1);
    }
};
GuestThreads::GuestThreads(GuestMemory& memory, TlsTemplate tls, uint32_t stack, TargetValidator validator, EntryFactory factory)
    : impl_(std::make_unique<Impl>(memory, tls, stack, std::move(validator), std::move(factory))) {}
GuestThreads::~GuestThreads() { shutdown(); }
GuestThreads::ResumeResult GuestThreads::resume(uint32_t handle, uint32_t previous_output) {
    // A detached guest resumes and suspends without the global permit: the
    // registry's shared lock keeps create and close out, and each record's
    // suspension_mutex orders its counts.
    std::shared_lock registry(impl_->registry_mutex);
    auto* record = impl_->find_handle(handle);
    if (!record) return {0xC0000008, 0, 0};
    auto& memory = impl_->memory;
    const auto count_address = record->state.thread_object + 0xBC;
    if (previous_output & 3) throw RuntimeStop("thread-output", previous_output, "resume output must be aligned");
    if (previous_output) {
        memory.check_write(previous_output, 4);
        if (previous_output <= count_address && uint64_t(previous_output) + 4 > count_address)
            throw RuntimeStop("thread-output", previous_output, "resume output overlaps suspend count");
    }
    memory.check_write(count_address, 1);
    std::unique_lock suspension_lock(record->suspension_mutex);
    if (record->guest_suspends) {
        const auto previous = memory.load<uint8_t>(count_address);
        if (previous != record->guest_suspends + (record->native->suspended() ? 1u : 0u))
            throw RuntimeStop("thread-resume", handle, "guest suspend count differs from recorded suspensions");
        memory.store<uint8_t>(count_address, uint8_t(previous - 1));
        if (previous_output) memory.store<uint32_t>(previous_output, previous);
        // Publish the guest-visible outputs before either the notified or the
        // polling waiter can observe the final decrement.
        const bool released = record->guest_suspends.fetch_sub(1, std::memory_order_release) == 1;
        suspension_lock.unlock();
        if (released) record->suspension_changed.notify_all();
        return {0, previous, record->state.id};
    }
    const auto expected = record->native->suspended() ? 1u : 0u;
    if (memory.load<uint8_t>(count_address) != expected)
        throw RuntimeStop("thread-resume", handle, "guest suspend state differs from owned native state");
    const auto previous = record->native->resume();
    if (previous != expected)
        throw RuntimeStop("thread-resume", handle, "unexpected external native suspension change");
    memory.store<uint8_t>(count_address, 0);
    if (previous_output) memory.store<uint32_t>(previous_output, previous);
    return {0, previous, record->state.id, previous == 1};
}
GuestThreads::ResumeResult GuestThreads::suspend(uint32_t handle, uint32_t previous_output) {
    std::shared_lock registry(impl_->registry_mutex);
    return suspend_unlocked(handle, previous_output);
}
GuestThreads::ResumeResult GuestThreads::suspend_unlocked(uint32_t handle, uint32_t previous_output) {
    auto* record = impl_->find_handle(handle);
    if (!record) return {0xC0000008, 0, 0};
    auto& memory = impl_->memory;
    const auto count_address = record->state.thread_object + 0xBC;
    if (previous_output & 3) throw RuntimeStop("thread-output", previous_output, "suspend output must be aligned");
    if (previous_output) {
        memory.check_write(previous_output, 4);
        if (previous_output <= count_address && uint64_t(previous_output) + 4 > count_address)
            throw RuntimeStop("thread-output", previous_output, "suspend output overlaps suspend count");
    }
    memory.check_write(count_address, 1);
    std::lock_guard suspension_lock(record->suspension_mutex);
    const auto previous = memory.load<uint8_t>(count_address);
    if (previous != record->guest_suspends + (record->native->suspended() ? 1u : 0u))
        throw RuntimeStop("thread-suspend", handle, "guest suspend count differs from recorded suspensions");
    if (previous >= 0x7F) return {0xC000004A, previous, record->state.id};  // STATUS_SUSPEND_COUNT_EXCEEDED
    memory.store<uint8_t>(count_address, uint8_t(previous + 1));
    if (previous_output) memory.store<uint32_t>(previous_output, previous);
    record->guest_suspends.fetch_add(1, std::memory_order_release);
    return {0, previous, record->state.id};
}
uint32_t GuestThreads::handle_for_object(uint32_t object) const {
    std::shared_lock registry(impl_->registry_mutex);
    for (const auto& record : impl_->records)
        if (record->owns_storage && record->handle_open && record->state.thread_object == object) return record->state.handle;
    return 0;
}
bool GuestThreads::owns_object(uint32_t object) const {
    std::shared_lock registry(impl_->registry_mutex);
    for (const auto& record : impl_->records)
        if (record->owns_storage && record->state.thread_object == object) return true;
    return false;
}
void* GuestThreads::host_handle(uint32_t handle) const {
    std::shared_lock lock(impl_->registry_mutex);
    auto* record = impl_->find_handle(handle);
    return record ? record->native->native_handle() : nullptr;
}
void GuestThreads::with_host_handle(uint32_t handle, const std::function<void(void*)>& use) const {
    std::shared_lock lock(impl_->registry_mutex);
    auto* record = impl_->find_handle(handle);
    use(record ? record->native->native_handle() : nullptr);
}
uint32_t GuestThreads::guest_suspends(uint32_t handle) const {
    std::shared_lock registry(impl_->registry_mutex);
    auto* record = impl_->find_handle(handle);
    return record ? record->guest_suspends.load() : 0;
}
GuestThreads::ResumeResult GuestThreads::prepare_self_suspend(uint32_t handle) {
    std::shared_lock registry(impl_->registry_mutex);
    auto* record = impl_->find_handle(handle);
    if (!record) return {0xC0000008, 0, 0};
    if (record->prepared_self_suspend)
        throw RuntimeStop("thread-suspend", handle, "self suspension already prepared");
    const auto result = suspend_unlocked(handle, 0);
    if (!result.status) {
        record->prepared_previous = result.previous;
        record->prepared_self_suspend = true;
    }
    return result;
}
GuestThreads::ResumeResult GuestThreads::suspend_self(uint32_t handle, uint32_t output) {
    std::shared_lock registry(impl_->registry_mutex);
    auto* record = impl_->find_handle(handle);
    if (record && record->prepared_self_suspend) {
        const uint32_t count_address = record->state.thread_object + 0xBC;
        if (output & 3) throw RuntimeStop("thread-output", output, "suspend output must be aligned");
        if (output) {
            impl_->memory.check_write(output, 4);
            if (output <= count_address && uint64_t(output) + 4 > count_address)
                throw RuntimeStop("thread-output", output, "suspend output overlaps suspend count");
            impl_->memory.store<uint32_t>(output, record->prepared_previous);
        }
        record->prepared_self_suspend = false;
        return {0, record->prepared_previous, record->state.id};
    }
    return suspend_unlocked(handle, output);
}
std::function<void(std::stop_token)> GuestThreads::suspension_waiter(uint32_t handle) const {
    std::shared_lock registry(impl_->registry_mutex);
    auto* record = impl_->find_handle(handle);
    if (!record) throw RuntimeStop("thread-suspend", handle, "unknown suspension waiter");
    return [record, notify = impl_->suspend_notify](std::stop_token stop) {
        if (!notify) {
            while (!stop.stop_requested() && record->guest_suspends.load(std::memory_order_acquire))
                precise_sleep(std::chrono::milliseconds(1));
            return;
        }
        std::unique_lock lock(record->suspension_mutex);
        record->suspension_changed.wait(lock, stop, [record] {
            return record->guest_suspends.load(std::memory_order_acquire) == 0;
        });
        // The lock is destroyed before this callback returns to the caller
        // that reacquires the guest execution permit.
    };
}
void GuestThreads::shutdown() noexcept {
    for (const auto& record : impl_->records) record->native->request_stop();
    // Keep every record intact until all callbacks have stopped using the registry.
    for (const auto& record : impl_->records) record->native->cancel_and_join();
}
uint32_t GuestThreads::create(const Request& r) {
    std::unique_lock registry(impl_->registry_mutex);
    auto& i = *impl_;
    // Storage can be reused, but published identities remain in the dispatch
    // range for thread handles. Reject before retiring any existing storage.
    if (!is_handle_range(i.next_handle))
        throw RuntimeStop("thread-capacity", i.next_handle, "thread handle identities exhausted");
    const uint32_t affinity = r.flags >> 24;
    if ((r.flags & 0x00FFFFFFu) != 1 ||
        (affinity && ((affinity & ~0x3Fu) || !std::has_single_bit(affinity))))
        throw RuntimeStop("thread-flags", r.flags,
            "only suspended creation with inherited or single guest processor affinity is supported");
    check_outputs(i.memory, r);
    if (r.host_driven ? (r.startup || r.worker) :
        (!r.startup || !r.worker || (r.startup & 3) || (r.worker & 3) ||
         !i.validator(r.startup) || !i.validator(r.worker)))
        throw RuntimeStop("thread-entry", r.startup, "original startup and worker must be mapped targets");
    if (r.parent_cpu >= 6) throw RuntimeStop("thread-cpu", r.parent_cpu, "invalid parent processor");
    // The original startup puts a one-bit processor mask in creation_flags[31:24].
    const uint32_t cpu = affinity ? static_cast<uint32_t>(std::countr_zero(affinity)) : r.parent_cpu;
    const uint64_t requested = r.stack_size ? r.stack_size : i.default_stack;
    const uint64_t stack_size = std::max<uint64_t>(0x4000, (requested + 0xFFF) & ~uint64_t(0xFFF));
    if (stack_size > stack_max) throw RuntimeStop("thread-stack", requested, "guest stack exceeds supported slot");
    Impl::Record* retired = nullptr;
    for (const auto& previous : i.records) {
        if (previous->owns_storage && !previous->handle_open && previous->duplicates.empty() && !previous->references &&
            previous->state.stack_base - previous->state.stack_limit == stack_size &&
            previous->native->completed()) {
            retired = previous.get();
            break;
        }
    }
    if (!retired && i.next_slot == slot_count)
        throw RuntimeStop("thread-capacity", i.next_slot, "thread slots exhausted");
    const uint32_t slot = retired ? retired->state.pcr : slots_begin + i.next_slot * slot_stride;
    if (!retired && !i.memory.available(slot, slot_stride))
        throw RuntimeStop("thread-memory", slot, "thread slot already reserved");
    auto record = std::make_unique<Impl::Record>();
    record->state = {i.next_handle, i.next_id, slot, slot + 0x1000, slot + 0x2000,
        slot + 0x2000 + i.tls.data_size, slot + 0x21000, slot + 0x21000 + static_cast<uint32_t>(stack_size),
        r.startup, r.worker, r.argument, retired != nullptr};
    if (retired) {
        // Entry completion precedes the final host exit. Join before touching its
        // guest stack/PCR, but retain the native handle and record for old waiters.
        retired->native->join();
        retired->owns_storage = false;
    } else {
        i.memory.reserve(slot, slot_stride);
        ++i.next_slot;
    }
    // Failed creations retire their slot, including any partly reset reused slot.
    i.initialize(record->state, r, cpu, retired != nullptr);
    record->native = std::make_unique<NativeThread>(i.factory(record->state));
    place_guest_worker(*record->native, cpu);
    i.records.push_back(std::move(record)); // Complete allocation before publishing outputs.
    try {
        check_outputs(i.memory, r);
        i.memory.store<uint32_t>(r.handle_output, i.next_handle);
        if (r.id_output) i.memory.store<uint32_t>(r.id_output, i.next_id);
    } catch (...) {
        i.records.pop_back(); // Cancels before entry, joins and closes the native thread.
        throw;
    }
    i.next_handle += 4;
    ++i.next_id;
    return 0;
}
GuestThreads::Snapshot GuestThreads::snapshot(uint32_t handle) const {
    for (const auto& record : impl_->records)
        if (record->state.handle == handle ||
            std::find(record->duplicates.begin(), record->duplicates.end(), handle) != record->duplicates.end())
            return {record->state, record->native->native_id(), record->native->suspended(), record->native->entry_started()};
    throw RuntimeStop("thread-handle", handle, "unknown native thread handle");
}
size_t GuestThreads::size() const { return impl_->records.size(); }
uint32_t GuestThreads::reference(uint32_t handle, uint32_t type, uint32_t output) {
    auto* record = impl_->find_handle(handle);
    if (!record) return 0xC0000008;
    if (type && type != object_type) return 0xC0000024;
    if (output & 3) throw RuntimeStop("thread-output", output, "object output must be aligned");
    if (output) impl_->memory.check_write(output, 4);
    if (record->references == std::numeric_limits<uint32_t>::max())
        throw RuntimeStop("thread-reference", handle, "object reference count overflow");
    if (output) impl_->memory.store<uint32_t>(output, record->state.thread_object);
    ++record->references;
    return 0;
}
void GuestThreads::dereference(uint32_t object) {
    auto& record = impl_->find_object(object);
    if (!record.references) throw RuntimeStop("thread-reference", object, "unbalanced object dereference");
    --record.references;
}
uint32_t GuestThreads::references(uint32_t object) const { return impl_->find_object(object).references; }
uint32_t GuestThreads::duplicate(uint32_t handle, uint32_t& duplicate) {
    std::unique_lock registry(impl_->registry_mutex);
    auto* record = impl_->find_handle(handle);
    if (!record) return 0xC0000008;
    if (!is_handle_range(impl_->next_handle))
        throw RuntimeStop("thread-capacity", impl_->next_handle, "thread handle identities exhausted");
    duplicate = impl_->next_handle;
    impl_->next_handle += 4;
    record->duplicates.push_back(duplicate);
    return 0;
}
uint32_t GuestThreads::close(uint32_t handle) {
    std::unique_lock registry(impl_->registry_mutex);
    auto* record = impl_->find_handle(handle);
    if (!record) return 0xC0000008;
    if (const auto duplicate = std::find(record->duplicates.begin(), record->duplicates.end(), handle);
        duplicate != record->duplicates.end()) {
        record->duplicates.erase(duplicate);
        return 0;
    }
    record->handle_open = false;
    // Live execution owns itself independently of handles and explicit references.
    // Parked-only execution is retained until this diagnostic session shuts down.
    return 0;
}
int32_t GuestThreads::set_priority(uint32_t object, int32_t increment) {
    // An increment of 16 saturates the thread at the top of its range (the
    // NT kernel's rule, |increment| >= 16). The title asks it for its
    // time-critical threads (guests 10, 17, 28, 30 in a race) and gives every
    // other thread -2..2. This experiment raises those threads one host level;
    // its scheduling benefit needs device measurements. Pinned Xenia Canary
    // instead leaves 16 at normal, above this title's 0..2 at lowest, and
    // raises 17 to above-normal. Negative increments retain
    // the existing mapping. Opt in with SFR_GUEST_SATURATED_PRIORITY=1 until
    // device measurements establish a benefit.
    static const bool saturated_above = [] {
        const char* setting = std::getenv("SFR_GUEST_SATURATED_PRIORITY");
        return setting && *setting == '1';
    }();
    const int32_t host_priority = increment > 34 ? 2 : increment > 17 ? 1 :
                                  saturated_above && increment >= 16 ? 1 :
                                  increment < -34 ? -2 : increment < -17 ? -1 : 0;
    return impl_->find_object(object).native->set_priority(host_priority);
}
int32_t GuestThreads::priority(uint32_t object) const { return impl_->find_object(object).native->priority(); }
uint32_t GuestThreads::set_affinity(uint32_t object, uint32_t mask, uint32_t previous_output) {
    auto& record = impl_->find_object(object);
    if (!mask) return 0xC000000D;
    if ((mask & ~0x3Fu) || !std::has_single_bit(mask))
        throw RuntimeStop("thread-affinity", mask, "only single guest processor affinity is supported");
    if (previous_output & 3) throw RuntimeStop("thread-output", previous_output, "affinity output must be aligned");
    auto& memory = impl_->memory;
    const uint32_t pcr_cpu = record.state.pcr + 0x10C, object_cpu = object + 0xBF;
    if (previous_output) {
        memory.check_write(previous_output, 4);
        for (const auto field : {pcr_cpu, object_cpu})
            if (previous_output <= field && uint64_t(previous_output) + 4 > field)
                throw RuntimeStop("thread-output", previous_output, "affinity output overlaps processor state");
    }
    memory.check_write(pcr_cpu, 1);
    memory.check_write(object_cpu, 1);
    const auto previous_cpu = memory.load<uint8_t>(pcr_cpu);
    if (previous_cpu >= 6 || memory.load<uint8_t>(object_cpu) != previous_cpu)
        throw RuntimeStop("thread-affinity", object, "guest processor state is inconsistent");
    const auto cpu = static_cast<uint8_t>(std::countr_zero(mask));
    place_guest_worker(*record.native, cpu);
    memory.store<uint8_t>(pcr_cpu, cpu);
    memory.store<uint8_t>(object_cpu, cpu);
    if (previous_output) memory.store<uint32_t>(previous_output, uint32_t(1) << previous_cpu);
    return 0;
}
uint64_t GuestThreads::host_affinity(uint32_t object) const { return impl_->find_object(object).native->affinity_mask(); }
}
