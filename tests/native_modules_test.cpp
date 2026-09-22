#include "native_modules.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void put_string(sfr::GuestMemory& memory, uint32_t address, const std::string& value) {
    for (size_t i = 0; i < value.size(); ++i)
        memory.store<uint8_t>(uint64_t(address) + i, static_cast<uint8_t>(value[i]));
    memory.store<uint8_t>(uint64_t(address) + value.size(), 0);
}

template<class F>
void rejects(F operation, const char* category, const char* message) {
    try { operation(); }
    catch (const sfr::RuntimeStop& stop) {
        require(stop.category == category, message);
        return;
    }
    throw std::runtime_error(message);
}

void succeeds_with_persistent_native_identity() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 0x1000);
    put_string(memory, 0x10000, "xam.xex");
    memory.store<uint32_t>(0x10100, 0xfeedface);
    sfr::NativeModules modules(memory);

    require(modules.xam_load_count() == 0, "load count starts at zero");
    require(modules.load_image(0x10000, 9, 0, 0x10100) == 0, "confirmed load succeeds");
    require(memory.load<uint32_t>(0x10100) == sfr::NativeModules::xam_handle,
            "load publishes the native XAM namespace handle in guest byte order");
    require(modules.xam_load_count() == 1, "successful load increments count");
    require(modules.load_image(0x10000, 9, 0, 0x10104) == 0, "repeated load succeeds");
    require(memory.load<uint32_t>(0x10104) == sfr::NativeModules::xam_handle,
            "repeated load preserves namespace identity");
    require(modules.xam_load_count() == 2, "repeated load increments count");

    rejects([&] { memory.load<uint8_t>(sfr::NativeModules::xam_handle); }, "import-variable",
            "first handle byte is guarded");
    rejects([&] { memory.load<uint8_t>(uint64_t(sfr::NativeModules::xam_handle) + 3); }, "import-variable",
            "last handle byte is guarded");
    rejects([&] { memory.store<uint32_t>(sfr::NativeModules::xam_handle, 0); }, "import-variable",
            "handle word cannot be overwritten");
    rejects([&] { memory.load<uint8_t>(uint64_t(sfr::NativeModules::xam_handle) + 4); }, "memory-access",
            "bytes following handle are unmapped");
    rejects([&] { memory.load<uint32_t>(uint64_t(sfr::NativeModules::xam_handle) + 0x18); }, "memory-access",
            "guessed loader fields are unmapped");
}

void captures_name_before_an_aliasing_output_write() {
    sfr::GuestMemory memory;
    memory.map(0x20000, 0x1000);
    put_string(memory, 0x20000, "xam.xex");
    sfr::NativeModules modules(memory);
    require(modules.load_image(0x20000, 9, 0, 0x20000) == 0, "source/output alias succeeds");
    require(memory.load<uint32_t>(0x20000) == sfr::NativeModules::xam_handle, "alias writes handle after capture");
    require(modules.xam_load_count() == 1, "alias increments count once");
}

void rejected_requests_preserve_output_and_count() {
    auto request_rejected = [](uint32_t name, uint32_t flags, uint32_t version,
                               auto prepare_name, const char* message) {
        sfr::GuestMemory memory;
        memory.map(0x30000, 0x1000);
        prepare_name(memory);
        memory.store<uint32_t>(0x30f00, 0x11223344);
        sfr::NativeModules modules(memory);
        rejects([&] { modules.load_image(name, flags, version, 0x30f00); }, "native-module-request", message);
        require(memory.load<uint32_t>(0x30f00) == 0x11223344, "bad request preserves output sentinel");
        require(modules.xam_load_count() == 0, "bad request preserves count");
    };
    auto string_at = [](std::string text) { return [text](sfr::GuestMemory& m) { put_string(m, 0x30000, text); }; };
    request_rejected(0x30000, 9, 0, string_at("XAM.XEX"), "wrong case is rejected");
    request_rejected(0x30000, 9, 0, string_at("path/xam.xex"), "path is rejected");
    request_rejected(0x30000, 9, 0, string_at("other.xex"), "unknown module is rejected");
    request_rejected(0x30000, 9, 0, string_at(""), "empty name is rejected");
    request_rejected(0x30000, 8, 0, string_at("xam.xex"), "bad flags are rejected");
    request_rejected(0x30000, 9, 1, string_at("xam.xex"), "bad version is rejected");
    request_rejected(0, 9, 0, [](sfr::GuestMemory&) {}, "null name is rejected");
    request_rejected(0x40000, 9, 0, [](sfr::GuestMemory&) {}, "unmapped name is rejected");

    {
        sfr::GuestMemory memory;
        memory.map(0x34000, 0x1000); put_string(memory, 0x34000, "xam.xex");
        memory.add_import_variable(0x34000, "guarded-name");
        memory.map(0x35000, 4); memory.store<uint32_t>(0x35000, 0x1234abcd);
        sfr::NativeModules modules(memory);
        rejects([&] { modules.load_image(0x34000, 9, 0, 0x35000); }, "native-module-request",
                "guarded name is rejected as an invalid request");
        require(memory.load<uint32_t>(0x35000) == 0x1234abcd && modules.xam_load_count() == 0,
                "guarded name preserves state");
    }

    request_rejected(0x30000, 9, 0, [](sfr::GuestMemory& m) {
        for (uint32_t i = 0; i < 260; ++i) m.store<uint8_t>(0x30000 + i, 'a');
    }, "unterminated 260-byte name is rejected");

    {
        sfr::GuestMemory memory;
        memory.map(0x31000, 3);
        memory.store<uint8_t>(0x31000, 'x'); memory.store<uint8_t>(0x31001, 'a'); memory.store<uint8_t>(0x31002, 'm');
        memory.map(0x32000, 4); memory.store<uint32_t>(0x32000, 0xabcdef01);
        sfr::NativeModules modules(memory);
        rejects([&] { modules.load_image(0x31000, 9, 0, 0x32000); }, "native-module-request", "truncated name is rejected");
        require(memory.load<uint32_t>(0x32000) == 0xabcdef01 && modules.xam_load_count() == 0,
                "truncated name preserves state");
    }
    {
        sfr::GuestMemory memory;
        memory.map(0xfffff000, 0x1000);
        memory.store<uint8_t>(0xffffffff, 'x');
        memory.map(0x33000, 4); memory.store<uint32_t>(0x33000, 0xabcdef02);
        sfr::NativeModules modules(memory);
        rejects([&] { modules.load_image(0xffffffff, 9, 0, 0x33000); }, "native-module-request", "end-of-space name is rejected");
        require(memory.load<uint32_t>(0x33000) == 0xabcdef02 && modules.xam_load_count() == 0,
                "end-of-space rejection preserves state");
    }
}

void rejected_outputs_preserve_state() {
    auto output_rejected = [](uint32_t output, auto prepare, const char* message) {
        sfr::GuestMemory memory;
        memory.map(0x50000, 0x1000); put_string(memory, 0x50000, "xam.xex");
        prepare(memory);
        sfr::NativeModules modules(memory);
        rejects([&] { modules.load_image(0x50000, 9, 0, output); }, "memory-access", message);
        require(modules.xam_load_count() == 0, "bad output preserves count");
    };
    output_rejected(0, [](sfr::GuestMemory&) {}, "null output is rejected");
    output_rejected(0x60000, [](sfr::GuestMemory&) {}, "unmapped output is rejected");
    output_rejected(0x60001, [](sfr::GuestMemory& m) { m.map(0x60000, 4); }, "tail output is rejected");

    {
        sfr::GuestMemory memory;
        memory.map(0x50000, 0x1000); put_string(memory, 0x50000, "xam.xex");
        memory.map(0x61000, 4); memory.store<uint32_t>(0x61000, 0x44556677);
        memory.add_import_variable(0x61000, "guarded-output");
        sfr::NativeModules modules(memory);
        rejects([&] { modules.load_image(0x50000, 9, 0, 0x61000); }, "import-variable", "guarded output is rejected");
        require(modules.xam_load_count() == 0, "guarded output preserves count");
    }
    {
        sfr::GuestMemory memory;
        memory.map(0x50000, 0x1000); put_string(memory, 0x50000, "xam.xex");
        memory.map(0x62000, 4); memory.store<uint32_t>(0x62000, 0x55667788);
        memory.add_read_only_word(0x62000, [] { return 0xaabbccddu; });
        sfr::NativeModules modules(memory);
        rejects([&] { modules.load_image(0x50000, 9, 0, 0x62000); }, "memory-readonly", "read-only output is rejected");
        require(memory.load<uint32_t>(0x62000) == 0xaabbccddu && modules.xam_load_count() == 0,
                "read-only rejection preserves state");
    }
    {
        sfr::GuestMemory memory;
        memory.map(0x50000, 0x1000); put_string(memory, 0x50000, "xam.xex");
        memory.map(0x63000, 4); memory.store<uint32_t>(0x63000, 0x66778899);
        memory.load_reserved_word(0x63000);
        sfr::NativeModules modules(memory);
        rejects([&] { modules.load_image(0x50000, 9, 0, 0x63000); }, "reservation-interference", "reserved output is rejected");
        require(memory.load<uint32_t>(0x63000) == 0x66778899 && modules.xam_load_count() == 0,
                "reservation rejection preserves state");
    }
}

void rejects_mapping_collisions() {
    sfr::GuestMemory memory;
    memory.map(sfr::NativeModules::xam_handle, 4);
    memory.store<uint32_t>(sfr::NativeModules::xam_handle, 0xdecafbadu);
    rejects([&] { sfr::NativeModules modules(memory); }, "memory-map", "existing mapping is rejected");
    require(memory.load<uint32_t>(sfr::NativeModules::xam_handle) == 0xdecafbadu,
            "constructor collision preserves sentinel");
}

void count_is_bounded() {
    sfr::GuestMemory memory;
    memory.map(0x70000, 0x1000); put_string(memory, 0x70000, "xam.xex");
    sfr::NativeModules modules(memory);
    for (uint32_t i = 0; i < 65535; ++i)
        require(modules.load_image(0x70000, 9, 0, 0x70100) == 0, "load before cap succeeds");
    require(modules.xam_load_count() == 65535, "count reaches cap");
    memory.store<uint32_t>(0x70100, 0x778899aa);
    rejects([&] { modules.load_image(0x70000, 9, 0, 0x70100); }, "native-module-count", "load beyond cap is rejected");
    require(memory.load<uint32_t>(0x70100) == 0x778899aa && modules.xam_load_count() == 65535,
            "overflow preserves output and count");
}

void missing_optional_exports_return_xbox_status_and_zero_output() {
    constexpr uint32_t missing_export = 0xc0000263;
    constexpr uint32_t ordinals[] = {0xaff, 0xb00, 0xb0b, 0xb10, 0x305, 0x30b, 0x24};
    sfr::GuestMemory memory;
    memory.map(0x80000, 0x1000); put_string(memory, 0x80000, "xam.xex");
    sfr::NativeModules modules(memory);
    require(modules.load_image(0x80000, 9, 0, 0x80100) == 0, "XAM acquisition succeeds");

    for (const uint32_t ordinal : ordinals) {
        memory.store<uint8_t>(0x801ff, 0x5a);
        memory.store<uint32_t>(0x80200, 0xa1b2c3d4);
        memory.store<uint8_t>(0x80204, 0xc7);
        const uint32_t count = modules.xam_load_count();
        require(modules.get_procedure_address(sfr::NativeModules::xam_handle, ordinal, 0x80200) == missing_export,
                "optional export reports procedure not found");
        require(memory.load<uint32_t>(0x80200) == 0, "missing export writes a big-endian null procedure");
        require(memory.load<uint8_t>(0x801ff) == 0x5a && memory.load<uint8_t>(0x80204) == 0xc7,
                "missing export preserves output guards");
        require(modules.xam_load_count() == count, "missing export preserves acquisition count");
    }
}

void procedure_lookup_rejects_unsupported_requests_before_mutation() {
    auto rejected = [](bool acquire, uint32_t handle, uint32_t ordinal, const char* message) {
        sfr::GuestMemory memory;
        memory.map(0x90000, 0x1000); put_string(memory, 0x90000, "xam.xex");
        memory.store<uint32_t>(0x90200, 0x11223344);
        sfr::NativeModules modules(memory);
        if (acquire) modules.load_image(0x90000, 9, 0, 0x90100);
        const uint32_t count = modules.xam_load_count();
        rejects([&] { modules.get_procedure_address(handle, ordinal, 0x90200); },
                "native-module-export", message);
        require(memory.load<uint32_t>(0x90200) == 0x11223344, "unsupported lookup preserves output");
        require(modules.xam_load_count() == count, "unsupported lookup preserves count");
    };
    rejected(false, sfr::NativeModules::xam_handle, 0xaff, "unacquired XAM is rejected");
    rejected(true, 0, 0xaff, "executable handle is rejected");
    rejected(true, 0x12345678, 0xaff, "unknown handle is rejected");
    rejected(true, sfr::NativeModules::xam_handle, 0, "named-pointer style request is rejected");
    rejected(true, sfr::NativeModules::xam_handle, 0xafe, "adjacent ordinal is rejected");
    rejected(true, sfr::NativeModules::xam_handle, 0xb01, "unknown party ordinal is rejected");
}

void procedure_lookup_validates_full_output_before_writing() {
    auto output_rejected = [](uint32_t output, auto prepare, const char* category, const char* message) {
        sfr::GuestMemory memory;
        memory.map(0xa0000, 0x1000); put_string(memory, 0xa0000, "xam.xex");
        prepare(memory);
        sfr::NativeModules modules(memory);
        modules.load_image(0xa0000, 9, 0, 0xa0100);
        const uint32_t count = modules.xam_load_count();
        rejects([&] { modules.get_procedure_address(sfr::NativeModules::xam_handle, 0xaff, output); }, category, message);
        require(modules.xam_load_count() == count, "invalid procedure output preserves count");
    };
    output_rejected(0, [](sfr::GuestMemory&) {}, "memory-access", "null procedure output is rejected");
    output_rejected(0xb0000, [](sfr::GuestMemory&) {}, "memory-access", "unmapped procedure output is rejected");
    {
        sfr::GuestMemory memory;
        memory.map(0xa0000, 0x1000); put_string(memory, 0xa0000, "xam.xex");
        memory.map(0xb0000, 4);
        memory.store<uint8_t>(0xb0000, 0x10); memory.store<uint8_t>(0xb0001, 0x11);
        memory.store<uint8_t>(0xb0002, 0x22); memory.store<uint8_t>(0xb0003, 0x33);
        sfr::NativeModules modules(memory);
        modules.load_image(0xa0000, 9, 0, 0xa0100);
        rejects([&] { modules.get_procedure_address(sfr::NativeModules::xam_handle, 0xaff, 0xb0001); },
                "memory-access", "tail procedure output is rejected");
        require(memory.load<uint32_t>(0xb0000) == 0x10112233,
                "tail procedure rejection preserves every mapped sentinel byte");
        require(modules.xam_load_count() == 1, "tail procedure rejection preserves count");
    }
    output_rejected(0xb1000, [](sfr::GuestMemory& m) {
        m.map(0xb1000, 4); m.store<uint32_t>(0xb1000, 0x55667788); m.add_import_variable(0xb1000, "guarded-output");
    }, "import-variable", "guarded procedure output is rejected");
    output_rejected(0xb2000, [](sfr::GuestMemory& m) {
        m.map(0xb2000, 4); m.store<uint32_t>(0xb2000, 0x66778899); m.add_read_only_word(0xb2000, [] { return 0x66778899u; });
    }, "memory-readonly", "read-only procedure output is rejected");
    {
        sfr::GuestMemory memory;
        memory.map(0xa0000, 0x1000); put_string(memory, 0xa0000, "xam.xex");
        memory.map(0xb3000, 4); memory.store<uint32_t>(0xb3000, 0x778899aa);
        sfr::NativeModules modules(memory);
        modules.load_image(0xa0000, 9, 0, 0xa0100);
        memory.load_reserved_word(0xb3000);
        rejects([&] { modules.get_procedure_address(sfr::NativeModules::xam_handle, 0xaff, 0xb3000); },
                "reservation-interference", "reserved procedure output is rejected");
        require(memory.load<uint32_t>(0xb3000) == 0x778899aa,
                "reserved procedure rejection preserves output sentinel");
        require(modules.xam_load_count() == 1, "reserved procedure rejection preserves count");
    }
}

void unload_retains_kernel_resident_xam_identity() {
    sfr::GuestMemory memory;
    memory.map(0xc0000, 0x1000); put_string(memory, 0xc0000, "xam.xex");
    sfr::NativeModules modules(memory);
    rejects([&] { modules.unload_image(sfr::NativeModules::xam_handle); }, "native-module-request",
            "unload before acquisition is rejected");
    rejects([&] { modules.unload_image(0x12345678); }, "native-module-request", "unknown unload is rejected");
    modules.load_image(0xc0000, 9, 0, 0xc0100);
    require(modules.unload_image(sfr::NativeModules::xam_handle) == 0, "acquired XAM unload succeeds");
    require(modules.unload_image(sfr::NativeModules::xam_handle) == 0, "repeated XAM unload succeeds");
    require(modules.xam_load_count() == 1, "kernel-resident unload retains acquisition count");
    modules.load_image(0xc0000, 9, 0, 0xc0104);
    require(memory.load<uint32_t>(0xc0104) == sfr::NativeModules::xam_handle,
            "load after unload retains namespace identity");
    require(modules.xam_load_count() == 2, "load after unload increments retained count");
}

void converts_only_procedure_not_found_status() {
    const auto translated = sfr::module_status_to_dos_error(0xc0000263);
    require(translated && *translated == 127, "procedure-not-found maps to ERROR_PROC_NOT_FOUND");
    require(!sfr::module_status_to_dos_error(0xc0000262), "preceding status is not translated");
    require(!sfr::module_status_to_dos_error(0xc0000264), "following status is not translated");
    require(!sfr::module_status_to_dos_error(0), "success is not translated");
}
void named_query_borrows_identity_without_acquisition() {
    sfr::GuestMemory memory;
    memory.map(0xd0000, 0x1000);
    put_string(memory, 0xd0000, "xam.xex");
    sfr::NativeModules modules(memory);
    require(modules.get_module_handle(0xd0000, 0xd0100) == 0, "resident XAM query succeeds");
    require(memory.load<uint32_t>(0xd0100) == sfr::NativeModules::xam_handle && modules.xam_load_count() == 0,
            "borrowed identity must not acquire a load reference");
    require(modules.get_procedure_address(sfr::NativeModules::xam_handle, 0xaff, 0xd0104) == 0xc0000263,
            "borrowed module identity supports an existing known export query");
    require(modules.get_procedure_address(sfr::NativeModules::xam_handle, 36, 0xd0104) == 0xc0000263 &&
            memory.load<uint32_t>(0xd0104) == 0,
            "unimplemented optional WSAStartupEx allows the original plain startup fallback");
    rejects([&] { modules.get_procedure_address(sfr::NativeModules::xam_handle, 37, 0xd0104); },
            "native-module-export", "unrelated unknown export remains a visible stop");
    require(modules.get_module_handle(0xd0000, 0xd0000) == 0, "query captures name before aliased output");
    require(memory.load<uint32_t>(0xd0000) == sfr::NativeModules::xam_handle, "aliased name publishes same identity");
    require(modules.xam_load_count() == 0, "repeated query does not acquire references");
}
void rejected_named_query_does_not_publish_identity() {
    sfr::GuestMemory memory;
    memory.map(0xe0000, 0x1000);
    put_string(memory, 0xe0000, "xam.xex");
    put_string(memory, 0xe0020, "other.xex");
    memory.store<uint32_t>(0xe0100, 0x12345678);
    sfr::NativeModules modules(memory);
    rejects([&] { modules.get_module_handle(0xe0020, 0xe0100); }, "native-module-request", "unknown namespace rejected");
    rejects([&] { modules.get_module_handle(0, 0xe0100); }, "native-module-request", "null name belongs to executable lookup");
    rejects([&] { modules.get_module_handle(0xe0000, 0); }, "memory-access", "null output rejected");
    rejects([&] { modules.get_module_handle(0xe0000, 0xe0ffe); }, "memory-access", "partial output rejected");
    memory.add_import_variable(0xe0104, "query-output-guard");
    rejects([&] { modules.get_module_handle(0xe0000, 0xe0104); }, "import-variable", "guarded output rejected");
    rejects([&] { modules.get_procedure_address(sfr::NativeModules::xam_handle, 0xaff, 0xe0100); },
            "native-module-export", "failed query must not authorize borrowed identity");
    require(memory.load<uint32_t>(0xe0100) == 0x12345678 && modules.xam_load_count() == 0,
            "rejected queries preserve output and acquisition state");
}
}

int main() {
    try {
        succeeds_with_persistent_native_identity();
        captures_name_before_an_aliasing_output_write();
        rejected_requests_preserve_output_and_count();
        rejected_outputs_preserve_state();
        rejects_mapping_collisions();
        count_is_bounded();
        missing_optional_exports_return_xbox_status_and_zero_output();
        procedure_lookup_rejects_unsupported_requests_before_mutation();
        procedure_lookup_validates_full_output_before_writing();
        unload_retains_kernel_resident_xam_identity();
        converts_only_procedure_not_found_status();
        named_query_borrows_identity_without_acquisition();
        rejected_named_query_does_not_publish_identity();
        std::cout << "Native module checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
