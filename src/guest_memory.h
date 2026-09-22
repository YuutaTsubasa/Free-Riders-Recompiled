#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace sfr {
struct RuntimeStop : std::runtime_error {
    std::string category;
    uint64_t address;
    std::string detail;
    RuntimeStop(std::string category, uint64_t address, std::string detail);
};

// A lwarx/ldarx reservation belongs to the host thread that made it, as each
// console core has its own: guest threads running at once must not see
// each other's. Active when owner is the id of the GuestMemory it was made
// in (never 0; ids are not reused, unlike addresses of destroyed instances).
struct GuestReservation {
    uint64_t owner = 0;
    uint64_t address = 0, value = 0;
    uint8_t size = 0;
};
inline thread_local GuestReservation guest_reservation;

class GuestMemory {
    struct PendingState;
    struct PendingWrite;
public:
    struct Range { uint64_t address, size; };
    // Creation/publication require exclusive guest execution. Reset only drops
    // host ownership and may also run after execution has stopped and drained.
    class WriteLease {
    public:
        WriteLease() = default;
        WriteLease(WriteLease&&) noexcept = default;
        WriteLease& operator=(WriteLease&&) noexcept = default;
        WriteLease(const WriteLease&) = delete;
        WriteLease& operator=(const WriteLease&) = delete;
        void check() const;
        void reset() noexcept { entry_.reset(); state_.reset(); }
        template<typename T> void store(uint64_t address, T value) {
            static_assert(std::is_unsigned_v<T> && sizeof(T) <= sizeof(uint64_t));
            write_scalar(address, value, sizeof(T));
        }
        // A whole transfer with the checks done once (a completed read
        // published byte by byte ran them for every byte).
        void write_bytes(uint64_t address, std::span<const uint8_t> bytes);
    private:
        friend class GuestMemory;
        WriteLease(std::shared_ptr<PendingState> state, std::shared_ptr<PendingWrite> entry)
            : state_(std::move(state)), entry_(std::move(entry)) {}
        void write_scalar(uint64_t address, uint64_t value, uint64_t size);
        std::shared_ptr<PendingState> state_;
        std::shared_ptr<PendingWrite> entry_;
    };
    WriteLease pin_writes(std::span<const Range> ranges);
    static constexpr uint64_t address_space_size = 0x100000000ull;
    static constexpr uint64_t default_backing_budget = 0x20000000ull;
    struct Usage { uint64_t reserved_bytes; uint64_t committed_bytes; };
    explicit GuestMemory(uint64_t backing_budget = default_backing_budget);
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;
    uint8_t* base() const { return base_; }
    uint64_t backing_budget() const { return backing_budget_; }
    Usage usage(uint64_t begin = 0, uint64_t size = address_space_size) const;
    void map(uint64_t address, uint64_t size);
    void map_write_combined(uint64_t address, uint64_t size);
    // Whether a write-combined request is given write-combined host pages.
    // The title marks its GPU buffers write-combined because on the console
    // only the GPU reads them; here the renderer reads them on the CPU, and a
    // read of write-combined memory goes to DRAM every time -- about five
    // hundred cycles, which a race frame paid for every index and every
    // vertex (docs/performance.md). Off by default, so those pages are
    // cached: x86 cached memory is more strongly ordered than
    // write-combined, so nothing the guest relies on is weakened.
    // SFR_WRITE_COMBINED=1 turns the console's mapping back on.
    static void set_host_write_combining(bool enabled) noexcept;
    static bool host_write_combining() noexcept;
    void reserve(uint64_t address, uint64_t size);
    void commit(uint64_t address, uint64_t size);
    void decommit(uint64_t address, uint64_t size);
    void release(uint64_t address, uint64_t size);
    bool available(uint64_t address, uint64_t size) const;
    // Lowest start of a reservation overlapping the range (the range's end when none).
    uint64_t lowest_conflict(uint64_t address, uint64_t size) const;
    void check(uint64_t address, uint64_t size) const;
    // check() as a question rather than a throw. Asking which of a few
    // candidate addresses a range is mapped at costs microseconds an
    // exception, and the renderer asks that for every texture of every draw
    // (docs/performance.md).
    bool readable(uint64_t address, uint64_t size) const noexcept;
    void check_write(uint64_t address, uint64_t size) const;
    // Stores bytes as they are, with the checks of a store done once for the
    // whole range.
    void write_bytes(uint64_t address, std::span<const uint8_t> bytes);
    void zero_cache_block(uint32_t effective_address);
    void zero_cache_line(uint32_t effective_address);
    uint32_t load_reserved_word(uint64_t address);
    bool store_conditional_word(uint64_t address, uint32_t value);
    uint64_t load_reserved_doubleword(uint64_t address);
    bool store_conditional_doubleword(uint64_t address, uint64_t value);
    bool has_reservation() const { return guest_reservation.owner == reservation_owner_; }
    // The layout (regions, import variables, computed words, pending I/O)
    // changes only under exclusive guest execution. A thread that runs guest
    // code without it (a detached guest) sets concurrent_reader: its checks
    // then read the layout under a shared lock the changes take exclusively.
    // Computed words call host providers, so before one is read such a
    // thread's slow_access_hook must return holding exclusive execution.
    // Accesses to fast pages need neither.
    static inline thread_local bool concurrent_reader = false;
    static inline thread_local void (*slow_access_hook)(uint64_t address) = nullptr;
    void add_read_only_word(uint32_t address, std::function<uint32_t()> provider);
    void add_import_variable(uint32_t address, std::string name);
    // Write watches: stores to watched pages take the checked path and mark
    // the page dirty. Used to notice CPU rewrites of data with native copies.
    void watch_writes(uint64_t address, uint64_t size);
    // True when any watched page of the range was written since the last
    // call for that range; clears those pages' dirty marks.
    bool take_written(uint64_t address, uint64_t size);
    // Write epochs, beside the written bits: once enabled, a store to a
    // watched page records the current epoch there, and written_since tells
    // whether any page of a range was stored to in or after an epoch. Many
    // owners can ask about the same pages, which take_written's shared bits
    // do not allow. The caller advances the epoch (once a frame).
    void enable_write_epochs();
    void advance_write_epoch() noexcept { write_epoch_.fetch_add(1, std::memory_order_relaxed); }
    uint32_t write_epoch() const noexcept { return write_epoch_.load(std::memory_order_relaxed); }
    bool written_since(uint64_t address, uint64_t size, uint32_t epoch) const;
    // The fast paths of load and store for a caller that handles byte order
    // itself: a pointer to the range when it lies in one fast page (and, for
    // a write, no reservation is live and no pending I/O pins it), else null
    // and the caller takes the checked path. A write marks a watched page
    // written, as store does. Sixteen-byte vector accesses use these: each
    // one otherwise ran the checks twice over and assembled bytes one at a
    // time (docs/performance.md).
    const uint8_t* fast_read(uint64_t address, uint64_t size) const {
        return (fast_page(address, size) & fast_access) ? base_ + address : nullptr;
    }
    uint8_t* fast_write(uint64_t address, uint64_t size) {
        const uint8_t page = fast_page(address, size);
        if (!(page & fast_access) || has_reservation() || page_pinned(address)) return nullptr;
        if (page & fast_watched) note_watched_write(address / fast_page_size);
        return base_ + address;
    }
    // Every guest load and store comes through these: the fast path is a
    // byte-swapped access inlined into the generated code, the checked path a
    // call (store_checked, read_scalar). The accesses are volatile, as in
    // XenonRecomp's own PPC_LOAD/PPC_STORE: guest threads run in parallel,
    // and the compiler must neither reorder nor merge a thread's accesses
    // (the host's TSO then keeps the order lwsync asks for). Without it a
    // race called a pure virtual function (R6025) once checkpoints stopped
    // being calls the compiler could not see through.
    template<typename T> __attribute__((always_inline)) T load(uint64_t address) const {
        static_assert(std::is_unsigned_v<T>);
        static_assert(sizeof(T) <= sizeof(uint64_t));
        const uint8_t page = fast_page(address, sizeof(T));
        if ((page & fast_access) || ((page & fast_special) && !special_word(address, sizeof(T)))) [[likely]]
            return byte_swap(T(*reinterpret_cast<const volatile T*>(base_ + address)));
        return static_cast<T>(read_scalar(address, sizeof(T)));
    }
    template<typename T> __attribute__((always_inline)) void store(uint64_t address, T value) {
        static_assert(std::is_unsigned_v<T>);
        const uint8_t page = fast_page(address, sizeof(T));
        if ((page & fast_access) && !has_reservation() && !page_pinned(address)) [[likely]] {
            *reinterpret_cast<volatile T*>(base_ + address) = byte_swap(value);
            if (page & fast_watched) [[unlikely]] note_watched_write(address / fast_page_size);
            return;
        }
        store_checked(address, uint64_t(value), sizeof(T));
    }
private:
    static inline std::atomic<uint64_t> next_reservation_owner_{1};
    const uint64_t reservation_owner_ = next_reservation_owner_.fetch_add(1);
    // An access inside one 4 KiB page that lies wholly in one committed region
    // and holds no import variable or computed word needs none of the
    // per-access checks (fast_access); a store to a watched page only records
    // the write (fast_watched). Everything else takes the full path.
    static constexpr uint64_t fast_page_size = 4096;
    static constexpr uint8_t fast_access = 1, fast_watched = 2;
    std::unique_ptr<std::atomic<uint32_t>[]> page_epochs_;
    std::atomic<uint32_t> write_epoch_{1};
    void note_watched_write(uint64_t page) const {
        std::atomic_ref<uint8_t>(watched_pages_[page]).fetch_or(2, std::memory_order_relaxed);
        if (page_epochs_) page_epochs_[page].store(write_epoch_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    // A wholly committed page that holds an import variable or computed word:
    // a load that touches neither reads it directly. The title reads words
    // beside the XEX's import variables millions of times a race.
    static constexpr uint8_t fast_special = 4;
    // Addresses of the import variables and computed words (set at startup).
    std::vector<uint32_t> special_words_;
    bool special_word(uint64_t address, uint64_t size) const {
        for (const uint32_t word : special_words_)
            if (address < uint64_t(word) + 4 && word < address + size) return true;
        return false;
    }
    uint8_t fast_page(uint64_t address, uint64_t size) const {
        return address < address_space_size &&
               ((address + size - 1) / fast_page_size) == address / fast_page_size
            ? fast_pages_[address / fast_page_size] : uint8_t{0};
    }
    // Under a pending I/O output range (PendingState::pinned).
    bool page_pinned(uint64_t address) const {
        return pinned_pages_[address / fast_page_size].load(std::memory_order_relaxed) != 0;
    }
    std::atomic<uint16_t>* pinned_pages_ = nullptr;
    void store_checked(uint64_t address, uint64_t value, uint64_t size);
    template<typename T> static T byte_swap(T value) {
        if constexpr (sizeof(T) == 1) return value;
        else if constexpr (sizeof(T) == 2) return T(__builtin_bswap16(value));
        else if constexpr (sizeof(T) == 4) return T(__builtin_bswap32(value));
        else return T(__builtin_bswap64(value));
    }
    // Shared by concurrent readers' layout scans; see concurrent_reader.
    mutable std::shared_mutex layout_mutex_;
    std::shared_lock<std::shared_mutex> read_layout() const {
        return concurrent_reader ? std::shared_lock(layout_mutex_) : std::shared_lock<std::shared_mutex>();
    }
    bool zero_fast(uint32_t address, uint32_t size);
    void rebuild_fast_pages(uint64_t begin = 0, uint64_t size = address_space_size);
    std::unique_ptr<uint8_t[]> fast_pages_;
    // Per page: bit 0 watched, bit 1 written since last taken.
    std::unique_ptr<uint8_t[]> watched_pages_;
    void mark_written(uint64_t address, uint64_t size) const;
    struct Region { uint64_t address, size; };
    // The committed region holding address, or null. committed_ is sorted
    // and merged, so this is a binary search: the checked path used to scan
    // every region, and the title's TLS block and the XEX's import page are
    // read through it millions of times a race.
    const Region* committed_region(uint64_t address) const;
    // The lowest reservation overlapping [address, end) in whole pages, or null.
    const Region* first_conflict(uint64_t address, uint64_t end) const;
    struct Variable { uint32_t address; std::string name; };
    struct ReadOnlyWord { uint32_t address; std::function<uint32_t()> provider; };
    void check_store_access(uint64_t address, uint64_t size, const PendingWrite* owner = nullptr) const;
    void check_pending_writes(uint64_t address, uint64_t size, const PendingWrite* owner = nullptr) const;
    void commit_impl(uint64_t address, uint64_t size, bool write_combined);
    void complete_store(uint64_t address, uint64_t size) const;
    bool intersects_write_combined(uint64_t address, uint64_t size) const;
    uint64_t read_scalar(uint64_t address, uint64_t size) const;
    uint8_t* base_ = nullptr;
    uint64_t page_size_ = 4096;
    uint64_t backing_budget_ = default_backing_budget;
    std::vector<Region> reservations_;
    std::vector<Region> committed_;
    std::vector<Region> backing_;
    std::vector<Region> write_combined_;
    // Per page: nonzero when a write-combined region covers any of it. The
    // title makes about 1500 such regions, and every reserved load,
    // conditional store and slow-path store asks whether it is in one; the
    // page tells it no without walking them (docs/performance.md).
    std::unique_ptr<uint8_t[]> write_combined_pages_;
    std::vector<Variable> variables_;
    std::vector<ReadOnlyWord> read_only_words_;
    std::shared_ptr<PendingState> pending_state_;
    // Guest execution is serialized, with no ordinary stores or imports while a
    // reservation is live. This does not model hardware reservation granularity.
};
}
