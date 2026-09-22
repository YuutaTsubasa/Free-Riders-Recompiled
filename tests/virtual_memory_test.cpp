#include "virtual_memory.h"
#include <iostream>
#include <stdexcept>

namespace {
constexpr uint32_t reserve = 0x2000, commit = 0x1000, large = 0x20000000;
constexpr uint32_t invalid = 0xc000000d, no_memory = 0xc0000017;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> bool stopped(F operation, const char* category) {
    try { operation(); } catch (const sfr::RuntimeStop& e) { return e.category == category; }
    return false;
}
struct Fixture {
    sfr::GuestMemory memory;
    sfr::VirtualMemory vm{memory};
    Fixture() { memory.map(0x70000000, 0x1000); }
    uint32_t allocate(uint32_t base, uint32_t size, uint32_t type, uint32_t protect = 4, uint32_t debug = 0) {
        memory.store<uint32_t>(0x70000000, base);
        memory.store<uint32_t>(0x70000004, size);
        return vm.allocate(0x70000000, 0x70000004, type, protect, debug);
    }
    uint32_t base() { return memory.load<uint32_t>(0x70000000); }
    uint32_t free_request(uint32_t base, uint32_t size, uint32_t type = 0x4000, uint32_t debug = 0) {
        memory.store<uint32_t>(0x70000000, base);
        memory.store<uint32_t>(0x70000004, size);
        return vm.free(0x70000000, 0x70000004, type, debug);
    }
    uint32_t size() { return memory.load<uint32_t>(0x70000004); }
};
void reserve_then_commit() {
    Fixture f;
    require(f.allocate(0, 0x100000, 0x60002000) == 0, "observed heap reserve succeeds");
    require(f.base() == 0x40000000 && f.size() == 0x100000, "observed reserve uses 64 KiB arena");
    require(f.memory.base()[0x70000000] == 0x40, "outputs are big endian");
    require(stopped([&] { f.memory.load<uint32_t>(f.base()); }, "memory-access"), "reserve does not commit");
    require(f.allocate(0x40010000, 1, commit) == 0, "fixed commit within reservation");
    require(f.size() == 0x10000, "fixed arena determines 64 KiB page without flag");
    require(f.memory.load<uint32_t>(0x40010000) == 0, "fresh pages zero");
    require(stopped([&] { f.memory.load<uint32_t>(0x40000000); }, "memory-access"), "partial commit preserves inaccessible prefix");
    f.memory.store<uint32_t>(0x40010000, 0xdecafbad);
    require(f.allocate(0x40000000, 0x20000, commit) == 0, "overlapping commit succeeds");
    require(f.memory.load<uint32_t>(0x40010000) == 0xdecafbad, "recommit preserves data");
    f.memory.store<uint64_t>(0x4000fffc, 0x0123456789abcdefull);
    require(f.memory.load<uint64_t>(0x4000fffc) == 0x0123456789abcdefull, "access crosses committed spans");
}
void rounding_and_arenas() {
    Fixture f;
    require(f.allocate(0, 4097, reserve | commit) == 0, "reserve plus commit");
    require(f.base() == 0x10000000 && f.size() == 8192, "4 KiB rounding");
    require(f.allocate(0, 1, commit | 0x00800000) == 0, "zero-base commit and nozero supported");
    require(f.base() == 0x10002000 && f.memory.load<uint32_t>(f.base()) == 0, "disjoint new zero pages");
    require(f.allocate(0, 1, reserve | large) == 0 && f.base() == 0x40000000 && f.size() == 65536, "separate large arena");
    require(f.allocate(0x10000000, 1, commit | large) == 0 && f.size() == 4096, "fixed 4 KiB arena ignores large flag");
    require(f.allocate(0, 4097, reserve | 0x00100000) == 0 && f.base() == 0x1fffe000, "top-down 4 KiB allocation");
    require(f.allocate(0, 1, reserve | large | 0x00100000) == 0 && f.base() == 0x4fff0000, "top-down large allocation");
}
void conflicts_and_exhaustion() {
    Fixture f;
    f.memory.map(0x10000000, 1);
    require(f.allocate(0, 1, reserve) == 0 && f.base() == 0x10001000, "allocator skips fixed mapping rounded page");
    require(f.allocate(0x10000000, 1, reserve) == no_memory, "fixed mapping conflict");
    require(f.base() == 0x10000000 && f.size() == 1, "conflict leaves outputs unchanged");
    require(f.allocate(0x10001000, 1, reserve | commit) == no_memory, "cannot reserve twice");
    require(f.allocate(0x10001000, 8192, commit) == no_memory, "commit cannot exceed reservation");
    require(f.allocate(0x10002000, 4096, commit) == no_memory, "fixed commit needs owned reservation");
    require(f.allocate(0, 0x10000000, reserve | large) == 0, "reserve entire large arena without physical commit");
    require(f.allocate(0, 1, reserve | large) == no_memory, "arena exhaustion returns Xenia status");
    require(f.base() == 0 && f.size() == 1, "exhaustion leaves outputs unchanged");
    require(f.allocate(0x1ffff000, 8192, reserve) == no_memory, "fixed range cannot cross arena");
}
void invalid_requests() {
    Fixture f;
    require(f.allocate(0, 0, reserve) == invalid, "zero size invalid");
    require(f.allocate(0xfffff000, 8192, reserve) == invalid, "32-bit range overflow invalid");
    require(f.allocate(0x60000000, 4096, reserve) == invalid, "fixed base outside supported arenas invalid");
    require(f.allocate(0, 4096, 0) == invalid, "allocation mode missing invalid");
    require(f.vm.allocate(0, 0x70000004, reserve, 4, 0) == invalid, "null base pointer invalid");
    require(f.vm.allocate(0x70000000, 0, reserve, 4, 0) == invalid, "null size pointer invalid");
    require(f.vm.allocate(0x70000000, 0x70000002, reserve, 4, 0) == invalid, "overlapping output pointers invalid");
    require(f.vm.allocate(0xfffffffe, 0x70000004, reserve, 4, 0) == invalid, "overflowing pointer invalid");
    require(f.vm.allocate(0x70000000, 0x60000000, reserve, 4, 0) == invalid, "unmapped size pointer invalid");
    require(f.vm.allocate(0x60000000, 0x70000004, reserve, 4, 0) == invalid, "unmapped base pointer invalid");
    require(f.base() == 0 && f.size() == 4096, "invalid pointer requests preserve output bytes");
    require(f.allocate(0, 1, reserve) == 0 && f.base() == 0x10000000, "failed requests do not reserve memory");
    f.memory.add_import_variable(0x70000010, "UnimplementedVariable");
    require(stopped([&] { f.vm.allocate(0x70000010, 0x70000004, reserve, 4, 0); }, "import-variable"), "pointer validation retains import guard");
}
void unsupported_requests() {
    Fixture f;
    for (uint32_t type : {reserve | 0x80000000u, reserve | 0x80000u, reserve | 0x4000u, reserve | 1u}) {
        require(stopped([&] { f.allocate(0, 1, type); }, "virtual-memory-request"), "unsupported allocation flag stops");
        require(f.base() == 0 && f.size() == 1, "unsupported flags preserve outputs");
    }
    for (uint32_t protect : {0u, 1u, 2u, 0x40u, 0x104u})
        require(stopped([&] { f.allocate(0, 1, reserve, protect); }, "virtual-memory-request"), "unsupported protection stops");
    require(stopped([&] { f.allocate(0, 1, reserve, 4, 1); }, "virtual-memory-request"), "debug memory stops");
    require(stopped([&] { f.allocate(0x10000001, 1, reserve); }, "virtual-memory-request"), "unaligned fixed request stops");
    require(stopped([&] { f.allocate(0, 0x80000000, reserve); }, "virtual-memory-request"), "negative size stops");
    require(f.allocate(0, 1, reserve) == 0 && f.base() == 0x10000000, "unsupported requests do not mutate allocator");
}
void reservation_budget() {
    Fixture f;
    for (uint32_t i = 0; i < 4096; ++i)
        require(f.allocate(0x10000000 + i * 4096, 1, reserve) == 0, "bounded reservation succeeds");
    require(stopped([&] { f.allocate(0, 1, reserve); }, "virtual-memory-request"), "reservation budget stops explicitly");
    require(f.allocate(0x10000000, 1, commit) == 0, "budget does not prevent existing reservation commit");
}

void readonly_outputs_reject_before_reserving_or_writing() {
    for (uint32_t readonly_pointer : {0x70000000u, 0x70000004u}) {
        Fixture f;
        f.memory.store<uint32_t>(0x70000000, 0);
        f.memory.store<uint32_t>(0x70000004, 1);
        unsigned samples = 0;
        f.memory.add_read_only_word(readonly_pointer, [&] {
            ++samples;
            return readonly_pointer == 0x70000000 ? 0u : 1u;
        });
        require(stopped([&] { f.vm.allocate(0x70000000, 0x70000004, reserve | commit, 4, 0); }, "memory-readonly"),
                "readonly allocation output stops explicitly");
        require(f.memory.base()[0x70000000] == 0 && f.memory.base()[0x70000007] == 1,
                "readonly second output cannot leave first output changed");
        require(f.memory.available(0x10000000, 4096), "readonly outputs cannot reserve guest memory");
        require(samples == 0, "readonly output checks precede provider reads");
        f.memory.store<uint32_t>(0x70000010, 0);
        f.memory.store<uint32_t>(0x70000014, 1);
        require(f.vm.allocate(0x70000010, 0x70000014, reserve | commit, 4, 0) == 0 &&
                f.memory.load<uint32_t>(0x70000010) == 0x10000000,
                "readonly failure leaves allocator metadata unchanged");
    }
}

void decommit_preserves_reservation_and_rounds_by_heap() {
    for (const uint32_t page : {0x1000u, 0x10000u}) {
        Fixture f;
        const uint32_t base = page == 0x1000 ? 0x10000000 : 0x40000000;
        require(f.allocate(base, page * 4, reserve | commit) == 0, "decommit fixture");
        f.memory.store<uint32_t>(base, 0x11223344);
        f.memory.store<uint32_t>(base + page, 0x55667788);
        f.memory.store<uint32_t>(base + 3 * page, 0x99aabbcc);
        const auto before = f.vm.statistics();
        require(f.free_request(base + page, page + 1) == 0, "middle decommit succeeds");
        require(f.base() == base + page && f.size() == page * 2, "unchanged base and heap-rounded output size");
        require(f.vm.statistics().reserved_bytes == before.reserved_bytes &&
                f.vm.statistics().committed_bytes == before.committed_bytes - 2 * page, "only commitment reclaimed");
        require(!f.memory.available(base + page, page) && f.vm.query_address_protect(base + page) == 4,
                "decommit retains ownership and protection");
        require(stopped([&] { f.memory.load<uint32_t>(base + page); }, "memory-access"), "decommitted inaccessible");
        require(f.memory.load<uint32_t>(base) == 0x11223344 && f.memory.load<uint32_t>(base + page * 3) == 0x99aabbcc,
                "neighbors preserved");
        require(f.free_request(base + page, page + 1) == 0, "repeat decommit succeeds");
        require(f.allocate(base + page, page, reserve) == no_memory, "decommit cannot reuse reservation");
        require(f.allocate(base, page * 4, commit) == 0, "recommit live and discarded pages");
        require(f.memory.load<uint32_t>(base + page) == 0 && f.memory.load<uint32_t>(base + 2 * page) == 0,
                "recommitted pages zero");
        require(f.memory.load<uint32_t>(base) == 0x11223344 && f.memory.load<uint32_t>(base + 3 * page) == 0x99aabbcc,
                "recommit preserves live pages");
        require(f.allocate(base + page * 4, page, reserve) == 0 && f.free_request(base + page * 4, 1) == 0,
                "reserved-only decommit succeeds");
    }
}

void decommit_rejections_preserve_state() {
    Fixture f;
    require(f.allocate(0x10000000, 0x2000, reserve | commit) == 0, "rejection fixture");
    require(f.allocate(0x10002000, 0x1000, reserve | commit) == 0, "adjacent independent reservation");
    f.memory.store<uint32_t>(0x10000000, 0xabcdef01);
    const auto before = f.vm.statistics();
    for (auto request : {std::pair{0u, 1u}, {0x60000000u, 1u}, {0x10000000u, 0u},
                         {0x10000000u, 0xffffffffu}, {0x10001000u, 0x1001u}, {0x10003000u, 1u}}) {
        const auto expected = request.first == 0 ? 0xc00000a0u :
            (request.first >= 0x10001000 && request.first < 0x20000000 ? 0xc0000001u : invalid);
        require(f.free_request(request.first, request.second) == expected, "invalid/foreign decommit status");
        require(f.base() == request.first && f.size() == request.second, "failure leaves output values unchanged");
    }
    for (uint32_t flags : {0u, 0xc000u, 0x4001u})
        require(stopped([&] { f.free_request(0x10000000, 1, flags); }, "virtual-memory-request"), "unimplemented free flags stop");
    require(stopped([&] { f.free_request(0x10000000, 1, 0x4000, 1); }, "virtual-memory-request"), "debug free stops");
    require(stopped([&] { f.free_request(0x10000001, 1); }, "virtual-memory-request"), "unaligned free stops");
    require(f.free_request(0x10000001, 0) == invalid && f.free_request(0x10000001, 0xffffffff) == invalid,
            "decommit zero and overflowing size retain precedence over alignment validation");
    require(f.vm.free(0, 0x70000004, 0x4000, 0) == invalid, "null output pointer");
    require(f.vm.free(0x70000000, 0x70000002, 0x4000, 0) == invalid, "overlapping output words");
    require(f.vm.free(0xfffffffe, 0x70000004, 0x4000, 0) == invalid, "output overflow");
    require(f.vm.free(0x70000000, 0x60000000, 0x4000, 0) == invalid, "unmapped output");
    // Output in the rounded tail must be rejected before any page is discarded.
    f.memory.store<uint32_t>(0x70000000, 0x10000000);
    f.memory.store<uint32_t>(0x10001ffc, 0x1001);
    require(f.vm.free(0x70000000, 0x10001ffc, 0x4000, 0) == invalid, "output overlaps rounded target");
    require(f.memory.load<uint32_t>(0x10001ffc) == 0x1001, "overlapping target output preserved");
    f.memory.store<uint32_t>(0x70000000, 0x10001000);
    f.memory.store<uint32_t>(0x10000ffe, 1);
    require(f.vm.free(0x70000000, 0x10000ffe, 0x4000, 0) == invalid, "partial output word overlaps target start");
    require(f.memory.load<uint32_t>(0x10000ffe) == 1, "straddling output remains accessible and unchanged");
    require(f.memory.load<uint32_t>(0x10000000) == 0xabcdef01 &&
            f.vm.statistics().committed_bytes == before.committed_bytes, "all rejected frees preserve state");
}

void decommit_guarded_outputs() {
    for (uint32_t pointer : {0x70000000u, 0x70000004u}) {
        Fixture f;
        require(f.allocate(0x10000000, 0x1000, reserve | commit) == 0, "guard fixture");
        f.memory.store<uint32_t>(0x10000000, 0xabcdef01);
        unsigned samples = 0;
        f.memory.add_read_only_word(pointer, [&] { ++samples; return 0u; });
        require(stopped([&] { f.vm.free(0x70000000, 0x70000004, 0x4000, 0); }, "memory-readonly"), "readonly output rejected");
        require(samples == 0 && f.memory.load<uint32_t>(0x10000000) == 0xabcdef01, "guards before reads and decommit");
    }
    Fixture f;
    f.memory.map(0x10000000, 0x1000);
    require(f.free_request(0x10000000, 1) == 0xc0000001, "foreign mapped arena page is not owned by allocator");
    f.memory.store<uint32_t>(0x10000000, 0x55);
    require(f.memory.load<uint32_t>(0x10000000) == 0x55, "foreign mapped page stays committed");
    f.memory.add_import_variable(0x70000004, "UnknownFreeOutput");
    require(stopped([&] { f.vm.free(0x70000000, 0x70000004, 0x4000, 0); }, "import-variable"), "import output guard preserved");
}

void release_returns_full_reservation_and_allows_zeroed_reuse() {
    for (uint32_t page : {0x1000u, 0x10000u}) {
        Fixture f;
        const uint32_t base = page == 0x1000 ? 0x10000000 : 0x40000000;
        require(f.allocate(base, 4 * page, reserve) == 0, "release reserve fixture");
        require(f.allocate(base, page, commit) == 0 && f.allocate(base + 2 * page, 2 * page, commit) == 0,
                "separate private allocations and placeholder gap");
        f.memory.store<uint32_t>(base, 0x11223344);
        f.memory.store<uint32_t>(base + 2 * page, 0x55667788);
        require(f.free_request(base + 3 * page, page) == 0, "partially decommitted release fixture");
        require(f.allocate(base + 4 * page, page, reserve | commit) == 0, "adjacent independent fixture");
        f.memory.store<uint32_t>(base + 4 * page, 0x99aabbcc);
        const auto before = f.vm.statistics();
        require(f.free_request(base, 0, 0x8000) == 0, "whole release succeeds");
        require(f.base() == base && f.size() == 4 * page, "release returns unchanged base and full owned size");
        const auto after = f.vm.statistics();
        require(after.reserved_bytes == before.reserved_bytes - 4 * page &&
                after.committed_bytes == before.committed_bytes - 2 * page, "release reclaims reservation and remaining commitment");
        require(f.memory.available(base, 4 * page) && f.vm.query_address_protect(base + page) == 0,
                "released gap reusable with no guest protection");
        require(stopped([&] { f.memory.load<uint32_t>(base); }, "memory-access"), "released data inaccessible");
        require(f.memory.load<uint32_t>(base + 4 * page) == 0x99aabbcc, "release preserves neighbor");
        require(f.free_request(base, 0, 0x8000) == 0xc0000001 && f.size() == 0, "repeat release fails without output writes");
        require(f.allocate(base, 4 * page, reserve | commit) == 0, "released exact base can be allocated again");
        require(f.memory.load<uint32_t>(base) == 0 && f.memory.load<uint32_t>(base + 2 * page) == 0,
                "reused private backing has fresh zero data");
        require(f.free_request(base, 0, 0x8000) == 0, "reused allocation releases again");
        require(f.allocate(base, 4 * page, reserve) == 0 && f.free_request(base, 0, 0x8000) == 0,
                "reserved-only release succeeds");
    }
}

void release_guards_preserve_full_reservation() {
    Fixture f;
    require(f.allocate(0x10000000, 0x3000, reserve | commit) == 0, "release guard fixture");
    f.memory.store<uint32_t>(0x10000000, 0xabcdef01);
    const auto before = f.vm.statistics();
    require(f.free_request(0, 0, 0x8000) == 0xc00000a0, "null release base status");
    require(f.free_request(0x60000000, 0, 0x8000) == invalid, "release outside virtual arena");
    require(f.free_request(0x10001000, 0, 0x8000) == 0xc0000001, "interior page cannot release reservation");
    require(stopped([&] { f.free_request(0x10000001, 0, 0x8000); }, "virtual-memory-request"), "unaligned release explicit");
    require(stopped([&] { f.free_request(0x10000000, 1, 0x8000); }, "virtual-memory-request"), "nonzero release size unsupported");
    require(stopped([&] { f.free_request(0x10000000, 0, 0x8000, 1); }, "virtual-memory-request"), "debug release unsupported");
    f.memory.store<uint32_t>(0x70000000, 0x10000000);
    f.memory.store<uint32_t>(0x10002ffc, 0);
    require(f.vm.free(0x70000000, 0x10002ffc, 0x8000, 0) == invalid, "zero input size does not bypass full target alias guard");
    require(f.memory.load<uint32_t>(0x10002ffc) == 0 && f.memory.load<uint32_t>(0x70000000) == 0x10000000,
                "failed release retains both output words");
    require(f.memory.load<uint32_t>(0x10000000) == 0xabcdef01 &&
            f.vm.statistics().reserved_bytes == before.reserved_bytes && f.vm.statistics().committed_bytes == before.committed_bytes,
                "all failed releases preserve bytes and allocator metadata");
    for (uint32_t pointer : {0x70000000u, 0x70000004u}) {
        Fixture guarded;
        require(guarded.allocate(0x10000000, 0x1000, reserve | commit) == 0, "release readonly fixture");
        unsigned samples = 0;
        guarded.memory.add_read_only_word(pointer, [&] { ++samples; return 0u; });
        require(stopped([&] { guarded.vm.free(0x70000000, 0x70000004, 0x8000, 0); }, "memory-readonly"),
                "release readonly output guard");
        require(samples == 0 && !guarded.memory.available(0x10000000, 0x1000), "release guard runs before provider and effects");
    }
    for (uint32_t pointer : {0x10000ffeu, 0x10003ffeu}) {
        Fixture edge;
        require(edge.allocate(0x10000000, 0x1000, reserve | commit) == 0 &&
                edge.allocate(0x10001000, 0x3000, reserve | commit) == 0 &&
                edge.allocate(0x10004000, 0x1000, reserve | commit) == 0, "both alias edges mapped");
        edge.memory.store<uint32_t>(0x70000000, 0x10001000);
        edge.memory.store<uint32_t>(pointer, 0);
        const auto usage = edge.vm.statistics();
        require(edge.vm.free(0x70000000, pointer, 0x8000, 0) == invalid, "word straddles full release boundary");
        require(edge.memory.load<uint32_t>(pointer) == 0 && edge.vm.statistics().reserved_bytes == usage.reserved_bytes &&
                edge.vm.statistics().committed_bytes == usage.committed_bytes, "straddling output rejects before effects");
    }
}

void protection_queries_use_owned_reservations() {
    Fixture f;
    const auto before = f.vm.statistics();
    require(f.vm.query_address_protect(0x10000000) == 0, "free page in virtual arena has no protection");
    for (uint32_t address : {0x10000001u, 0x10000fffu, 0x40000001u, 0x4000ffffu})
        require(f.vm.query_address_protect(address) == 0, "unaligned free address queries its guest page");
    require(f.vm.statistics().reserved_bytes == before.reserved_bytes &&
            f.vm.statistics().committed_bytes == before.committed_bytes,
            "free protection query does not allocate memory");

    require(f.allocate(0x10001000, 1, reserve) == 0, "reserve-only query fixture allocates");
    const auto reserved = f.vm.statistics();
    require(f.vm.query_address_protect(0x10001000) == 4 &&
            f.vm.query_address_protect(0x10001fff) == 4,
            "reserve-only allocation reports protection over its rounded tail");
    require(stopped([&] { (void)f.memory.load<uint8_t>(0x10001000); }, "memory-access"),
            "reserve-only protection query does not make memory accessible");
    require(f.vm.statistics().reserved_bytes == reserved.reserved_bytes &&
            f.vm.statistics().committed_bytes == reserved.committed_bytes,
            "owned protection queries do not change usage");

    require(f.allocate(0x10003000, 1, reserve | commit) == 0, "committed query fixture allocates");
    f.memory.store<uint8_t>(0x10003000, 0xa5);
    require(f.vm.query_address_protect(0x10003000) == 4 && f.memory.load<uint8_t>(0x10003000) == 0xa5,
            "committed protection query preserves bytes");
    require(f.vm.query_address_protect(0x10002000) == 0, "free hole between owned allocations reports zero");

    f.memory.map(0x10004000, 1);
    require(stopped([&] { (void)f.vm.query_address_protect(0x10004000); }, "memory-protection"),
            "foreign mapping in virtual arena stops instead of guessing protection");
    require(stopped([&] { (void)f.vm.query_address_protect(0x30000000); }, "memory-protection"),
            "address outside supported virtual arenas stops explicitly");
}
}
int main() {
    try {
        reserve_then_commit(); rounding_and_arenas(); conflicts_and_exhaustion();
        invalid_requests(); unsupported_requests(); reservation_budget();
        readonly_outputs_reject_before_reserving_or_writing();
        protection_queries_use_owned_reservations();
        decommit_preserves_reservation_and_rounds_by_heap();
        decommit_rejections_preserve_state();
        decommit_guarded_outputs();
        release_returns_full_reservation_and_allows_zeroed_reuse();
        release_guards_preserve_full_reservation();
        std::cout << "Virtual memory checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
