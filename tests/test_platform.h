#pragma once
// The few Windows calls tests use for fixture names and handle-leak checks,
// with POSIX stand-ins (a process's open descriptors count as its handles).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <filesystem>

using DWORD = uint32_t;
#ifndef FALSE
#define FALSE 0
#endif
inline int GetCurrentProcess() { return 0; }
inline uint32_t GetCurrentProcessId() { return uint32_t(getpid()); }
inline uint64_t GetTickCount64() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
constexpr DWORD INFINITE = 0xFFFFFFFFu;
#include <cstdlib>
inline void* _aligned_malloc(std::size_t size, std::size_t alignment) {
    return std::aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
}
inline void _aligned_free(void* pointer) { std::free(pointer); }
inline int GetProcessHandleCount(int, DWORD* count) {
    std::error_code error;
    DWORD open = 0;
    for (auto it = std::filesystem::directory_iterator("/proc/self/fd", error);
         !error && it != std::filesystem::directory_iterator(); it.increment(error))
        ++open;
    *count = open;
    return !error;
}
#endif
