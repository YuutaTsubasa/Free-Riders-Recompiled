#include "guest_threads.h"
#include "guest_memory.h"
#include "system_time.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <utility>
#include <vector>
namespace sfr {
namespace {
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
        uint32_t guest_suspends = 0;
        bool handle_open = true;
    };
    GuestMemory& memory;
    TlsTemplate tls;
    uint32_t default_stack;
    TargetValidator validator;
    EntryFactory factory;
    std::vector<std::unique_ptr<Record>> records;
    uint32_t next_slot = 0, next_handle = 0x72200004, next_id = 2;
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
            if (record->handle_open && record->state.handle == handle) return record.get();
        return nullptr;
    }
    Record& find_object(uint32_t object) const {
        for (const auto& record : records)
            if (record->state.thread_object == object) return *record;
        throw RuntimeStop("thread-object", object, "unknown thread object");
    }
    void initialize(const State& s, const Request& r, uint32_t cpu) {
        memory.commit(s.pcr, 0x2D8);
        memory.commit(s.thread_object, 0xAB0);
        memory.commit(s.tls_static, tls.data_size + tls.slots * 4);
        memory.commit(s.stack_limit, s.stack_base - s.stack_limit);
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
    if (record->guest_suspends) {
        const auto previous = memory.load<uint8_t>(count_address);
        if (previous != record->guest_suspends + (record->native->suspended() ? 1u : 0u))
            throw RuntimeStop("thread-resume", handle, "guest suspend count differs from recorded suspensions");
        --record->guest_suspends;
        memory.store<uint8_t>(count_address, uint8_t(previous - 1));
        if (previous_output) memory.store<uint32_t>(previous_output, previous);
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
    const auto previous = memory.load<uint8_t>(count_address);
    if (previous != record->guest_suspends + (record->native->suspended() ? 1u : 0u))
        throw RuntimeStop("thread-suspend", handle, "guest suspend count differs from recorded suspensions");
    if (previous >= 0x7F) return {0xC000004A, previous, record->state.id};  // STATUS_SUSPEND_COUNT_EXCEEDED
    ++record->guest_suspends;
    memory.store<uint8_t>(count_address, uint8_t(previous + 1));
    if (previous_output) memory.store<uint32_t>(previous_output, previous);
    return {0, previous, record->state.id};
}
uint32_t GuestThreads::handle_for_object(uint32_t object) const {
    for (const auto& record : impl_->records)
        if (record->handle_open && record->state.thread_object == object) return record->state.handle;
    return 0;
}
bool GuestThreads::owns_object(uint32_t object) const {
    for (const auto& record : impl_->records)
        if (record->state.thread_object == object) return true;
    return false;
}
void* GuestThreads::host_handle(uint32_t handle) const {
    auto* record = impl_->find_handle(handle);
    return record ? record->native->native_handle() : nullptr;
}
uint32_t GuestThreads::guest_suspends(uint32_t handle) const {
    auto* record = impl_->find_handle(handle);
    return record ? record->guest_suspends : 0;
}
void GuestThreads::shutdown() noexcept {
    for (const auto& record : impl_->records) record->native->request_stop();
    // Keep every record intact until all callbacks have stopped using the registry.
    for (const auto& record : impl_->records) record->native->cancel_and_join();
}
uint32_t GuestThreads::create(const Request& r) {
    auto& i = *impl_;
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
    if (i.next_slot == slot_count) throw RuntimeStop("thread-capacity", i.next_slot, "thread slots exhausted");
    const uint32_t slot = slots_begin + i.next_slot * slot_stride;
    if (!i.memory.available(slot, slot_stride)) throw RuntimeStop("thread-memory", slot, "thread slot already reserved");
    auto record = std::make_unique<Impl::Record>();
    record->state = {i.next_handle, i.next_id, slot, slot + 0x1000, slot + 0x2000,
        slot + 0x2000 + i.tls.data_size, slot + 0x21000, slot + 0x21000 + static_cast<uint32_t>(stack_size),
        r.startup, r.worker, r.argument};
    i.memory.reserve(slot, slot_stride);
    // GuestMemory has no release operation: failed creations retire their session slot.
    ++i.next_slot;
    i.initialize(record->state, r, cpu);
    record->native = std::make_unique<NativeThread>(i.factory(record->state));
    record->native->set_guest_processor(cpu);
    i.records.push_back(std::move(record)); // Capacity was reserved before any outputs.
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
        if (record->state.handle == handle)
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
uint32_t GuestThreads::close(uint32_t handle) {
    auto* record = impl_->find_handle(handle);
    if (!record) return 0xC0000008;
    record->handle_open = false;
    // Live execution owns itself independently of handles and explicit references.
    // Parked-only execution is retained until this diagnostic session shuts down.
    return 0;
}
int32_t GuestThreads::set_priority(uint32_t object, int32_t increment) {
    const int32_t host_priority = increment > 34 ? 2 : increment > 17 ? 1 :
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
    record.native->set_guest_processor(cpu);
    memory.store<uint8_t>(pcr_cpu, cpu);
    memory.store<uint8_t>(object_cpu, cpu);
    if (previous_output) memory.store<uint32_t>(previous_output, uint32_t(1) << previous_cpu);
    return 0;
}
uint64_t GuestThreads::host_affinity(uint32_t object) const { return impl_->find_object(object).native->affinity_mask(); }
}
