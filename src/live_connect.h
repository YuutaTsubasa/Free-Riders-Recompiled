#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfr {
// Reaching this project's online service (live_service.h,
// server/live_server.py): what the game's LiveClient and the launcher's
// "Test connection" share, so both read an address and an answer the same way.

constexpr uint16_t live_default_port = 47800;
// What PING answers: "OK freeriders <version>". Raised when a change to the
// requests means games of different releases cannot play together.
constexpr int live_protocol_version = 1;

// "host[:port]", the port 47800 when left out. Spaces around it are dropped;
// nothing (no host, or a port that is not 1-65535) when it cannot be one.
struct LiveServerAddress {
    std::string host;
    uint16_t port = live_default_port;
};
std::optional<LiveServerAddress> parse_live_server(std::string_view text);

// HELLO's answer: "OK <console> <address> <mac (12 hex)> <machine> [<token>]".
// The token (16 hex; servers since the room password) goes with the UDP
// registration, so nobody else can take this console's datagrams.
struct LiveHello {
    uint32_t console = 0, address = 0;
    std::array<uint8_t, 6> mac{};
    uint64_t machine = 0;
    std::optional<std::array<uint8_t, 8>> token;
};
std::optional<LiveHello> parse_live_hello(std::string_view line);
// The datagram that tells the server where this console's datagrams come
// from: 0x00, the console number, and the token when HELLO gave one. Sent
// again now and then, which also keeps a NAT's mapping open.
std::vector<uint8_t> live_udp_registration(const LiveHello& hello);

// PING's answer, "OK freeriders <version> [password]": the version, or nothing.
std::optional<int> parse_live_ping(std::string_view line);
// Whether that answer says the server has a room password.
bool live_ping_wants_password(std::string_view line);
// PASS's argument: the password's bytes as hex (it may hold spaces).
std::string live_password_hex(std::string_view password);

// How a try to reach the service went.
enum class LiveReach {
    ok,           // a Free Riders server of this version answered
    older,        // a Free Riders server answered, from a release before PING
    newer,        // a Free Riders server of another version answered
    bad_address,  // nothing to connect to: empty or malformed
    not_found,    // the name or address could not be looked up
    refused,      // the device is there, but nothing listens on the port
    no_answer,    // nothing answered in time (off, another network, a firewall)
    unexpected,   // something answered, but not a Free Riders server
    password_required,  // a server with a room password, and none was given
    wrong_password,     // the password given is not the server's
};
const char* live_reach_name(LiveReach reach);

#ifdef _WIN32
using LiveSocket = uintptr_t;  // SOCKET
#else
using LiveSocket = int;
#endif
constexpr LiveSocket no_live_socket = LiveSocket(-1);

// A TCP connection to the service within timeout_ms, blocking again once
// open; no_live_socket and why (not_found, refused, no_answer) otherwise.
LiveSocket open_live_connection(const LiveServerAddress& server, int timeout_ms, LiveReach& failure);
void close_live_connection(LiveSocket socket);
// Sends one request line and reads the answer's lines up to the closing
// ".", waiting up to timeout_ms in all (0: as long as it takes). pending
// keeps what arrived past the answer. Empty when the connection failed or
// timed out.
std::vector<std::string> live_exchange(LiveSocket socket, const std::string& line, std::string& pending,
                                       int timeout_ms);

// Joins as a console: PASS with the password when one is given, then HELLO.
// The answer, or nothing and why (wrong_password, password_required,
// unexpected). A server from before passwords answers PASS with an error,
// which is passed over: it asks for none.
struct LiveSignIn {
    std::optional<LiveHello> hello;
    LiveReach failure = LiveReach::ok;
};
LiveSignIn live_sign_in(LiveSocket socket, std::string_view password, uint64_t xuid, const std::string& gamertag,
                        std::string& pending, int timeout_ms);

// What the launcher's check found: the result, the address it connected to
// (the IPv4 address, when it was looked up), and the version the server said.
struct LiveCheck {
    LiveReach reach = LiveReach::bad_address;
    std::string address;
    int version = 0;
    bool password = false;  // the server has a room password
};
// Connects and asks PING, which a server answers without taking the caller
// as a console (no HELLO), so checking does not appear to the others. When
// the server has a room password, PASS checks the one given (none given:
// password_required).
LiveCheck check_live_server(std::string_view server, int timeout_ms, std::string_view password = {});

// This device's IPv4 addresses on its networks, the ones friends would type
// (no loopback, no link-local 169.254.x.y), private ranges first.
std::vector<std::string> local_ipv4_addresses();
}
