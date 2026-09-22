// Decodes the verified Free Riders XEX into the image directory the runtime
// loads (image.bin and its tables). No game bytes are embedded.
#include "xex_image.h"
#include <image.h>
#include <xex.h>
#include <TinySHA1.hpp>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) {ORDINAL, #TYPE}
static const std::unordered_map<size_t, std::string> xam_types = {
#include "xbox/xam_table.inc"
};
static const std::unordered_map<size_t, std::string> kernel_types = {
#include "xbox/xboxkrnl_table.inc"
};
#undef XE_EXPORT

namespace sfr {
namespace fs = std::filesystem;

bool is_supported_xex(std::span<const uint8_t> bytes) {
    if (bytes.size() != supported_xex_size) return false;
    const std::array<uint8_t, 20> expected = {0x34,0x3d,0xcf,0xb1,0xa9,0xb5,0x04,0xd1,0xd5,0x9c,
        0xe8,0x87,0x63,0x58,0x70,0xa0,0xc0,0x50,0xfe,0x24};
    std::array<uint8_t, 20> digest{};
    sha1::SHA1 hash;
    hash.processBytes(bytes.data(), bytes.size());
    hash.finalize(digest.data());
    return digest == expected;
}

DecodedImage write_image_dump(std::span<const uint8_t> xex, const fs::path& output) {
    // Fixed-input guard before the upstream parser; this is not a
    // general-purpose XEX parser.
    if (!is_supported_xex(xex)) throw std::runtime_error("unsupported XEX fingerprint");
    std::vector<uint8_t> bytes(xex.begin(), xex.end());
    if (fs::exists(output)) throw std::runtime_error("output already exists");
    auto image = Image::ParseImage(bytes.data(), bytes.size());
    if (!image.data || image.base != 0x82000000 || image.entry_point != 0x824d22f0)
        throw std::runtime_error("unexpected decoded image");
    fs::create_directories(output);
    auto open = [&](const char* name, std::ios::openmode mode = std::ios::out) {
        std::ofstream stream(output / name, mode);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        return stream;
    };
    auto binary = open("image.bin", std::ios::binary);
    binary.write(reinterpret_cast<const char*>(image.data.get()), image.size);
    binary.close();
    // Preserve the original big-endian header, before upstream import
    // normalization. The fixed fingerprint above establishes PE offset 0x5000.
    auto header = open("xex_header.bin", std::ios::binary);
    header.write(reinterpret_cast<const char*>(bytes.data()), 0x5000);
    header.close();
    auto metadata = open("image.tsv");
    metadata << "base\t" << image.base << "\nsize\t" << image.size
             << "\nentry\t" << image.entry_point << '\n';
    metadata.close();
    auto sections = open("sections.tsv");
    sections << "name\taddress\tsize\tflags\n";
    for (const auto& s : image.sections) {
        if (s.base < image.base || s.base + s.size > image.base + image.size)
            throw std::runtime_error("decoded section outside image");
        sections << s.name.substr(0, 8) << '\t' << s.base << '\t' << s.size << '\t' << unsigned(s.flags) << '\n';
    }
    sections.close();
    auto imports = open("imports.tsv");
    imports << "name\taddress\tsize\n";
    for (const auto& symbol : image.symbols)
        imports << symbol.name << '\t' << symbol.address << '\t' << symbol.size << '\n';
    imports.close();
    auto variables = open("import_variables.tsv");
    variables << "name\taddress\tordinal\n";
    const auto* import_header = static_cast<const Xex2ImportHeader*>(getOptHeaderPtr(bytes.data(), XEX_HEADER_IMPORT_LIBRARIES));
    const char* names = reinterpret_cast<const char*>(import_header + 1);
    const auto* library = reinterpret_cast<const Xex2ImportLibrary*>(names + import_header->sizeOfStringTable);
    size_t name_offset = 0;
    for (uint32_t i = 0; i < import_header->numImports; ++i) {
        std::string module(names + name_offset);
        name_offset += (module.size() + 4) & ~size_t(3);
        const auto* descriptors = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);
        const auto& table = module == "xam.xex" ? XamExports : XboxKernelExports;
        const auto& types = module == "xam.xex" ? xam_types : kernel_types;
        for (uint32_t n = 0; n < library->numberOfImports; ++n) {
            uint32_t address = descriptors[n].firstThunk;
            if (image.symbols.find(address) != image.symbols.end()) continue;
            uint32_t normalized;
            std::memcpy(&normalized, image.Find(address), 4);
            // XenonUtils normalizes variable records to host endian; function
            // records have instead been rewritten to PPC nop instructions.
            if ((normalized >> 24) == 0) {
                uint32_t ordinal = normalized & 0xffff;
                const auto found = table.find(ordinal);
                if (found == table.end()) throw std::runtime_error("unresolved import-variable ordinal");
                if (types.at(ordinal) != "kVariable") continue;
                variables << found->second << '\t' << address << '\t' << ordinal << '\n';
            }
        }
        library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const uint8_t*>(library + 1)
            + library->numberOfImports * sizeof(Xex2ImportDescriptor));
    }
    variables.close();
    // Completion marker is written only after every output stream closes.
    auto complete = open("complete.txt");
    complete << "Verified Free Riders image decoded using XenonUtils.\n"
             << "Import thunks are normalized by the upstream loader.\n";
    complete.close();
    return {uint32_t(image.base), uint32_t(image.entry_point), uint32_t(image.size), image.symbols.size()};
}
}
