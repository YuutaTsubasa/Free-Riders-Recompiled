#include "xex_module.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace sfr {
XexModule::XexModule(GuestMemory& memory, std::span<const uint8_t> header) : memory_(memory) {
    const auto invalid = [](const char* detail) {
        throw RuntimeStop("xex-header", header_address, detail);
    };
    if (header.size() < 24 || header.size() > GuestMemory::address_space_size - header_address)
        invalid("invalid raw header size");
    const auto fits = [&](uint64_t offset, uint64_t size) {
        return offset <= header.size() && size <= header.size() - offset;
    };
    const auto word = [&](uint32_t offset) {
        if (!fits(offset, 4)) invalid("truncated header word");
        return (uint32_t(header[offset]) << 24) | (uint32_t(header[offset + 1]) << 16) |
               (uint32_t(header[offset + 2]) << 8) | uint32_t(header[offset + 3]);
    };
    if (word(0) != 0x58455832) invalid("expected XEX2 magic");
    const uint32_t count = word(20);
    if (count > 256 || !fits(24, uint64_t(count) * 8))
        invalid("invalid optional header table size");
    fields_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = 24 + i * 8;
        const uint32_t key = word(entry);
        const uint32_t value = word(entry + 4);
        if (std::any_of(fields_.begin(), fields_.end(), [key](const Field& field) { return field.key == key; }))
            invalid("duplicate optional header key");
        const uint32_t kind = key & 0xff;
        uint32_t result = value;
        // Runtime lookup follows Xenia's GetOptHeader: kind 0 is an immediate,
        // kind 1 points into the table, and all other kinds use header offsets.
        // XenonUtils getOptHeaderPtr instead returns a host pointer for kind 0.
        if (kind == 1) {
            result = header_address + entry + 4;
        } else if (kind > 1) {
            const uint32_t size = kind == 0xff ? word(value) : kind * 4;
            if (size < 4 || !fits(value, size)) invalid("optional header data outside raw header");
            result = header_address + value;
        }
        if (key == 0x000002ff) {
            const uint32_t size = word(value);
            if ((size - 4) % 16) invalid("invalid resource table length");
            for (uint64_t offset = uint64_t(value) + 4; offset < uint64_t(value) + size; offset += 16) {
                if (!fits(offset, 16)) invalid("truncated resource record");
                std::string name;
                for (uint32_t j = 0; j < 8 && header[offset + j]; ++j) {
                    const uint8_t ch = header[offset + j];
                    if (ch >= 0x80) invalid("non-ASCII resource names are unsupported");
                    name.push_back(static_cast<char>(ch));
                }
                if (name.empty()) invalid("empty resource names are unsupported");
                if (std::any_of(resources_.begin(), resources_.end(), [&](const Resource& resource) {
                        return resource.name == name;
                    }))
                    invalid("duplicate resource name");
                const uint32_t address = word(static_cast<uint32_t>(offset + 8));
                const uint32_t resource_size = word(static_cast<uint32_t>(offset + 12));
                if (uint64_t(address) + resource_size > GuestMemory::address_space_size)
                    invalid("resource range exceeds guest address space");
                resources_.push_back({std::move(name), address, resource_size});
            }
        }
        fields_.push_back({key, result});
    }

    // GuestMemory requires page-aligned mapping starts. Its logical range ends
    // at the supported module field, and guards reject every intervening word.
    constexpr uint32_t header_pointer = module_address + 0x58;
    memory.map(handle_address, header_pointer + 4 - handle_address);
    memory.store<uint32_t>(handle_address, module_address);
    memory.store<uint32_t>(header_pointer, header_address);
    for (uint32_t address = handle_address + 4; address < header_pointer; address += 4)
        memory.add_import_variable(address, "unsupported XexExecutableModule field");
    memory.map(header_address, header.size());
    std::memcpy(memory.base() + header_address, header.data(), header.size());
}

uint32_t XexModule::header_field(uint32_t queried_header, uint32_t key) const {
    if (queried_header != header_address)
        throw RuntimeStop("xex-header", queried_header, "lookup requires the loaded XEX header address");
    for (const auto& field : fields_)
        if (field.key == key) return field.result;
    return 0;
}

uint32_t XexModule::check_privilege(uint32_t privilege) const {
    if (privilege >= 32)
        throw RuntimeStop("xex-privilege", privilege, "privilege index must be below 32");
    const uint32_t flags = header_field(header_address, 0x00030000);
    return (flags & (uint32_t{1} << privilege)) != 0;
}

uint32_t XexModule::get_module_handle(uint32_t name, uint32_t output) const {
    if (name)
        throw RuntimeStop("xex-module-handle", name, "named module lookup is unsupported");
    if (!output || output % 4)
        throw RuntimeStop("xex-module-handle", output,
                          "module handle output must be nonnull and four-byte aligned");
    // Xenia's null-name query returns hmodule_ptr(), the existing loader entry,
    // without retaining it. The writable export cell is not the module identity.
    // store validates the complete output before writing any big-endian bytes.
    memory_.store<uint32_t>(output, module_address);
    return 0;
}

uint32_t XexModule::get_module_section(uint32_t handle, uint32_t name, uint32_t data_output,
                                      uint32_t size_output) const {
    if (handle != module_address) return 0xc0000008; // STATUS_INVALID_HANDLE
    if (!name) throw RuntimeStop("xex-module-section", name, "resource name must be nonnull");
    std::array<char, 260> captured_name{};
    size_t length = 0;
    for (; length < captured_name.size(); ++length) {
        const uint8_t ch = memory_.load<uint8_t>(uint64_t(name) + length);
        if (!ch) break;
        if (ch >= 0x80)
            throw RuntimeStop("xex-module-section", name, "non-ASCII resource names are unsupported");
        captured_name[length] = static_cast<char>(ch);
    }
    if (length == captured_name.size())
        throw RuntimeStop("xex-module-section", name, "resource name must terminate within 260 bytes");
    const std::string_view queried_name(captured_name.data(), length);
    // Xenia UserModule::GetSection compares the complete name through its NUL,
    // case-sensitively, against up to eight resource name bytes. The supported
    // ASCII profile preserves that comparison without reading mutable metadata.
    const auto found = std::find_if(resources_.begin(), resources_.end(), [&](const Resource& resource) {
        return resource.name == queried_name;
    });
    if (found == resources_.end()) return 0xc0000225; // STATUS_NOT_FOUND
    const uint32_t address = found->address;
    const uint32_t size = found->size;
    if (!size)
        throw RuntimeStop("xex-module-section", address, "zero-sized resources are unsupported");
    memory_.check(address, size);
    for (const uint32_t output : {data_output, size_output}) {
        if (!output || output % 4)
            throw RuntimeStop("xex-module-section", output,
                              "resource output must be nonnull and four-byte aligned");
        memory_.check_write(output, 4);
    }
    if (data_output == size_output)
        throw RuntimeStop("xex-module-section", data_output, "resource outputs must be distinct words");
    // Capture the name and both immutable values, then validate both destinations
    // before writing either: callers may place the outputs over the query name.
    memory_.store<uint32_t>(data_output, address);
    memory_.store<uint32_t>(size_output, size);
    return 0;
}
}
