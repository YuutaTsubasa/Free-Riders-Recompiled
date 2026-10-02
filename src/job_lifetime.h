#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace sfr {
// 8249FD50 tests the count at +4 of the list held at manager+184 before
// invoking actor destructors. Its other lists register/remove update entries.
template<class LoadWord>
bool has_deferred_job_contexts(uint32_t manager, LoadWord&& load) {
    return load(load(manager + 184) + 4) != 0;
}

// Original work batches may run beside the main thread, but their actor
// contexts must survive until both the dispatcher and its helpers are done.
// Deletion closes admission to the next batch while current helpers keep
// running concurrently. This is deliberately not a lock around callbacks.
class JobLifetime {
public:
    enum class Kind { batch, helper, deletion };
    class Lease {
    public:
        Lease(JobLifetime& gate, Kind kind) : gate_(gate), kind_(kind) {
            if (kind_ == Kind::deletion) {
                std::lock_guard lock(gate_.mutex_);
                ++gate_.waiting_deletions_;
            }
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() {
            std::lock_guard lock(gate_.mutex_);
            if (acquired_) {
                if (kind_ == Kind::batch) --gate_.batches_;
                else if (kind_ == Kind::helper) --gate_.helpers_;
                else gate_.deleting_ = false;
            } else if (kind_ == Kind::deletion) {
                --gate_.waiting_deletions_;
            }
            gate_.changed_.notify_all();
        }
        bool try_acquire() {
            std::lock_guard lock(gate_.mutex_);
            if (acquired_) return true;
            if (gate_.deleting_) return false;
            if (kind_ == Kind::batch) {
                if (gate_.waiting_deletions_) return false;
                ++gate_.batches_;
            } else if (kind_ == Kind::helper) {
                // A pending deletion must not prevent an existing batch's
                // helpers from finishing: the dispatcher waits for them.
                ++gate_.helpers_;
            } else {
                if (gate_.batches_ || gate_.helpers_) return false;
                --gate_.waiting_deletions_;
                gate_.deleting_ = true;
            }
            acquired_ = true;
            return true;
        }
    private:
        JobLifetime& gate_;
        Kind kind_;
        bool acquired_ = false;
    };
    bool helpers_idle() const {
        std::lock_guard lock(mutex_);
        return helpers_ == 0;
    }
    // Call only inside the runtime's host-wait wrapper, without either
    // execution permit. A bounded sleep lets that wrapper observe shutdown;
    // the caller rechecks the condition and never treats a timeout as done.
    void wait_for_change() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, std::chrono::milliseconds(1));
    }
private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    unsigned batches_ = 0, helpers_ = 0, waiting_deletions_ = 0;
    bool deleting_ = false;
};
}
