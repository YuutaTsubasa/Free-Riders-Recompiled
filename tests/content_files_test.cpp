#include "content_files.h"
#include "guest_memory.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const char* detail) { if (!ok) throw std::runtime_error(detail); }

constexpr uint32_t base = 0x40000000;
constexpr uint32_t root_name = base, data = base + 0x100, disposition = base + 0x300, license = base + 0x304;
constexpr uint32_t handle = base + 0x310, io = base + 0x318, buffer = base + 0x400, offset = base + 0x330;
constexpr uint32_t info = base + 0x340;
constexpr uint64_t xuid = 0xE000123456789ABCull;

void put_string(sfr::GuestMemory& memory, uint32_t address, const std::string& text) {
    for (size_t i = 0; i <= text.size(); ++i) memory.store<uint8_t>(address + i, i < text.size() ? uint8_t(text[i]) : 0);
}

void content_data(sfr::GuestMemory& memory, const std::string& file_name) {
    for (uint32_t i = 0; i < 0x134; ++i) memory.store<uint8_t>(data + i, 0);
    memory.store<uint32_t>(data, sfr::ContentFiles::device_id);
    memory.store<uint32_t>(data + 4, 1);  // XCONTENTTYPE_SAVEDGAME
    const std::u16string display = u"Records";
    for (size_t i = 0; i < display.size(); ++i) memory.store<uint16_t>(data + 8 + i * 2, uint16_t(display[i]));
    put_string(memory, data + 0x108, file_name);
}

uint32_t open(sfr::ContentFiles& files, const std::string& path, uint32_t disposition_value) {
    return files.open(path, handle, 0xC0100080, io, 0, disposition_value, 0x60);
}

void run(const std::filesystem::path& root) {
    sfr::GuestMemory memory;
    memory.map(base, 0x10000);
    sfr::ContentFiles files(memory, root);
    put_string(memory, root_name, "sav");
    content_data(memory, "SonicSave");

    require(files.create(xuid, root_name, data, 3, disposition, license) == 2, "opening missing content fails");
    require(files.create(xuid, root_name, data, 4, disposition, license) == 0 && memory.load<uint32_t>(disposition) == 1,
            "open-always creates the package");
    require(files.claims("sav:\\SrnData.sav") && files.claims("\\??\\SAV:\\SrnData.sav") && !files.claims("game:\\x"),
            "the mounted root claims its paths only");
    require(!files.claims("sav:\\..\\escape"), "paths cannot climb out of the package");

    require(open(files, "sav:\\SrnData.sav", 1) == 0xC0000034, "a missing file is not found");
    require(open(files, "sav:\\SrnData.sav", 5) == 0 && memory.load<uint32_t>(io + 4) == 2, "overwrite-if creates");
    const uint32_t file = memory.load<uint32_t>(handle);
    require(sfr::ContentFiles::is_handle_range(file) && files.owns(file), "content handles have their own range");
    for (uint32_t i = 0; i < 100; ++i) memory.store<uint8_t>(buffer + i, uint8_t(i * 3));
    memory.store<uint64_t>(offset, 0xFFFFFFFFFFFFFFFEull);  // current position
    require(files.write(file, io, buffer, 100, offset) == 0 && memory.load<uint32_t>(io + 4) == 100, "write");
    require(files.query_information(file, io, info, 24, 5) == 0 && memory.load<uint64_t>(info + 8) == 100,
            "standard information reports the size");
    memory.store<uint64_t>(offset, 10);
    for (uint32_t i = 0; i < 100; ++i) memory.store<uint8_t>(buffer + i, 0);
    require(files.read(file, io, buffer, 50, offset) == 0 && memory.load<uint32_t>(io + 4) == 50 &&
            memory.load<uint8_t>(buffer) == 30 && memory.load<uint8_t>(buffer + 49) == uint8_t(59 * 3),
            "read at an offset");
    require(files.read(file, io, buffer, 50, 0) == 0 && memory.load<uint32_t>(io + 4) == 40, "read stops at the end");
    require(files.read(file, io, buffer, 50, 0) == 0xC0000011, "reading past the end reports end of file");
    memory.store<uint64_t>(info, 20);
    require(files.set_information(file, io, info, 8, 20) == 0 && files.query_information(file, io, info, 56, 34) == 0 &&
            memory.load<uint64_t>(info + 40) == 20, "end of file can be set");
    require(files.close_handle(file) == 0 && !files.owns(file), "close");
    require(files.close(root_name) == 0 && !files.claims("sav:\\SrnData.sav"), "closing the content unmounts it");

    require(files.create(xuid, root_name, data, 3, disposition, license) == 0 && memory.load<uint32_t>(disposition) == 2,
            "the package opens again");
    require(files.query_full_attributes("sav:\\SrnData.sav", info) == 0 && memory.load<uint64_t>(info + 40) == 20,
            "the file persisted");
    require(files.create(xuid, root_name, data, 1, disposition, license) == 183, "create-new of existing content fails");
    const auto packages = files.packages(xuid, 1);
    require(packages.size() == 1 && packages[0].file_name == "SonicSave" && packages[0].display_name == u"Records",
            "enumeration finds the package and its display name");
    require(files.packages(xuid + 1, 1).empty(), "another profile has its own content");
    require(files.device_data(sfr::ContentFiles::device_id, info) == 0 && memory.load<uint32_t>(info + 4) == 1 &&
            memory.load<uint64_t>(info + 16) <= memory.load<uint64_t>(info + 8), "device data");
    require(files.create(xuid, root_name, data, 5, disposition, license) == 0 &&
            files.query_full_attributes("sav:\\SrnData.sav", info) == 0xC0000034, "truncate-existing empties the package");
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("sfr-content-test-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    int code = 0;
    try {
        run(root);
        std::cout << "Content file checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        code = 1;
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return code;
}
