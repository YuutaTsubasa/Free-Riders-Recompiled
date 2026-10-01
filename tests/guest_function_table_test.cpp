#include "guest_function_table.h"
#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

static void first(int& result) { result = 7; }
static void second(int& result) { result = 19; }
static void require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}
template<class F> static void invalid(F operation) {
    try { operation(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("malformed table/mapping was accepted");
}

int main() {
    try {
        using Function = void(int&);
        sfr::GuestFunctionTable<Function> table(0x82210000, 0x4000);
        require(table.size() == 0 && !table.find(0x82210000), "empty table");
        require(table.insert(0x82210000, first), "insert first mapping");
        require(table.insert(0x82213FFC, second), "insert last mapping");
        require(!table.insert(0x82210000, second), "duplicates preserve first mapping");
        require(table.size() == 2, "duplicate does not change size");
        int result = 0;
        table.find(0x82210000)(result);
        require(result == 7, "actual first function dispatch");
        table.find(0x82213FFC)(result);
        require(result == 19, "actual last function dispatch");
        for (uint32_t address : {0u, 0x8220FFFCu, 0x82210001u, 0x82210002u,
                                0x82210003u, 0x82210004u, 0x82214000u, 0xFFFFFFFFu})
            require(!table.contains(address), "hole, misalignment or outside address");
        invalid([&] { table.insert(0x82210001, first); });
        invalid([&] { table.insert(0x82214000, first); });
        invalid([&] { table.insert(0x82210004, nullptr); });
        invalid([] { sfr::GuestFunctionTable<Function> bad(1, 4); });
        invalid([] { sfr::GuestFunctionTable<Function> bad(0, 3); });
        invalid([] { sfr::GuestFunctionTable<Function> bad(0xFFFFFFFC, 8); });
        sfr::GuestFunctionTable<Function> top(0xFFFFFFFC, 4);
        require(top.insert(0xFFFFFFFC, first) && top.find(0xFFFFFFFC) == first && !top.find(0),
                "range ending exactly at 4 GiB does not wrap");
        sfr::GuestFunctionTable<Function> empty(0, 0);
        require(!empty.find(0) && !empty.find(0xFFFFFFFF), "zero-sized range");

        // Compare exact keys and holes with the previous implementation, then
        // read the published table concurrently (no mutation after startup).
        constexpr uint32_t base = 0x82210000, bytes = 0x20000;
        sfr::GuestFunctionTable<Function> many(base, bytes);
        std::unordered_map<uint32_t, Function*> expected;
        uint32_t random = 0x1234abcd;
        for (unsigned i = 0; i != 20000; ++i) {
            random = random * 1664525 + 1013904223;
            const uint32_t address = base + (random % bytes & ~3u);
            auto* function = i & 1 ? first : second;
            require(many.insert(address, function) == expected.emplace(address, function).second,
                    "insertion agrees with prior map");
        }
        require(many.size() == expected.size(), "unique mapping count agrees");
        std::atomic<bool> valid{true};
        const auto& reference = expected;
        std::array<std::thread, 4> readers;
        for (auto& reader : readers) reader = std::thread([&] {
            for (uint32_t offset = 0; offset < bytes; ++offset) {
                const auto found = reference.find(base + offset);
                auto* want = found == reference.end() ? nullptr : found->second;
                if (many.find(base + offset) != want) valid = false;
            }
        });
        for (auto& reader : readers) reader.join();
        require(valid, "concurrent exact dispatch matches old map for every byte address");
        std::cout << "guest function table tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
