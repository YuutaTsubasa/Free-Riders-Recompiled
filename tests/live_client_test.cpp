// The game's online client against a server that signs it in and then never
// answers again, as when the Wi-Fi is gone or the server froze: a request
// must give up instead of holding the game (Issue #329).
#include "live_client.h"
#include "live_connect.h"

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
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static void close_socket(socket_t s) { close(s); }
static constexpr socket_t no_socket = -1;
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Answers HELLO, then reads requests without answering any.
class SilentServer {
public:
    SilentServer() {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
        socklen_t size = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        port_ = ntohs(address.sin_port);
        require(listen(listener_, 1) == 0, "listen");
        thread_ = std::thread([this] { serve(); });
    }
    ~SilentServer() {
        // Wakes the thread's recv; a close alone may not.
#ifdef _WIN32
        if (console_ != no_socket) shutdown(console_, SD_BOTH);
#else
        if (console_ != no_socket) shutdown(console_, SHUT_RDWR);
#endif
        thread_.join();
        if (console_ != no_socket) close_socket(console_);
        close_socket(listener_);
    }
    uint16_t port() const { return port_; }
    int requests() const { return requests_.load(); }

private:
    void serve() {
        console_ = accept(listener_, nullptr, nullptr);
        if (console_ == no_socket) return;
        std::string pending;
        char buffer[512];
        for (int got; (got = int(recv(console_, buffer, sizeof(buffer), 0))) > 0;) {
            pending.append(buffer, size_t(got));
            for (size_t end; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1)) {
                if (pending.rfind("HELLO", 0) == 0) {
                    const std::string answer = "OK 3 0a4d0003 024652000003 fa00000000000003\n.\n";
                    send(console_, answer.data(), int(answer.size()), 0);
                } else {
                    ++requests_;  // and no answer
                }
            }
        }
    }
    socket_t listener_ = no_socket, console_ = no_socket;
    uint16_t port_ = 0;
    std::atomic<int> requests_{0};
    std::thread thread_;
};
}

int main() {
#ifdef _WIN32
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    try {
        SilentServer server;
        const std::string address = "127.0.0.1:" + std::to_string(server.port());
#ifdef _WIN32
        _putenv_s("SFR_LIVE_SERVER", address.c_str());
#else
        setenv("SFR_LIVE_SERVER", address.c_str(), 1);
#endif
        auto* client = sfr::LiveClient::instance();
        require(client, "the client signs in");
        const auto started = std::chrono::steady_clock::now();
        const auto answer = client->request("SEARCH 4");
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "a request to a silent server came back after " << seconds << " s\n";
        require(answer.empty() && server.requests() == 1, "no answer from a silent server");
        require(seconds < 30.0, "within the time allowed");
        // The connection is given up: a late answer must not be taken for
        // the next request's, so later requests fail at once.
        const auto again = std::chrono::steady_clock::now();
        require(client->request("SEARCH 4").empty(), "the next request fails too");
        require(std::chrono::steady_clock::now() - again < std::chrono::seconds(1), "at once");
        std::cout << "Live client checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
