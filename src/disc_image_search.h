#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace sfr::launcher {

// A disc image the player put straight into a folder (on iOS, the app's
// Documents through the Files app): the largest .iso file in it, not in its
// subfolders. Null when there is none or the folder cannot be read.
inline std::optional<std::filesystem::path> find_disc_image(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::directory_iterator it(folder, error), end;
    std::optional<fs::path> found;
    uintmax_t found_size = 0;
    for (; !error && it != end; it.increment(error)) {
        const auto name = it->path().extension().u8string();
        std::string extension(name.begin(), name.end());
        for (char& c : extension) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (extension != ".iso") continue;
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error) || entry_error) continue;
        const uintmax_t size = it->file_size(entry_error);
        if (entry_error || size == 0) continue;
        if (!found || size > found_size || (size == found_size && it->path() < *found)) {
            found = it->path();
            found_size = size;
        }
    }
    return found;
}

}  // namespace sfr::launcher
