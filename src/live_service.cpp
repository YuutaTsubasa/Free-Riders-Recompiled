#include "live_service.h"

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
static constexpr socket_t no_socket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static void close_socket(socket_t s) { close(s); }
static constexpr socket_t no_socket = -1;
#endif

#include <algorithm>
#include <cerrno>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

namespace sfr {
namespace {
std::string hex(const std::vector<uint8_t>& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (uint8_t b : bytes) {
        text += digits[b >> 4];
        text += digits[b & 15];
    }
    return text.empty() ? "-" : text;
}
std::vector<uint8_t> unhex(const std::string& text) {
    std::vector<uint8_t> bytes;
    if (text == "-") return bytes;
    for (size_t i = 0; i + 1 < text.size(); i += 2)
        bytes.push_back(uint8_t(std::strtoul(text.substr(i, 2).c_str(), nullptr, 16)));
    return bytes;
}
std::vector<std::string> split(const std::string& line) {
    std::istringstream stream(line);
    std::vector<std::string> out;
    for (std::string word; stream >> word;) out.push_back(word);
    return out;
}

struct Console {
    uint32_t number = 0, address = 0;
    std::array<uint8_t, 6> mac{};
    uint64_t machine = 0, xuid = 0;
    std::string name, pending;
    socket_t control = no_socket;
    sockaddr_storage udp{};
    socklen_t udp_size = 0;
};

struct Session {
    std::vector<uint8_t> id, key, nonce, properties, contexts, qos;
    uint32_t host = 0, flags = 0, public_slots = 0, private_slots = 0;
    std::map<uint64_t, bool> members;
    std::string state = "lobby";
    uint32_t filled(bool is_private) const {
        return uint32_t(std::count_if(members.begin(), members.end(), [&](const auto& m) { return m.second == is_private; }));
    }
};
}

struct LiveService::Impl {
    socket_t listener = no_socket, datagrams = no_socket;
    std::atomic<bool> stopping{false};
    std::thread worker;
    std::map<uint32_t, Console> consoles;  // by number
    std::map<std::string, Session> sessions;  // by id (hex)
    uint32_t next_number = 1;
    std::mt19937_64 random{std::random_device{}()};

    std::vector<uint8_t> random_bytes(size_t size) {
        std::vector<uint8_t> bytes(size);
        for (auto& b : bytes) b = uint8_t(random());
        return bytes;
    }
    Console* by_address(uint32_t address) {
        for (auto& [number, console] : consoles)
            if (console.address == address) return &console;
        return nullptr;
    }
    std::string line_of(const Session& s) {
        const Console& host = consoles[s.host];
        char text[160];
        std::snprintf(text, sizeof(text), " %08x %s %016llx %x %u %u %u %u ", host.address,
                      hex(std::vector<uint8_t>(host.mac.begin(), host.mac.end())).c_str(),
                      static_cast<unsigned long long>(host.machine), s.flags, s.public_slots, s.private_slots,
                      s.filled(false), s.filled(true));
        return hex(s.id) + ' ' + hex(s.key) + text + hex(s.properties) + ' ' + hex(s.contexts);
    }

    std::vector<std::string> answer(Console& console, const std::vector<std::string>& w) {
        const std::string& verb = w[0];
        char text[160];
        if (verb == "HELLO") {
            console.xuid = std::strtoull(w.at(1).c_str(), nullptr, 16);
            console.name.clear();
            for (size_t i = 2; i < w.size(); ++i) console.name += (i > 2 ? " " : "") + w[i];
            std::snprintf(text, sizeof(text), "OK %u %08x %s %016llx", console.number, console.address,
                          hex(std::vector<uint8_t>(console.mac.begin(), console.mac.end())).c_str(),
                          static_cast<unsigned long long>(console.machine));
            std::cerr << "LIVE_SERVICE console=" << console.number << " gamertag=" << console.name << '\n';
            return {text};
        }
        if (verb == "CREATE") {
            Session s;
            s.id = random_bytes(8);
            s.key = random_bytes(16);
            s.nonce = random_bytes(8);
            s.host = console.number;
            s.flags = uint32_t(std::strtoul(w.at(1).c_str(), nullptr, 16));
            s.public_slots = uint32_t(std::stoul(w.at(2)));
            s.private_slots = uint32_t(std::stoul(w.at(3)));
            s.properties = unhex(w.at(4));
            s.contexts = unhex(w.at(5));
            const std::string id = hex(s.id);
            std::cerr << "LIVE_SERVICE session=" << id << " host=" << console.name << '\n';
            const std::string reply = "OK " + id + ' ' + hex(s.key) + ' ' + hex(s.nonce);
            sessions[id] = std::move(s);
            return {reply};
        }
        if (verb == "SEARCH") {
            const size_t limit = std::stoul(w.at(1));
            std::vector<std::string> found;
            for (const auto& [id, s] : sessions)
                if (s.host != console.number && s.state == "lobby" && s.filled(false) < s.public_slots &&
                    found.size() < limit)
                    found.push_back(line_of(s));
            std::vector<std::string> reply{"OK " + std::to_string(found.size())};
            reply.insert(reply.end(), found.begin(), found.end());
            return reply;
        }
        auto found = sessions.find(w.size() > 1 ? w[1] : "");
        if (verb == "PEER") {
            Console* peer = by_address(uint32_t(std::strtoul(w.at(1).c_str(), nullptr, 16)));
            if (!peer) return {"ERR unknown peer"};
            std::snprintf(text, sizeof(text), "OK %s %016llx", hex(std::vector<uint8_t>(peer->mac.begin(), peer->mac.end())).c_str(),
                          static_cast<unsigned long long>(peer->machine));
            return {text};
        }
        if (verb == "DELETE") {
            if (found != sessions.end()) sessions.erase(found);
            return {"OK"};
        }
        if (found == sessions.end()) return {"ERR unknown session"};
        Session& s = found->second;
        if (verb == "SESSION") {
            std::vector<std::string> reply{"OK " + std::to_string(s.members.size()), line_of(s)};
            for (const auto& [xuid, is_private] : s.members) {
                std::snprintf(text, sizeof(text), "%016llx %d", static_cast<unsigned long long>(xuid), int(is_private));
                reply.push_back(text);
            }
            return reply;
        }
        if (verb == "JOIN") {
            for (size_t i = 2; i < w.size(); ++i) {
                const auto colon = w[i].find(':');
                s.members[std::strtoull(w[i].substr(0, colon).c_str(), nullptr, 16)] =
                    colon != std::string::npos && w[i].substr(colon + 1) == "1";
            }
            std::cerr << "LIVE_SERVICE session=" << w[1] << " members=" << s.members.size() << '\n';
            return {"OK"};
        }
        if (verb == "LEAVE") {
            for (size_t i = 2; i < w.size(); ++i) s.members.erase(std::strtoull(w[i].c_str(), nullptr, 16));
            return {"OK"};
        }
        if (verb == "MODIFY") {
            s.flags = uint32_t(std::strtoul(w.at(2).c_str(), nullptr, 16));
            s.public_slots = uint32_t(std::stoul(w.at(3)));
            s.private_slots = uint32_t(std::stoul(w.at(4)));
            return {"OK"};
        }
        if (verb == "STATE") {
            s.state = w.at(2);
            return {"OK"};
        }
        if (verb == "QOSSET") {
            s.qos = w.size() > 2 ? unhex(w[2]) : std::vector<uint8_t>{};
            return {"OK"};
        }
        if (verb == "QOSGET") return {"OK " + hex(s.qos)};
        return {"ERR unknown request"};
    }

    void goodbye(uint32_t number) {
        std::cerr << "LIVE_SERVICE left console=" << number << '\n';
        for (auto it = sessions.begin(); it != sessions.end();)
            it = it->second.host == number ? sessions.erase(it) : std::next(it);
        const uint64_t xuid = consoles[number].xuid;
        for (auto& [id, s] : sessions) s.members.erase(xuid);
        close_socket(consoles[number].control);
        consoles.erase(number);
    }

    void run() {
        std::vector<uint8_t> datagram(65536);
        while (!stopping) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener, &readable);
            FD_SET(datagrams, &readable);
            socket_t highest = (std::max)(listener, datagrams);
            for (const auto& [number, console] : consoles) {
                FD_SET(console.control, &readable);
                highest = (std::max)(highest, console.control);
            }
            timeval wait{0, 200000};
            if (select(int(highest + 1), &readable, nullptr, nullptr, &wait) <= 0) continue;
            if (FD_ISSET(listener, &readable)) {
                const socket_t accepted = accept(listener, nullptr, nullptr);
                if (accepted != no_socket) {
                    Console console;
                    console.number = next_number++;
                    console.address = (10u << 24) | (77u << 16) | console.number;
                    console.mac = {0x02, 0x46, 0x52, 0x00, uint8_t(console.number >> 8), uint8_t(console.number)};
                    console.machine = 0xFA00000000000000ull | console.number;
                    console.control = accepted;
                    consoles[console.number] = console;
                }
            }
            if (FD_ISSET(datagrams, &readable)) {
                sockaddr_storage from{};
                socklen_t from_size = sizeof(from);
                const int got = recvfrom(datagrams, reinterpret_cast<char*>(datagram.data()), int(datagram.size()), 0,
                                         reinterpret_cast<sockaddr*>(&from), &from_size);
                const auto word = [&](int at) {
                    return uint32_t(datagram[at]) << 24 | uint32_t(datagram[at + 1]) << 16 | uint32_t(datagram[at + 2]) << 8 | datagram[at + 3];
                };
                if (got >= 5 && datagram[0] == 0) {
                    const auto console = consoles.find(word(1));
                    if (console != consoles.end()) {
                        console->second.udp = from;
                        console->second.udp_size = from_size;
                    }
                } else if (got >= 13 && datagram[0] == 1) {
                    Console* to = by_address(word(1));
                    if (to && to->udp_size)
                        sendto(datagrams, reinterpret_cast<const char*>(datagram.data()), got, 0,
                               reinterpret_cast<const sockaddr*>(&to->udp), to->udp_size);
                }
            }
            std::vector<uint32_t> gone;
            for (auto& [number, console] : consoles) {
                if (!FD_ISSET(console.control, &readable)) continue;
                char buffer[4096];
                const int got = recv(console.control, buffer, sizeof(buffer), 0);
                if (got <= 0) {
                    gone.push_back(number);
                    continue;
                }
                console.pending.append(buffer, size_t(got));
                for (size_t end; (end = console.pending.find('\n')) != std::string::npos;) {
                    const std::string line = console.pending.substr(0, end);
                    console.pending.erase(0, end + 1);
                    const auto w = split(line);
                    if (w.empty()) continue;
                    std::vector<std::string> reply;
                    try {
                        reply = answer(console, w);
                    } catch (const std::exception& error) {
                        reply = {std::string("ERR ") + error.what()};
                    }
                    std::string text;
                    for (const auto& r : reply) text += r + '\n';
                    text += ".\n";
                    send(console.control, text.data(), int(text.size()), 0);
                }
            }
            for (uint32_t number : gone) goodbye(number);
        }
    }
};

std::unique_ptr<LiveService> LiveService::start(uint16_t port) {
#ifdef _WIN32
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    std::unique_ptr<LiveService> service(new LiveService());
    auto impl = std::make_unique<Impl>();
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_port = htons(port);
    impl->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    impl->datagrams = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int on = 1;
    if (impl->listener != no_socket)
        setsockopt(impl->listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
    // Which step failed, and the system's error (on Android, socket fails
    // with EACCES without the INTERNET permission).
    const char* failed = impl->listener == no_socket || impl->datagrams == no_socket ? "socket"
        : bind(impl->listener, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0  ? "bind-tcp"
        : listen(impl->listener, 16) != 0                                            ? "listen"
        : bind(impl->datagrams, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0 ? "bind-udp"
                                                                                     : nullptr;
    if (failed) {
#ifdef _WIN32
        const int error = WSAGetLastError();
#else
        const int error = errno;
#endif
        std::cerr << "LIVE_SERVICE started=0 port=" << port << " reason=" << failed << " error=" << error << '\n';
        if (impl->listener != no_socket) close_socket(impl->listener);
        if (impl->datagrams != no_socket) close_socket(impl->datagrams);
        return nullptr;
    }
    std::cerr << "LIVE_SERVICE started=1 port=" << port << '\n';
    Impl* raw = impl.get();
    impl->worker = std::thread([raw] { raw->run(); });
    service->impl_ = std::move(impl);
    return service;
}

LiveService::~LiveService() {
    if (!impl_) return;
    impl_->stopping = true;
    if (impl_->worker.joinable()) impl_->worker.join();
    for (auto& [number, console] : impl_->consoles) close_socket(console.control);
    close_socket(impl_->listener);
    close_socket(impl_->datagrams);
}

std::string start_hosted_live_service() {
    static const std::string address = []() -> std::string {
        const char* host = std::getenv("SFR_LIVE_HOST");
        if (!host || *host != '1') return {};
        const char* port_text = std::getenv("SFR_LIVE_PORT");
        const uint16_t port = uint16_t(port_text && *port_text ? std::strtoul(port_text, nullptr, 10) : 47800);
        // Kept for the life of the process: the other games depend on it.
        static std::unique_ptr<LiveService> service = LiveService::start(port);
        return service ? "127.0.0.1:" + std::to_string(port) : std::string();
    }();
    return address;
}
}
