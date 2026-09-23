// NativeThread on POSIX hosts (native_thread.cpp is the Windows one): a
// pthread with a 16 MiB stack that waits at a gate until resumed, whose exit
// signals a portable thread-exit object (the handle guest waits use).
#include "native_thread.h"

#include "guest_memory.h"
#include "portable_waitables.h"

#include <pthread.h>
#include <sched.h>
#ifdef __APPLE__
#include <unistd.h>
#include <map>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <fstream>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef __APPLE__
// macOS cannot pin a thread to a processor. The processor a thread was given
// is remembered (so it reads back as on Linux) but only guides the scheduler
// as much as nothing does.
struct cpu_set_t { uint64_t bits; };
#define CPU_ZERO(set) ((set)->bits = 0)
#define CPU_SET(cpu, set) ((set)->bits |= uint64_t{1} << (cpu))
#define CPU_ISSET(cpu, set) (((set)->bits >> (cpu)) & 1)
namespace {
std::mutex apple_affinity_lock;
std::map<pthread_t, cpu_set_t> apple_affinity;
cpu_set_t all_processors() {
    cpu_set_t set{};
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    for (long cpu = 0; cpu < count && cpu < 64; ++cpu) CPU_SET(cpu, &set);
    return set;
}
int sched_getaffinity(int, size_t, cpu_set_t* set) {
    *set = all_processors();
    return 0;
}
}
#endif

namespace sfr {
namespace {
[[noreturn]] void throw_host_error(const char* operation, int error) {
    throw RuntimeStop("thread-host", uint32_t(error),
        std::string(operation) + " failed with error " + std::to_string(error));
}

uint64_t mask_of(const cpu_set_t& set) {
    uint64_t mask = 0;
    for (int cpu = 0; cpu < 64; ++cpu)
        if (CPU_ISSET(cpu, &set)) mask |= uint64_t{1} << cpu;
    return mask;
}

std::atomic<uint32_t> next_id{1};
}

// Bionic (Android) has no pthread_*affinity_np; a thread's kernel id works
// with sched_*affinity there.
static int set_thread_affinity(pthread_t thread, const cpu_set_t& set) {
#if defined(__APPLE__)
    std::lock_guard lock(apple_affinity_lock);
    apple_affinity[thread] = set;
    return 0;
#elif defined(__ANDROID__)
    return sched_setaffinity(pthread_gettid_np(thread), sizeof(set), &set) == 0 ? 0 : errno;
#else
    return pthread_setaffinity_np(thread, sizeof(set), &set);
#endif
}
static int get_thread_affinity(pthread_t thread, cpu_set_t& set) {
#if defined(__APPLE__)
    std::lock_guard lock(apple_affinity_lock);
    const auto found = apple_affinity.find(thread);
    set = found != apple_affinity.end() ? found->second : all_processors();
    return 0;
#elif defined(__ANDROID__)
    return sched_getaffinity(pthread_gettid_np(thread), sizeof(set), &set) == 0 ? 0 : errno;
#else
    return pthread_getaffinity_np(thread, sizeof(set), &set);
#endif
}

struct NativeThread::Impl {
    Entry entry;
    std::stop_source stop_source;
    pthread_t thread{};
    uint32_t id = 0;
    std::atomic<bool> started = false;
    std::atomic<bool> completed = false;
    std::exception_ptr failure;
    uint32_t exit_code = 0;
    // The suspend count and the gate a new thread waits at until it is 0.
    std::mutex gate_mutex;
    std::condition_variable gate;
    uint32_t suspend_count = 1;
    bool joined = false;
    int32_t priority = 0;
    uint64_t allowed_affinity = 0;
    portable::WaitablePtr exit = portable::make_thread_exit();

    Impl(Entry callback, uint64_t allowed) : entry(std::move(callback)), allowed_affinity(allowed) {}

    static void* trampoline(void* raw) noexcept {
        auto& self = *static_cast<Impl*>(raw);
        {
            std::unique_lock lock(self.gate_mutex);
            self.gate.wait(lock, [&] { return self.suspend_count == 0; });
        }
        uint32_t result = 0;
        if (!self.stop_source.stop_requested()) {
            self.started.store(true, std::memory_order_release);
            try {
                result = self.entry(self.stop_source.get_token());
            } catch (...) {
                self.failure = std::current_exception();
            }
        }
        self.exit_code = result;
        self.completed.store(true, std::memory_order_release);
        portable::set_event(*self.exit);
        return nullptr;
    }
};

NativeThread::NativeThread(Entry entry) {
    if (!entry) throw RuntimeStop("thread-host", 0, "native thread entry is empty");
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) throw_host_error("sched_getaffinity", errno);
    const uint64_t process_mask = mask_of(allowed);
    if (process_mask == 0) throw RuntimeStop("thread-host", 0, "process has no allowed processors");

    auto state = std::make_unique<Impl>(std::move(entry), process_mask);
    state->id = next_id.fetch_add(1);
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 16u * 1024u * 1024u);
    const int created = pthread_create(&state->thread, &attributes, &Impl::trampoline, state.get());
    pthread_attr_destroy(&attributes);
    if (created != 0) throw_host_error("pthread_create", created);
    impl_ = std::move(state);
}

NativeThread::~NativeThread() noexcept {
    if (!impl_) return;
    cancel_and_join();
}

uint32_t NativeThread::native_id() const { return impl_->id; }
bool NativeThread::entry_started() const { return impl_->started.load(std::memory_order_acquire); }
bool NativeThread::suspended() const {
    std::lock_guard lock(impl_->gate_mutex);
    return impl_->suspend_count != 0;
}
void* NativeThread::native_handle() const { return impl_->exit.get(); }

uint32_t NativeThread::resume() {
    uint32_t previous;
    {
        std::lock_guard lock(impl_->gate_mutex);
        previous = impl_->suspend_count;
        if (impl_->suspend_count) --impl_->suspend_count;
    }
    if (previous == 1) impl_->gate.notify_all();
    return previous;
}

uint32_t NativeThread::join() {
    if (suspended())
        throw RuntimeStop("thread-host", impl_->id, "cannot join a thread that has never been resumed");
    if (!impl_->joined) {
        const int result = pthread_join(impl_->thread, nullptr);
        if (result != 0) throw_host_error("pthread_join", result);
        if (!impl_->completed.load(std::memory_order_acquire))
            throw RuntimeStop("thread-host", impl_->id, "thread exited without publishing completion");
        impl_->joined = true;
    }
    if (impl_->failure) std::rethrow_exception(impl_->failure);
    return impl_->exit_code;
}

void NativeThread::request_stop() noexcept { impl_->stop_source.request_stop(); }

void NativeThread::cancel_and_join() noexcept {
    request_stop();
    if (impl_->joined) return;
    {
        std::lock_guard lock(impl_->gate_mutex);
        impl_->suspend_count = 0;
    }
    impl_->gate.notify_all();
    if (pthread_join(impl_->thread, nullptr) != 0) std::terminate();
    if (!impl_->completed.load(std::memory_order_acquire)) std::terminate();
    impl_->joined = true;
}

// Relative priorities are recorded but not applied: raising a thread above
// normal needs privileges an ordinary Linux user lacks.
int32_t NativeThread::priority() const { return impl_->priority; }

int32_t NativeThread::set_priority(int32_t host_relative) {
    if (host_relative < -2 || host_relative > 2)
        throw RuntimeStop("thread-host", static_cast<uint32_t>(host_relative),
            "thread priority must be one of -2, -1, 0, 1, or 2");
    return std::exchange(impl_->priority, host_relative);
}

namespace {
// The allowed processors, fastest first. A phone's cores are not alike: on
// this Snapdragon, CPU 0 and 1 run at 2.27 GHz and CPU 7 at 3.30 GHz, and
// taking them in index order put the guest's processor 0 - the title's main
// thread - on the slowest core of the eight. Where every core reports the
// same clock (or none reports one, as on a desktop Linux without cpufreq)
// the order is the index order, as it was.
std::vector<int> processors_by_speed(uint64_t allowed) {
    std::vector<int> order;
    for (int bit = 0; bit < 64; ++bit)
        if (allowed >> bit & 1) order.push_back(bit);
    std::vector<long> speed(order.size(), 0);
    for (size_t i = 0; i < order.size(); ++i) {
        std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(order[i]) +
                           "/cpufreq/cpuinfo_max_freq");
        if (file) file >> speed[i];
    }
    std::vector<size_t> index(order.size());
    for (size_t i = 0; i < index.size(); ++i) index[i] = i;
    std::stable_sort(index.begin(), index.end(),
                     [&](size_t a, size_t b) { return speed[a] > speed[b]; });
    std::vector<int> sorted;
    sorted.reserve(index.size());
    for (const size_t i : index) sorted.push_back(order[i]);
    return sorted;
}
}

uint64_t NativeThread::set_guest_processor(uint32_t guest_cpu) {
    if (guest_cpu >= 6) throw RuntimeStop("thread-host", guest_cpu, "guest processor index must be below 6");
    static std::mutex order_lock;
    static uint64_t ordered_for = 0;
    static std::vector<int> ordered;
    std::vector<int> processors;
    {
        std::lock_guard guard(order_lock);
        if (ordered.empty() || ordered_for != impl_->allowed_affinity) {
            ordered = processors_by_speed(impl_->allowed_affinity);
            ordered_for = impl_->allowed_affinity;
        }
        processors = ordered;
    }
    const int selected = processors.empty() ? -1 : processors[guest_cpu % processors.size()];
    if (selected < 0) throw RuntimeStop("thread-host", guest_cpu, "could not select an allowed host processor");
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(selected, &set);
    const int result = set_thread_affinity(impl_->thread, set);
    if (result != 0) throw_host_error("pthread_setaffinity_np", result);
    return uint64_t{1} << selected;
}

uint64_t NativeThread::affinity_mask() const {
    cpu_set_t set;
    CPU_ZERO(&set);
    const int result = get_thread_affinity(impl_->thread, set);
    if (result != 0) throw_host_error("pthread_getaffinity_np", result);
    return mask_of(set);
}
}
