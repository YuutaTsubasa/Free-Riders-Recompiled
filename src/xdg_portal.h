#pragma once
#include <optional>
#include <string>
#include <string_view>

// The desktop's own file dialog on Linux, through the XDG desktop portal
// (org.freedesktop.portal.FileChooser over the session D-Bus): KDE's,
// GNOME's or the sandbox's, with nothing to install. libdbus-1 is loaded
// when first needed, so the launcher does not depend on it.
namespace sfr::xdg_portal {
enum class Outcome { chosen, cancelled, unavailable };
struct Result {
    Outcome outcome = Outcome::unavailable;
    std::string path;  // when chosen
};
// Blocks until the dialog closes. unavailable: no libdbus, no session bus or
// no portal answering, so another way should be tried.
Result open_file(const std::string& title, bool folders);

// A file:// URI's path, %XX escapes decoded; nothing for another scheme.
std::optional<std::string> path_from_uri(std::string_view uri);
}
