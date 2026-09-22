#include "portable_waitables.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
namespace p = sfr::portable;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void events_follow_their_reset_mode() {
    auto automatic = p::make_event(false, true);
    p::Waitable* one[] = {automatic.get()};
    require(p::wait_any(one, 0) == 0, "a set auto-reset event is taken");
    require(p::wait_any(one, 0) == p::wait_timeout, "and reset by the wait that took it");
    auto manual = p::make_event(true, false);
    p::Waitable* both[] = {manual.get()};
    require(p::wait_any(both, 0) == p::wait_timeout, "an unset event times out");
    p::set_event(*manual);
    require(p::wait_any(both, 0) == 0 && p::wait_any(both, 0) == 0, "a manual-reset event stays set");
    p::reset_event(*manual);
    require(p::wait_any(both, 0) == p::wait_timeout, "until it is reset");
}

void semaphores_count_and_refuse_overflow() {
    auto semaphore = p::make_semaphore(1, 2);
    p::Waitable* one[] = {semaphore.get()};
    int32_t previous = -1;
    require(p::release_semaphore(*semaphore, 1, &previous) && previous == 1, "a release reports the previous count");
    require(!p::release_semaphore(*semaphore, 1, &previous), "passing the maximum is refused");
    require(p::wait_any(one, 0) == 0 && p::wait_any(one, 0) == 0 && p::wait_any(one, 0) == p::wait_timeout,
            "each wait takes one count");
    bool threw = false;
    try { p::make_semaphore(3, 2); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "an initial count above the maximum is invalid");
}

void wait_any_takes_the_first_signaled() {
    auto a = p::make_event(false, false), b = p::make_event(false, true), c = p::make_event(false, true);
    p::Waitable* objects[] = {a.get(), b.get(), c.get()};
    require(p::wait_any(objects, 0) == 1, "the lowest signaled index is returned");
    p::Waitable* c_only[] = {c.get()};
    require(p::wait_any(c_only, 0) == 0, "later objects are left untaken");
}

void wait_all_takes_everything_or_nothing() {
    auto event = p::make_event(false, true);
    auto semaphore = p::make_semaphore(0, 5);
    p::Waitable* objects[] = {event.get(), semaphore.get()};
    require(p::wait_all(objects, 0) == p::wait_timeout, "wait-all needs every object");
    p::Waitable* event_only[] = {event.get()};
    require(p::wait_any(event_only, 0) == 0, "a failed wait-all took nothing");
    p::set_event(*event);
    p::release_semaphore(*semaphore, 1, nullptr);
    require(p::wait_all(objects, 0) == 0, "wait-all succeeds once all are signaled");
    require(p::wait_any(event_only, 0) == p::wait_timeout, "and takes every object");
    p::release_semaphore(*semaphore, 1, nullptr);
    p::Waitable* twice[] = {semaphore.get(), semaphore.get()};
    require(p::wait_all(twice, 0) == p::wait_timeout, "one semaphore listed twice needs two counts");
}

void waits_wake_time_out_and_cancel() {
    auto event = p::make_event(false, false);
    p::Waitable* one[] = {event.get()};
    std::thread setter([&] {
        std::this_thread::sleep_for(20ms);
        p::set_event(*event);
    });
    require(p::wait_any(one, p::infinite) == 0, "a waiter wakes when another thread signals");
    setter.join();

    const auto start = std::chrono::steady_clock::now();
    require(p::wait_any(one, 30) == p::wait_timeout, "a finite wait times out");
    require(std::chrono::steady_clock::now() - start >= 25ms, "after about its timeout");

    std::stop_source stop;
    std::thread canceller([&] {
        std::this_thread::sleep_for(20ms);
        stop.request_stop();
    });
    require(p::wait_any(one, p::infinite, stop.get_token()) == p::wait_cancelled, "a stop request cancels the wait");
    canceller.join();
    std::stop_source stopped;
    stopped.request_stop();
    require(p::wait_all(one, p::infinite, stopped.get_token()) == p::wait_cancelled, "an already stopped wait returns at once");
}

void many_threads_share_a_semaphore() {
    auto semaphore = p::make_semaphore(0, 1000);
    std::atomic<int> taken{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i)
        workers.emplace_back([&] {
            p::Waitable* one[] = {semaphore.get()};
            for (int n = 0; n < 50; ++n)
                if (p::wait_any(one, 2000) == 0) ++taken;
        });
    for (int n = 0; n < 400; ++n) p::release_semaphore(*semaphore, 1, nullptr);
    for (auto& worker : workers) worker.join();
    require(taken == 400, "every count is taken exactly once");
}
}

int main() {
    try {
        events_follow_their_reset_mode();
        semaphores_count_and_refuse_overflow();
        wait_any_takes_the_first_signaled();
        wait_all_takes_everything_or_nothing();
        waits_wake_time_out_and_cancel();
        many_threads_share_a_semaphore();
        std::cout << "Portable waitable checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
