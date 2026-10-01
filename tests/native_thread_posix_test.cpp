#include "native_thread.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

// A guest worker may create workers for other guest processors. Pinning the
// parent must not narrow the child's set of available host processors.
int main(int argc, char** argv) {
    try {
#ifdef __ANDROID__
        const bool pinned = argc > 1 && std::string(argv[1]) == "--pinned";
        if (pinned) setenv("SFR_WORKER_AFFINITY", "1", 1);
        else unsetenv("SFR_WORKER_AFFINITY");
        if (!pinned) {
            sfr::NativeThread worker([](std::stop_token) { return 0; });
            const auto inherited = worker.affinity_mask();
            for (uint32_t cpu = 0; cpu < 6; ++cpu) {
                worker.set_guest_processor(cpu);
                if (worker.affinity_mask() != inherited)
                    throw std::runtime_error("Android guest affinity narrowed the inherited host CPU set");
            }
            bool refused = false;
            try { worker.set_guest_processor(6); }
            catch (const std::exception&) { refused = true; }
            if (!refused) throw std::runtime_error("invalid guest CPU was accepted");
        }
#endif
        std::array<uint64_t, 6> expected{};
        sfr::NativeThread reference([](std::stop_token) { return 0; });
        for (uint32_t cpu = 0; cpu < expected.size(); ++cpu)
            expected[cpu] = reference.set_guest_processor(cpu);

        sfr::NativeThread parent([&](std::stop_token) {
            sfr::NativeThread child([](std::stop_token) { return 0; });
            for (uint32_t cpu = 0; cpu < expected.size(); ++cpu) {
                const auto actual = child.set_guest_processor(cpu);
                std::cout << "guest_cpu=" << cpu << " expected=" << expected[cpu]
                          << " actual=" << actual << '\n';
                if (actual != expected[cpu] || child.affinity_mask() != expected[cpu])
                    throw std::runtime_error("child inherited the parent's single-core restriction");
            }
            return 0;
        });
        parent.set_guest_processor(5);
        parent.resume();
        parent.join();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
