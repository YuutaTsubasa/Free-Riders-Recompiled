#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sfr {
template<class Function> class GuestFunctionTable {
    uint32_t base_;
    uint64_t bytes_;
    std::vector<Function*> slots_;
    size_t count_ = 0;
public:
    GuestFunctionTable() : GuestFunctionTable(0, 0) {}
    // Built from verified mappings before guest workers start; readers only
    // index the immutable slots. No guest address is dereferenced as host code.
    GuestFunctionTable(uint32_t base, uint64_t bytes) : base_(base), bytes_(bytes) {
        if ((base & 3) || (bytes & 3) || bytes > (uint64_t{1} << 32) - base)
            throw std::invalid_argument("invalid guest function table range");
        slots_.resize(size_t(bytes / 4), nullptr);
    }
    bool insert(uint32_t address, Function* function) {
        const uint32_t offset = address - base_;
        if (!function || (address & 3) || uint64_t(offset) >= bytes_)
            throw std::invalid_argument("invalid guest function mapping");
        auto& slot = slots_[offset / 4];
        if (slot) return false; // Preserve unordered_map::emplace semantics.
        slot = function;
        ++count_;
        return true;
    }
    Function* find(uint32_t address) const {
        const uint32_t offset = address - base_;
        if ((address & 3) || uint64_t(offset) >= bytes_) return nullptr;
        return slots_[offset / 4];
    }
    bool contains(uint32_t address) const { return find(address) != nullptr; }
    size_t size() const { return count_; }
};
}
