#include "native_notifications.h"

#include "guest_memory.h"
#include "native_sync_objects.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sfr {
namespace {
constexpr uint32_t status_success = 0;
constexpr uint32_t status_invalid_handle = 0xc0000008u;
constexpr size_t maximum_listeners = 1024;
constexpr size_t maximum_items = 4096;

[[noreturn]] void stop(const char* category, uint32_t value, const char* detail) {
    throw RuntimeStop(category, value, detail);
}
}

struct NativeNotifications::Impl {
    struct Item { uint32_t id, parameter; };
    struct Listener {
        uint64_t mask;
        uint32_t maximum_version;
        std::unique_ptr<NativeSyncObjects::RetainedEvent> event;
        std::vector<Item> items;
        // Items held back until this listener has drained once (see
        // publish_when_drained).
        std::vector<Item> deferred;
    };

    Impl(GuestMemory& guest_memory, NativeSyncObjects& native_sync)
        : memory(guest_memory), sync(native_sync) {}
    GuestMemory& memory;
    NativeSyncObjects& sync;
    mutable std::mutex mutex;
    std::unordered_map<uint32_t, Listener> listeners;
};

NativeNotifications::NativeNotifications(GuestMemory& memory, NativeSyncObjects& sync_objects)
    : impl_(std::make_unique<Impl>(memory, sync_objects)) {}

NativeNotifications::~NativeNotifications() {
    std::lock_guard lock(impl_->mutex);
    for (const auto& listener : impl_->listeners) {
        try { (void)impl_->sync.close(listener.first); }
        catch (...) { std::terminate(); }
    }
}

uint32_t NativeNotifications::create(uint64_t mask, uint32_t maximum_version) {
    if (maximum_version > 10)
        stop("notification-version", maximum_version,
             "maximum notification version above 10 is unsupported");
    std::lock_guard lock(impl_->mutex);
    if (impl_->listeners.size() >= maximum_listeners)
        stop("notification-listeners", static_cast<uint32_t>(impl_->listeners.size()),
             "notification listener capacity exhausted");

    Impl::Listener listener{mask, maximum_version, {}, {}};
    listener.items.reserve(maximum_items);
    const auto created = impl_->sync.create_notification_event();
    try {
        listener.event = impl_->sync.retain_notification_event(created.handle);
        if (!listener.event)
            stop("notification-handle", created.handle,
                 "created notification event could not be retained");
        const auto inserted = impl_->listeners.emplace(created.handle, std::move(listener));
        if (!inserted.second)
            stop("notification-handle", created.handle, "notification listener handle collision");
    } catch (...) {
        (void)impl_->sync.close(created.handle);
        throw;
    }
    return created.handle;
}

bool NativeNotifications::get_next(uint32_t handle, uint32_t match, uint32_t id_output,
                                   uint32_t parameter_output) {
    if (!id_output)
        stop("notification-output", id_output, "notification ID output is required");
    impl_->memory.check_write(id_output, 4);
    if (parameter_output) impl_->memory.check_write(parameter_output, 4);

    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->listeners.find(handle);
    if (found == impl_->listeners.end())
        stop("notification-handle", handle, "notification listener handle is not live");
    auto& listener = found->second;
    if (listener.items.empty() && !listener.deferred.empty()) {
        listener.items = std::move(listener.deferred);
        listener.deferred.clear();
        listener.event->signal();
        // Still nothing this time: the title reads them on its next pump,
        // which is what held them back.
        impl_->memory.store<uint32_t>(id_output, 0);
        if (parameter_output) impl_->memory.store<uint32_t>(parameter_output, 0);
        return false;
    }
    const auto item = match == 0
        ? listener.items.begin()
        : std::find_if(listener.items.begin(), listener.items.end(),
                       [&](const auto& candidate) { return candidate.id == match; });
    if (item == listener.items.end()) {
        impl_->memory.store<uint32_t>(id_output, 0);
        if (parameter_output) impl_->memory.store<uint32_t>(parameter_output, 0);
        return false;
    }

    // Publish outputs before removal. Complete-range preflight above makes these
    // checked stores deterministic even when the two output words alias.
    impl_->memory.store<uint32_t>(id_output, item->id);
    if (parameter_output) impl_->memory.store<uint32_t>(parameter_output, item->parameter);
    listener.items.erase(item);
    if (listener.items.empty()) listener.event->reset();
    return true;
}

uint32_t NativeNotifications::close(uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->listeners.find(handle);
    if (found == impl_->listeners.end()) return status_invalid_handle;
    const uint32_t status = impl_->sync.close(handle);
    if (status != status_success) return status;
    impl_->listeners.erase(found);
    return status_success;
}

bool NativeNotifications::owns(uint32_t handle) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->listeners.find(handle) != impl_->listeners.end();
}

void NativeNotifications::publish(uint32_t id, uint32_t parameter) {
    if (!id) stop("notification-id", id, "notification ID zero is reserved for wildcard dequeue");
    const uint32_t category = (id >> 25) & 0x3fu;
    const uint32_t version = (id >> 16) & 0x1ffu;
    std::lock_guard lock(impl_->mutex);

    for (const auto& entry : impl_->listeners) {
        const auto& listener = entry.second;
        if ((listener.mask & (uint64_t{1} << category)) &&
            version <= listener.maximum_version && listener.items.size() >= maximum_items)
            stop("notification-queue", entry.first, "notification listener queue capacity exhausted");
    }
    for (auto& entry : impl_->listeners) {
        auto& listener = entry.second;
        if (!(listener.mask & (uint64_t{1} << category)) || version > listener.maximum_version) continue;
        listener.items.push_back({id, parameter}); // capacity was reserved at listener creation
        listener.event->signal();
    }
}

void NativeNotifications::publish_when_drained(uint32_t id, uint32_t parameter) {
    if (!id) stop("notification-id", id, "notification ID zero is reserved for wildcard dequeue");
    const uint32_t category = (id >> 25) & 0x3fu;
    const uint32_t version = (id >> 16) & 0x1ffu;
    std::lock_guard lock(impl_->mutex);

    for (const auto& entry : impl_->listeners) {
        const auto& listener = entry.second;
        if ((listener.mask & (uint64_t{1} << category)) &&
            version <= listener.maximum_version && listener.deferred.size() >= maximum_items)
            stop("notification-queue", entry.first, "notification listener queue capacity exhausted");
    }
    for (auto& entry : impl_->listeners) {
        auto& listener = entry.second;
        if (!(listener.mask & (uint64_t{1} << category)) || version > listener.maximum_version) continue;
        listener.deferred.push_back({id, parameter});
    }
}
}
