#include "race_progress_probe.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr uint32_t global = 0x83E52F8C;
constexpr uint32_t object = 0x1000;
constexpr uint32_t controller = 0x2000;
constexpr std::array<uint32_t, 13> expected_offsets{
    20, 96, 100, 104, 116, 120, 128, 132, 136, 140, 148, 152, 156};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Exposes only reads to the probe. Setup methods are deliberately separate
// from the const interface used by read_race_progress_probe.
class MockMemory {
public:
    void region(uint32_t start, uint32_t size) { regions_.emplace_back(start, size); }
    void word(uint32_t address, uint32_t value) { words_[address] = value; }
    void sequence(uint32_t address, std::vector<uint32_t> values) {
        sequences_[address] = std::move(values);
    }

    bool readable(uint64_t address, uint32_t size) const {
        if (address + size > 0x100000000ull) return false;
        for (const auto& [start, length] : regions_)
            if (address >= start && address + size <= uint64_t(start) + length) return true;
        return false;
    }

    template<class T>
    T load(uint64_t address) const {
        static_assert(sizeof(T) == sizeof(uint32_t));
        if (!readable(address, sizeof(T))) throw std::runtime_error("probe read outside readable memory");
        const uint32_t key = static_cast<uint32_t>(address);
        auto script = sequences_.find(key);
        if (script != sequences_.end()) {
            auto& index = sequence_reads_[key];
            if (index < script->second.size()) return static_cast<T>(script->second[index++]);
        }
        auto found = words_.find(key);
        if (found == words_.end()) throw std::runtime_error("probe read an unset word");
        return static_cast<T>(found->second);
    }

private:
    std::vector<std::pair<uint32_t, uint32_t>> regions_;
    std::map<uint32_t, uint32_t> words_;
    std::map<uint32_t, std::vector<uint32_t>> sequences_;
    mutable std::map<uint32_t, size_t> sequence_reads_;
};

MockMemory base_memory() {
    MockMemory memory;
    memory.region(global, 4);
    memory.region(object, 160);
    memory.region(controller, 16);
    memory.word(global, object);
    memory.word(object, 0x11112222);
    memory.word(object + 80, controller);
    memory.word(controller, 0x33334444);
    memory.word(controller + 12, 0x55667788);
    for (unsigned i = 0; i < expected_offsets.size(); ++i)
        memory.word(object + expected_offsets[i], 0xA0000000u + i);
    return memory;
}

void absent_global_and_object() {
    MockMemory missing;
    const auto unreadable = sfr::read_race_progress_probe(missing);
    require(unreadable.status == sfr::RaceProbeStatus::unreadable, "unreadable global is rejected");

    MockMemory zero;
    zero.region(global, 4);
    zero.word(global, 0);
    const auto absent = sfr::read_race_progress_probe(zero);
    require(absent.status == sfr::RaceProbeStatus::absent && absent.object == 0,
            "null race object is absent");
    std::ostringstream output;
    sfr::write_race_progress_probe(output, absent);
    require(output.str().find("race_sample=absent") != std::string::npos,
            "absent status is emitted");
    require(output.str().find("race_state_raw=") == std::string::npos &&
            output.str().find("race_raw_") == std::string::npos,
            "absent sample omits raw values");
}

void invalid_object_pointers_are_rejected_before_dereference() {
    for (uint32_t pointer : {0x1001u, 0xFFFFFF80u}) {
        MockMemory memory;
        memory.region(global, 4);
        memory.word(global, pointer);
        const auto sample = sfr::read_race_progress_probe(memory);
        require(sample.status == sfr::RaceProbeStatus::unreadable,
                "misaligned or wrapping object is unreadable");
    }

    MockMemory unreadable;
    unreadable.region(global, 4);
    unreadable.word(global, 0x3000);
    const auto sample = sfr::read_race_progress_probe(unreadable);
    require(sample.status == sfr::RaceProbeStatus::unreadable,
            "unreadable object is rejected before loading its vtable");
}

void invalid_controller_pointers_are_rejected_before_dereference() {
    for (uint32_t pointer : {0u, 0x4001u, 0xFFFFFFF8u}) {
        auto memory = base_memory();
        memory.word(object + 80, pointer);
        const auto sample = sfr::read_race_progress_probe(memory);
        require(sample.status == sfr::RaceProbeStatus::unreadable,
                "null, misaligned, or wrapping controller is unreadable");
    }

    auto unreadable = base_memory();
    unreadable.word(object + 80, 0x3000);
    const auto sample = sfr::read_race_progress_probe(unreadable);
    require(sample.status == sfr::RaceProbeStatus::unreadable,
            "unreadable controller is rejected before loading it");
}

void complete_sample_uses_constructor_offsets() {
    const auto sample = sfr::read_race_progress_probe(base_memory());
    require(sample.status == sfr::RaceProbeStatus::complete, "valid memory yields complete sample");
    require(sample.object == object && sample.vtable == 0x11112222, "object identity and vtable are captured");
    require(sample.controller == controller && sample.controller_vtable == 0x33334444,
            "controller identity and vtable are captured");
    require(sample.state == 0x55667788, "controller state remains raw");
    require(sample.config.size() == expected_offsets.size(), "snapshot has the expected number of fields");
    for (unsigned i = 0; i < expected_offsets.size(); ++i)
        require(sample.config[i] == 0xA0000000u + i,
                "all independent constructor offsets are read in declared order");

    std::ostringstream output;
    sfr::write_race_progress_probe(output, sample);
    const std::string line = output.str();
    require(line.find("race_sample=complete") != std::string::npos, "complete status is emitted");
    require(line.find("race_state_raw=1432778632") != std::string::npos, "raw state is emitted");
    for (unsigned i = 0; i < expected_offsets.size(); ++i) {
        const std::string field = "race_raw_" + std::to_string(expected_offsets[i]) + "=" +
                                  std::to_string(sample.config[i]);
        require(line.find(field) != std::string::npos, "each constructor byte offset is emitted");
    }
}

void ownership_changes_are_rejected_and_raw_fields_are_omitted() {
    auto object_changed = base_memory();
    object_changed.sequence(global, {object, object + 0x100});
    const auto first = sfr::read_race_progress_probe(object_changed);
    require(first.status == sfr::RaceProbeStatus::changed, "changed global object identity is detected");

    auto controller_changed = base_memory();
    controller_changed.sequence(object + 80, {controller, controller + 0x100});
    const auto second = sfr::read_race_progress_probe(controller_changed);
    require(second.status == sfr::RaceProbeStatus::changed, "changed controller identity is detected");

    auto object_vtable_changed = base_memory();
    object_vtable_changed.sequence(object, {0x11112222, 0x9999AAAA});
    const auto third = sfr::read_race_progress_probe(object_vtable_changed);
    require(third.status == sfr::RaceProbeStatus::changed, "changed object vtable is detected");

    auto controller_vtable_changed = base_memory();
    controller_vtable_changed.sequence(controller, {0x33334444, 0xBBBBCCCC});
    const auto fourth = sfr::read_race_progress_probe(controller_vtable_changed);
    require(fourth.status == sfr::RaceProbeStatus::changed, "changed controller vtable is detected");

    for (const auto& sample : {first, second, third, fourth}) {
        std::ostringstream output;
        sfr::write_race_progress_probe(output, sample);
        const std::string line = output.str();
        require(line.find("race_sample=changed") != std::string::npos, "changed status is emitted");
        require(line.find("race_state_raw=") == std::string::npos &&
                line.find("race_raw_") == std::string::npos,
                "changed sample omits raw fields");
    }

    sfr::RaceProgressProbe unreadable;
    std::ostringstream output;
    sfr::write_race_progress_probe(output, unreadable);
    require(output.str().find("race_state_raw=") == std::string::npos &&
            output.str().find("race_raw_") == std::string::npos,
            "unreadable sample omits raw fields");
}
}

int main() {
    try {
        absent_global_and_object();
        invalid_object_pointers_are_rejected_before_dereference();
        invalid_controller_pointers_are_rejected_before_dereference();
        complete_sample_uses_constructor_offsets();
        ownership_changes_are_rejected_and_raw_fields_are_omitted();
        std::cout << "Race progress probe checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
