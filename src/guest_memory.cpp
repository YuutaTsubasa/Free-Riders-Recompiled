#include "guest_memory.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstdlib>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <memoryapi.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace sfr {
struct GuestMemory::PendingState {
    GuestMemory* owner = nullptr;
    // Unexpired pending writes per 4 KiB page: a store to any other page
    // needs no registry scan even while I/O (e.g. movie streaming) is live.
    std::unique_ptr<std::atomic<uint16_t>[]> pinned =
        std::make_unique<std::atomic<uint16_t>[]>(address_space_size / fast_page_size);
    void pin(const std::vector<Range>& ranges, int delta) {
        for (const auto& r : ranges)
            for (uint64_t page = r.address / fast_page_size; page <= (r.address + r.size - 1) / fast_page_size; ++page)
                pinned[page].fetch_add(uint16_t(delta), std::memory_order_relaxed);
    }
    // Only admission mutates this vector, under exclusive guest execution.
    // Token destruction expires a weak reference without changing the registry.
    std::vector<std::weak_ptr<PendingWrite>> entries;
};
struct GuestMemory::PendingWrite {
    std::vector<Range> ranges;
    std::shared_ptr<PendingState> state;
    ~PendingWrite() {
        if (!state) return;
        state->pin(ranges, -1);
    }
};
GuestMemory::WriteLease GuestMemory::pin_writes(std::span<const Range> ranges) {
    if (ranges.empty()) throw RuntimeStop("memory-pending-write", 0, "pending output ranges are required");
    for (size_t i=0; i<ranges.size(); ++i) {
        const auto& r=ranges[i];
        check_write(r.address,r.size);
        for (size_t j=0; j<i; ++j) {
            const auto& previous=ranges[j];
            if (r.address<previous.address+previous.size && previous.address<r.address+r.size)
                throw RuntimeStop("memory-pending-write", r.address, "pending output ranges overlap");
        }
    }
    auto entry=std::make_shared<PendingWrite>();
    entry->ranges.assign(ranges.begin(),ranges.end());
    pending_state_->pin(entry->ranges, 1);
    entry->state=pending_state_;
    auto& registry=pending_state_->entries;
    std::unique_lock layout(layout_mutex_);
    std::erase_if(registry,[](const auto& item){return item.expired();});
    registry.emplace_back(entry); // Last throwing step; failed admission retains no ownership.
    return WriteLease(pending_state_,std::move(entry));
}
void GuestMemory::WriteLease::check() const {
    if (!state_ || !entry_ || !state_->owner)
        throw RuntimeStop("memory-pending-write", 0, "pending output lease is no longer live");
    const auto& memory=*state_->owner;
    if (memory.has_reservation())
        throw RuntimeStop("reservation-interference", 0, "pending publication during live reservation");
    for (const auto& r:entry_->ranges) memory.check_store_access(r.address,r.size,entry_.get());
}
void GuestMemory::WriteLease::write_scalar(uint64_t address, uint64_t value, uint64_t size) {
    if (!state_ || !entry_ || !state_->owner)
        throw RuntimeStop("memory-pending-write", address, "pending output lease is no longer live");
    const bool owned=std::any_of(entry_->ranges.begin(),entry_->ranges.end(),[&](const auto& r){
        return address>=r.address && address-r.address<=r.size && size<=r.size-(address-r.address);
    });
    if (!owned) throw RuntimeStop("memory-pending-write", address, "publication exceeds its owned output ranges");
    auto& memory=*state_->owner;
    memory.check_store_access(address,size,entry_.get());
    if (memory.has_reservation())
        throw RuntimeStop("reservation-interference", address, "pending publication during live reservation");
    for (uint64_t i=0; i<size; ++i)
        memory.base_[address+i]=static_cast<uint8_t>(value>>((size-1-i)*8));
    memory.complete_store(address,size);
}
void GuestMemory::WriteLease::write_bytes(uint64_t address, std::span<const uint8_t> bytes) {
    if (bytes.empty()) return;
    if (!state_ || !entry_ || !state_->owner)
        throw RuntimeStop("memory-pending-write", address, "pending output lease is no longer live");
    const uint64_t size = bytes.size();
    const bool owned=std::any_of(entry_->ranges.begin(),entry_->ranges.end(),[&](const auto& r){
        return address>=r.address && address-r.address<=r.size && size<=r.size-(address-r.address);
    });
    if (!owned) throw RuntimeStop("memory-pending-write", address, "publication exceeds its owned output ranges");
    auto& memory=*state_->owner;
    memory.check_store_access(address,size,entry_.get());
    if (memory.has_reservation())
        throw RuntimeStop("reservation-interference", address, "pending publication during live reservation");
    std::memcpy(memory.base_+address,bytes.data(),bytes.size());
    memory.complete_store(address,size);
}
void GuestMemory::write_bytes(uint64_t address, std::span<const uint8_t> bytes) {
    if (bytes.empty()) return;
    check_write(address, bytes.size());
    std::memcpy(base_ + address, bytes.data(), bytes.size());
    complete_store(address, bytes.size());
}
void GuestMemory::check_pending_writes(uint64_t address, uint64_t size, const PendingWrite* owner) const {
    // Only a page under pending output can overlap one (pins precede admission).
    bool pinned=false;
    for (uint64_t page=address/fast_page_size; page<=(address+size-1)/fast_page_size && page<address_space_size/fast_page_size; ++page)
        pinned|=pending_state_->pinned[page].load(std::memory_order_relaxed)!=0;
    if (!pinned) return;
    const auto layout=read_layout();
    for (const auto& weak:pending_state_->entries) {
        const auto entry=weak.lock();
        if (!entry || entry.get()==owner) continue;
        for (const auto& r:entry->ranges)
            if (address<r.address+r.size && r.address<address+size)
                throw RuntimeStop("memory-pending-write", address, "operation overlaps pending I/O output");
    }
}
#ifdef _WIN32
namespace {
using VirtualAlloc2Function = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);

VirtualAlloc2Function get_virtual_alloc2() {
    static const auto function = [] {
        const auto module = GetModuleHandleW(L"KernelBase.dll");
        return module ? reinterpret_cast<VirtualAlloc2Function>(GetProcAddress(module, "VirtualAlloc2"))
                      : nullptr;
    }();
    return function;
}

PVOID virtual_alloc2(PVOID address, SIZE_T size, ULONG allocation_type, ULONG protection) {
    const auto function = get_virtual_alloc2();
    return function ? function(GetCurrentProcess(), address, size, allocation_type,
                               protection, nullptr, 0) : nullptr;
}

bool virtual_alloc2_available() {
    return get_virtual_alloc2() != nullptr;
}
}
#endif

RuntimeStop::RuntimeStop(std::string category, uint64_t address, std::string detail)
    : std::runtime_error(category + ": " + detail), category(std::move(category)),
      address(address), detail(std::move(detail)) {}

GuestMemory::GuestMemory(uint64_t backing_budget)
    : fast_pages_(std::make_unique<uint8_t[]>(address_space_size / fast_page_size)),
      watched_pages_(std::make_unique<uint8_t[]>(address_space_size / fast_page_size)),
      write_combined_pages_(std::make_unique<uint8_t[]>(address_space_size / fast_page_size)),
      backing_budget_(backing_budget), pending_state_(std::make_shared<PendingState>()) {
    pending_state_->owner=this;
    pinned_pages_=pending_state_->pinned.get();
    static_assert(sizeof(void*) == 8, "The diagnostic requires a 64-bit host");
#ifdef _WIN32
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    page_size_ = info.dwPageSize;
#else
    page_size_ = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#endif
    if (page_size_ < 4096 || page_size_ % 4096 || !backing_budget_ ||
        backing_budget_ > address_space_size || backing_budget_ % page_size_)
        throw RuntimeStop("memory-budget", 0, "invalid host page size or guest backing budget");
#ifdef _WIN32
    if (!virtual_alloc2_available())
        throw RuntimeStop("memory", 0, "VirtualAlloc2 is unavailable on this Windows host");
    base_ = static_cast<uint8_t*>(virtual_alloc2(nullptr, address_space_size,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS));
#else
    auto* result = mmap(nullptr, address_space_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result != MAP_FAILED) base_ = static_cast<uint8_t*>(result);
#endif
    if (!base_) throw RuntimeStop("memory", 0, "cannot reserve 4 GiB virtual guest address space");
}

GuestMemory::~GuestMemory() {
    pending_state_->owner=nullptr;
#ifdef _WIN32
    if (base_) {
        auto* current = base_;
        auto* const end = base_ + address_space_size;
        while (current < end) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(current, &info, sizeof(info)) || !info.RegionSize) break;
            auto* const region_base = static_cast<uint8_t*>(info.BaseAddress);
            auto* const allocation_base = static_cast<uint8_t*>(info.AllocationBase);
            if (region_base != current || info.RegionSize > static_cast<size_t>(end - current) ||
                (info.State != MEM_FREE &&
                 (!allocation_base || allocation_base < base_ || allocation_base >= end)))
                break;
            auto* const next = current + info.RegionSize;
            if (info.State != MEM_FREE)
                VirtualFree(allocation_base, 0, MEM_RELEASE);
            current = next;
        }
    }
#else
    if (base_) munmap(base_, address_space_size);
#endif
}

void GuestMemory::map(uint64_t address, uint64_t size) {
    reserve(address, size);
    commit(address, size);
}

namespace {
// See GuestMemory::set_host_write_combining.
std::atomic<bool> host_write_combining_enabled{[] {
    const char* const text = std::getenv("SFR_WRITE_COMBINED");
    return text && *text != '0';
}()};
}

void GuestMemory::set_host_write_combining(bool enabled) noexcept {
    host_write_combining_enabled.store(enabled, std::memory_order_relaxed);
}

bool GuestMemory::host_write_combining() noexcept {
    return host_write_combining_enabled.load(std::memory_order_relaxed);
}

// Elsewhere than Windows x86-64 the pages are ordinary cached memory, as
// they are on Windows unless SFR_WRITE_COMBINED asks for the console's
// mapping; the guest-side bookkeeping is the same.
void GuestMemory::map_write_combined(uint64_t address, uint64_t size) {
    reserve(address, size);
    commit_impl(address, size, true);
}

// Reservations are kept sorted by address and never overlap (in whole
// pages), so the first that reaches past an address is found by binary
// search: allocating memory probes these thousands of times, and a scan
// over the title's thousands of reservations was a fifth of a race load.
const GuestMemory::Region* GuestMemory::first_conflict(uint64_t address, uint64_t end) const {
    const auto rounded_end = [&](const Region& r) { return r.address + (r.size + page_size_ - 1) / page_size_ * page_size_; };
    auto next = std::upper_bound(reservations_.begin(), reservations_.end(), address,
                                 [](uint64_t value, const Region& r) { return value < r.address; });
    if (next != reservations_.begin() && rounded_end(*std::prev(next)) > address) return &*std::prev(next);
    return next != reservations_.end() && next->address < end ? &*next : nullptr;
}

bool GuestMemory::available(uint64_t address, uint64_t size) const {
    if (!size || address >= address_space_size || size > address_space_size - address || address % page_size_)
        return false;
    const auto end = address + (size + page_size_ - 1) / page_size_ * page_size_;
    return !first_conflict(address, end);
}

uint64_t GuestMemory::lowest_conflict(uint64_t address, uint64_t size) const {
    const auto end = address + (size + page_size_ - 1) / page_size_ * page_size_;
    const Region* const conflict = first_conflict(address, end);
    return conflict ? conflict->address : end;
}

void GuestMemory::reserve(uint64_t address, uint64_t size) {
    if (!available(address, size))
        throw RuntimeStop("memory-map", address, "invalid or overlapping guest reservation");
    // The entire guest address space already has inaccessible host backing.
    std::unique_lock layout(layout_mutex_);
    reservations_.insert(std::upper_bound(reservations_.begin(), reservations_.end(), address,
                                          [](uint64_t value, const Region& r) { return value < r.address; }),
                         {address, size});
}

void GuestMemory::commit(uint64_t address, uint64_t size) {
    commit_impl(address, size, false);
}

void GuestMemory::decommit(uint64_t address, uint64_t size) {
    if (!size || address >= address_space_size || size > address_space_size - address ||
        address % page_size_ || size % page_size_)
        throw RuntimeStop("memory-decommit", address, "invalid guest decommit range/alignment");
    const bool reserved = std::any_of(reservations_.begin(), reservations_.end(), [&](const Region& r) {
        return address >= r.address && address - r.address <= r.size && size <= r.size - (address - r.address);
    });
    if (!reserved)
        throw RuntimeStop("memory-decommit", address, "decommit range is not inside one guest reservation");
    if (has_reservation())
        throw RuntimeStop("reservation-interference", address, "decommit during live reservation");
    check_pending_writes(address,size);
    const uint64_t end = address + size;
    for (const auto& variable : variables_)
        if (address < uint64_t(variable.address) + 4 && variable.address < end)
            throw RuntimeStop("import-variable", variable.address, variable.name);
    for (const auto& word : read_only_words_)
        if (address < uint64_t(word.address) + 4 && word.address < end)
            throw RuntimeStop("memory-readonly", word.address, "decommit overlaps computed read-only word");
    if (intersects_write_combined(address, size))
        throw RuntimeStop("memory-cache", address, "decommit does not support write-combined memory");

    auto next = std::vector<Region>{};
    next.reserve(committed_.size() + 1);
    for (const auto& r : committed_) {
        const uint64_t region_end = r.address + r.size;
        if (r.address >= end || region_end <= address) {
            next.push_back(r);
            continue;
        }
        if (r.address < address) next.push_back({r.address, address - r.address});
        if (region_end > end) next.push_back({end, region_end - end});
    }

#ifdef _WIN32
    struct HostOperation { uint64_t address, size; };
    std::vector<HostOperation> operations;
    for (const auto& backing : backing_) {
        const uint64_t overlap_begin = std::max(address, backing.address);
        const uint64_t overlap_end = std::min(end, backing.address + backing.size);
        uint64_t cursor = overlap_begin;
        while (cursor < overlap_end) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(base_ + cursor, &info, sizeof(info)) || !info.RegionSize)
                throw RuntimeStop("memory-decommit", address, "owned private backing query failed");
            const auto* const region_base = static_cast<uint8_t*>(info.BaseAddress);
            const auto* const allocation_base = static_cast<uint8_t*>(info.AllocationBase);
            const auto* const expected_allocation = base_ + backing.address;
            const auto* const region_end = region_base + info.RegionSize;
            if (region_base > base_ + cursor || region_end <= base_ + cursor ||
                region_end > base_ + backing.address + backing.size ||
                allocation_base != expected_allocation ||
                (info.State != MEM_COMMIT && info.State != MEM_RESERVE) ||
                (info.State == MEM_COMMIT && info.Type != MEM_PRIVATE))
                throw RuntimeStop("memory-decommit", address, "owned private backing is inconsistent");
            const uint64_t chunk_end = std::min<uint64_t>(overlap_end, region_end - base_);
            if (info.State == MEM_COMMIT) operations.push_back({cursor, chunk_end - cursor});
            cursor = chunk_end;
        }
    }
    size_t completed = 0;
    for (const auto& operation : operations) {
        if (!VirtualFree(base_ + operation.address, operation.size, MEM_DECOMMIT))
            throw RuntimeStop("memory-decommit", address,
                completed ? "partial native decommit failed after discarding earlier pages"
                          : "native decommit failed before discarding pages");
        ++completed;
    }
#else
    std::vector<Region> operations;
    for (const auto& backing : backing_) {
        const uint64_t overlap_begin = std::max(address, backing.address);
        const uint64_t overlap_end = std::min(end, backing.address + backing.size);
        for (uint64_t page = overlap_begin; page < overlap_end; page += page_size_) {
            const bool committed = std::any_of(committed_.begin(), committed_.end(), [&](const Region& r) {
                const uint64_t committed_end = r.address +
                    (r.size + page_size_ - 1) / page_size_ * page_size_;
                return page >= r.address && page < committed_end;
            });
            if (!committed) continue;
            if (!operations.empty() && operations.back().address + operations.back().size == page)
                operations.back().size += page_size_;
            else
                operations.push_back({page, page_size_});
        }
    }
    size_t completed = 0;
    for (const auto& operation : operations) {
        void* const result = mmap(base_ + operation.address, operation.size, PROT_NONE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (result != base_ + operation.address)
            throw RuntimeStop("memory-decommit", address,
                completed ? "partial native decommit failed after discarding earlier pages"
                          : "native decommit failed before discarding pages");
        ++completed;
    }
#endif
    std::unique_lock layout(layout_mutex_);
    committed_.swap(next);
    rebuild_fast_pages(address, size);
}

void GuestMemory::release(uint64_t address, uint64_t size) {
    if (!size || address >= address_space_size || size > address_space_size - address ||
        address % page_size_)
        throw RuntimeStop("memory-release", address, "invalid guest release range/alignment");
    const auto reservation = std::find_if(reservations_.begin(), reservations_.end(), [&](const Region& r) {
        return r.address == address && r.size == size;
    });
    if (reservation == reservations_.end())
        throw RuntimeStop("memory-release", address, "release must exactly match one guest reservation");
    if (has_reservation())
        throw RuntimeStop("reservation-interference", address, "release during live reservation");
    const uint64_t host_size = (size + page_size_ - 1) / page_size_ * page_size_;
    const uint64_t end = address + host_size;
    check_pending_writes(address,host_size);
    for (const auto& variable : variables_)
        if (address < uint64_t(variable.address) + 4 && variable.address < end)
            throw RuntimeStop("import-variable", variable.address, variable.name);
    for (const auto& word : read_only_words_)
        if (address < uint64_t(word.address) + 4 && word.address < end)
            throw RuntimeStop("memory-readonly", word.address, "release overlaps computed read-only word");
    if (intersects_write_combined(address, host_size))
        throw RuntimeStop("memory-cache", address, "release does not support write-combined memory");

    auto next_reservations = reservations_;
    next_reservations.erase(next_reservations.begin() + (reservation - reservations_.begin()));
    std::vector<Region> next_committed;
    next_committed.reserve(committed_.size() + 1);
    for (const auto& r : committed_) {
        const uint64_t region_end = r.address + r.size;
        if (r.address >= end || region_end <= address) {
            next_committed.push_back(r);
            continue;
        }
        if (r.address < address) next_committed.push_back({r.address, address - r.address});
        if (region_end > end) next_committed.push_back({end, region_end - end});
    }
    std::vector<Region> released_backing;
    released_backing.reserve(backing_.size());
    std::vector<Region> next_backing;
    next_backing.reserve(backing_.size());
    for (const auto& backing : backing_) {
        const bool intersects = address < backing.address + backing.size && backing.address < end;
        if (!intersects) {
            next_backing.push_back(backing);
            continue;
        }
        if (backing.address < address || backing.address + backing.size > end)
            throw RuntimeStop("memory-release", address,
                              "owned private backing crosses the release reservation boundary");
        released_backing.push_back(backing);
    }

#ifdef _WIN32
    for (const auto& backing : released_backing) {
        uint64_t cursor = backing.address;
        while (cursor < backing.address + backing.size) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(base_ + cursor, &info, sizeof(info)) || !info.RegionSize)
                throw RuntimeStop("memory-release", address, "owned private backing query failed");
            const auto* const region_base = static_cast<uint8_t*>(info.BaseAddress);
            const auto* const allocation_base = static_cast<uint8_t*>(info.AllocationBase);
            const auto* const region_end = region_base + info.RegionSize;
            if (region_base > base_ + cursor || region_end <= base_ + cursor ||
                region_end > base_ + backing.address + backing.size ||
                allocation_base != base_ + backing.address ||
                (info.State != MEM_COMMIT && info.State != MEM_RESERVE) ||
                (info.State == MEM_COMMIT && info.Type != MEM_PRIVATE))
                throw RuntimeStop("memory-release", address, "owned private backing is inconsistent");
            cursor = region_end - base_;
        }
    }
    size_t completed = 0;
    for (const auto& backing : released_backing) {
        if (!VirtualFree(base_ + backing.address, backing.size,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            const DWORD error = GetLastError();
            throw RuntimeStop("memory-release", address,
                (completed ? "partial native release failed after restoring earlier placeholders; error "
                           : "native release failed before restoring placeholders; error ") +
                std::to_string(error));
        }
        ++completed;
    }
#else
    void* const result = mmap(base_ + address, host_size, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (result != base_ + address)
        throw RuntimeStop("memory-release", address, "cannot restore released guest placeholder backing");
#endif
    std::unique_lock layout(layout_mutex_);
    reservations_.swap(next_reservations);
    committed_.swap(next_committed);
    backing_.swap(next_backing);
    rebuild_fast_pages(address, size);
}

void GuestMemory::commit_impl(uint64_t address, uint64_t size, bool write_combined) {
    if (!size || address >= address_space_size || size > address_space_size - address || address % page_size_)
        throw RuntimeStop("memory-map", address, "invalid guest commit range/alignment");
    const bool reserved = std::any_of(reservations_.begin(), reservations_.end(), [&](const Region& r) {
        return address >= r.address && address - r.address <= r.size && size <= r.size - (address - r.address);
    });
    if (!reserved) throw RuntimeStop("memory-map", address, "commit outside guest reservation");

    const uint64_t host_size = (size + page_size_ - 1) / page_size_ * page_size_;
    check_pending_writes(address,host_size);
    const auto host_intersects = [&](const Region& r) {
        return address < r.address + r.size && r.address < address + host_size;
    };
    if ((!write_combined && std::any_of(write_combined_.begin(), write_combined_.end(), host_intersects)) ||
        (write_combined && std::any_of(committed_.begin(), committed_.end(), [&](const Region& r) {
            const Region host_region{r.address, (r.size + page_size_ - 1) / page_size_ * page_size_};
            return host_intersects(host_region);
        })))
        throw RuntimeStop("memory-cache", address, "guest commit conflicts with existing host cache policy");

    // Prepare merged logical extents before changing host protection. Rounded
    // host tails remain inaccessible through check(). Recommit never clears data.
    auto next = committed_;
    next.push_back({address, size});
    std::sort(next.begin(), next.end(), [](const Region& a, const Region& b) { return a.address < b.address; });
    size_t count = 0;
    for (const auto& r : next) {
        if (count && r.address <= next[count - 1].address + next[count - 1].size) {
            auto& previous = next[count - 1];
            previous.size = std::max(previous.address + previous.size, r.address + r.size) - previous.address;
        } else {
            next[count++] = r;
        }
    }
    next.resize(count);
    uint64_t committed_bytes = 0;
    for (const auto& r : next)
        committed_bytes += (r.size + page_size_ - 1) / page_size_ * page_size_;
    if (committed_bytes > backing_budget_)
        throw RuntimeStop("memory-budget", address, "guest backing budget exceeded");
    auto next_write_combined = write_combined_;
    if (write_combined) next_write_combined.push_back({address, host_size});
    std::vector<Region> fresh;
    for (uint64_t page = address; page < address + host_size; page += page_size_) {
        const bool backed = std::any_of(backing_.begin(), backing_.end(), [&](const Region& r) {
            return page >= r.address && page < r.address + r.size;
        });
        if (backed) continue;
        if (!fresh.empty() && fresh.back().address + fresh.back().size == page)
            fresh.back().size += page_size_;
        else
            fresh.push_back({page, page_size_});
    }
#ifdef _WIN32
    struct Replacement { Region region; bool needs_split; };
    std::vector<Replacement> replacements;
    for (const auto& r : fresh) {
        uint64_t cursor = r.address;
        while (cursor < r.address + r.size) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(base_ + cursor, &info, sizeof(info)))
                throw RuntimeStop("memory-map", address, "guest placeholder backing is inconsistent");
            const auto* allocation_base = static_cast<uint8_t*>(info.AllocationBase);
            if (info.State != MEM_RESERVE || allocation_base < base_ ||
                allocation_base >= base_ + address_space_size)
                throw RuntimeStop("memory-map", address, "guest placeholder backing is inconsistent");
            const auto region_end = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize;
            const uint64_t chunk = std::min<uint64_t>(r.address + r.size - cursor,
                                                       region_end - (base_ + cursor));
            if (!chunk)
                throw RuntimeStop("memory-map", address, "guest placeholder backing is inconsistent");
            replacements.push_back({{cursor, chunk},
                info.AllocationBase != base_ + cursor || info.RegionSize != chunk});
            cursor += chunk;
        }
    }
    std::vector<Region> recommits;
    for (const auto& backing : backing_) {
        const uint64_t overlap_begin = std::max(address, backing.address);
        const uint64_t overlap_end = std::min(address + host_size, backing.address + backing.size);
        uint64_t cursor = overlap_begin;
        while (cursor < overlap_end) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(base_ + cursor, &info, sizeof(info)) || !info.RegionSize)
                throw RuntimeStop("memory-map", address, "owned private backing query failed during recommit");
            const auto* const region_base = static_cast<uint8_t*>(info.BaseAddress);
            const auto* const allocation_base = static_cast<uint8_t*>(info.AllocationBase);
            const auto* const region_end = region_base + info.RegionSize;
            if (region_base > base_ + cursor || region_end <= base_ + cursor ||
                region_end > base_ + backing.address + backing.size ||
                allocation_base != base_ + backing.address ||
                (info.State != MEM_COMMIT && info.State != MEM_RESERVE) ||
                (info.State == MEM_COMMIT && info.Type != MEM_PRIVATE))
                throw RuntimeStop("memory-map", address, "owned private backing is inconsistent during recommit");
            const uint64_t chunk_end = std::min<uint64_t>(overlap_end, region_end - base_);
            if (info.State == MEM_RESERVE) recommits.push_back({cursor, chunk_end - cursor});
            cursor = chunk_end;
        }
    }
    auto next_backing = backing_;
    for (const auto& replacement : replacements) next_backing.push_back(replacement.region);
    const DWORD protection = write_combined && host_write_combining()
                                 ? PAGE_READWRITE | PAGE_WRITECOMBINE
                                 : PAGE_READWRITE;
    bool host_ok = true;
    std::vector<Region> created;
    created.reserve(replacements.size());
    std::vector<Region> recommitted;
    recommitted.reserve(recommits.size());
    for (const auto& replacement : replacements) {
        const auto& r = replacement.region;
        const bool prepared = !replacement.needs_split ||
            VirtualFree(base_ + r.address, r.size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        if (!prepared || !virtual_alloc2(base_ + r.address, r.size,
                MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, protection)) {
            host_ok = false;
            break;
        }
        created.push_back(r);
    }
    if (host_ok) {
        for (const auto& r : recommits) {
            if (VirtualAlloc(base_ + r.address, r.size, MEM_COMMIT, protection) != base_ + r.address) {
                host_ok = false;
                break;
            }
            recommitted.push_back(r);
        }
    }
    if (!host_ok) {
        for (auto r = recommitted.rbegin(); r != recommitted.rend(); ++r)
            VirtualFree(base_ + r->address, r->size, MEM_DECOMMIT);
        for (auto r = created.rbegin(); r != created.rend(); ++r)
            VirtualFree(base_ + r->address, r->size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    }
    if (!host_ok)
#else
    auto next_backing = backing_;
    next_backing.insert(next_backing.end(), fresh.begin(), fresh.end());
    (void)write_combined;  // cached, see map_write_combined
    if (mprotect(base_ + address, host_size, PROT_READ | PROT_WRITE) != 0)
#endif
        throw RuntimeStop("memory-map", address, "cannot commit guest pages");
    std::unique_lock layout(layout_mutex_);
    committed_.swap(next);
    backing_.swap(next_backing);
    write_combined_.swap(next_write_combined);
    if (write_combined) {
        const uint64_t first = address / fast_page_size;
        const uint64_t end = std::min<uint64_t>(address_space_size / fast_page_size,
                                                (address + host_size + fast_page_size - 1) / fast_page_size);
        std::fill(write_combined_pages_.get() + first, write_combined_pages_.get() + end, uint8_t{1});
    }
    rebuild_fast_pages(address, size);
}

GuestMemory::Usage GuestMemory::usage(uint64_t begin, uint64_t size) const {
    if (!size || begin >= address_space_size || size > address_space_size - begin ||
        begin % page_size_ || size % page_size_)
        throw RuntimeStop("memory-statistics", begin, "invalid guest memory statistics range/alignment");
    const uint64_t end = begin + size;
    auto total_intersection = [&](const std::vector<Region>& regions) {
        uint64_t total = 0;
        for (const auto& r : regions) {
            const uint64_t region_end = r.address + (r.size + page_size_ - 1) / page_size_ * page_size_;
            const uint64_t clipped_begin = std::max(begin, r.address);
            const uint64_t clipped_end = std::min(end, region_end);
            if (clipped_begin < clipped_end) total += clipped_end - clipped_begin;
        }
        return total;
    };
    return {total_intersection(reservations_), total_intersection(committed_)};
}

bool GuestMemory::readable(uint64_t address, uint64_t size) const noexcept {
    if (!size || address >= address_space_size || size > address_space_size - address) return false;
    uint64_t page = address / fast_page_size;
    const uint64_t last = (address + size - 1) / fast_page_size;
    while (page <= last && (fast_pages_[page] & fast_access)) ++page;
    if (page > last) return true;
    const auto layout = read_layout();
    for (const auto& variable : variables_)
        if (address < uint64_t(variable.address) + 4 && variable.address < address + size) return false;
    uint64_t covered = address;
    const uint64_t end = address + size;
    while (covered < end) {
        const Region* const r = committed_region(covered);
        if (!r) return false;
        covered = r->address + r->size;
    }
    return true;
}

const GuestMemory::Region* GuestMemory::committed_region(uint64_t address) const {
    auto next = std::upper_bound(committed_.begin(), committed_.end(), address,
                                 [](uint64_t value, const Region& r) { return value < r.address; });
    if (next == committed_.begin()) return nullptr;
    const Region& r = *--next;
    return address < r.address + r.size ? &r : nullptr;
}

void GuestMemory::check(uint64_t address, uint64_t size) const {
    if (!size || address >= address_space_size || size > address_space_size - address)
        throw RuntimeStop("memory-access", address, "guest address overflow");
    // Pages marked fast are committed and hold no import variable.
    uint64_t page = address / fast_page_size;
    const uint64_t last = (address + size - 1) / fast_page_size;
    while (page <= last && (fast_pages_[page] & fast_access)) ++page;
    if (page > last) return;
    const auto layout = read_layout();
    for (const auto& variable : variables_)
        if (address < uint64_t(variable.address) + 4 && variable.address < address + size)
            throw RuntimeStop("import-variable", variable.address, variable.name);
    // Adjacent committed regions (e.g. separately committed heap chunks) form
    // one contiguous range; an object may straddle their boundary.
    uint64_t covered = address;
    const uint64_t end = address + size;
    while (covered < end) {
        const Region* const r = committed_region(covered);
        if (!r) throw RuntimeStop("memory-access", covered, "unmapped guest memory");
        covered = r->address + r->size;
    }
}

void GuestMemory::add_import_variable(uint32_t address, std::string name) {
    check(address, 4);
    check_pending_writes(address,4);
    std::unique_lock layout(layout_mutex_);
    variables_.push_back({address, std::move(name)});
    special_words_.push_back(address);
    rebuild_fast_pages();
}

void GuestMemory::store_checked(uint64_t address, uint64_t value, uint64_t size) {
    check_write(address, size);
    for (uint64_t i = 0; i < size; ++i)
        base_[address + i] = static_cast<uint8_t>(value >> ((size - 1 - i) * 8));
    complete_store(address, size);
}

void GuestMemory::rebuild_fast_pages(uint64_t begin, uint64_t size) {
    // Recomputes the pages of [begin, begin + size) (all pages by default).
    const uint64_t first_page = begin / fast_page_size;
    const uint64_t end_page = std::min<uint64_t>(address_space_size / fast_page_size,
                                                 (begin + size + fast_page_size - 1) / fast_page_size);
    if (first_page >= end_page) return;
    std::fill(fast_pages_.get() + first_page, fast_pages_.get() + end_page, uint8_t{0});
    for (const auto& r : committed_) {
        const uint64_t first = std::max((r.address + fast_page_size - 1) / fast_page_size, first_page);
        const uint64_t end = std::min((r.address + r.size) / fast_page_size, end_page);
        for (uint64_t page = first; page < end; ++page) fast_pages_[page] = 1;
    }
    const auto clear = [&](uint64_t address, uint64_t length) {
        const uint64_t first = std::max(address / fast_page_size, first_page);
        const uint64_t last = std::min((address + length - 1) / fast_page_size + 1, end_page);
        for (uint64_t page = first; page < last; ++page)
            fast_pages_[page] = (fast_pages_[page] & (fast_access | fast_special)) ? fast_special : 0;
    };
    for (const auto& variable : variables_) clear(variable.address, 4);
    for (const auto& word : read_only_words_) clear(word.address, 4);
    for (uint64_t page = first_page; page < end_page; ++page)
        if ((watched_pages_[page] & 1) && fast_pages_[page]) fast_pages_[page] |= fast_watched;
}

uint32_t GuestMemory::load_reserved_word(uint64_t address) {
    if (address % 4)
        throw RuntimeStop("memory-alignment", address, "reserved word load requires four-byte alignment");
    // Writable ordinary memory only: never sample a computed provider, and allow
    // replacement of a live reservation only after the complete load succeeds.
    check_store_access(address, 4);
    if (intersects_write_combined(address, 4))
        throw RuntimeStop("memory-cache", address, "reserved word load does not support write-combined memory");
    const auto value = load<uint32_t>(address);
    guest_reservation = {reservation_owner_, address, value, 4};
    return value;
}

bool GuestMemory::store_conditional_word(uint64_t address, uint32_t value) {
    if (address % 4)
        throw RuntimeStop("memory-alignment", address, "conditional word store requires four-byte alignment");
    check_store_access(address, 4);
    if (intersects_write_combined(address, 4))
        throw RuntimeStop("memory-cache", address, "conditional word store does not support write-combined memory");
    auto& reservation = guest_reservation;
    if (reservation.owner != reservation_owner_) return false;
    if (address != reservation.address)
        throw RuntimeStop("reservation-address", address, "conditional store differs from reserved word address");
    if (reservation.size != 4)
        throw RuntimeStop("reservation-width", address,
                          "conditional word store differs from reserved access width");
    reservation.owner = 0;
    // Succeeds only if the word still holds what the reserved load read, as
    // one atomic step: another host thread may be running guest code too.
    // The access checks above already marked the page written.
    uint32_t expected = __builtin_bswap32(uint32_t(reservation.value));
    if (!std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(base_ + address))
             .compare_exchange_strong(expected, __builtin_bswap32(value)))
        return false;
    return true;
}

uint64_t GuestMemory::load_reserved_doubleword(uint64_t address) {
    if (address % 8)
        throw RuntimeStop("memory-alignment", address,
                          "reserved doubleword load requires eight-byte alignment");
    check_store_access(address, 8);
    if (intersects_write_combined(address, 8))
        throw RuntimeStop("memory-cache", address,
                          "reserved doubleword load does not support write-combined memory");
    const auto value = load<uint64_t>(address);
    guest_reservation = {reservation_owner_, address, value, 8};
    return value;
}

bool GuestMemory::store_conditional_doubleword(uint64_t address, uint64_t value) {
    if (address % 8)
        throw RuntimeStop("memory-alignment", address,
                          "conditional doubleword store requires eight-byte alignment");
    check_store_access(address, 8);
    if (intersects_write_combined(address, 8))
        throw RuntimeStop("memory-cache", address,
                          "conditional doubleword store does not support write-combined memory");
    auto& reservation = guest_reservation;
    if (reservation.owner != reservation_owner_) return false;
    if (address != reservation.address)
        throw RuntimeStop("reservation-address", address,
                          "conditional store differs from reserved doubleword address");
    if (reservation.size != 8)
        throw RuntimeStop("reservation-width", address,
                          "conditional doubleword store differs from reserved access width");
    reservation.owner = 0;
    uint64_t expected = __builtin_bswap64(reservation.value);
    if (!std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(base_ + address))
             .compare_exchange_strong(expected, __builtin_bswap64(value)))
        return false;
    return true;
}

void GuestMemory::check_write(uint64_t address, uint64_t size) const {
    check_store_access(address, size);
    if (has_reservation())
        throw RuntimeStop("reservation-interference", address, "ordinary write during live reservation");
}

bool GuestMemory::zero_fast(uint32_t address, uint32_t size) {
    // The whole block lies in one fast page (blocks are aligned and smaller).
    const uint8_t page = fast_page(address, size);
    if (!(page & fast_access) || has_reservation() || page_pinned(address)) return false;
    std::fill_n(base_ + address, size, uint8_t{0});
    if (page & fast_watched) note_watched_write(address / fast_page_size);
    return true;
}

void GuestMemory::zero_cache_block(uint32_t effective_address) {
    const uint32_t address = effective_address & ~uint32_t{31};
    if (zero_fast(address, 32)) return;
    check_write(address, 32);
    std::fill_n(base_ + address, 32, uint8_t{0});
    complete_store(address, 32);
}

void GuestMemory::zero_cache_line(uint32_t effective_address) {
    const uint32_t address = effective_address & ~uint32_t{127};
    if (zero_fast(address, 128)) return;
    check_write(address, 128);
    std::fill_n(base_ + address, 128, uint8_t{0});
    complete_store(address, 128);
}

void GuestMemory::watch_writes(uint64_t address, uint64_t size) {
    if (!size || address >= address_space_size || size > address_space_size - address) return;
    for (uint64_t page = address / fast_page_size; page <= (address + size - 1) / fast_page_size; ++page) {
        std::atomic_ref<uint8_t>(watched_pages_[page]).fetch_or(1, std::memory_order_relaxed);
        if (fast_pages_[page]) fast_pages_[page] |= fast_watched;
    }
}

void GuestMemory::enable_write_epochs() {
    if (page_epochs_) return;
    page_epochs_ = std::make_unique<std::atomic<uint32_t>[]>(address_space_size / fast_page_size);
}

bool GuestMemory::written_since(uint64_t address, uint64_t size, uint32_t epoch) const {
    if (!page_epochs_) return true;
    if (!size || address >= address_space_size || size > address_space_size - address) return true;
    for (uint64_t page = address / fast_page_size; page <= (address + size - 1) / fast_page_size; ++page) {
        if (!(watched_pages_[page] & 1)) return true;  // not watched: no record of its writes
        if (page_epochs_[page].load(std::memory_order_relaxed) >= epoch) return true;
    }
    return false;
}

bool GuestMemory::take_written(uint64_t address, uint64_t size) {
    if (!size || address >= address_space_size || size > address_space_size - address) return false;
    bool written = false;
    for (uint64_t page = address / fast_page_size; page <= (address + size - 1) / fast_page_size; ++page) {
        // Written bits are set by stores on other threads too.
        written |= (std::atomic_ref<uint8_t>(watched_pages_[page]).fetch_and(uint8_t(~2), std::memory_order_relaxed) & 2) != 0;
    }
    return written;
}

std::atomic<uint64_t> watched_writes{0};
void GuestMemory::mark_written(uint64_t address, uint64_t size) const {
    for (uint64_t page = address / fast_page_size; page <= (address + size - 1) / fast_page_size; ++page)
        if (watched_pages_[page] & 1) {
            note_watched_write(page);
            watched_writes.fetch_add(1, std::memory_order_relaxed);
        }
}

void GuestMemory::check_store_access(uint64_t address, uint64_t size, const PendingWrite* owner) const {
    check(address, size);
    mark_written(address, size);
    check_pending_writes(address,size,owner);
    for (const auto& word : read_only_words_)
        if (address < uint64_t(word.address) + 4 && word.address < address + size)
            throw RuntimeStop("memory-readonly", word.address, "write overlaps computed read-only word");
}

bool GuestMemory::intersects_write_combined(uint64_t address, uint64_t size) const {
    if (!size || address >= address_space_size) return false;
    const uint64_t first = address / fast_page_size;
    const uint64_t last = std::min((address + size - 1) / fast_page_size, address_space_size / fast_page_size - 1);
    bool flagged = false;
    for (uint64_t page = first; page <= last && !flagged; ++page) flagged = write_combined_pages_[page] != 0;
    if (!flagged) return false;
    // A flagged page may hold ordinary bytes beside the region's edge.
    const auto layout = read_layout();
    return std::any_of(write_combined_.begin(), write_combined_.end(), [&](const Region& r) {
        return address < r.address + r.size && r.address < address + size;
    });
}

void GuestMemory::complete_store(uint64_t address, uint64_t size) const {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    if (intersects_write_combined(address, size)) _mm_mfence();
#else
    (void)address;
    (void)size;
#endif
}

void GuestMemory::add_read_only_word(uint32_t address, std::function<uint32_t()> provider) {
    if (address % 4 || !provider || read_only_words_.size() >= 16)
        throw RuntimeStop("memory-provider", address, "invalid read-only word alignment, provider, or quota");
    try {
        check(address, 4);
    } catch (const RuntimeStop& e) {
        throw RuntimeStop("memory-provider", address, "read-only word must be mapped and unguarded: " + e.detail);
    }
    for (const auto& word : read_only_words_)
        if (address < uint64_t(word.address) + 4 && word.address < uint64_t(address) + 4)
            throw RuntimeStop("memory-provider", address, "overlapping read-only word");
    check_pending_writes(address,4);
    std::unique_lock layout(layout_mutex_);
    read_only_words_.push_back({address, std::move(provider)});
    special_words_.push_back(address);
    rebuild_fast_pages();
}

uint64_t GuestMemory::read_scalar(uint64_t address, uint64_t size) const {
    // Validate the complete scalar before querying any computed value.
    check(address, size);
    uint64_t result = 0;
    for (uint64_t i = 0; i < size; ++i)
        result = (result << 8) | base_[address + i];
    for (const auto& word : read_only_words_) {
        const uint64_t begin = std::max(address, uint64_t(word.address));
        const uint64_t end = std::min(address + size, uint64_t(word.address) + 4);
        if (begin >= end) continue;
        if (slow_access_hook) slow_access_hook(address);  // providers read host state
        const uint32_t value = word.provider();
        for (uint64_t byte = begin; byte < end; ++byte) {
            const auto source_shift = (3 - (byte - word.address)) * 8;
            const auto target_shift = (size - 1 - (byte - address)) * 8;
            const uint64_t computed_byte = (value >> source_shift) & 0xff;
            result = (result & ~(uint64_t(0xff) << target_shift)) | (computed_byte << target_shift);
        }
    }
    return result;
}
}
