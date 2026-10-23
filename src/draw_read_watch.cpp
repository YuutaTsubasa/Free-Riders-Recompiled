// SFR_TEST_DRAW_READS=1 (testing, Windows): finds the code that reads what a
// race frame's drawing wrote before the next frame's drawing rewrites it.
// Between the two, the pages holding such words are made inaccessible; each
// access faults, is recorded when it touches one of the words (the host
// instruction, which names the recompiled function through the link map, and
// the word's owner), and is let through by single-stepping it with the page
// readable, then protecting the page again.
#include "draw_read_watch.h"
#ifdef _WIN32
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <iostream>
#include <map>
#include <memory>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace sfr::draw_reads {
namespace {
uint8_t* guest_base = nullptr;
std::vector<Word> words;                    // sorted by address while a window is open
// Per word: written since the drawing (by anyone else), so no longer what the
// drawing left: a later read of it does not depend on the drawing.
std::unique_ptr<std::atomic<uint8_t>[]> overwritten;
std::vector<uint32_t> pages;                // the pages protected
std::unique_ptr<std::atomic<uint8_t>[]> page_marks;  // per 4 KiB guest page: 1 protected
std::atomic<bool> open{false};
std::atomic<int> protecting{0};  // handlers between their check of open and their protect
thread_local uint32_t reprotect_page = 0;
thread_local bool reprotect_pending = false;
struct Hit { uint64_t rip; uint32_t address; uint32_t vtable; uint32_t offset; uint32_t thread; uint32_t reader_kind; uint32_t reader_key; };
std::atomic<uint32_t> reader_kind{0}, reader_key{0};
std::array<Hit, 1 << 18> hits;
std::atomic<uint32_t> hit_count{0};
std::atomic<uint64_t> faults{0};
uint32_t (*thread_id)() = nullptr;

// Only pages that are plain read-write are taken: the runtime protects some
// itself (watched GPU buffers, computed read-only words), and changing those
// broke its own fault handling and stopped runs.
bool plain(uint32_t page) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(guest_base + page, &info, sizeof info) && info.State == MEM_COMMIT &&
           info.Protect == PAGE_READWRITE;
}

void protect(uint32_t page, DWORD access) {
    DWORD old = 0;
    VirtualProtect(guest_base + page, 4096, access, &old);
}

// Write capture (marks: 1 read-only, 3 being opened, 2 opened): during the
// drawing the committed pages are read-only; the
// first write to each saves the page as it was and opens it. Only the main
// thread's (thread 1) pages are kept: other threads write at the same time.
std::atomic<bool> capturing{false};
std::unique_ptr<std::atomic<uint8_t>[]> capture_marks;  // per page: 1 read-only for capture
constexpr uint32_t capture_limit = 1 << 15;
std::unique_ptr<uint8_t[]> capture_pool;  // capture_limit pages
std::array<uint32_t, capture_limit> captured_pages;
std::atomic<uint32_t> captured_count{0};
std::vector<std::pair<uint64_t, uint64_t>> capture_ranges;

LONG CALLBACK handler(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 &&
        record->ExceptionInformation[0] == 1 && guest_base && capture_marks) {
        const uintptr_t host = uintptr_t(record->ExceptionInformation[1]);
        if (host >= uintptr_t(guest_base) && host - uintptr_t(guest_base) < 0x100000000ull) {
            const uint32_t page = uint32_t(host - uintptr_t(guest_base)) & ~0xFFFu;
            uint8_t expected = 1;
            if (capture_marks[page >> 12].compare_exchange_strong(expected, 3)) {
                if (thread_id && thread_id() == 1) {
                    const uint32_t slot = captured_count.fetch_add(1, std::memory_order_relaxed);
                    if (slot < capture_limit) {
                        std::memcpy(capture_pool.get() + size_t(slot) * 4096, guest_base + page, 4096);
                        captured_pages[slot] = page;
                    }
                }
                protect(page, PAGE_READWRITE);
                capture_marks[page >> 12].store(2, std::memory_order_release);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // Another thread is opening it (3) or has (2): try again.
            if (expected >= 2) {
                if (expected == 3) SwitchToThread();
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && reprotect_pending) {
        reprotect_pending = false;
        // Only a page still watched: the window may have closed, and another
        // opened on other pages, while this instruction ran.
        protecting.fetch_add(1, std::memory_order_acq_rel);
        if (open.load(std::memory_order_acquire) && page_marks[reprotect_page >> 12].load(std::memory_order_acquire))
            protect(reprotect_page, PAGE_NOACCESS);
        protecting.fetch_sub(1, std::memory_order_acq_rel);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || !guest_base)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t host = uintptr_t(record->ExceptionInformation[1]);
    if (host < uintptr_t(guest_base) || host - uintptr_t(guest_base) >= 0x100000000ull) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t guest = uint32_t(host - uintptr_t(guest_base));
    if (!page_marks || !page_marks[guest >> 12].load(std::memory_order_acquire)) {
        // Not one of ours: say how the page stands, for when nobody takes it.
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(guest_base + (guest & ~0xFFFu), &mbi, sizeof mbi);
        // A page this watch had closed and has opened again since the
        // access faulted (end_capture on the main thread): run it again.
        if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE) return EXCEPTION_CONTINUE_EXECUTION;
        char text[160];
        const int length = std::snprintf(text, sizeof text,
            "DRAW_READS_PASS guest=0x%08x write=%u protect=0x%lx state=0x%lx capture=%u open=%u capturing=%u\n",
            guest, unsigned(record->ExceptionInformation[0]), mbi.Protect, mbi.State,
            capture_marks ? unsigned(capture_marks[guest >> 12].load()) : 9u, unsigned(open.load()), unsigned(capturing.load()));
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, DWORD(length), &written, nullptr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uint32_t page = guest & ~0xFFFu;
    protect(page, PAGE_READWRITE);
    if (!open.load(std::memory_order_acquire)) return EXCEPTION_CONTINUE_EXECUTION;  // a window already closed
    faults.fetch_add(1, std::memory_order_relaxed);
    // The word the access starts in. A write (a 16-byte one when aligned to
    // 16, as the vector stores are) replaces what the drawing left; only a
    // read of a word still as the drawing left it is a dependency.
    const uint32_t first = guest & ~3u;
    const bool write = record->ExceptionInformation[0] == 1;
    const uint32_t span = write && guest % 16 == 0 ? 16 : 4;
    auto found = std::lower_bound(words.begin(), words.end(), first,
                                  [](const Word& w, uint32_t a) { return w.address < a; });
    if (write) {
        for (; found != words.end() && found->address < first + span; ++found)
            overwritten[found - words.begin()].store(1, std::memory_order_relaxed);
    } else if (found != words.end() && found->address == first &&
               !overwritten[found - words.begin()].load(std::memory_order_relaxed)) {
        const uint32_t slot = hit_count.fetch_add(1, std::memory_order_relaxed);
        const uint32_t thread = thread_id ? thread_id() : 0;
        if (slot < hits.size())
            hits[slot] = {uint64_t(info->ContextRecord->Rip) - uint64_t(uintptr_t(GetModuleHandleW(nullptr))),
                          found->address, found->vtable, found->offset, thread,
                          thread == 1 ? reader_kind.load(std::memory_order_relaxed) : 0,
                          thread == 1 ? reader_key.load(std::memory_order_relaxed) : 0};
    }
    reprotect_page = page;
    reprotect_pending = true;
    info->ContextRecord->EFlags |= 0x100;  // trap after this instruction
    return EXCEPTION_CONTINUE_EXECUTION;
}
}

bool enabled() {
    static const bool on = [] { const char* t = std::getenv("SFR_TEST_DRAW_READS"); return t && *t == '1'; }();
    return on;
}

void begin(uint8_t* base, std::vector<Word> written, uint32_t (*current_thread)()) {
    static const bool installed = AddVectoredExceptionHandler(1, handler) != nullptr;
    (void)installed;
    if (!page_marks) page_marks = std::make_unique<std::atomic<uint8_t>[]>(1u << 20);
    guest_base = base;
    thread_id = current_thread;
    words = std::move(written);
    std::erase_if(words, [](const Word& w) { return !plain(w.address & ~0xFFFu); });
    std::sort(words.begin(), words.end(), [](const Word& a, const Word& b) { return a.address < b.address; });
    overwritten = std::make_unique<std::atomic<uint8_t>[]>(words.size() + 1);
    pages.clear();
    for (const auto& w : words)
        if (pages.empty() || pages.back() != (w.address & ~0xFFFu)) pages.push_back(w.address & ~0xFFFu);
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    for (const uint32_t page : pages) page_marks[page >> 12].store(1, std::memory_order_release);
    open.store(true, std::memory_order_release);
    for (const uint32_t page : pages) protect(page, PAGE_NOACCESS);
}

void begin_capture(uint8_t* base, std::vector<std::pair<uint64_t, uint64_t>> ranges, uint32_t (*current_thread)()) {
    static const bool installed = AddVectoredExceptionHandler(1, handler) != nullptr;
    (void)installed;
    if (!capture_marks) {
        capture_marks = std::make_unique<std::atomic<uint8_t>[]>(1u << 20);
        capture_pool = std::make_unique<uint8_t[]>(size_t(capture_limit) * 4096);
    }
    if (!page_marks) page_marks = std::make_unique<std::atomic<uint8_t>[]>(1u << 20);
    guest_base = base;
    thread_id = current_thread;
    captured_count.store(0);
    capture_ranges = std::move(ranges);
    for (const auto& [address, size] : capture_ranges) {
        // Whole guest pages only, and none the read watch has closed.
        const uint64_t first = (address + 4095) & ~4095ull, last = (address + size) & ~4095ull;
        for (uint64_t page = first; page < last;) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(guest_base + page, &info, sizeof info)) break;
            const uint64_t region_end = (std::min)(last, uint64_t(static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize - guest_base));
            const bool usable = info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE;
            for (; page < region_end; page += 4096)
                if (usable && !page_marks[page >> 12].load()) capture_marks[page >> 12].store(1);
        }
    }
    capturing.store(true, std::memory_order_release);
    for (const auto& [address, size] : capture_ranges) {
        const uint64_t first = (address + 4095) & ~4095ull, last = (address + size) & ~4095ull;
        uint64_t run = first;
        for (uint64_t page = first; page <= last; page += 4096) {
            const bool marked = page < last && capture_marks[page >> 12].load();
            if (!marked) {
                DWORD old = 0;
                if (page > run) VirtualProtect(guest_base + run, page - run, PAGE_READONLY, &old);
                run = page + 4096;
            }
        }
    }
}

namespace {
struct Copy { uint32_t address; uint32_t size; size_t at; };
std::vector<Copy> copies;
std::vector<uint8_t> copied;
}

void snapshot_physical(uint8_t* base, const std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
    copies.clear();
    size_t used = 0;
    for (const auto& [address, size] : ranges) {
        if (address < 0xA0000000u) continue;
        for (uint64_t at = address; at < address + size;) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(base + at, &info, sizeof info)) break;
            const uint64_t end = (std::min)(address + size, uint64_t(static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize - base));
            // Plain read-write only: not write-combined GPU buffers (slow to
            // read) nor pages the runtime or this watch protects.
            if (info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE) {
                copies.push_back({uint32_t(at), uint32_t(end - at), used});
                used += size_t(end - at);
            }
            at = end;
        }
    }
    if (copied.size() < used) copied.resize(used);
    for (const auto& c : copies) std::memcpy(copied.data() + c.at, base + c.address, c.size);
}

void diff_physical(uint8_t* base, const std::function<void(uint32_t)>& changed) {
    for (const auto& c : copies)
        for (uint32_t offset = 0; offset < c.size; offset += 4096) {
            const uint32_t length = (std::min)(4096u, c.size - offset);
            if (!std::memcmp(base + c.address + offset, copied.data() + c.at + offset, length)) continue;
            for (uint32_t word = 0; word < length; word += 4)
                if (std::memcmp(base + c.address + offset + word, copied.data() + c.at + offset + word, 4))
                    changed(c.address + offset + word);
        }
    copies.clear();
}

void rearm(const std::function<void(const CapturedPage&)>& written) {
    const uint32_t count = (std::min)(captured_count.load(), capture_limit);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t page = captured_pages[i];
        written({page, capture_pool.get() + size_t(i) * 4096});
        // Read-only again, saved again at its next write.
        uint8_t expected = 2;
        if (capture_marks[page >> 12].compare_exchange_strong(expected, 1)) protect(page, PAGE_READONLY);
    }
    captured_count.store(0);
}

std::vector<CapturedPage> end_capture() {
    capturing.store(false, std::memory_order_release);
    for (const auto& [address, size] : capture_ranges) {
        const uint64_t first = (address + 4095) & ~4095ull, last = (address + size) & ~4095ull;
        for (uint64_t page = first; page < last; page += 4096) {
            uint8_t expected = 1;
            if (capture_marks[page >> 12].compare_exchange_strong(expected, 0)) protect(uint32_t(page), PAGE_READWRITE);
        }
    }
    for (const auto& [address, size] : capture_ranges) {
        const uint64_t first = (address + 4095) & ~4095ull, last = (address + size) & ~4095ull;
        for (uint64_t page = first; page < last; page += 4096) capture_marks[page >> 12].store(0);
    }
    std::vector<CapturedPage> result;
    const uint32_t count = (std::min)(captured_count.load(), capture_limit);
    for (uint32_t i = 0; i < count; ++i)
        result.push_back({captured_pages[i], capture_pool.get() + size_t(i) * 4096});
    return result;
}

void end() {
    if (!open.exchange(false, std::memory_order_acq_rel)) return;
    while (protecting.load(std::memory_order_acquire)) std::this_thread::yield();
    for (const uint32_t page : pages) protect(page, PAGE_READWRITE);
    for (const uint32_t page : pages) page_marks[page >> 12].store(0, std::memory_order_release);
}

std::vector<Word> close() {
    end();
    std::vector<Word> kept;
    for (size_t i = 0; i < words.size(); ++i)
        if (!overwritten[i].load(std::memory_order_relaxed)) kept.push_back(words[i]);
    words.clear();
    return kept;
}

void set_reader(uint32_t kind, uint32_t key) {
    reader_kind.store(kind, std::memory_order_relaxed);
    reader_key.store(key, std::memory_order_relaxed);
}

// rip=<offset>:<owner kind>+<offset>@<reader kind>/<key>/t<thread>x<count>
void report(uint32_t frame) {
    const uint32_t count = (std::min<uint32_t>)(hit_count.exchange(0), uint32_t(hits.size()));
    std::map<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, uint64_t> by_reader;
    for (uint32_t i = 0; i < count; ++i)
        ++by_reader[{hits[i].rip, hits[i].vtable, hits[i].offset, hits[i].reader_kind, hits[i].reader_key, hits[i].thread}];
    std::cerr << "DRAW_READS frame=" << frame << " faults=" << faults.exchange(0) << " hits=" << count;
    for (const auto& [key, n] : by_reader)
        std::cerr << " rip=0x" << std::hex << std::get<0>(key) << ":0x" << std::get<1>(key) << "+" << std::get<2>(key)
                  << "@" << std::get<3>(key) << "/" << std::get<4>(key) << "/t" << std::dec << std::get<5>(key) << "x" << n;
    std::cerr << '\n';
}
}
#else
namespace sfr::draw_reads {
bool enabled() { return false; }
void begin(uint8_t*, std::vector<Word>, uint32_t (*)()) {}
void end() {}
std::vector<Word> close() { return {}; }
void set_reader(uint32_t, uint32_t) {}
void report(uint32_t) {}
void begin_capture(uint8_t*, std::vector<std::pair<uint64_t, uint64_t>>, uint32_t (*)()) {}
std::vector<CapturedPage> end_capture() { return {}; }
void rearm(const std::function<void(const CapturedPage&)>&) {}
void snapshot_physical(uint8_t*, const std::vector<std::pair<uint64_t, uint64_t>>&) {}
void diff_physical(uint8_t*, const std::function<void(uint32_t)>&) {}
}
#endif
