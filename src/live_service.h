#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace sfr {
// This project's online service (the same protocol as scripts/live_server.py,
// described there) inside the game: SFR_LIVE_HOST=1 runs it on this device,
// on SFR_LIVE_PORT (47800), and the game then connects to it itself, so any
// device (a PC, a handheld, a phone) can be the one the others connect to.
class LiveService {
public:
    // Starts listening on all interfaces; null (logged) when the port is taken.
    static std::unique_ptr<LiveService> start(uint16_t port);
    ~LiveService();
    LiveService(const LiveService&) = delete;
    LiveService& operator=(const LiveService&) = delete;

private:
    LiveService() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// SFR_LIVE_HOST=1: starts the service once (for this process) and returns the
// address the game should connect to, or empty when it is not wanted.
std::string start_hosted_live_service();
}
