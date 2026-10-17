#include "live_connect.h"
#include "live_service.h"

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void addresses_are_read() {
    auto plain = sfr::parse_live_server("192.168.1.10");
    require(plain && plain->host == "192.168.1.10" && plain->port == 47800, "an address alone takes port 47800");
    auto named = sfr::parse_live_server("  rider-pc.local:1234 ");
    require(named && named->host == "rider-pc.local" && named->port == 1234, "a name and port, spaces dropped");
    require(!sfr::parse_live_server(""), "nothing is no address");
    require(!sfr::parse_live_server("   "), "spaces are no address");
    require(!sfr::parse_live_server("host:"), "an empty port");
    require(!sfr::parse_live_server(":47800"), "a port without a host");
    require(!sfr::parse_live_server("host:0"), "port 0");
    require(!sfr::parse_live_server("host:65536"), "a port past 65535");
    require(!sfr::parse_live_server("host:47a"), "a port that is not a number");
    require(!sfr::parse_live_server("fe80::1"), "IPv6 (the service is IPv4 only)");
    require(!sfr::parse_live_server("my host"), "a space inside");
    require(!sfr::parse_live_server("http://host"), "a web address");
}

void answers_are_read() {
    const auto hello = sfr::parse_live_hello("OK 3 0a4d0003 024652000003 fa00000000000003");
    require(hello && hello->console == 3 && hello->address == 0x0A4D0003 && hello->mac[0] == 0x02 && hello->mac[5] == 0x03 &&
                hello->machine == 0xFA00000000000003ull,
            "HELLO's answer");
    require(!sfr::parse_live_hello("ERR hello first"), "an error is no HELLO answer");
    require(!sfr::parse_live_hello("OK 3 0a4d0003 0246520000 fa00000000000003"), "a short MAC");
    require(!sfr::parse_live_hello("OK 3 0a4d0003 02465200000z fa00000000000003"), "a MAC that is not hex");
    require(!sfr::parse_live_hello("OK 3 0a4d0003 024652000003"), "no machine");
    require(!sfr::parse_live_hello(""), "nothing");
    require(sfr::parse_live_ping("OK freeriders 1") == 1, "PING's answer");
    require(sfr::parse_live_ping("OK freeriders 7") == 7, "another version");
    require(!sfr::parse_live_ping("OK 1 0a4d0001 024652000001 fa00000000000001"), "a HELLO answer is not PING's");
    require(!sfr::parse_live_ping("ERR unknown request"), "an older server's answer");
    require(!sfr::parse_live_ping("HTTP/1.1 400 Bad Request"), "a web server");
}

// The game's own service on a port of this device that is free.
std::unique_ptr<sfr::LiveService> start_service(uint16_t& port) {
    for (port = 47911; port < 47960; ++port)
        if (auto service = sfr::LiveService::start(port)) return service;
    throw std::runtime_error("no free port for the service");
}

std::string at(uint16_t port) { return "127.0.0.1:" + std::to_string(port); }

void the_game_service_answers() {
    uint16_t port = 0;
    auto service = start_service(port);
    const auto check = sfr::check_live_server(at(port), 3000);
    require(check.reach == sfr::LiveReach::ok, "the game's service answers PING");
    require(check.version == sfr::live_protocol_version && check.address == "127.0.0.1", "its version and address");
    // A check leaves joining as it was: a HELLO after it is answered as usual.
    sfr::LiveReach failure = sfr::LiveReach::ok;
    const auto socket = sfr::open_live_connection(*sfr::parse_live_server(at(port)), 3000, failure);
    require(socket != sfr::no_live_socket && failure == sfr::LiveReach::ok, "a game connects after the check");
    std::string pending;
    const auto answer = sfr::live_exchange(socket, "HELLO 0009000000001234 Rider", pending, 3000);
    require(!answer.empty() && sfr::parse_live_hello(answer[0]), "HELLO after a check");
    const auto search = sfr::live_exchange(socket, "SEARCH 4", pending, 3000);
    require(!search.empty() && search[0] == "OK 0", "the console's requests work");
    sfr::close_live_connection(socket);
}

void nothing_listening_is_refused() {
    uint16_t port = 0;
    start_service(port).reset();  // a port nothing listens on now
    const auto started = std::chrono::steady_clock::now();
    const auto check = sfr::check_live_server(at(port), 3000);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "refused: " << sfr::live_reach_name(check.reach) << " in " << seconds << " s\n";
    // Windows tries a refused connection again for about 2 s first.
    require(check.reach == sfr::LiveReach::refused || check.reach == sfr::LiveReach::no_answer,
            "a port without a server is refused");
    require(seconds < 4.0, "within the time allowed");
}

void unknown_names_are_not_found() {
    const auto check = sfr::check_live_server("no-such-rider.invalid", 3000);
    require(check.reach == sfr::LiveReach::not_found, "a name that does not exist");
    require(sfr::check_live_server("host:99999", 3000).reach == sfr::LiveReach::bad_address, "a malformed address");
}

// A one-connection TCP server on the loopback that sends `reply` (or
// nothing) to whatever arrives.
struct FakeServer {
#ifdef _WIN32
    SOCKET listener = INVALID_SOCKET;
#else
    int listener = -1;
#endif
    uint16_t port = 0;
    std::thread worker;
    explicit FakeServer(std::string reply) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local));
        socklen_t size = sizeof(local);
        getsockname(listener, reinterpret_cast<sockaddr*>(&local), &size);
        port = ntohs(local.sin_port);
        listen(listener, 1);
        worker = std::thread([this, reply] {
            const auto client = accept(listener, nullptr, nullptr);
            char buffer[256];
            recv(client, buffer, sizeof(buffer), 0);
            if (!reply.empty()) send(client, reply.data(), int(reply.size()), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            sfr::close_live_connection(sfr::LiveSocket(client));
        });
    }
    ~FakeServer() {
        worker.join();
        sfr::close_live_connection(sfr::LiveSocket(listener));
    }
};

void other_servers_are_told_apart() {
    {
        FakeServer web("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
        require(sfr::check_live_server(at(web.port), 1000).reach == sfr::LiveReach::unexpected, "a web server");
    }
    {
        FakeServer silent("");
        require(sfr::check_live_server(at(silent.port), 1000).reach == sfr::LiveReach::unexpected, "a server that says nothing");
    }
    {
        FakeServer older("ERR unknown request\n.\n");
        require(sfr::check_live_server(at(older.port), 1000).reach == sfr::LiveReach::older, "a service from before PING");
    }
    {
        FakeServer newer("OK freeriders 99\n.\n");
        const auto check = sfr::check_live_server(at(newer.port), 1000);
        require(check.reach == sfr::LiveReach::newer && check.version == 99, "a service of another version");
    }
}

void own_addresses_leave_out_loopback() {
    for (const auto& address : sfr::local_ipv4_addresses()) {
        std::cout << "this device: " << address << '\n';
        require(address.rfind("127.", 0) != 0 && address.rfind("169.254.", 0) != 0, "no loopback or link-local address");
    }
}
}

int main() {
#ifdef _WIN32
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    try {
        addresses_are_read();
        answers_are_read();
        the_game_service_answers();
        nothing_listening_is_refused();
        unknown_names_are_not_found();
        other_servers_are_told_apart();
        own_addresses_leave_out_loopback();
        std::cout << "Live connection checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
