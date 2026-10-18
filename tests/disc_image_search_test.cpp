#include "disc_image_search.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void write(const fs::path& file, size_t bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << std::string(bytes, 'x');
}

void search(const fs::path& root) {
    using sfr::launcher::find_disc_image;
    require(!find_disc_image(root / "missing"), "a missing folder has none");
    fs::create_directories(root);
    require(!find_disc_image(root), "an empty folder has none");
    write(root / "settings.ini", 100);
    write(root / "shaders.pack", 5000);
    require(!find_disc_image(root), "other files are not disc images");
    write(root / "empty.iso", 0);
    require(!find_disc_image(root), "an empty .iso is not a disc image");
    fs::create_directories(root / "game");
    write(root / "game" / "inner.iso", 9000);
    require(!find_disc_image(root), "subfolders are not searched");
    fs::create_directories(root / "folder.iso");
    require(!find_disc_image(root), "a folder named .iso is not one");
    write(root / "Small.ISO", 200);
    const auto upper = find_disc_image(root);
    require(upper && upper->filename() == "Small.ISO", "the extension is matched in any case");
    write(root / "Sonic Free Riders.iso", 3000);
    const auto largest = find_disc_image(root);
    require(largest && largest->filename() == "Sonic Free Riders.iso", "the largest one is taken");
    write(root / "A copy.iso", 3000);
    const auto tie = find_disc_image(root);
    require(tie && tie->filename() == "A copy.iso", "equal sizes: the first by name");
}
}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("sfr-disc-image-search-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    int result = 0;
    try {
        search(root);
        std::cout << "disc_image_search_test passed\n";
    } catch (const std::exception& error) {
        std::cerr << "disc_image_search_test failed: " << error.what() << '\n';
        result = 1;
    }
    std::error_code error;
    fs::remove_all(root, error);
    return result;
}
