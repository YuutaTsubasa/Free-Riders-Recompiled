// Developer tool for the verified Free Riders XEX only. No game bytes are embedded.
#include "xex_image.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: sfr_image_dump verified-default.xex NEW-output-directory\n";
            return 2;
        }
        const fs::path source(argv[1]), output(argv[2]);
        if (fs::file_size(source) != sfr::supported_xex_size) throw std::runtime_error("unsupported XEX size");
        std::vector<uint8_t> bytes(sfr::supported_xex_size);
        std::ifstream input(source, std::ios::binary);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            throw std::runtime_error("cannot read complete XEX");
        const auto image = sfr::write_image_dump(bytes, output);
        std::cout << "Decoded image: base=0x" << std::hex << image.base << " entry=0x" << image.entry_point
                  << std::dec << " bytes=" << image.size << " import_functions=" << image.import_functions << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "sfr_image_dump: " << e.what() << '\n';
        return 1;
    }
}
