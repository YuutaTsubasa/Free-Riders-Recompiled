#include "live_client.h"
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
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static void close_socket(socket_t s) { close(s); }
static constexpr socket_t no_socket = -1;
#endif

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <random>
#include <thread>

namespace sfr {
namespace {
uint64_t parse_hex(const std::string& text) { return std::strtoull(text.c_str(), nullptr, 16); }

uint64_t fnv(const std::string& text, uint64_t hash = 1469598103934665603ull) {
    for (unsigned char c : text) hash = (hash ^ c) * 1099511628211ull;
    return hash;
}
}

std::string live_gamertag() {
    for (const char* name : {"SFR_LIVE_NAME", "SFR_PROFILE_NAME"})
        if (const char* value = std::getenv(name); value && *value) return std::string(value).substr(0, 15);
    return "Player";
}

uint64_t live_xuid() {
    std::string id;
    if (const char* setting = std::getenv("SFR_LIVE_ID"); setting && *setting) {
        id = setting;
    } else {
        // Made once per install and kept beside the game's files.
        if (FILE* file = std::fopen("live-id.txt", "rb")) {
            char text[64] = {};
            const size_t got = std::fread(text, 1, sizeof(text) - 1, file);
            std::fclose(file);
            id.assign(text, got);
        }
        while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
        if (id.empty()) {
            std::random_device device;
            char text[32];
            std::snprintf(text, sizeof(text), "%08x%08x", device(), device());
            id = text;
            if (FILE* file = std::fopen("live-id.txt", "wb")) {
                std::fputs(id.c_str(), file);
                std::fclose(file);
            }
        }
    }
    return 0x0009000000000000ull | (fnv(id) & 0x0000FFFFFFFFFFFFull);
}

struct LiveClient::Impl {
    socket_t control = no_socket, datagrams = no_socket;
    sockaddr_storage server{};
    socklen_t server_size = 0;
    std::string pending;
    std::mutex request_mutex;
    std::mutex queue_mutex;
    std::condition_variable arrived;
    std::map<uint16_t, std::deque<Datagram>> queues;
    std::thread receiver;
    bool stopping = false;
};

LiveClient* LiveClient::instance() {
    static LiveClient* client = []() -> LiveClient* {
        // SFR_LIVE_HOST=1 runs the service in this game and connects to it.
        std::string server = start_hosted_live_service();
        if (server.empty())
            if (const char* setting = std::getenv("SFR_LIVE_SERVER"); setting && *setting) server = setting;
        if (server.empty()) return nullptr;
        auto made = new LiveClient();
        if (!made->connect(server)) {
            delete made;
            return nullptr;
        }
        return made;
    }();
    return client;
}

LiveClient::~LiveClient() {
    if (!impl_) return;
    impl_->stopping = true;
    if (impl_->datagrams != no_socket) close_socket(impl_->datagrams);
    if (impl_->control != no_socket) close_socket(impl_->control);
    if (impl_->receiver.joinable()) impl_->receiver.join();
}

bool LiveClient::connect(const std::string& server) {
#ifdef _WIN32
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    impl_ = std::make_unique<Impl>();
    std::string host = server, port = "47800";
    if (const auto colon = server.rfind(':'); colon != std::string::npos) {
        host = server.substr(0, colon);
        port = server.substr(colon + 1);
    }
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || !found) {
        std::cerr << "LIVE_SERVER unreachable=" << server << " reason=address\n";
        return false;
    }
    std::memcpy(&impl_->server, found->ai_addr, found->ai_addrlen);
    impl_->server_size = socklen_t(found->ai_addrlen);
    impl_->control = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const bool connected = impl_->control != no_socket &&
        ::connect(impl_->control, found->ai_addr, int(found->ai_addrlen)) == 0;
    freeaddrinfo(found);
    if (!connected) {
        std::cerr << "LIVE_SERVER unreachable=" << server << " reason=connect\n";
        return false;
    }
    int on = 1;
    setsockopt(impl_->control, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));

    const std::string name = live_gamertag();
    xuid_ = live_xuid();
    char line[128];
    std::snprintf(line, sizeof(line), "HELLO %016llx %s", static_cast<unsigned long long>(xuid_), name.c_str());
    const auto hello = request(line);
    std::istringstream words(hello.empty() ? std::string() : hello[0]);
    std::string ok, number, address, mac, machine;
    words >> ok >> number >> address >> mac >> machine;
    if (ok != "OK" || mac.size() != 12) {
        std::cerr << "LIVE_SERVER unreachable=" << server << " reason=hello\n";
        return false;
    }
    address_ = uint32_t(parse_hex(address));
    for (size_t i = 0; i < 6; ++i) mac_[i] = uint8_t(parse_hex(mac.substr(2 * i, 2)));
    machine_ = parse_hex(machine);

    impl_->datagrams = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->datagrams == no_socket) return false;
    sockaddr_in any{};
    any.sin_family = AF_INET;
    bind(impl_->datagrams, reinterpret_cast<sockaddr*>(&any), sizeof(any));
    // Tells the service where this console's datagrams come from; repeated
    // by the receiver until the first datagram comes back is not needed on a
    // LAN, so it is sent a few times now.
    uint8_t hello_datagram[5] = {0};
    const uint32_t console = uint32_t(std::strtoul(number.c_str(), nullptr, 10));
    for (int i = 0; i < 4; ++i) hello_datagram[1 + i] = uint8_t(console >> (24 - 8 * i));
    for (int i = 0; i < 3; ++i)
        sendto(impl_->datagrams, reinterpret_cast<const char*>(hello_datagram), sizeof(hello_datagram), 0,
               reinterpret_cast<const sockaddr*>(&impl_->server), impl_->server_size);
    impl_->receiver = std::thread([this] { receive_loop(); });
    std::cerr << "LIVE_SERVER connected=" << server << " console=" << console << " address=10.77."
              << (address_ >> 8 & 0xFF) << '.' << (address_ & 0xFF) << " xuid=0x" << std::hex << xuid_ << std::dec
              << " gamertag=" << name << '\n';
    return true;
}

std::vector<std::string> LiveClient::request(const std::string& line) {
    std::lock_guard lock(impl_->request_mutex);
    const std::string text = line + '\n';
    if (::send(impl_->control, text.data(), int(text.size()), 0) != int(text.size())) return {};
    std::vector<std::string> lines;
    for (;;) {
        const auto end = impl_->pending.find('\n');
        if (end != std::string::npos) {
            std::string answer = impl_->pending.substr(0, end);
            impl_->pending.erase(0, end + 1);
            if (!answer.empty() && answer.back() == '\r') answer.pop_back();
            if (answer == ".") return lines;
            lines.push_back(std::move(answer));
            continue;
        }
        char buffer[4096];
        const int got = recv(impl_->control, buffer, sizeof(buffer), 0);
        if (got <= 0) return {};
        impl_->pending.append(buffer, size_t(got));
    }
}

void LiveClient::open_port(uint16_t port) {
    std::lock_guard lock(impl_->queue_mutex);
    impl_->queues[port];
}

void LiveClient::close_port(uint16_t port) {
    std::lock_guard lock(impl_->queue_mutex);
    impl_->queues.erase(port);
}

void LiveClient::send(uint32_t to, uint16_t to_port, uint16_t from_port, const uint8_t* data, size_t size) {
    if (to == address_ || to == 0x7F000001) {
        Datagram datagram;
        datagram.from = address_;
        datagram.from_port = from_port;
        datagram.payload.assign(data, data + size);
        std::lock_guard lock(impl_->queue_mutex);
        const auto queue = impl_->queues.find(to_port);
        if (queue != impl_->queues.end() && queue->second.size() < 1024) queue->second.push_back(std::move(datagram));
        impl_->arrived.notify_all();
        return;
    }
    std::vector<uint8_t> datagram(13 + size);
    datagram[0] = 1;
    for (int i = 0; i < 4; ++i) datagram[1 + i] = uint8_t(to >> (24 - 8 * i));
    datagram[5] = uint8_t(to_port >> 8);
    datagram[6] = uint8_t(to_port);
    for (int i = 0; i < 4; ++i) datagram[7 + i] = uint8_t(address_ >> (24 - 8 * i));
    datagram[11] = uint8_t(from_port >> 8);
    datagram[12] = uint8_t(from_port);
    if (size) std::memcpy(datagram.data() + 13, data, size);
    sendto(impl_->datagrams, reinterpret_cast<const char*>(datagram.data()), int(datagram.size()), 0,
           reinterpret_cast<const sockaddr*>(&impl_->server), impl_->server_size);
}

void LiveClient::receive_loop() {
    std::vector<uint8_t> buffer(65536);
    while (!impl_->stopping) {
        const int got = recv(impl_->datagrams, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0);
        if (got <= 0) {
            if (impl_->stopping) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (got < 13 || buffer[0] != 1) continue;
        const auto word = [&](int at) {
            return uint32_t(buffer[at]) << 24 | uint32_t(buffer[at + 1]) << 16 | uint32_t(buffer[at + 2]) << 8 | buffer[at + 3];
        };
        const uint16_t port = uint16_t(buffer[5] << 8 | buffer[6]);
        Datagram datagram;
        datagram.from = word(7);
        datagram.from_port = uint16_t(buffer[11] << 8 | buffer[12]);
        datagram.payload.assign(buffer.begin() + 13, buffer.begin() + got);
        std::lock_guard lock(impl_->queue_mutex);
        const auto queue = impl_->queues.find(port);
        if (queue == impl_->queues.end()) continue;  // nobody bound: dropped, as UDP would
        if (queue->second.size() < 1024) queue->second.push_back(std::move(datagram));
        impl_->arrived.notify_all();
    }
}

std::optional<LiveClient::Datagram> LiveClient::receive(uint16_t port, int timeout_ms) {
    std::unique_lock lock(impl_->queue_mutex);
    const auto ready = [&] {
        const auto queue = impl_->queues.find(port);
        return queue == impl_->queues.end() || !queue->second.empty();
    };
    if (timeout_ms > 0) impl_->arrived.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
    const auto queue = impl_->queues.find(port);
    if (queue == impl_->queues.end() || queue->second.empty()) return std::nullopt;
    Datagram datagram = std::move(queue->second.front());
    queue->second.pop_front();
    return datagram;
}

bool LiveClient::pending(uint16_t port) {
    std::lock_guard lock(impl_->queue_mutex);
    const auto queue = impl_->queues.find(port);
    return queue != impl_->queues.end() && !queue->second.empty();
}
}
