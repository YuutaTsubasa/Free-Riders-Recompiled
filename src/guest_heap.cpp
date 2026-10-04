#include "guest_heap.h"
#include "guest_memory.h"
#include "o1heap/o1heap.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <span>
#include <unordered_map>

namespace sfr {
GuestHeap* active_heap = nullptr;

namespace {
struct Arena { uint32_t begin, end; };
constexpr std::array<Arena, 2> arenas{{{0x20400000u, 0x40000000u}, {0x50000000u, 0x70000000u}}};
}

struct GuestHeap::Impl {
    GuestMemory& memory;
    std::array<O1HeapInstance*, arenas.size()> heaps{};
    struct Block { uint32_t heap, size; };
    std::unordered_map<uint32_t, Block> blocks;
    mutable std::mutex mutex;
    uint64_t live_bytes = 0, peak_bytes = 0;

    explicit Impl(GuestMemory& memory) : memory(memory) {}
    uint32_t guest(const void* host) const { return uint32_t(static_cast<const uint8_t*>(host) - memory.base()); }
    // Under the mutex.
    uint32_t take(uint32_t size) {
        for (auto* heap : heaps)
            if (void* block = o1heapAllocate(heap, std::max<uint32_t>(size, 1))) return guest(block);
        return 0;
    }
    void give(uint32_t address) {
        void* const host = memory.base() + address;
        for (size_t i = 0; i < arenas.size(); ++i)
            if (address >= arenas[i].begin && address < arenas[i].end) o1heapFree(heaps[i], host);
    }
    // Writes through GuestMemory, so watched pages see them.
    void clear(uint32_t address, uint32_t size) {
        static const std::array<uint8_t, 0x10000> zeros{};
        for (uint32_t done = 0; done < size;) {
            const uint32_t chunk = std::min<uint32_t>(size - done, zeros.size());
            memory.write_bytes(address + done, std::span<const uint8_t>(zeros.data(), chunk));
            done += chunk;
        }
    }
};

bool GuestHeap::in_arena(uint32_t address) {
    return std::any_of(arenas.begin(), arenas.end(), [&](const Arena& a) { return address >= a.begin && address < a.end; });
}

bool GuestHeap::enabled() {
    static const bool on = [] { const char* text = std::getenv("SFR_HOST_HEAP"); return text && *text == '1'; }();
    return on;
}

GuestHeap::GuestHeap(GuestMemory& memory) : impl_(std::make_unique<Impl>(memory)) {
    for (size_t i = 0; i < arenas.size(); ++i) {
        const uint32_t size = arenas[i].end - arenas[i].begin;
        memory.reserve(arenas[i].begin, size);
        memory.commit(arenas[i].begin, size);
        impl_->heaps[i] = o1heapInit(memory.base() + arenas[i].begin, size);
        if (!impl_->heaps[i]) throw RuntimeStop("guest-heap", arenas[i].begin, "o1heap arena initialization failed");
    }
    std::cerr << "GUEST_HEAP arenas=" << arenas.size() << " bytes=" << (arenas[0].end - arenas[0].begin) +
                     (arenas[1].end - arenas[1].begin) << '\n';
}

GuestHeap::~GuestHeap() = default;

uint32_t GuestHeap::allocate(uint32_t heap, uint32_t size, bool zero) {
    uint32_t address;
    {
        std::lock_guard lock(impl_->mutex);
        address = impl_->take(size);
        if (!address) {
            std::cerr << "GUEST_HEAP_EXHAUSTED heap=0x" << std::hex << heap << std::dec << " size=" << size
                      << " live=" << impl_->live_bytes << '\n';
            return 0;
        }
        impl_->blocks[address] = {heap, size};
        impl_->live_bytes += size;
        impl_->peak_bytes = std::max(impl_->peak_bytes, impl_->live_bytes);
    }
    if (zero) impl_->clear(address, size);
    return address;
}

bool GuestHeap::free(uint32_t address) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->blocks.find(address);
    if (found == impl_->blocks.end()) return false;
    impl_->live_bytes -= found->second.size;
    impl_->blocks.erase(found);
    impl_->give(address);
    return true;
}

uint32_t GuestHeap::size(uint32_t address) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->blocks.find(address);
    return found == impl_->blocks.end() ? ~0u : found->second.size;
}

bool GuestHeap::owns(uint32_t address) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->blocks.contains(address);
}

uint32_t GuestHeap::reallocate(uint32_t heap, uint32_t address, uint32_t size, bool zero, bool in_place_only) {
    const uint32_t old_size = this->size(address);
    if (old_size == ~0u) return 0;
    if (in_place_only) {
        // A block can shrink where it is; o1heap cannot grow one.
        if (size > old_size) return 0;
        std::lock_guard lock(impl_->mutex);
        auto& block = impl_->blocks.at(address);
        impl_->live_bytes -= block.size - size;
        block.size = size;
        return address;
    }
    const uint32_t moved = allocate(heap, size, false);
    if (!moved) return 0;
    const uint32_t kept = std::min(old_size, size);
    impl_->memory.write_bytes(moved, std::span<const uint8_t>(impl_->memory.base() + address, kept));
    if (zero && size > kept) impl_->clear(moved + kept, size - kept);
    free(address);
    return moved;
}

void GuestHeap::destroy(uint32_t heap) {
    std::lock_guard lock(impl_->mutex);
    uint64_t freed = 0;
    for (auto it = impl_->blocks.begin(); it != impl_->blocks.end();) {
        if (it->second.heap != heap) { ++it; continue; }
        freed += it->second.size;
        impl_->live_bytes -= it->second.size;
        impl_->give(it->first);
        it = impl_->blocks.erase(it);
    }
    std::cerr << "GUEST_HEAP_DESTROY heap=0x" << std::hex << heap << std::dec << " freed=" << freed
              << " live=" << impl_->live_bytes << " peak=" << impl_->peak_bytes << '\n';
}
}
