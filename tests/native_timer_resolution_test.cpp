#include "native_timer_resolution.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Requests {
    bool success = true;
    unsigned begins = 0, ends = 0, outstanding = 0;
    uint32_t begun_period = 0, ended_period = 0;

    sfr::NativeTimerResolution::Api api() {
        return {this,
            [](void* context, uint32_t period) noexcept {
                auto& requests = *static_cast<Requests*>(context);
                ++requests.begins;
                requests.begun_period = period;
                if (requests.success) ++requests.outstanding;
                return requests.success;
            },
            [](void* context, uint32_t period) noexcept {
                auto& requests = *static_cast<Requests*>(context);
                ++requests.ends;
                requests.ended_period = period;
                --requests.outstanding;
            }};
    }
};

void balances_successful_requests() {
    Requests requests;
    {
        sfr::NativeTimerResolution first(true, requests.api());
        require(first.active(), "successful request must be active");
        require(requests.begins == 1 && requests.outstanding == 1 && requests.ends == 0,
                "request must remain owned during scope");
        {
            sfr::NativeTimerResolution second(true, requests.api());
            require(second.active() && requests.outstanding == 2, "nested owners retain separate requests");
        }
        require(requests.outstanding == 1 && requests.ends == 1, "nested owner releases only its request");
    }
    require(requests.outstanding == 0 && requests.begins == 2 && requests.ends == 2,
            "each successful request must be released exactly once");
    require(requests.begun_period == 1 && requests.ended_period == 1,
            "release must match the requested 1 ms period");
}

void disabled_and_failed_requests_do_not_release() {
    Requests requests;
    {
        sfr::NativeTimerResolution disabled(false, requests.api());
        require(!disabled.active(), "disabled request must be inactive");
    }
    require(requests.begins == 0 && requests.ends == 0, "disabled mode must not change timer resolution");
    requests.success = false;
    {
        sfr::NativeTimerResolution failed(true, requests.api());
        require(!failed.active(), "failed request must be inactive");
    }
    require(requests.begins == 1 && requests.ends == 0 && requests.outstanding == 0,
            "failed begin must not be paired with end");
}

void unwinding_releases_request() {
    Requests requests;
    try {
        sfr::NativeTimerResolution resolution(true, requests.api());
        throw 7;
    } catch (int) {}
    require(requests.begins == 1 && requests.ends == 1 && requests.outstanding == 0,
            "exception unwinding must release the owned request");
}

void incomplete_api_does_not_acquire() {
    Requests requests;
    auto api = requests.api();
    api.end = nullptr;
    sfr::NativeTimerResolution missing_end(true, api);
    require(!missing_end.active() && requests.begins == 0,
            "must not acquire a request without a matching release function");
    api = requests.api();
    api.begin = nullptr;
    sfr::NativeTimerResolution missing_begin(true, api);
    require(!missing_begin.active() && requests.ends == 0,
            "incomplete API must remain inactive");
}

// Optional diagnostic only: scheduler timing varies with host load, so it is
// measured rather than asserted. Use separate processes for off/on comparison.
int calibrate(bool enabled) {
    sfr::NativeTimerResolution resolution(enabled);
    std::cout << "TIMER_CALIBRATION enabled=" << enabled << " active=" << resolution.active() << '\n';
    for (int requested_us : {50, 100, 1000, 2000, 10000}) {
        std::vector<double> samples;
        double sum = 0;
        for (int sample = 0; sample < 32; ++sample) {
            const auto before = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::microseconds(requested_us));
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - before).count();
            samples.push_back(us);
            sum += us;
        }
        std::sort(samples.begin(), samples.end());
        std::cout << "TIMER_SLEEP requested_us=" << requested_us << " samples=" << samples.size()
                  << " mean_us=" << sum / samples.size() << " p50_us=" << samples[16]
                  << " p95_us=" << samples[30] << " max_us=" << samples.back() << '\n';
    }
    return enabled && !resolution.active() ? 1 : 0;
}
}

static_assert(!std::is_copy_constructible_v<sfr::NativeTimerResolution>);
static_assert(!std::is_move_constructible_v<sfr::NativeTimerResolution>);

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--calibrate") {
            if (std::string_view(argv[2]) == "off") return calibrate(false);
            if (std::string_view(argv[2]) == "on") return calibrate(true);
        }
        if (argc != 1) throw std::runtime_error("usage: native_timer_resolution_test [--calibrate off|on]");
        balances_successful_requests();
        disabled_and_failed_requests_do_not_release();
        unwinding_releases_request();
        incomplete_api_does_not_acquire();
        sfr::NativeTimerResolution disabled(false);
        require(!disabled.active(), "native disabled mode must remain inactive");
#ifndef _WIN32
        sfr::NativeTimerResolution portable;
        require(!portable.active(), "non-Windows request must be a no-op");
#endif
        std::cout << "native timer resolution tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
