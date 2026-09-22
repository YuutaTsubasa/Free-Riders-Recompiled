#ifdef _WIN32
#include <winsock2.h>
#endif
#include "native_winsock.h"
#include "guest_memory.h"
#include <array>
#include <cstring>
#include <exception>
#include <limits>

namespace sfr {
#ifndef _WIN32
// POSIX sockets need no process-wide startup: report what Winsock 2.2 would.
namespace {
constexpr int WSAVERNOTSUPPORTED = 10092;
struct WSADATA {
    uint16_t wVersion, wHighVersion;
    char szDescription[257];
    char szSystemStatus[129];
    uint16_t iMaxSockets, iMaxUdpDg;
};
uint8_t LOBYTE(uint16_t value) { return static_cast<uint8_t>(value); }
int WSAStartup(uint16_t version, WSADATA* data) {
    *data = {};
    data->wHighVersion = 0x0202;
    if (LOBYTE(version) < 1) return WSAVERNOTSUPPORTED;
    // The lower of the requested version and 2.2 (major in the low byte).
    const auto order = [](uint16_t v) { return (v & 0xFF) << 8 | v >> 8; };
    data->wVersion = order(version) < order(0x0202) ? version : 0x0202;
    std::strcpy(data->szDescription, "WinSock 2.0");
    std::strcpy(data->szSystemStatus, "Running");
    return 0;
}
int WSACleanup() { return 0; }
}
#endif
NativeWinsock::~NativeWinsock() {
    while (acquisitions_) {
        // Cleanup failure indicates broken process-wide ownership. Do not
        // silently leave an acquisition behind or retry forever at shutdown.
        if (WSACleanup() != 0) std::terminate();
        --acquisitions_;
    }
}

int NativeWinsock::startup(uint32_t caller, uint16_t version, uint32_t output) {
    if (caller != 1)
        throw RuntimeStop("native-winsock", caller, "only the original title caller is supported");
    if (!output)
        throw RuntimeStop("memory-access", output, "WSADATA output must be non-null");
    memory_.check_write(output, 400);
    if (acquisitions_ == (std::numeric_limits<uint32_t>::max)())
        throw RuntimeStop("native-winsock", caller, "startup acquisition count overflow");

    WSADATA native{};
    const int result = WSAStartup(version, &native);
    if (result != 0) return result;
    ++acquisitions_;
    try {
        // Winsock 2 ignores the legacy vendor pointer. A 1.x profile would
        // require a separate guest allocation/translation contract.
        if (LOBYTE(native.wVersion) < 2)
            throw RuntimeStop("native-winsock", version, "legacy Winsock vendor data is unsupported");
        std::array<uint8_t, 400> guest{};
        const auto word = [&](size_t offset, uint16_t value) {
            guest[offset] = static_cast<uint8_t>(value >> 8);
            guest[offset + 1] = static_cast<uint8_t>(value);
        };
        word(0, native.wVersion);
        word(2, native.wHighVersion);
        for (size_t i = 0; i < 257; ++i) guest[4 + i] = static_cast<uint8_t>(native.szDescription[i]);
        for (size_t i = 0; i < 129; ++i) guest[261 + i] = static_cast<uint8_t>(native.szSystemStatus[i]);
        word(390, native.iMaxSockets);
        word(392, native.iMaxUdpDg);
        // Padding and guest vendor pointer at 396 remain zero. Offset 400
        // belongs to the original caller's saved r31, not to this result.
        for (size_t i = 0; i < guest.size(); ++i)
            memory_.store<uint8_t>(uint64_t(output) + i, guest[i]);
    } catch (...) {
        if (WSACleanup() != 0) std::terminate();
        --acquisitions_;
        throw;
    }
    return result;
}

int NativeWinsock::cleanup(uint32_t caller) {
    if (caller != 1 || !acquisitions_)
        throw RuntimeStop("native-winsock", caller, "cleanup requires an owned title acquisition");
    const int result = WSACleanup();
    if (result == 0) --acquisitions_;
    return result;
}
}
