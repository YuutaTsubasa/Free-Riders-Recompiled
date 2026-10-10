#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace sfr {
class GuestMemory;

// Xbox LIVE for the title, served by this project's own service
// (scripts/live_server.py, src/live_client.h) when SFR_LIVE_SERVER is set:
// sign-in, privileges, XNet addresses, QoS, virtual sockets and the XSession
// messages of the XGI app. Without SFR_LIVE_SERVER nothing here is used.
bool live_enabled();

// An import call: r[3..10] the argument registers, sp for the arguments
// past the eighth (at sp+0x54, 8 bytes apart), result what to return in r3.
struct LiveCall {
    uint32_t r[11] = {};
    uint64_t r64[11] = {};  // the same registers whole, for 64-bit arguments
    uint32_t sp = 0, lr = 0;
    uint64_t result = 0;
};

// Handles a LIVE or network import (true) or leaves it (false). set_event
// signals a guest event handle.
bool live_import(std::string_view name, LiveCall& call, GuestMemory& memory,
                 const std::function<void(uint32_t)>& set_event);

// An XGI (app 0xFB) message: its result, or nothing when it is not LIVE's.
std::optional<uint32_t> live_message(uint32_t message, uint32_t buffer, uint32_t length, GuestMemory& memory);

// An XLiveBase (app 0xFC) in-process message (XMsgInProcessCall): its
// result, or nothing when it is not handled.
std::optional<uint32_t> live_in_process(uint32_t app, uint32_t message, uint32_t buffer, uint32_t length,
                                        GuestMemory& memory);

// Session handles (XamSessionCreateHandle) and their objects.
bool live_owns_handle(uint32_t handle);
bool live_owns_object(uint32_t object);
}
