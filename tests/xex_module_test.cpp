#include "xex_module.h"
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void put(std::vector<uint8_t>& header, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i)
        header.at(offset + i) = static_cast<uint8_t>(value >> (24 - i * 8));
}

static std::vector<uint8_t> valid_header() {
    std::vector<uint8_t> header(128);
    put(header, 0, 0x58455832);
    put(header, 8, static_cast<uint32_t>(header.size()));
    put(header, 20, 4);
    put(header, 24, 0x30000); put(header, 28, 0x220);
    put(header, 32, 0x20001); put(header, 36, 0x12345678);
    put(header, 40, 0x30002); put(header, 44, 112);
    put(header, 48, 0x400ff); put(header, 52, 120);
    put(header, 112, 0x11223344); put(header, 116, 0x55667788);
    put(header, 120, 8); put(header, 124, 0xaabbccdd);
    return header;
}

template<typename F> static void rejects(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const sfr::RuntimeStop&) { rejected = true; }
    require(rejected, message);
}

static void fields_and_mapping() {
    sfr::GuestMemory memory;
    auto header = valid_header();
    sfr::XexModule module(memory, header);
    constexpr auto base = sfr::XexModule::header_address;
    require(module.header_field(base, 0x30000) == 0x220, "low-byte zero returns immediate value");
    require(module.header_field(base, 0x20001) == base + 36, "low-byte one returns inline value address");
    require(memory.load<uint32_t>(module.header_field(base, 0x20001)) == 0x12345678, "inline value has guest byte order");
    require(module.header_field(base, 0x30002) == base + 112, "fixed field returns bounded data address");
    require(memory.load<uint64_t>(base + 112) == 0x1122334455667788ull, "fixed field data preserved");
    require(module.header_field(base, 0x400ff) == base + 120, "variable field includes length word");
    require(module.header_field(base, 0x50001) == 0, "absent field returns null");
    rejects([&] { module.header_field(base + 4, 0x50001); }, "wrong header pointer fails even for absent key");
    rejects([&] { module.header_field(0xffffffff, 0x30000); }, "arbitrary header pointer fails");
    memory.map(0x10000, 4);
    memory.store<uint32_t>(0x10000, sfr::XexModule::handle_address);
    const auto handle = memory.load<uint32_t>(0x10000);
    const auto loaded_module = memory.load<uint32_t>(handle);
    require(loaded_module == sfr::XexModule::module_address, "import handle dereferences to module");
    require(memory.load<uint32_t>(loaded_module + 0x58) == base, "module header field resolves after double indirection");
    for (uint32_t offset = 0; offset < 0x1000; offset += 4) {
        if (offset == 0 || offset == 0x98) continue;
        rejects([&] { memory.load<uint32_t>(handle + offset); }, "unsupported loader words cannot return fake zero");
    }
    rejects([&] { memory.load<uint64_t>(loaded_module + 0x58); }, "read cannot extend past supported module field");
    rejects([&] { memory.load<uint16_t>(loaded_module + 0x57); }, "partial unsupported loader read fails");
    rejects([&] { memory.store<uint8_t>(loaded_module, 1); }, "unsupported module write fails");
    rejects([&] { memory.load<uint8_t>(base + header.size()); }, "header page padding remains inaccessible");
}

static void privileges() {
    {
        sfr::GuestMemory memory;
        auto header = valid_header();
        sfr::XexModule module(memory, header);
        for (uint32_t bit = 0; bit < 32; ++bit)
            require(module.check_privilege(bit) == (bit == 5 || bit == 9), "privilege bit must come from system flags");
    }
    {
        sfr::GuestMemory memory;
        auto header = valid_header();
        put(header, 20, 1);
        put(header, 24, 0x30001);
        sfr::XexModule module(memory, header);
        require(module.check_privilege(5) == 0, "missing system flags deny privilege");
    }
    {
        sfr::GuestMemory memory;
        auto header = valid_header();
        put(header, 28, 0x80000001);
        sfr::XexModule module(memory, header);
        require(module.check_privilege(0) == 1, "lowest privilege bit is supported");
        require(module.check_privilege(31) == 1, "highest privilege bit uses an unsigned shift");
    }
    {
        sfr::GuestMemory memory;
        auto header = valid_header();
        put(header, 32, 0x30001);
        put(header, 36, 0xffffffff);
        sfr::XexModule module(memory, header);
        require(module.check_privilege(5) == 1, "kind-one neighbor cannot replace system flags");
        require(module.check_privilege(2) == 0, "kind-one value and pointer cannot be interpreted as system flags");
        memory.store<uint32_t>(sfr::XexModule::header_address + 28, 0);
        put(header, 28, 0);
        require(module.check_privilege(5) == 1, "cached system flags survive guest and source mutations");
    }
    {
        sfr::GuestMemory memory;
        auto header = valid_header();
        sfr::XexModule module(memory, header);
        for (uint32_t privilege : {32u, 0xffffffffu}) {
            bool rejected = false;
            try { module.check_privilege(privilege); }
            catch (const sfr::RuntimeStop& stop) {
                rejected = stop.category == "xex-privilege" && stop.address == privilege &&
                           stop.detail == "privilege index must be below 32";
            }
            require(rejected, "out-of-range privilege must report bounded-policy diagnostic");
        }
    }
}

static void malformed_headers() {
    auto bad = [](std::vector<uint8_t> header) {
        sfr::GuestMemory memory;
        rejects([&] { sfr::XexModule module(memory, header); }, "malformed header must fail");
        rejects([&] { memory.load<uint32_t>(sfr::XexModule::handle_address); }, "invalid header must not publish a module");
    };
    bad({});
    bad(std::vector<uint8_t>(23));
    auto header = valid_header(); put(header, 0, 0); bad(header);
    header = valid_header(); put(header, 20, 257); bad(header);
    header = valid_header(); put(header, 20, 0xffffffff); bad(header);
    header = valid_header(); put(header, 20, 14); bad(header);
    header = valid_header(); put(header, 32, 0x30000); bad(header);
    header = valid_header(); put(header, 44, 121); bad(header);
    header = valid_header(); put(header, 44, 0xffffffff); bad(header);
    header = valid_header(); put(header, 52, 126); bad(header);
    header = valid_header(); put(header, 52, 0xffffffff); bad(header);
    for (uint32_t length : {0u, 3u, 9u, 0xffffffffu}) {
        header = valid_header(); put(header, 120, length); bad(header);
    }
}

static void module_handle_queries() {
    sfr::GuestMemory memory;
    sfr::XexModule module(memory, valid_header());
    constexpr uint32_t output = 0x10004;
    memory.map(0x10000, 12);
    memory.store<uint32_t>(output - 4, 0x11223344);
    memory.store<uint32_t>(output + 4, 0xaabbccdd);
    const auto original_usage = memory.usage();
    const auto original_identity = memory.load<uint32_t>(sfr::XexModule::handle_address);
    for (int query = 0; query < 3; ++query) {
        memory.store<uint32_t>(output, 0xdeadbeef);
        require(module.get_module_handle(0, output) == 0, "null-name module query succeeds");
        const auto identity = memory.load<uint32_t>(output);
        require(identity == original_identity && identity == sfr::XexModule::module_address,
                "query returns the existing loader entry identity");
        require(identity != sfr::XexModule::handle_address, "query does not return the export cell address");
        require(memory.base()[output] == 0x71 && memory.base()[output + 1] == 0 &&
                memory.base()[output + 2] == 0 && memory.base()[output + 3] == 0x40,
                "module handle output is a big-endian guest word");
        const auto header = memory.load<uint32_t>(identity + 0x58);
        require(header == sfr::XexModule::header_address && module.header_field(header, 0x30000) == 0x220,
                "queried loader identity leads to the existing validated header");
        require(memory.load<uint32_t>(output - 4) == 0x11223344 &&
                memory.load<uint32_t>(output + 4) == 0xaabbccdd,
                "module query preserves neighboring output words");
    }
    // The export cell is writable guest state, not the host's module identity.
    memory.store<uint32_t>(sfr::XexModule::handle_address, 0xdeadbeef);
    require(module.get_module_handle(0, output) == 0 &&
            memory.load<uint32_t>(output) == original_identity,
            "guest mutation of the export cell cannot redirect a module query");
    const auto final_usage = memory.usage();
    require(final_usage.reserved_bytes == original_usage.reserved_bytes &&
            final_usage.committed_bytes == original_usage.committed_bytes,
            "repeated module queries do not reserve or commit memory");
    rejects([&] { memory.load<uint32_t>(original_identity); }, "query does not expose unsupported loader fields");
}

static void module_handle_failures() {
    sfr::GuestMemory memory;
    sfr::XexModule module(memory, valid_header());
    memory.map(0x10000, 32);
    memory.map(0x20000, 6);
    memory.map(0x30000, 16);
    memory.map(0x40000, 16);
    memory.map(0, 4);
    memory.map(0xfffff000, 0x1000);
    memory.store<uint32_t>(0, 0x11223344);
    for (uint32_t i = 0; i < 32; ++i) memory.store<uint8_t>(0x10000 + i, 0xa5);
    for (uint32_t i = 0; i < 6; ++i) memory.store<uint8_t>(0x20000 + i, 0xa5);
    for (uint32_t i = 0; i < 16; ++i) {
        memory.store<uint8_t>(0x30000 + i, 0xa5);
        memory.store<uint8_t>(0x40000 + i, 0xa5);
    }
    // This guard overlaps only the last two bytes of an aligned output word.
    memory.add_import_variable(0x30006, "guarded module output");
    int provider_calls = 0;
    memory.add_read_only_word(0x40004, [&] { ++provider_calls; return 0x12345678; });
    memory.store<uint32_t>(0xfffffffc, 0xa5a5a5a5);
    const auto original_usage = memory.usage();
    for (uint32_t output : {0u, 0x10001u, 0x50000u, 0x20004u, 0x30004u,
                            0x40004u, 0xfffffffeu, 0xffffffffu}) {
        rejects([&] { module.get_module_handle(0, output); }, "invalid module output must stop");
        require(memory.load<uint32_t>(0) == 0x11223344, "null output preserves mapped address zero");
        for (uint32_t i = 0; i < 32; ++i)
            require(memory.base()[0x10000 + i] == 0xa5, "misaligned output cannot partially write");
        for (uint32_t i = 0; i < 6; ++i)
            require(memory.base()[0x20000 + i] == 0xa5, "truncated output cannot partially write");
        for (uint32_t i = 0; i < 16; ++i) {
            require(memory.base()[0x30000 + i] == 0xa5, "import output overlap preserves all backing bytes");
            require(memory.base()[0x40000 + i] == 0xa5, "read-only output preserves all backing bytes");
        }
        require(memory.load<uint32_t>(0xfffffffc) == 0xa5a5a5a5,
                "overflowing output preserves the final guest word");
    }
    require(provider_calls == 0, "output validation never invokes a read-only provider");
    // Even a readable, terminated executable name is outside the supported query.
    memory.store<uint8_t>(0x10010, 'a');
    memory.store<uint8_t>(0x10011, 0);
    for (uint32_t name : {0x10010u, 0x50000u, 0xffffffffu}) {
        bool unsupported = false;
        try { module.get_module_handle(name, 0x10004); }
        catch (const sfr::RuntimeStop& stop) {
            unsupported = stop.category == "xex-module-handle" && stop.address == name &&
                          stop.detail == "named module lookup is unsupported";
        }
        require(unsupported, "non-null names must explicitly report unsupported lookup");
        require(memory.load<uint32_t>(0x10004) == 0xa5a5a5a5,
                "unsupported name preserves the output sentinel");
    }
    require(module.get_module_handle(0, 0x1001c) == 0 &&
            memory.load<uint32_t>(0x1001c) == sfr::XexModule::module_address,
            "a full output word ending at the logical mapping boundary succeeds");
    require(module.get_module_handle(0, 0xfffffffc) == 0 &&
            memory.load<uint32_t>(0xfffffffc) == sfr::XexModule::module_address,
            "a full output word ending at the 4 GiB boundary succeeds");
    const auto final_usage = memory.usage();
    require(final_usage.reserved_bytes == original_usage.reserved_bytes &&
            final_usage.committed_bytes == original_usage.committed_bytes,
            "failed and boundary module queries do not change memory usage");
}

static void boundary_headers() {
    {
        sfr::GuestMemory memory;
        auto header = valid_header();
        header.resize(24);
        put(header, 8, 24);
        put(header, 20, 0);
        sfr::XexModule module(memory, header);
        require(module.header_field(sfr::XexModule::header_address, 0x10000) == 0, "empty optional table is valid");
    }
    {
        sfr::GuestMemory memory;
        auto header = valid_header();
        header.resize(24 + 256 * 8);
        put(header, 8, static_cast<uint32_t>(header.size()));
        put(header, 20, 256);
        for (uint32_t i = 0; i < 256; ++i) {
            put(header, 24 + i * 8, (i + 1) << 8);
            put(header, 28 + i * 8, i);
        }
        sfr::XexModule module(memory, header);
        require(module.header_field(sfr::XexModule::header_address, 256 << 8) == 255, "maximum supported table count is valid");
    }
    {
        sfr::GuestMemory memory;
        auto header = valid_header();
        put(header, 52, 124);
        put(header, 124, 4);
        sfr::XexModule module(memory, header);
        require(module.header_field(sfr::XexModule::header_address, 0x400ff) == sfr::XexModule::header_address + 124,
                "length-only variable field at exact header end is valid");
    }
}

static std::vector<uint8_t> resource_header() {
    std::vector<uint8_t> header(128);
    put(header, 0, 0x58455832);
    put(header, 8, static_cast<uint32_t>(header.size()));
    put(header, 20, 1);
    put(header, 24, 0x000002ff); put(header, 28, 64);
    put(header, 64, 36);
    const std::string_view names[] = {"TEX_00", "ABCDEFGH"};
    for (size_t i = 0; i < 2; ++i) {
        for (size_t j = 0; j < names[i].size(); ++j) header[68 + i * 16 + j] = names[i][j];
        put(header, 76 + i * 16, 0x30000 + static_cast<uint32_t>(i) * 16);
        put(header, 80 + i * 16, i ? 8 : 16);
    }
    return header;
}

static void guest_name(sfr::GuestMemory& memory, uint32_t address, std::string_view name) {
    for (size_t i = 0; i < name.size(); ++i) memory.store<uint8_t>(address + i, name[i]);
    memory.store<uint8_t>(address + name.size(), 0);
}

static void module_section_queries() {
    sfr::GuestMemory memory;
    auto header = resource_header();
    sfr::XexModule module(memory, header);
    constexpr auto handle = sfr::XexModule::module_address;
    memory.map(0x10000, 0x200);
    memory.map(0x30000, 24);
    for (uint32_t i = 0; i < 24; ++i) memory.store<uint8_t>(0x30000 + i, 0x80 + i);
    const auto usage = memory.usage();
    guest_name(memory, 0x10020, "TEX_00");
    require(module.get_module_section(handle, 0x10020, 0x10000, 0x10004) == 0,
            "embedded resource query succeeds");
    require(memory.load<uint32_t>(0x10000) == 0x30000 && memory.load<uint32_t>(0x10004) == 16,
            "resource query returns its original image address and size");
    require(memory.base()[0x10001] == 3 && memory.base()[0x10007] == 16,
            "resource outputs are big-endian guest words");
    require(memory.load<uint64_t>(memory.load<uint32_t>(0x10000)) == 0x8081828384858687ull,
            "returned resource points at existing image bytes");
    guest_name(memory, 0x10020, "ABCDEFGH");
    require(module.get_module_section(handle, 0x10020, 0x10000, 0x10004) == 0 &&
            memory.load<uint32_t>(0x10000) == 0x30010 && memory.load<uint32_t>(0x10004) == 8,
            "all eight non-NUL record name bytes participate in lookup");
    for (auto name : {"tex_00", "TEX", "TEX_00extra", "ABCDEFGHI", ""}) {
        guest_name(memory, 0x10020, name);
        memory.store<uint32_t>(0x10000, 0x11223344);
        memory.store<uint32_t>(0x10004, 0x55667788);
        require(module.get_module_section(handle, 0x10020, 0x10000, 0x10004) == 0xc0000225,
                "unknown and case-mismatched names return STATUS_NOT_FOUND");
        require(memory.load<uint32_t>(0x10000) == 0x11223344 && memory.load<uint32_t>(0x10004) == 0x55667788,
                "unknown resource preserves both output words");
    }
    require(module.get_module_section(sfr::XexModule::handle_address, 0, 0x10000, 0x10004) == 0xc0000008,
            "export cell is not a valid module handle and is checked before the name");
    require(memory.load<uint32_t>(0x10000) == 0x11223344 && memory.load<uint32_t>(0x10004) == 0x55667788,
            "invalid module handle preserves both output words");
    put(header, 76, 0xdeadbeef);
    memory.store<uint32_t>(sfr::XexModule::header_address + 76, 0xdeadbeef);
    memory.store<uint8_t>(sfr::XexModule::header_address + 68, 'X');
    guest_name(memory, 0x10020, "TEX_00");
    require(module.get_module_section(handle, 0x10020, 0x10020, 0x10024) == 0 &&
            memory.load<uint32_t>(0x10020) == 0x30000 && memory.load<uint32_t>(0x10024) == 16,
            "cached resource survives header mutation and outputs may alias the captured name");
    const auto after = memory.usage();
    require(after.committed_bytes == usage.committed_bytes && after.reserved_bytes == usage.reserved_bytes,
            "resource queries do not allocate or map guest memory");
}

static void module_section_failures() {
    sfr::GuestMemory memory;
    sfr::XexModule module(memory, resource_header());
    constexpr auto handle = sfr::XexModule::module_address;
    memory.map(0x10000, 0x200);
    memory.map(0x20000, 6);
    memory.map(0x40000, 16);
    memory.map(0x50000, 8);
    memory.map(0xfffff000, 0x1000);
    memory.add_import_variable(0x40006, "guarded section output");
    int provider_calls = 0;
    memory.add_read_only_word(0x50004, [&] { ++provider_calls; return 7; });
    guest_name(memory, 0x10020, "TEX_00");
    memory.store<uint32_t>(0x10000, 0x11223344);
    memory.store<uint32_t>(0x10004, 0x55667788);
    rejects([&] { module.get_module_section(handle, 0x10020, 0x10000, 0x10004); },
            "a resource with unmapped payload fails explicitly");
    require(memory.load<uint32_t>(0x10000) == 0x11223344 && memory.load<uint32_t>(0x10004) == 0x55667788,
            "unmapped payload preserves both outputs");
    memory.map(0x30000, 16);
    for (uint32_t output : {0u, 0x10001u, 0x10000u, 0x20004u, 0x40004u, 0x50004u, 0x60000u, 0xffffffffu}) {
        rejects([&] { module.get_module_section(handle, 0x10020, 0x10000, output); },
                "invalid second output or identical output pointers fail");
        require(memory.load<uint32_t>(0x10000) == 0x11223344,
                "second output is fully preflighted before first output is written");
    }
    require(provider_calls == 0, "output preflight cannot invoke read-only providers");
    rejects([&] { module.get_module_section(handle, 0x10020, 0x60000, 0x10004); },
            "invalid first output fails");
    require(memory.load<uint32_t>(0x10004) == 0x55667788, "invalid first output preserves second output");
    for (uint32_t name : {0u, 0x60000u})
        rejects([&] { module.get_module_section(handle, name, 0x10000, 0x10004); },
                "null or unmapped names fail explicitly");
    guest_name(memory, 0x10020, "\xc3\xa9");
    rejects([&] { module.get_module_section(handle, 0x10020, 0x10000, 0x10004); },
            "non-ASCII query names are explicitly unsupported");
    memory.store<uint8_t>(0xffffffff, 'T');
    rejects([&] { module.get_module_section(handle, 0xffffffff, 0x10000, 0x10004); },
            "name traversal cannot wrap past 4 GiB");
    guest_name(memory, 0x10040, std::string(259, 'A'));
    require(module.get_module_section(handle, 0x10040, 0, 0) == 0xc0000225,
            "a terminated 259-byte name is valid and absent names do not validate outputs");
    memory.store<uint8_t>(0x10040 + 259, 'A');
    rejects([&] { module.get_module_section(handle, 0x10040, 0x10000, 0x10004); },
            "name must terminate within 260 bytes even after an early mismatch");
    guest_name(memory, 0x10020, "TEX_00");
    memory.add_import_variable(0x3000c, "guarded resource payload");
    rejects([&] { module.get_module_section(handle, 0x10020, 0x10000, 0x10004); },
            "the entire resource payload is checked for import guards");
}

static void module_section_headers() {
    const auto bad = [](std::vector<uint8_t> header) {
        sfr::GuestMemory memory;
        rejects([&] { sfr::XexModule module(memory, header); }, "malformed resource metadata must fail");
        require(memory.available(sfr::XexModule::handle_address, 0x1000) &&
                memory.available(sfr::XexModule::header_address, 0x1000),
                "malformed resource metadata is rejected before any module mapping");
    };
    for (uint32_t size : {3u, 5u, 21u, 68u, 0xffffffffu}) {
        auto header = resource_header(); put(header, 64, size); bad(header);
    }
    auto header = resource_header();
    for (size_t i = 0; i < 8; ++i) header[84 + i] = header[68 + i];
    header[91] = 'X'; // Trailing bytes after the first NUL do not distinguish names.
    bad(header);
    header = resource_header(); put(header, 76, 0xfffffff8); put(header, 80, 16); bad(header);
    header = resource_header(); header[68] = 0; bad(header);
    header = resource_header(); header[68] = 0x80; bad(header);
    {
        sfr::GuestMemory memory;
        sfr::XexModule module(memory, valid_header());
        memory.map(0x10000, 16); guest_name(memory, 0x10000, "TEX_00");
        require(module.get_module_section(sfr::XexModule::module_address, 0x10000, 0, 0) == 0xc0000225,
                "missing resource table is an empty lookup");
    }
    {
        sfr::GuestMemory memory;
        header = resource_header(); put(header, 64, 4);
        sfr::XexModule module(memory, header);
        memory.map(0x10000, 16); guest_name(memory, 0x10000, "TEX_00");
        require(module.get_module_section(sfr::XexModule::module_address, 0x10000, 0, 0) == 0xc0000225,
                "length-only resource table is empty");
    }
    {
        sfr::GuestMemory memory;
        header = resource_header(); put(header, 76, 0xfffffff0); put(header, 80, 16);
        sfr::XexModule module(memory, header);
        memory.map(0x10000, 32); memory.map(0xfffff000, 0x1000);
        guest_name(memory, 0x10000, "TEX_00");
        require(module.get_module_section(sfr::XexModule::module_address, 0x10000, 0x10010, 0x10014) == 0 &&
                memory.load<uint32_t>(0x10010) == 0xfffffff0 && memory.load<uint32_t>(0x10014) == 16,
                "resource ending exactly at 4 GiB remains valid");
    }
    {
        sfr::GuestMemory memory;
        header = resource_header(); put(header, 80, 0);
        sfr::XexModule module(memory, header);
        memory.map(0x10000, 32); guest_name(memory, 0x10000, "TEX_00");
        rejects([&] { module.get_module_section(sfr::XexModule::module_address, 0x10000, 0x10010, 0x10014); },
                "zero-sized resources explicitly remain unsupported");
    }
}

int main() {
    try {
        fields_and_mapping();
        privileges();
        module_handle_queries();
        module_handle_failures();
        module_section_queries();
        module_section_failures();
        module_section_headers();
        malformed_headers();
        boundary_headers();
        std::cout << "XEX module checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
