#include "stream_reader_gate.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// The reader's thread object is not stored yet (the dispatcher runs before
// 8249BE48 has made the reader), then it is, but the reader is not waiting:
// neither is ready, and a bounded wait gives up.
void not_ready_until_the_reader_waits() {
    sfr::StreamReaderGate gate;
    require(!gate.ready(0), "no reader yet");
    require(!gate.ready(0x40001000), "a reader that has not reached its wait");
    std::atomic<uint32_t> thread{0};
    const auto started = std::chrono::steady_clock::now();
    require(!gate.wait_until_ready([&] { return thread.load(); }, 20ms), "times out without a reader");
    require(std::chrono::steady_clock::now() - started >= 20ms, "waits the whole limit");
}

// The handoff waits while the reader is still on its way to its wait - the
// startup order that lost the first read - and goes on once it gets there.
void handoff_waits_for_the_reader() {
    sfr::StreamReaderGate gate;
    std::atomic<uint32_t> thread{0};
    std::atomic<bool> reader_waiting{false};
    std::thread reader([&] {
        std::this_thread::sleep_for(10ms);
        thread.store(0x40001000);  // 8249BE48 stores the reader's thread object
        std::this_thread::sleep_for(20ms);
        reader_waiting.store(true);
        gate.reader_idle(0x40001000);  // the reader cleared its request and waits
    });
    const bool ready = gate.wait_until_ready([&] { return thread.load(); }, 5000ms);
    require(ready, "the reader reached its wait");
    require(reader_waiting.load(), "no handoff before the reader waits");
    reader.join();
    require(gate.ready(0x40001000), "stays ready for later handoffs");
}

// A reader that waited once is ready at once for every later handoff, and a
// different thread object (a new manager's reader) is not.
void ready_belongs_to_one_reader() {
    sfr::StreamReaderGate gate;
    gate.reader_idle(0x40001000);
    const auto started = std::chrono::steady_clock::now();
    require(gate.wait_until_ready([] { return 0x40001000u; }, 5000ms), "ready reader");
    require(std::chrono::steady_clock::now() - started < 1000ms, "no wait for a ready reader");
    require(!gate.ready(0x40002000), "another reader has not waited");
}
}

int main() {
    try {
        not_ready_until_the_reader_waits();
        handoff_waits_for_the_reader();
        ready_belongs_to_one_reader();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "stream_reader_gate: 3 tests passed\n";
    return 0;
}
