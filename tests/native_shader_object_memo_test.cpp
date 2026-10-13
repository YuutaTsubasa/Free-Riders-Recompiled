#include "native_shaders.h"
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void run() {
    using Memo = sfr::NativeShaderObjectMemo;
    constexpr uint32_t va = 0x41000000, vb = 0x41000020;
    constexpr uint32_t pa = 0x41000040, pb = 0x41000060, unknown = 0x42000000;
    std::unordered_map<uint32_t, uint32_t> objects{{va, 11}, {vb, 12}, {pa, 21}, {pb, 22}};
    std::vector<uint32_t> lookups;
    auto lookup = [&](uint32_t object) {
        lookups.push_back(object);
        const auto found = objects.find(object);
        return found == objects.end() ? 0u : found->second;
    };
    Memo memo;
    uint64_t manager = 1, generation = 1;
    uint32_t owner = 0x71600000;
    auto consume = [&](uint32_t vertex, uint32_t pixel) {
        return memo.resolve(manager, generation, owner, vertex, pixel, lookup);
    };
    require(lookups.empty(), "binding values are resolved only when consumed");
    require(consume(va, pa) == Memo::Bindings{11, 21} && lookups == std::vector<uint32_t>{va, pa},
            "first consumption resolves each stage once in VS then PS order");
    lookups.clear();
    for (unsigned i = 0; i < 8; ++i)
        require(consume(va, pa) == Memo::Bindings{11, 21}, "same pair keeps exact handles");
    require(lookups.empty(), "repeated draws perform no object map lookups");

    require(consume(vb, pa) == Memo::Bindings{12, 21} && lookups == std::vector<uint32_t>{vb},
            "changing only VS retains the resolved PS");
    lookups.clear();
    require(consume(vb, pb) == Memo::Bindings{12, 22} && lookups == std::vector<uint32_t>{pb},
            "changing only PS retains the resolved VS");
    lookups.clear();
    require(consume(va, pa) == Memo::Bindings{11, 21} && lookups == std::vector<uint32_t>{va, pa},
            "A to B to A resolves the restored pair without retaining an unbounded history");

    const auto queued = consume(va, pa);
    objects[va] = 13;
    ++generation;
    lookups.clear();
    require(consume(va, pa) == Memo::Bindings{13, 21} && lookups == std::vector<uint32_t>{va, pa},
            "reattaching the same guest address invalidates resolution despite unchanged binding words");
    require(queued == Memo::Bindings{11, 21}, "published handle values from an earlier draw remain unchanged");
    ++generation; // A successful attach of the same immutable handle is conservative too.
    lookups.clear();
    require(consume(va, pa) == Memo::Bindings{13, 21} && lookups.size() == 2,
            "every successful attach generation is part of the memo key");
    owner += 0x10000;
    lookups.clear();
    require(consume(va, pa) == Memo::Bindings{13, 21} && lookups.size() == 2,
            "a different device owner resolves both stages again");
    ++manager;
    generation = 1;
    objects[va] = 31;
    objects[pa] = 41;
    lookups.clear();
    require(consume(va, pa) == Memo::Bindings{31, 41} && lookups.size() == 2,
            "a new manager lifetime cannot reuse prior handles at identical guest addresses");
    memo.reset();
    lookups.clear();
    require(consume(va, pa) == Memo::Bindings{31, 41} && lookups.size() == 2,
            "explicit owner reset cannot reuse the previous publication");

    lookups.clear();
    require(!consume(0, pa) && lookups.empty(), "null VS fails at consumption without resolving PS");
    require(!consume(va, 0) && lookups.empty(), "null PS fails at consumption while known VS can be reused");
    require(!consume(unknown, pa) && lookups == std::vector<uint32_t>{unknown},
            "unknown VS is looked up once and fails before PS resolution");
    lookups.clear();
    require(!consume(vb, unknown) && lookups == std::vector<uint32_t>{vb, unknown},
            "a valid changed VS and unknown PS do not publish a partial pair");
    lookups.clear();
    require(consume(va, pa) == Memo::Bindings{31, 41} && lookups.empty(),
            "failed resolution leaves the previous valid publication usable");
    objects[unknown] = 42;
    ++generation;
    lookups.clear();
    require(consume(va, unknown) == Memo::Bindings{31, 42} && lookups == std::vector<uint32_t>{va, unknown},
            "a previously unknown object resolves after attach rather than retaining a negative result");
}
}
int main() {
    try {
        run();
        std::cout << "Shader object memo lifetime checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
