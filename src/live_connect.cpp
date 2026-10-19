#include "live_connect.h"

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace sfr {
namespace {
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    return text;
}

bool all_hex(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

void start_sockets() {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

int socket_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void set_blocking(LiveSocket socket, bool blocking) {
#ifdef _WIN32
    u_long off = blocking ? 0 : 1;
    ioctlsocket(SOCKET(socket), FIONBIO, &off);
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    fcntl(socket, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
#endif
}

using Clock = std::chrono::steady_clock;
int remaining_ms(Clock::time_point deadline) {
    return int(std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
}

std::string dotted(uint32_t address) {
    return std::to_string(address >> 24) + '.' + std::to_string(address >> 16 & 0xFF) + '.' +
           std::to_string(address >> 8 & 0xFF) + '.' + std::to_string(address & 0xFF);
}

bool is_private(uint32_t address) {
    return (address >> 24) == 10 || (address >> 16) == 0xC0A8 || (address >> 20) == 0xAC1;  // 10/8, 192.168/16, 172.16/12
}
}

std::optional<LiveServerAddress> parse_live_server(std::string_view text) {
    text = trimmed(text);
    if (text.empty()) return std::nullopt;
    LiveServerAddress server;
    const auto colon = text.rfind(':');
    // One colon is a port; more is an IPv6 address, which the service does
    // not listen on (it is IPv4 only).
    if (colon != std::string_view::npos) {
        if (text.find(':') != colon) return std::nullopt;
        const std::string port(trimmed(text.substr(colon + 1)));
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            return std::nullopt;
        const unsigned long number = std::strtoul(port.c_str(), nullptr, 10);
        if (number == 0 || number > 65535) return std::nullopt;
        server.port = uint16_t(number);
        text = trimmed(text.substr(0, colon));
    }
    if (text.empty() || text.find_first_of(" \t/") != std::string_view::npos) return std::nullopt;
    server.host = std::string(text);
    return server;
}

std::optional<LiveHello> parse_live_hello(std::string_view line) {
    std::istringstream words{std::string(line)};
    std::string ok, number, address, mac, machine;
    words >> ok >> number >> address >> mac >> machine;
    if (ok != "OK" || number.empty() || !std::all_of(number.begin(), number.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) ||
        !all_hex(address) || mac.size() != 12 || !all_hex(mac) || !all_hex(machine))
        return std::nullopt;
    LiveHello hello;
    hello.console = uint32_t(std::strtoul(number.c_str(), nullptr, 10));
    hello.address = uint32_t(std::strtoull(address.c_str(), nullptr, 16));
    for (size_t i = 0; i < 6; ++i) hello.mac[i] = uint8_t(std::strtoul(mac.substr(2 * i, 2).c_str(), nullptr, 16));
    hello.machine = std::strtoull(machine.c_str(), nullptr, 16);
    // A server from before the token sends none; anything else there is
    // not one.
    std::string token;
    if (words >> token && token.size() == 16 && all_hex(token)) {
        std::array<uint8_t, 8> bytes{};
        for (size_t i = 0; i < 8; ++i) bytes[i] = uint8_t(std::strtoul(token.substr(2 * i, 2).c_str(), nullptr, 16));
        hello.token = bytes;
    }
    return hello;
}

std::vector<uint8_t> live_udp_registration(const LiveHello& hello) {
    std::vector<uint8_t> datagram{0, uint8_t(hello.console >> 24), uint8_t(hello.console >> 16),
                                  uint8_t(hello.console >> 8), uint8_t(hello.console)};
    if (hello.token) datagram.insert(datagram.end(), hello.token->begin(), hello.token->end());
    return datagram;
}

bool live_ping_wants_password(std::string_view line) {
    if (!parse_live_ping(line)) return false;
    std::istringstream words{std::string(line)};
    std::string word;
    for (int i = 0; i < 3; ++i) words >> word;
    while (words >> word)
        if (word == "password") return true;
    return false;
}

std::string live_password_hex(std::string_view password) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (unsigned char c : password) {
        text += digits[c >> 4];
        text += digits[c & 15];
    }
    return text;
}

std::optional<int> parse_live_ping(std::string_view line) {
    std::istringstream words{std::string(line)};
    std::string ok, name, version;
    words >> ok >> name >> version;
    if (ok != "OK" || name != "freeriders" || version.empty() ||
        !std::all_of(version.begin(), version.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        return std::nullopt;
    return std::atoi(version.c_str());
}

const char* live_reach_name(LiveReach reach) {
    switch (reach) {
    case LiveReach::ok: return "ok";
    case LiveReach::older: return "older";
    case LiveReach::newer: return "newer";
    case LiveReach::bad_address: return "bad-address";
    case LiveReach::not_found: return "address";
    case LiveReach::refused: return "refused";
    case LiveReach::no_answer: return "no-answer";
    case LiveReach::unexpected: return "unexpected";
    case LiveReach::password_required: return "password-required";
    case LiveReach::wrong_password: return "wrong-password";
    }
    return "?";
}

LiveSocket open_live_connection(const LiveServerAddress& server, int timeout_ms, LiveReach& failure) {
    start_sockets();
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    const std::string port = std::to_string(server.port);
    if (getaddrinfo(server.host.c_str(), port.c_str(), &hints, &found) != 0 || !found) {
        failure = LiveReach::not_found;
        return no_live_socket;
    }
    const LiveSocket made = LiveSocket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (made == no_live_socket) {
        freeaddrinfo(found);
        failure = LiveReach::no_answer;
        return no_live_socket;
    }
    // Without a limit, connect waits for the system's own (21 s on Windows)
    // when the device is off or a firewall drops the packets.
    set_blocking(made, false);
    int started = ::connect(made, found->ai_addr, int(found->ai_addrlen));
    freeaddrinfo(found);
    bool connected = started == 0;
    failure = LiveReach::no_answer;
    if (!connected) {
        const int error = socket_error();
#ifdef _WIN32
        const bool waiting = error == WSAEWOULDBLOCK;
#else
        const bool waiting = error == EINPROGRESS;
#endif
        if (waiting) {
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(made, &writable);
            FD_SET(made, &failed);
            timeval wait{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            if (select(int(made + 1), nullptr, &writable, &failed, &wait) > 0) {
                int result = 0;
                socklen_t size = sizeof(result);
                getsockopt(made, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&result), &size);
#ifdef _WIN32
                const bool refused = result == WSAECONNREFUSED;
#else
                const bool refused = result == ECONNREFUSED;
#endif
                connected = result == 0 && !FD_ISSET(made, &failed);
                if (refused) failure = LiveReach::refused;
            }
        } else {
#ifdef _WIN32
            if (error == WSAECONNREFUSED) failure = LiveReach::refused;
#else
            if (error == ECONNREFUSED) failure = LiveReach::refused;
#endif
        }
    }
    if (!connected) {
        close_live_connection(made);
        return no_live_socket;
    }
    set_blocking(made, true);
    int on = 1;
    setsockopt(made, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
    failure = LiveReach::ok;
    return made;
}

void close_live_connection(LiveSocket socket) {
    if (socket == no_live_socket) return;
#ifdef _WIN32
    closesocket(SOCKET(socket));
#else
    close(socket);
#endif
}

std::vector<std::string> live_exchange(LiveSocket socket, const std::string& line, std::string& pending, int timeout_ms) {
    const std::string text = line + '\n';
    if (::send(socket, text.data(), int(text.size()), 0) != int(text.size())) return {};
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::vector<std::string> lines;
    for (;;) {
        const auto end = pending.find('\n');
        if (end != std::string::npos) {
            std::string answer = pending.substr(0, end);
            pending.erase(0, end + 1);
            if (!answer.empty() && answer.back() == '\r') answer.pop_back();
            if (answer == ".") return lines;
            lines.push_back(std::move(answer));
            continue;
        }
        if (timeout_ms > 0) {
            const int left = remaining_ms(deadline);
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(socket, &readable);
            timeval wait{left / 1000, (left % 1000) * 1000};
            if (left <= 0 || select(int(socket + 1), &readable, nullptr, nullptr, &wait) <= 0) return {};
        }
        char buffer[4096];
        const int got = recv(socket, buffer, sizeof(buffer), 0);
        if (got <= 0) return {};
        pending.append(buffer, size_t(got));
        // Something that is not this service may send anything, or nothing
        // that ends a line: a few kilobytes without an answer are enough.
        if (pending.size() > 65536) return {};
    }
}

LiveSignIn live_sign_in(LiveSocket socket, std::string_view password, uint64_t xuid, const std::string& gamertag,
                        std::string& pending, int timeout_ms) {
    LiveSignIn result;
    if (!password.empty()) {
        const auto answer = live_exchange(socket, "PASS " + live_password_hex(password), pending, timeout_ms);
        if (answer.empty()) {
            result.failure = LiveReach::unexpected;
            return result;
        }
        if (answer[0] == "ERR wrong password") {
            result.failure = LiveReach::wrong_password;
            return result;
        }
        // "OK", or a server from before passwords that knows no PASS.
    }
    char line[128];
    std::snprintf(line, sizeof(line), "HELLO %016llx %s", static_cast<unsigned long long>(xuid), gamertag.c_str());
    const auto answer = live_exchange(socket, line, pending, timeout_ms);
    const std::string first = answer.empty() ? std::string() : answer[0];
    result.hello = parse_live_hello(first);
    if (!result.hello)
        result.failure = first == "ERR password required" ? LiveReach::password_required : LiveReach::unexpected;
    return result;
}

LiveCheck check_live_server(std::string_view text, int timeout_ms, std::string_view password) {
    LiveCheck check;
    const auto server = parse_live_server(text);
    if (!server) return check;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    const LiveSocket socket = open_live_connection(*server, timeout_ms, check.reach);
    if (socket == no_live_socket) return check;
    sockaddr_in peer{};
    socklen_t size = sizeof(peer);
    if (getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &size) == 0)
        check.address = dotted(ntohl(peer.sin_addr.s_addr));
    std::string pending;
    const auto answer = live_exchange(socket, "PING", pending, std::max(remaining_ms(deadline), 500));
    const std::string first = answer.empty() ? std::string() : answer[0];
    if (const auto version = parse_live_ping(first)) {
        check.version = *version;
        check.reach = *version == live_protocol_version ? LiveReach::ok : LiveReach::newer;
        check.password = live_ping_wants_password(first);
        if (check.password && check.reach == LiveReach::ok) {
            if (password.empty()) {
                check.reach = LiveReach::password_required;
            } else {
                const auto passed = live_exchange(socket, "PASS " + live_password_hex(password), pending,
                                                  std::max(remaining_ms(deadline), 500));
                if (passed.empty() || passed[0] != "OK")
                    check.reach = !passed.empty() && passed[0] == "ERR wrong password" ? LiveReach::wrong_password
                                                                                       : LiveReach::unexpected;
            }
        }
    } else if (first == "ERR unknown request" || first == "ERR hello first") {
        // The game's own service and scripts/live_server.py before PING.
        check.reach = LiveReach::older;
    } else {
        check.reach = LiveReach::unexpected;
    }
    close_live_connection(socket);
    return check;
}

std::vector<std::string> local_ipv4_addresses() {
    start_sockets();
    std::vector<uint32_t> found;
    const auto add = [&](uint32_t address) {
        if ((address >> 24) == 127 || (address >> 16) == 0xA9FE || address == 0) return;  // loopback, link-local
        if (std::find(found.begin(), found.end(), address) == found.end()) found.push_back(address);
    };
    // The address the system would send from to reach other networks: on a
    // PC with virtual adapters (WSL, Hyper-V), the one the LAN sees. A UDP
    // connect sends nothing.
    if (const auto probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); LiveSocket(probe) != no_live_socket) {
        sockaddr_in outside{};
        outside.sin_family = AF_INET;
        outside.sin_port = htons(53);
        outside.sin_addr.s_addr = htonl(0x08080808);
        sockaddr_in local{};
        socklen_t size = sizeof(local);
        if (::connect(probe, reinterpret_cast<sockaddr*>(&outside), sizeof(outside)) == 0 &&
            getsockname(probe, reinterpret_cast<sockaddr*>(&local), &size) == 0)
            add(ntohl(local.sin_addr.s_addr));
        close_live_connection(LiveSocket(probe));
    }
    std::vector<uint32_t> others;
#ifdef _WIN32
    // Adapters that are up and have a gateway: a LAN or Wi-Fi, not the
    // virtual switches of WSL or Hyper-V.
    ULONG bytes = 16384;
    std::vector<uint8_t> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(bytes);
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bytes);
    }
    if (result == NO_ERROR)
        for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !adapter->FirstGatewayAddress)
                continue;
            for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
                if (unicast->Address.lpSockaddr->sa_family == AF_INET)
                    others.push_back(ntohl(reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr.s_addr));
        }
#else
    if (ifaddrs* list = nullptr; getifaddrs(&list) == 0) {
        for (ifaddrs* entry = list; entry; entry = entry->ifa_next)
            if (entry->ifa_addr && entry->ifa_addr->sa_family == AF_INET)
                others.push_back(ntohl(reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr));
        freeifaddrs(list);
    }
#endif
    std::stable_sort(others.begin(), others.end(), [](uint32_t a, uint32_t b) { return is_private(a) > is_private(b); });
    for (uint32_t address : others) add(address);
    std::vector<std::string> text;
    for (uint32_t address : found) text.push_back(dotted(address));
    return text;
}
}
