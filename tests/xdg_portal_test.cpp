#include "xdg_portal.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        using sfr::xdg_portal::path_from_uri;
        require(path_from_uri("file:///home/me/game.iso") == "/home/me/game.iso", "a plain file URI is its path");
        require(path_from_uri("file:///home/me/My%20Games/Sonic%20Free%20Riders.iso") ==
                    "/home/me/My Games/Sonic Free Riders.iso", "%20 escapes become spaces");
        require(path_from_uri("file:///home/me/%E9%81%8A%E6%88%B2/a.vrm") == "/home/me/\xE9\x81\x8A\xE6\x88\xB2/a.vrm",
                "escaped UTF-8 bytes come back as UTF-8");
        require(path_from_uri("file://localhost/tmp/x") == "/tmp/x", "localhost is this machine");
        require(!path_from_uri("file://server/share/x"), "another host's file is not a local path");
        require(!path_from_uri("https://example.org/x"), "another scheme is not a path");
        require(path_from_uri("file:///tmp/100%") == "/tmp/100%", "a lone % stays as it is");
        require(path_from_uri("file:///tmp/a%zzb") == "/tmp/a%zzb", "a malformed escape stays as it is");
        std::cout << "XDG portal checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
