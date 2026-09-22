#include "native_sync_objects.h"
#include <mutex>
#include "guest_memory.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include "portable_waitables.h"
#include <cerrno>
#endif

#include <algorithm>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace sfr {
namespace {
constexpr uint32_t status_success = 0;
constexpr uint32_t status_object_name_exists = 0x40000000u;
constexpr uint32_t status_timeout = 0x102;
constexpr uint32_t status_invalid_handle = 0xc0000008u;
constexpr uint32_t status_invalid_parameter = 0xc000000du;
constexpr uint32_t status_semaphore_limit_exceeded = 0xc0000047u;
constexpr uint32_t first_handle = 0x72100004u;
constexpr uint32_t handle_limit = 0x72200000u;

[[noreturn]] void stop_host(const char* operation, uint32_t handle, uint32_t error) {
    throw RuntimeStop("semaphore-host", handle,
                      std::string(operation) + " failed with host error " + std::to_string(error));
}

// The host objects behind guest handles: Windows kernel objects, or the
// portable emulation (portable_waitables.h) elsewhere. A duplicate is a
// second reference that outlives the guest handle's close.
#ifdef _WIN32
using Native = HANDLE;
uint32_t last_error() { return GetLastError(); }
void close_native(Native native) { CloseHandle(native); }
bool close_native_checked(Native native) { return CloseHandle(native) != FALSE; }
Native duplicate_native(Native native, DWORD access = 0, DWORD options = DUPLICATE_SAME_ACCESS) {
    HANDLE duplicate = nullptr;
    return DuplicateHandle(GetCurrentProcess(), native, GetCurrentProcess(), &duplicate, access, FALSE, options)
        ? duplicate : nullptr;
}
Native create_event_native(bool manual_reset, bool initial_state) {
    return CreateEventW(nullptr, manual_reset ? TRUE : FALSE, initial_state ? TRUE : FALSE, nullptr);
}
Native create_semaphore_native(int32_t initial, int32_t maximum) {
    return CreateSemaphoreW(nullptr, initial, maximum, nullptr);
}
bool set_native(Native native) { return SetEvent(native) != FALSE; }
bool reset_native(Native native) { return ResetEvent(native) != FALSE; }
#else
using Native = portable::WaitablePtr;
uint32_t last_error() { return uint32_t(errno); }
void close_native(Native&) {}
bool close_native_checked(Native&) { return true; }
Native duplicate_native(const Native& native) { return native; }
Native create_event_native(bool manual_reset, bool initial_state) {
    return portable::make_event(manual_reset, initial_state);
}
Native create_semaphore_native(int32_t initial, int32_t maximum) { return portable::make_semaphore(initial, maximum); }
bool set_native(const Native& native) { portable::set_event(*native); return true; }
bool reset_native(const Native& native) { portable::reset_event(*native); return true; }
#endif
}

struct NativeSyncObjects::Impl {
    enum class Kind { semaphore, event, notification };
    struct Entry {
        Native native;
        Kind kind;
        std::string name;
        uint32_t opens;
    };
    std::unordered_map<uint32_t, Entry> handles;
    std::unordered_map<std::string, uint32_t> names;
    uint32_t next_handle = first_handle;
    // Guest threads running beside the permit's owner create, signal and
    // retain objects too; waits happen on retained duplicates, outside it.
    mutable std::recursive_mutex mutex;

    ~Impl() {
        for (auto& entry : handles) close_native(entry.second.native);
    }
};

namespace {
std::string validate_name(std::string_view name) {
    if (name.empty()) return {};
    if (name.size() > 65535)
        throw RuntimeStop("sync-name", 0, "synchronization name exceeds 65535 bytes");
    std::string folded;
    folded.reserve(name.size());
    for (const unsigned char byte : name) {
        if (byte < 0x20 || byte > 0x7e || byte == '/' || byte == '\\')
            throw RuntimeStop("sync-name", 0,
                              "synchronization name requires printable ASCII without path separators");
        folded.push_back(byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A'))
                                                    : static_cast<char>(byte));
    }
    return folded;
}

template <typename Registry, typename Kind>
NativeSyncObjects::CreateResult reopen_named(Registry& impl, std::string_view name,
                                              const std::string& folded, Kind kind) {
    if (folded.empty()) return {status_success, 0};
    const auto indexed = impl.names.find(folded);
    if (indexed == impl.names.end()) return {status_success, 0};
    auto object = impl.handles.find(indexed->second);
    if (object == impl.handles.end())
        throw RuntimeStop("sync-name", indexed->second, "named synchronization index is inconsistent");
    if (object->second.name != name)
        throw RuntimeStop("sync-name", indexed->second,
                          "case-equivalent synchronization name semantics are not supported");
    if (object->second.kind != kind)
        throw RuntimeStop("sync-type", indexed->second,
                          "named synchronization object exists with a different type");
    if (object->second.opens == UINT32_MAX)
        throw RuntimeStop("sync-name", indexed->second,
                          "named synchronization open-reference count exhausted");
    ++object->second.opens;
    return {status_object_name_exists, indexed->second};
}

template <typename Registry, typename Kind>
NativeSyncObjects::CreateResult publish_new(Registry& impl, Native native, Kind kind,
                                             std::string_view name, std::string folded) {
    const uint32_t handle = impl.next_handle;
    try {
        auto inserted = impl.handles.emplace(
            handle, typename Registry::Entry{native, kind, std::string(name), 1});
        if (!inserted.second)
            throw RuntimeStop("semaphore-namespace", handle, "guest synchronization handle collision");
        try {
            if (!folded.empty()) {
                const auto named = impl.names.emplace(std::move(folded), handle);
                if (!named.second)
                    throw RuntimeStop("sync-name", handle, "named synchronization index collision");
            }
        } catch (...) {
            impl.handles.erase(inserted.first);
            throw;
        }
    } catch (...) {
        close_native(native);
        throw;
    }
    impl.next_handle += 4;
    return {status_success, handle};
}
}

struct NativeSyncObjects::WaitHandle::Impl {
    Native native;
    uint32_t guest_handle;

    Impl(Native native_handle, uint32_t handle)
        : native(std::move(native_handle)), guest_handle(handle) {}
    ~Impl() { close_native(native); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

NativeSyncObjects::WaitHandle::WaitHandle(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
NativeSyncObjects::WaitHandle::~WaitHandle() = default;
NativeSyncObjects::WaitHandle::WaitHandle(WaitHandle&&) noexcept = default;
NativeSyncObjects::WaitHandle& NativeSyncObjects::WaitHandle::operator=(WaitHandle&&) noexcept = default;

NativeSyncObjects::WaitResult NativeSyncObjects::WaitHandle::wait(uint32_t timeout_ms,
                                                                  std::stop_token stop) {
#ifndef _WIN32
    portable::Waitable* objects[] = {impl_->native.get()};
    const int result = portable::wait_any(objects, timeout_ms, stop);
    if (result == portable::wait_cancelled) return {status_success, true};
    if (result == portable::wait_timeout) return {status_timeout, false};
    return {status_success, false};
#else
    struct OwnedHandle {
        HANDLE value;
        ~OwnedHandle() { CloseHandle(value); }
    };
    struct CancelWait {
        HANDLE event;
        void operator()() const noexcept {
            if (!SetEvent(event)) std::terminate();
        }
    };

    OwnedHandle cancel_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!cancel_event.value)
        stop_host("CreateEventW", impl_->guest_handle, GetLastError());

    std::stop_callback callback(stop, CancelWait{cancel_event.value});
    const HANDLE handles[] = {cancel_event.value, impl_->native};
    const DWORD result = WaitForMultipleObjects(2, handles, FALSE, timeout_ms);
    if (result == WAIT_OBJECT_0) return {status_success, true};
    if (result == WAIT_OBJECT_0 + 1) return {status_success, false};
    if (result == WAIT_TIMEOUT) return {status_timeout, false};
    if (result == WAIT_FAILED)
        stop_host("WaitForMultipleObjects", impl_->guest_handle, GetLastError());
    throw RuntimeStop("semaphore-host", impl_->guest_handle,
                      "WaitForMultipleObjects returned unexpected result " +
                          std::to_string(result));
#endif
}

NativeSyncObjects::WaitResult NativeSyncObjects::WaitHandle::wait_multiple(
    const std::vector<WaitHandle*>& waits, bool wait_all, uint32_t timeout_ms, std::stop_token stop) {
    if (waits.empty() || waits.size() >= 64)
        throw RuntimeStop("semaphore-host", uint32_t(waits.size()), "unsupported multiple-object wait count");
#ifndef _WIN32
    std::vector<portable::Waitable*> objects;
    for (auto* wait : waits) objects.push_back(wait->impl_->native.get());
    const int result = wait_all ? portable::wait_all(objects, timeout_ms, stop)
                                : portable::wait_any(objects, timeout_ms, stop);
    if (result == portable::wait_cancelled) return {status_success, true};
    if (result == portable::wait_timeout) return {status_timeout, false};
    return {wait_all ? status_success : uint32_t(result), false};
#else
    std::vector<HANDLE> handles;
    for (auto* wait : waits) handles.push_back(wait->impl_->native);
    if (wait_all) {
        // A cancel event cannot join a wait-all set: wait in slices and poll it.
        const ULONGLONG start = GetTickCount64();
        for (;;) {
            if (stop.stop_requested()) return {status_success, true};
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (timeout_ms != INFINITE && elapsed >= timeout_ms) return {status_timeout, false};
            const DWORD slice = timeout_ms == INFINITE ? 50 : DWORD((std::min<ULONGLONG>)(50, timeout_ms - elapsed));
            const DWORD result = WaitForMultipleObjects(DWORD(handles.size()), handles.data(), TRUE, slice);
            if (result < WAIT_OBJECT_0 + handles.size()) return {status_success, false};
            if (result == WAIT_FAILED)
                stop_host("WaitForMultipleObjects", waits[0]->impl_->guest_handle, GetLastError());
        }
    }
    struct OwnedHandle {
        HANDLE value;
        ~OwnedHandle() { CloseHandle(value); }
    };
    OwnedHandle cancel_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!cancel_event.value) stop_host("CreateEventW", waits[0]->impl_->guest_handle, GetLastError());
    std::stop_callback callback(stop, [event = cancel_event.value]() noexcept { if (!SetEvent(event)) std::terminate(); });
    handles.insert(handles.begin(), cancel_event.value);
    const DWORD result = WaitForMultipleObjects(DWORD(handles.size()), handles.data(), FALSE, timeout_ms);
    if (result == WAIT_OBJECT_0) return {status_success, true};
    if (result > WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handles.size()) return {result - WAIT_OBJECT_0 - 1, false};
    if (result == WAIT_TIMEOUT) return {status_timeout, false};
    stop_host("WaitForMultipleObjects", waits[0]->impl_->guest_handle, GetLastError());
#endif
}

NativeSyncObjects::NativeSyncObjects() : impl_(std::make_unique<Impl>()) {}
NativeSyncObjects::~NativeSyncObjects() = default;

NativeSyncObjects::CreateResult NativeSyncObjects::create_notification_event() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->next_handle >= handle_limit)
        throw RuntimeStop("semaphore-namespace", impl_->next_handle, "guest sync-object handle namespace exhausted");
    Native native = create_event_native(true, false);
    if (!native) stop_host("CreateEventW notification", impl_->next_handle, last_error());
    return publish_new(*impl_, native, Impl::Kind::notification, {}, {});
}
std::unique_ptr<NativeSyncObjects::RetainedEvent> NativeSyncObjects::retain_notification_event(uint32_t handle) {
    return retain_event_impl(handle, true);
}

struct NativeSyncObjects::RetainedEvent::Impl {
    Native native;
    uint32_t guest_handle;
    Impl(Native value, uint32_t handle) : native(std::move(value)), guest_handle(handle) {}
    ~Impl() { close_native(native); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};
NativeSyncObjects::RetainedEvent::RetainedEvent(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NativeSyncObjects::RetainedEvent::~RetainedEvent() = default;
NativeSyncObjects::RetainedEvent::RetainedEvent(RetainedEvent&&) noexcept = default;
NativeSyncObjects::RetainedEvent& NativeSyncObjects::RetainedEvent::operator=(RetainedEvent&&) noexcept = default;
void NativeSyncObjects::RetainedEvent::reset() {
    if (!impl_) throw RuntimeStop("event-retain", 0, "retained event capability has been moved");
    if (!reset_native(impl_->native)) stop_host("ResetEvent retained", impl_->guest_handle, last_error());
}
void NativeSyncObjects::RetainedEvent::signal() {
    if (!impl_) throw RuntimeStop("event-retain", 0, "retained event capability has been moved");
    if (!set_native(impl_->native)) stop_host("SetEvent retained", impl_->guest_handle, last_error());
}
std::unique_ptr<NativeSyncObjects::RetainedEvent> NativeSyncObjects::retain_event(uint32_t handle) {
    return retain_event_impl(handle, false);
}
std::unique_ptr<NativeSyncObjects::RetainedEvent> NativeSyncObjects::retain_event_impl(uint32_t handle,
                                                                                  bool notification) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->handles.find(handle);
    if (found == impl_->handles.end()) return nullptr;
    const auto required = notification ? Impl::Kind::notification : Impl::Kind::event;
    if (found->second.kind != required)
        throw RuntimeStop("event-retain", handle, "retained event capability requires matching object type");
    Native duplicate = duplicate_native(found->second.native);
    if (!duplicate) stop_host("DuplicateHandle retained event", handle, last_error());
    std::unique_ptr<RetainedEvent::Impl> event;
    try {
        event = std::make_unique<RetainedEvent::Impl>(duplicate, handle);
    } catch (...) {
        close_native(duplicate);
        throw;
    }
    // If allocating the outer capability fails, event still owns the duplicate.
    return std::unique_ptr<RetainedEvent>(new RetainedEvent(std::move(event)));
}

NativeSyncObjects::CreateResult NativeSyncObjects::create_semaphore(int32_t initial, int32_t maximum,
                                                                    std::string_view name) {
    std::lock_guard lock(impl_->mutex);
    std::string folded = validate_name(name);
    const auto reopened = reopen_named(*impl_, name, folded, Impl::Kind::semaphore);
    if (reopened.handle) return reopened;
    if (initial < 0 || maximum <= 0 || initial > maximum)
        return {status_invalid_parameter, 0};
    if (impl_->next_handle >= handle_limit)
        throw RuntimeStop("semaphore-namespace", impl_->next_handle,
                          "guest semaphore handle namespace exhausted");

    Native native = create_semaphore_native(initial, maximum);
    if (!native) stop_host("CreateSemaphoreW", impl_->next_handle, last_error());

    return publish_new(*impl_, native, Impl::Kind::semaphore, name, std::move(folded));
}

NativeSyncObjects::CreateResult NativeSyncObjects::create_event(bool manual_reset, bool initial_state,
                                                                 std::string_view name) {
    std::lock_guard lock(impl_->mutex);
    std::string folded = validate_name(name);
    const auto reopened = reopen_named(*impl_, name, folded, Impl::Kind::event);
    if (reopened.handle) return reopened;
    if (impl_->next_handle >= handle_limit)
        throw RuntimeStop("semaphore-namespace", impl_->next_handle,
                          "guest sync-object handle namespace exhausted");

    Native native = create_event_native(manual_reset, initial_state);
    if (!native) stop_host("CreateEventW", impl_->next_handle, last_error());

    return publish_new(*impl_, native, Impl::Kind::event, name, std::move(folded));
}

std::unique_ptr<NativeSyncObjects::WaitHandle> NativeSyncObjects::retain_wait(uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->handles.find(handle);
    if (found == impl_->handles.end()) return nullptr;

    Native duplicate = duplicate_native(found->second.native);
    if (!duplicate) stop_host("DuplicateHandle", handle, last_error());
    std::unique_ptr<WaitHandle::Impl> wait_impl;
    try {
        wait_impl = std::make_unique<WaitHandle::Impl>(duplicate, handle);
    } catch (...) {
        close_native(duplicate);
        throw;
    }
    return std::unique_ptr<WaitHandle>(new WaitHandle(std::move(wait_impl)));
}

std::unique_ptr<NativeSyncObjects::WaitHandle> NativeSyncObjects::wait_on_host(void* host_handle, uint32_t guest_handle) {
#ifdef _WIN32
    Native duplicate = duplicate_native(static_cast<HANDLE>(host_handle), SYNCHRONIZE, 0);
#else
    // A NativeThread's exit object (native_thread.cpp), kept alive by this reference.
    Native duplicate = static_cast<portable::Waitable*>(host_handle)->shared_from_this();
#endif
    if (!duplicate) stop_host("DuplicateHandle", guest_handle, last_error());
    std::unique_ptr<WaitHandle::Impl> wait_impl;
    try {
        wait_impl = std::make_unique<WaitHandle::Impl>(duplicate, guest_handle);
    } catch (...) {
        close_native(duplicate);
        throw;
    }
    return std::unique_ptr<WaitHandle>(new WaitHandle(std::move(wait_impl)));
}

uint32_t NativeSyncObjects::wait(uint32_t handle, uint32_t timeout_ms) {
    auto retained = retain_wait(handle);
    if (!retained) return status_invalid_handle;
    return retained->wait(timeout_ms).status;
}

NativeSyncObjects::ReleaseResult NativeSyncObjects::release_semaphore(uint32_t handle, int32_t adjustment) {
    std::lock_guard lock(impl_->mutex);
    if (adjustment <= 0) return {status_invalid_parameter, 0};
    const auto found = impl_->handles.find(handle);
    if (found == impl_->handles.end()) return {status_invalid_handle, 0};
    if (found->second.kind != Impl::Kind::semaphore) return {0xc0000024u, 0};
#ifdef _WIN32
    LONG previous = 0;
    if (ReleaseSemaphore(found->second.native, adjustment, &previous))
        return {status_success, static_cast<uint32_t>(previous)};
    const DWORD error = GetLastError();
    if (error == ERROR_TOO_MANY_POSTS) return {status_semaphore_limit_exceeded, 0};
    stop_host("ReleaseSemaphore", handle, error);
#else
    int32_t previous = 0;
    if (portable::release_semaphore(*found->second.native, adjustment, &previous))
        return {status_success, static_cast<uint32_t>(previous)};
    return {status_semaphore_limit_exceeded, 0};
#endif
}

uint32_t NativeSyncObjects::set_event(uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->handles.find(handle);
    if (found == impl_->handles.end()) return status_invalid_handle;
    if (found->second.kind != Impl::Kind::event) return 0xc0000024u;
    if (!set_native(found->second.native)) stop_host("SetEvent", handle, last_error());
    return status_success;
}

uint32_t NativeSyncObjects::reset_event(uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->handles.find(handle);
    if (found == impl_->handles.end()) return status_invalid_handle;
    if (found->second.kind != Impl::Kind::event) return 0xc0000024u;
    if (!reset_native(found->second.native)) stop_host("ResetEvent", handle, last_error());
    return status_success;
}

uint32_t NativeSyncObjects::close(uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->handles.find(handle);
    if (found == impl_->handles.end()) return status_invalid_handle;
    if (found->second.opens > 1) {
        --found->second.opens;
        return status_success;
    }
    auto named = impl_->names.end();
    if (!found->second.name.empty()) {
        // Resolve the index before closing the native object: folding may allocate and throw.
        const std::string folded = validate_name(found->second.name);
        named = impl_->names.find(folded);
        if (named == impl_->names.end() || named->second != handle)
            throw RuntimeStop("sync-name", handle, "named synchronization index is inconsistent");
    }
    if (!close_native_checked(found->second.native)) stop_host("CloseHandle", handle, last_error());
    if (named != impl_->names.end()) impl_->names.erase(named);
    impl_->handles.erase(found);
    return status_success;
}

std::string_view NativeSyncObjects::name(uint32_t handle) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->handles.find(handle);
    if (found == impl_->handles.end())
        throw RuntimeStop("sync-name", handle, "synchronization handle is not owned");
    return found->second.name;
}

bool NativeSyncObjects::owns(uint32_t handle) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->handles.find(handle) != impl_->handles.end();
}

size_t NativeSyncObjects::open_count() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->handles.size();
}

bool NativeSyncObjects::is_handle_range(uint32_t handle) {
    return handle >= 0x72100000u && handle < 0x72200000u;
}
}
