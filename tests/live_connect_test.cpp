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

    // The room password and the UDP token.
    require(sfr::parse_live_ping("OK freeriders 1 password") == 1, "a password server's PING answer is still read");
    require(sfr::live_ping_wants_password("OK freeriders 1 password"), "and says it wants one");
    require(!sfr::live_ping_wants_password("OK freeriders 1"), "a server without one");
    require(!sfr::live_ping_wants_password("ERR password"), "an error is no PING answer");
    require(sfr::live_password_hex("ab \xc3\xbc") == "616220c3bc", "the password's UTF-8 bytes as hex");
    const auto token = sfr::parse_live_hello("OK 3 0a4d0003 024652000003 fa00000000000003 0011223344556677");
    require(token && token->token && (*token->token)[0] == 0x00 && (*token->token)[7] == 0x77, "HELLO's token");
    require(hello && !hello->token, "a server from before the token gives none");
    const auto with = sfr::live_udp_registration(*token);
    require(with.size() == 13 && with[0] == 0 && with[4] == 3 && with[5] == 0x00 && with[12] == 0x77,
            "the registration carries the token");
    const auto without = sfr::live_udp_registration(*hello);
    require(without.size() == 5 && without[4] == 3, "and without one, the old five bytes");
}

// The game's own service on a port of this device that is free.
std::unique_ptr<sfr::LiveService> start_service(uint16_t& port, const std::string& password = {}) {
    for (port = 47911; port < 47960; ++port)
        if (auto service = sfr::LiveService::start(port, password)) return service;
    throw std::runtime_error("no free port for the service");
}

std::string at(uint16_t port) { return "127.0.0.1:" + std::to_string(port); }

sfr::LiveSocket connect_to(uint16_t port) {
    sfr::LiveReach failure = sfr::LiveReach::ok;
    const auto socket = sfr::open_live_connection(*sfr::parse_live_server(at(port)), 3000, failure);
    require(socket != sfr::no_live_socket, "a connection to the service");
    return socket;
}

void the_room_password_is_asked_for() {
    uint16_t port = 0;
    auto service = start_service(port, "ride \xc3\xb6n");
    {
        const auto check = sfr::check_live_server(at(port), 3000);
        require(check.reach == sfr::LiveReach::password_required && check.password, "a check without the password");
        require(sfr::check_live_server(at(port), 3000, "ride on").reach == sfr::LiveReach::wrong_password,
                "a check with a wrong one");
        const auto right = sfr::check_live_server(at(port), 3000, "ride \xc3\xb6n");
        require(right.reach == sfr::LiveReach::ok && right.password, "a check with the right one");
    }
    {
        // A game from before passwords says HELLO at once, and is told why not.
        const auto socket = connect_to(port);
        std::string pending;
        auto answer = sfr::live_exchange(socket, "HELLO 0009000000000001 Old", pending, 3000);
        require(!answer.empty() && answer[0] == "ERR password required", "an old game is refused, saying why");
        answer = sfr::live_exchange(socket, "SEARCH 4", pending, 3000);
        require(!answer.empty() && answer[0] == "ERR hello first", "nothing else before HELLO");
        sfr::close_live_connection(socket);
    }
    {
        const auto socket = connect_to(port);
        std::string pending;
        const auto none = sfr::live_sign_in(socket, "", 1, "Rider", pending, 3000);
        require(!none.hello && none.failure == sfr::LiveReach::password_required, "joining without the password");
        sfr::close_live_connection(socket);
    }
    {
        const auto socket = connect_to(port);
        std::string pending;
        const auto wrong = sfr::live_sign_in(socket, "ride on", 1, "Rider", pending, 3000);
        require(!wrong.hello && wrong.failure == sfr::LiveReach::wrong_password, "joining with a wrong one");
        // One try per connection: the service closes it.
        const auto after = sfr::live_exchange(socket, "PING", pending, 1000);
        require(after.empty(), "a wrong password ends the connection");
        sfr::close_live_connection(socket);
    }
    {
        const auto socket = connect_to(port);
        std::string pending;
        const auto right = sfr::live_sign_in(socket, "ride \xc3\xb6n", 1, "Rider", pending, 3000);
        require(right.hello && right.hello->token, "joining with the password, and a UDP token");
        const auto search = sfr::live_exchange(socket, "SEARCH 4", pending, 3000);
        require(!search.empty() && search[0] == "OK 0", "the console's requests work");
        sfr::close_live_connection(socket);
    }
    {
        // A server without a password takes PASS (a game that has one set
        // still joins a friend's open server).
        uint16_t open_port = 0;
        auto open = start_service(open_port);
        require(sfr::check_live_server(at(open_port), 3000, "anything").reach == sfr::LiveReach::ok,
                "a password given to an open server");
        const auto socket = connect_to(open_port);
        std::string pending;
        require(sfr::live_sign_in(socket, "anything", 1, "Rider", pending, 3000).hello.has_value(), "joining an open server");
        sfr::close_live_connection(socket);
    }
}

// A UDP socket on the loopback that waits up to 1 s for each datagram.
struct Datagrams {
#ifdef _WIN32
    SOCKET socket_ = INVALID_SOCKET;
#else
    int socket_ = -1;
#endif
    sockaddr_in server{};
    explicit Datagrams(uint16_t port) {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local));
        server.sin_family = AF_INET;
        server.sin_port = htons(port);
        server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef _WIN32
        const DWORD wait = 1000;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&wait), sizeof(wait));
#else
        const timeval wait{1, 0};
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
#endif
    }
    ~Datagrams() { sfr::close_live_connection(sfr::LiveSocket(socket_)); }
    void send(const std::vector<uint8_t>& data) {
        sendto(socket_, reinterpret_cast<const char*>(data.data()), int(data.size()), 0,
               reinterpret_cast<const sockaddr*>(&server), sizeof(server));
    }
    std::vector<uint8_t> receive() {
        std::vector<uint8_t> data(2048);
        const int got = recv(socket_, reinterpret_cast<char*>(data.data()), int(data.size()), 0);
        data.resize(got > 0 ? size_t(got) : 0);
        return data;
    }
};

std::vector<uint8_t> relayed(uint32_t to, uint32_t from, uint8_t payload) {
    return {1, uint8_t(to >> 24), uint8_t(to >> 16), uint8_t(to >> 8), uint8_t(to), 0x03, 0xE8,
            uint8_t(from >> 24), uint8_t(from >> 16), uint8_t(from >> 8), uint8_t(from), 0x03, 0xE8, payload};
}

void datagrams_go_only_between_registered_consoles() {
    uint16_t port = 0;
    auto service = start_service(port, "room");
    std::vector<sfr::LiveSocket> controls;
    std::vector<sfr::LiveHello> hellos;
    for (uint64_t xuid = 1; xuid <= 2; ++xuid) {
        controls.push_back(connect_to(port));
        std::string pending;
        const auto joined = sfr::live_sign_in(controls.back(), "room", xuid, "Rider", pending, 3000);
        require(joined.hello.has_value(), "a console joins");
        hellos.push_back(*joined.hello);
    }
    Datagrams one(port), two(port), stranger(port);
    // A password server takes no registration without the token.
    two.send({0, 0, 0, 0, uint8_t(hellos[1].console)});
    one.send(sfr::live_udp_registration(hellos[0]));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    one.send(relayed(hellos[1].address, hellos[0].address, 1));
    require(two.receive().empty(), "console 2 is not registered yet");
    two.send(sfr::live_udp_registration(hellos[1]));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    one.send(relayed(hellos[1].address, hellos[0].address, 2));
    const auto got = two.receive();
    require(got.size() == 14 && got[13] == 2, "a datagram from console 1 reaches console 2");
    // Nobody can take console 2's datagrams with a wrong token, send as
    // another console, or send without registering.
    auto forged = sfr::live_udp_registration(hellos[1]);
    forged[12] ^= 1;
    stranger.send(forged);
    one.send(relayed(hellos[1].address, hellos[1].address, 3));
    stranger.send(relayed(hellos[1].address, hellos[0].address, 4));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    one.send(relayed(hellos[1].address, hellos[0].address, 5));
    const auto next = two.receive();
    require(next.size() == 14 && next[13] == 5, "only console 1's own datagram arrives, at console 2 still");
    for (const auto socket : controls) sfr::close_live_connection(socket);
}

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
        the_room_password_is_asked_for();
        datagrams_go_only_between_registered_consoles();
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
