#include <winsock2.h>
#include "native_winsock.h"
#include "guest_memory.h"
#include <iostream>
#include <stdexcept>

static void require(bool v, const char* m) { if (!v) throw std::runtime_error(m); }
template<class F> static void rejects(F f) {
    try { f(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("unsupported Winsock request accepted");
}
static void inactive() {
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket != INVALID_SOCKET) { closesocket(socket); throw std::runtime_error("Winsock acquisition leaked"); }
    require(WSAGetLastError() == WSANOTINITIALISED, "native stack must be uninitialized");
}
int main() {
    try {
        inactive();
        WSADATA reference{};
        require(WSAStartup(2, &reference) == 0, "independent Windows negotiation succeeds");
        require(WSACleanup() == 0, "independent reference acquisition cleaned");
        inactive();
        sfr::GuestMemory memory;
        memory.map(0x10000, 0x1000);
        for (uint32_t i=0;i<0x1000;++i) memory.store<uint8_t>(0x10000+i,0xa5);
        {
            sfr::NativeWinsock network(memory);
            require(network.startup(1,2,0x10100)==0 && network.acquisitions()==1, "actual startup acquired");
            require(memory.load<uint16_t>(0x10100)==reference.wVersion &&
                    memory.load<uint16_t>(0x10102)==reference.wHighVersion, "actual negotiated versions marshalled BE16");
            for (uint32_t i=0;i<257;++i)
                require(memory.load<uint8_t>(0x10104+i)==uint8_t(reference.szDescription[i]), "native description marshalled");
            for (uint32_t i=0;i<129;++i)
                require(memory.load<uint8_t>(0x10205+i)==uint8_t(reference.szSystemStatus[i]), "native status marshalled");
            require(memory.load<uint16_t>(0x10286)==reference.iMaxSockets &&
                    memory.load<uint16_t>(0x10288)==reference.iMaxUdpDg, "native legacy numeric fields retained");
            require(memory.load<uint32_t>(0x1028c)==0, "no host vendor pointer exposed");
            require(memory.load<uint8_t>(0x100ff)==0xa5 && memory.load<uint8_t>(0x10290)==0xa5,
                    "400-byte result preserves neighbors and original saved-register boundary");
            require(network.startup(1,2,0x10400)==0 && network.acquisitions()==2, "repeated native acquisition counted");
            require(network.cleanup(1)==0 && network.acquisitions()==1, "explicit cleanup releases one acquisition");
            const SOCKET socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
            require(socket!=INVALID_SOCKET, "remaining native acquisition stays operational");
            closesocket(socket);
        }
        inactive();
        {
            sfr::NativeWinsock network(memory);
            WSADATA ignored{};
            const int actual_error=WSAStartup(0,&ignored);
            require(actual_error!=0, "invalid version gives a real native error");
            require(network.startup(1,0,0x10800)==actual_error && network.acquisitions()==0,
                    "native failure propagated without acquisition");
            for(uint32_t i=0;i<400;++i) require(memory.load<uint8_t>(0x10800+i)==0xa5, "failed startup preserves output");
            rejects([&]{ network.startup(0,2,0x10800); });
            rejects([&]{ network.startup(1,2,0); });
            rejects([&]{ network.startup(1,2,0x10fff); });
            rejects([&]{ network.startup(1,2,0xffffff00); });
            rejects([&]{ network.startup(1,0x101,0x10800); });
            rejects([&]{ network.cleanup(1); });
            rejects([&]{ network.cleanup(0); });
            memory.add_import_variable(0x1098c,"wsa-output-tail");
            rejects([&]{ network.startup(1,2,0x10800); });
            require(network.acquisitions()==0, "preflight rejection acquires nothing");
            require(memory.load<uint8_t>(0x10800)==0xa5, "late guard preserves beginning");
        }
        inactive();
        std::cout << "Native Winsock startup and lifetime checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
