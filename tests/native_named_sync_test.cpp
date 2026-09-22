#include "native_sync_objects.h"
#include "guest_memory.h"

#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
constexpr uint32_t success = 0;
constexpr uint32_t timeout = 0x102;
constexpr uint32_t name_exists = 0x40000000;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template <typename F> void rejects(F&& operation, const char* message) {
    try {
        operation();
    } catch (const sfr::RuntimeStop&) {
        return;
    }
    throw std::runtime_error(message);
}

DWORD handle_count() {
    DWORD count = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &count) != FALSE,
            "process handle count is available");
    return count;
}

void named_events_share_state_without_reinitializing() {
    sfr::NativeSyncObjects objects;
    const auto first = objects.create_event(false, true, "Event_NuiGetSkeleton");
    const auto reopened = objects.create_event(true, false, "Event_NuiGetSkeleton");
    require(first.status == success && reopened.status == name_exists && first.handle == reopened.handle &&
                objects.open_count() == 1 && objects.name(first.handle) == "Event_NuiGetSkeleton",
            "same-name event reopens by retaining the same guest handle and its registry name");
    require(objects.wait(reopened.handle, 0) == success && objects.wait(first.handle, 0) == timeout,
            "reopen ignores reset mode and initial state and shares the original auto-event state");

    const auto manual = objects.create_event(true, true, "Manual");
    const auto manual_again = objects.create_event(false, false, "Manual");
    require(manual_again.status == name_exists && objects.wait(manual.handle, 0) == success &&
                objects.wait(manual_again.handle, 0) == success,
            "reopened manual event remains manual and signaled");
}

void named_semaphores_share_count_and_ignore_reopen_parameters() {
    sfr::NativeSyncObjects objects;
    const auto first = objects.create_semaphore(2, 3, "Counted");
    const auto reopened = objects.create_semaphore(-7, -9, "Counted");
    require(first.status == success && reopened.status == name_exists,
            "existing semaphore is found before new-object parameter validation");
    require(objects.wait(first.handle, 0) == success && objects.wait(reopened.handle, 0) == success &&
                objects.wait(first.handle, 0) == timeout,
            "same-name semaphore handles consume one shared count");
}

void namespace_is_byte_exact_isolated_and_cross_kind_safe() {
    sfr::NativeSyncObjects first;
    sfr::NativeSyncObjects second;
    const auto upper = first.create_event(false, false, "Case");
    rejects([&] { first.create_event(false, true, "case"); },
            "ASCII-fold-equivalent nonexact spelling is rejected until case semantics are pinned");
    require(first.open_count() == 1 && first.wait(upper.handle, 0) == timeout,
            "rejected case alias neither creates an object nor changes existing state");
    require(second.create_event(false, true, "Case").status == success,
            "separate registries have isolated guest namespaces");

    const auto count = first.open_count();
    rejects([&] { first.create_semaphore(1, 1, "Case"); },
            "same name with a different synchronization kind is rejected");
    require(first.open_count() == count && first.wait(upper.handle, 0) == timeout,
            "cross-kind collision has no registry or object-state effect");
}

void closing_last_guest_handle_removes_name_but_retained_wait_survives() {
    sfr::NativeSyncObjects objects;
    const auto first = objects.create_event(false, true, "Transient");
    const auto second = objects.create_event(false, false, "Transient");
    auto retained = objects.retain_wait(first.handle);
    require(first.handle == second.handle && objects.close(first.handle) == success &&
                objects.owns(first.handle) && objects.name(first.handle) == "Transient" &&
                objects.close(second.handle) == success && !objects.owns(first.handle),
            "close decrements retained opens and removes the name only after the final close");
    const auto replacement = objects.create_event(false, false, "Transient");
    require(replacement.status == success && retained->wait(0).status == success &&
                objects.wait(replacement.handle, 0) == timeout,
            "final close removes name while a transient wait duplicate retains only old native state");
}

void invalid_names_fail_without_ids_or_native_handle_leaks() {
    sfr::NativeSyncObjects objects;
    const DWORD before = handle_count();
    const std::string too_long(65536, 'A');
    const std::string embedded_nul("ab\0cd", 5);
    const std::string non_ascii(1, static_cast<char>(0x80));
    const std::string control(1, static_cast<char>(0x1f));
    const std::string_view invalid[] = {too_long, embedded_nul, non_ascii, control, "a/b", "a\\b"};
    for (const auto name : invalid)
        rejects([&] { objects.create_event(false, false, name); },
                "invalid synchronization name is rejected");
    require(objects.open_count() == 0 && handle_count() == before,
            "invalid names consume neither guest IDs nor native handles");
    const auto unnamed = objects.create_event(false, false, {});
    require(unnamed.status == success && unnamed.handle == 0x72100004u && objects.name(unnamed.handle).empty(),
            "empty name remains unnamed and rejected names do not consume the first ID");
}

void names_do_not_enter_the_windows_kernel_namespace_and_cleanup_is_exact() {
    const std::string name = "SFR_Private_Event_824D01FC";
    HANDLE external = CreateEventA(nullptr, FALSE, FALSE, name.c_str());
    require(external != nullptr, "external named-event fixture creates");
    const DWORD before = handle_count();
    {
        sfr::NativeSyncObjects objects;
        const auto event = objects.create_event(false, true, name);
        require(event.status == success && objects.wait(event.handle, 0) == success,
                "guest object does not reopen the host named event");
        require(WaitForSingleObject(external, 0) == WAIT_TIMEOUT,
                "guest signaling state does not pollute the Windows named-object namespace");
    }
    require(handle_count() == before, "registry destruction closes named guest object handles");
    CloseHandle(external);
}
}

int main() {
    try {
        named_events_share_state_without_reinitializing();
        named_semaphores_share_count_and_ignore_reopen_parameters();
        namespace_is_byte_exact_isolated_and_cross_kind_safe();
        closing_last_guest_handle_removes_name_but_retained_wait_survives();
        invalid_names_fail_without_ids_or_native_handle_leaks();
        names_do_not_enter_the_windows_kernel_namespace_and_cleanup_is_exact();
        std::cout << "Native named synchronization checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
