#include "memory_statistics.h"

namespace sfr {
MemoryStatistics::MemoryStatistics(GuestMemory& memory, const VirtualMemory& allocations,
                                 const PhysicalMemory& physical,
                                 MemoryRange image, MemoryRange stack, MemoryRange tls)
    : memory_(memory), allocations_(allocations), physical_(physical), image_(image), stack_(stack), tls_(tls) {
    const std::array ranges{image_, stack_, tls_};
    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto range = ranges[i];
        const auto usage = memory_.usage(range.address, range.size);
        if (usage.committed_bytes != range.size || VirtualMemory::overlaps_arena(range.address, range.size) ||
            PhysicalMemory::overlaps_arena(range.address, range.size))
            throw RuntimeStop("memory-statistics", range.address, "category must be fully backed outside allocation arenas");
        for (size_t j = 0; j < i; ++j)
            if (range.address < ranges[j].address + ranges[j].size && ranges[j].address < range.address + range.size)
                throw RuntimeStop("memory-statistics", range.address, "memory categories overlap");
    }
}

std::array<uint32_t, 26> MemoryStatistics::snapshot() const {
    const auto total = memory_.usage();
    const auto arena = allocations_.statistics();
    const auto physical = physical_.statistics().committed_bytes;
    const auto image = memory_.usage(image_.address, image_.size).committed_bytes;
    const auto stack = memory_.usage(stack_.address, stack_.size).committed_bytes;
    const auto tls = memory_.usage(tls_.address, tls_.size).committed_bytes;
    const auto title = image + stack + tls + arena.committed_bytes + physical;
    if (title > total.committed_bytes || total.committed_bytes > memory_.backing_budget())
        throw RuntimeStop("memory-statistics", 0, "inconsistent native backing accounting");
    std::array<uint32_t, 26> words{};
    words[0] = structure_size;
    words[1] = static_cast<uint32_t>(memory_.backing_budget() / 4096);
    words[2] = static_cast<uint32_t>((total.committed_bytes - title) / 4096);
    words[3] = static_cast<uint32_t>((memory_.backing_budget() - total.committed_bytes) / 4096);
    words[4] = static_cast<uint32_t>(arena.capacity_bytes);
    words[5] = static_cast<uint32_t>(arena.reserved_bytes);
    words[6] = static_cast<uint32_t>(physical / 4096);
    words[8] = static_cast<uint32_t>(stack / 4096);
    words[9] = static_cast<uint32_t>(image / 4096);
    words[11] = static_cast<uint32_t>((arena.committed_bytes + tls) / 4096);
    words[25] = words[1] - 1;
    // Other categories have no separate native allocator/process in this runtime.
    return words;
}

uint32_t MemoryStatistics::query(uint32_t output) const {
    if (!output) return 0xc000000d;
    if (memory_.load<uint32_t>(output) != structure_size) return 0xc0000023;
    memory_.check_write(output, structure_size);
    const auto words = snapshot();
    for (size_t i = 0; i < words.size(); ++i)
        memory_.store<uint32_t>(uint64_t(output) + i * 4, words[i]);
    return 0;
}
}
