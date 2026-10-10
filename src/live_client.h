#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sfr {
// The connection to this project's own online service (scripts/live_server.py)
// that stands in for Xbox LIVE. SFR_LIVE_SERVER=host[:port] turns it on (port
// 47800 by default). The service puts this game on a virtual network: an
// address 10.77.x.y, a MAC address and a machine id. Every datagram the title
// sends goes through the service's UDP port to the console that owns the
// destination address, so two games on one PC, or behind NAT, reach each
// other without opening ports.
class LiveClient {
public:
    // The client for SFR_LIVE_SERVER, connected on first use; null without it,
    // or when the service cannot be reached (logged once).
    static LiveClient* instance();
    ~LiveClient();
    LiveClient(const LiveClient&) = delete;
    LiveClient& operator=(const LiveClient&) = delete;

    uint32_t address() const { return address_; }
    const std::array<uint8_t, 6>& mac() const { return mac_; }
    uint64_t machine() const { return machine_; }
    uint64_t xuid() const { return xuid_; }

    // One request line; the answer's lines, the first "OK ..." or "ERR ..."
    // (empty when the connection is gone).
    std::vector<std::string> request(const std::string& line);

    struct Datagram {
        uint32_t from = 0;
        uint16_t from_port = 0;
        std::vector<uint8_t> payload;
    };
    // Datagrams for a virtual port: opened by bind, closed with the socket.
    void open_port(uint16_t port);
    void close_port(uint16_t port);
    // To another console through the service, or straight back to this one
    // (its own address or 127.0.0.1, as a console's loopback).
    void send(uint32_t to, uint16_t to_port, uint16_t from_port, const uint8_t* data, size_t size);
    // The next datagram for the port, waiting up to timeout_ms (0: none).
    std::optional<Datagram> receive(uint16_t port, int timeout_ms);
    bool pending(uint16_t port);

private:
    LiveClient() = default;
    bool connect(const std::string& server);
    void receive_loop();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    uint32_t address_ = 0;
    std::array<uint8_t, 6> mac_{};
    uint64_t machine_ = 0;
    uint64_t xuid_ = 0;
};

// The gamertag shown online: SFR_LIVE_NAME, else the profile's name.
// (The profile's own name also names its saves, so it is not changed here.)
std::string live_gamertag();
// The XUID this game uses on the service: an online XUID (0x0009...) from
// this install's own random id (live-id.txt beside the game, or SFR_LIVE_ID),
// so two players who both kept the name "Player" still differ.
uint64_t live_xuid();
}
