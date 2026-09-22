#include "guest_files.h"
#include "asset_files.h"
#include "guest_memory.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include "test_platform.h"

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void stop(F f) {
    try { f(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("expected checked guest file stop");
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("sfr-guest-files-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Fixture() {
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("test directory collision");
        std::ofstream(root / "test.bin", std::ios::binary) << "actual-file-bytes";
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};
}
int main() {
    try {
        Fixture fixture;
        sfr::AssetFiles files(fixture.root);
        sfr::GuestMemory memory;
        memory.map(0x10000, 0x1000);
        sfr::GuestFiles guest(memory, files);
        constexpr uint32_t output=0x10000, io=0x10008, attrs=0x10020, ansi=0x10040, path=0x10100;
        auto name = [&](const std::string& value) {
            memory.store<uint16_t>(ansi, uint16_t(value.size()));
            memory.store<uint16_t>(ansi+2, uint16_t(value.size()));
            memory.store<uint32_t>(ansi+4, path);
            for (size_t i=0; i<value.size(); ++i) memory.store<uint8_t>(path+i, uint8_t(value[i]));
            // Counted strings need not have a trailing NUL.
            memory.store<uint8_t>(path+value.size(), 0x7f);
        };
        name("game:\\test.bin");
        memory.store<uint32_t>(attrs, 0xfffffffd);
        memory.store<uint32_t>(attrs+4, ansi);
        memory.store<uint32_t>(attrs+8, 0x40);
        sfr::GuestFiles::OpenRequest request{output,0x80100080,attrs,io,0,0,0,1,0x60};
        auto sentinels = [&] {
            memory.store<uint32_t>(output, 0x11223344);
            memory.store<uint64_t>(io, 0x5566778899aabbccull);
        };
        auto rejected = [&](auto changed) {
            sentinels(); const auto count=files.open_count();
            stop([&]{ guest.open(changed); });
            require(files.open_count()==count, "rejection must not open host file");
            require(memory.load<uint32_t>(output)==0x11223344 && memory.load<uint64_t>(io)==0x5566778899aabbccull,
                    "rejection preserves outputs");
        };
        for (int field=0; field<6; ++field) {
            auto changed=request;
            switch(field) {
            case 0: changed.access=0x40100080; break;
            case 1: changed.allocation_size=path; break;
            case 2: changed.file_attributes=1; break;
            case 3: changed.disposition=2; break;
            case 4: changed.options=0x41; break;  // FILE_DIRECTORY_FILE is outside the read profile
            case 5: changed.share_access=8; break;
            }
            rejected(changed);
        }
        auto overlap=request; overlap.handle_output=io+4; rejected(overlap);
        auto truncated=request; truncated.io_output=0x10ffc; rejected(truncated);
        auto unmapped=request; unmapped.attributes=0x20000; rejected(unmapped);
        memory.store<uint32_t>(attrs, 123); rejected(request); memory.store<uint32_t>(attrs, 0xfffffffd);
        memory.store<uint32_t>(attrs+8, 0); rejected(request); memory.store<uint32_t>(attrs+8, 0x40);
        memory.store<uint16_t>(ansi+2, 1); rejected(request); name("game:\\test.bin");
        memory.add_read_only_word(0x10200, [] { return 0u; });
        auto readonly=request; readonly.handle_output=0x10200; rejected(readonly);
        require(guest.open(request)==0, "real open succeeds");
        const auto handle=memory.load<uint32_t>(output);
        require(handle==0x72000004 && memory.base()[output]==0x72 && memory.base()[output+3]==4,
                "guest handle is big-endian");
        require(memory.load<uint64_t>(io)==1 && files.size(handle)==17 && files.open_count()==1,
                "IO block status0/action1 matches actual file");
        constexpr uint32_t info=0x10300;
        auto fill_info = [&] {
            for (uint32_t i=0; i<64; ++i) memory.store<uint8_t>(info+i, 0xab);
            memory.store<uint64_t>(io, 0x1122334455667788ull);
        };
        auto unchanged_info = [&] {
            for (uint32_t i=0; i<64; ++i) require(memory.load<uint8_t>(info+i)==0xab, "failed query preserves information");
            require(memory.load<uint64_t>(io)==0x1122334455667788ull, "failed query preserves IO block");
        };
        fill_info();
        require(guest.query_information(handle,io,info,55,34)==0xc0000004, "short query length status");
        unchanged_info();
        require(guest.query_information(0xffffffff,io,info,56,34)==0xc0000008, "invalid query handle status");
        unchanged_info();
        stop([&]{ guest.query_information(handle,io,info,56,4); }); unchanged_info();
        stop([&]{ guest.query_information(handle,io,info,57,34); }); unchanged_info();
        stop([&]{ guest.query_information(handle,info+48,info,56,34); }); unchanged_info();
        stop([&]{ guest.query_information(handle,io,0x10fe0,56,34); }); unchanged_info();
        stop([&]{ guest.query_information(handle,0x10ffc,info,56,34); }); unchanged_info();
        stop([&]{ guest.query_information(handle,io,0x10200,56,34); }); unchanged_info();
        stop([&]{ guest.query_information(handle,0x10200,info,56,34); }); unchanged_info();
        require(guest.query_information(handle,io,info,56,34)==0, "actual metadata query succeeds");
#ifdef _WIN32  // compared with the NT metadata of an independent handle
        FILE_BASIC_INFO native_basic{};
        FILE_STANDARD_INFO native_standard{};
        HANDLE metadata=CreateFileW((fixture.root/"test.bin").c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        require(metadata!=INVALID_HANDLE_VALUE, "independent metadata handle");
        const bool got_metadata=GetFileInformationByHandleEx(metadata,FileBasicInfo,&native_basic,sizeof(native_basic)) &&
            GetFileInformationByHandleEx(metadata,FileStandardInfo,&native_standard,sizeof(native_standard));
        CloseHandle(metadata);
        require(got_metadata, "independent native metadata queries");
        const uint64_t expected_fields[]{uint64_t(native_basic.CreationTime.QuadPart),uint64_t(native_basic.LastAccessTime.QuadPart),
            uint64_t(native_basic.LastWriteTime.QuadPart),uint64_t(native_basic.ChangeTime.QuadPart),
            uint64_t(native_standard.AllocationSize.QuadPart),uint64_t(native_standard.EndOfFile.QuadPart)};
        for (uint32_t field=0; field<6; ++field)
            for(uint32_t byte=0; byte<8; ++byte)
                require(memory.load<uint8_t>(info+field*8+byte)==uint8_t(expected_fields[field]>>((7-byte)*8)),
                        "all guest metadata fields have independent native values and big-endian bytes");
        require(memory.load<uint32_t>(info+48)==native_basic.FileAttributes && memory.load<uint32_t>(info+52)==0,
                "attributes and reserved padding are encoded");
#endif
        require(memory.load<uint64_t>(io)==56 && memory.load<uint64_t>(info+56)==0xababababababababull,
                "IO result and exact56-byte boundary");
        fill_info();
        require(guest.query_information(handle,0,info,56,34)==0, "IO block is optional");
        require(memory.load<uint64_t>(io)==0x1122334455667788ull, "null IO leaves unrelated memory unchanged");
        files.close(handle);
        fill_info();
        require(guest.query_information(handle,io,info,56,34)==0xc0000008, "closed query handle status");
        unchanged_info();
        auto asynchronous=request;
        asynchronous.access=0x80120089;
        asynchronous.options=0x48;
        asynchronous.share_access=1;
        // Synchronous and unbuffered at once, unknown option bits and write access are rejected.
        auto mismatched=asynchronous; mismatched.options=0x68; rejected(mismatched);
        mismatched=asynchronous; mismatched.options=0x49; rejected(mismatched);
        mismatched=asynchronous; mismatched.options=0x08; rejected(mismatched);
        mismatched=asynchronous; mismatched.access=0xC0100080; rejected(mismatched);
        auto bad_async_output=asynchronous; bad_async_output.io_output=0x10ffc; rejected(bad_async_output);
        require(guest.open(asynchronous)==0, "original async read-only existing-file profile opens real asset");
        const auto async_handle=memory.load<uint32_t>(output);
        const auto native=files.native_information(async_handle);
        require((native.mode&0x38)==8 && (native.access_flags&0x120089)==0x120089,
                "guest asynchronous flags correspond to real unbuffered native mode and access");
        require(memory.load<uint64_t>(io)==1 && files.size(async_handle)==17 && files.open_count()==1,
                "async guest open publishes actual handle and successful IO result");
        require(guest.query_information(async_handle,io,info,56,34)==0 && memory.load<uint64_t>(info+40)==17,
                "async native handle supplies actual file metadata");
        fill_info();
        require(guest.query_information(async_handle,io,info,3,27)==0xc0000004, "short XCTD query length status");
        unchanged_info();
        require(guest.query_information(0xffffffff,io,info,4,27)==0xc0000008, "invalid XCTD file handle status");
        unchanged_info();
        stop([&]{ guest.query_information(async_handle,io,info,5,27); }); unchanged_info();
        stop([&]{ guest.query_information(async_handle,info+2,info,4,27); }); unchanged_info();
        stop([&]{ guest.query_information(async_handle,0x10ffc,info,4,27); }); unchanged_info();
        stop([&]{ guest.query_information(async_handle,io,0x10200,4,27); }); unchanged_info();
        stop([&]{ guest.query_information(async_handle,io,0,4,27); }); unchanged_info();
        require(guest.query_information(async_handle,io,info,4,27)==0xc000000d,
                "Xbox XCTD metadata is explicitly unavailable, never host uncompressed success");
        require(memory.load<uint32_t>(info)==0 && memory.load<uint32_t>(io)==0xc000000d &&
                memory.load<uint32_t>(io+4)==0, "unavailable XCTD query publishes failure and zero returned length");
        for (uint32_t i=4; i<64; ++i)
            require(memory.load<uint8_t>(info+i)==0xab, "XCTD query only touches its four-byte buffer");
        fill_info();
        require(guest.query_information(async_handle,0,info,4,27)==0xc000000d &&
                memory.load<uint32_t>(info)==0 && memory.load<uint64_t>(io)==0x1122334455667788ull,
                "unavailable XCTD query accepts absent IO block without touching unrelated memory");
        require(guest.close(async_handle)==0 && files.open_count()==0, "guest close owns async native handle");
        fill_info();
        require(guest.query_information(async_handle,io,info,4,27)==0xc0000008, "closed XCTD query handle status");
        unchanged_info();
        name("game:\\missing.bin");
        require(guest.open(request)==0xc0000034, "missing file status");
        require(memory.load<uint32_t>(output)==0xffffffff && memory.load<uint32_t>(io)==0xc0000034 &&
                memory.load<uint32_t>(io+4)==0 && files.open_count()==0, "failed open outputs");
        sentinels();
        require(guest.open(asynchronous)==0xc0000034 && memory.load<uint32_t>(output)==0xffffffff &&
                memory.load<uint32_t>(io)==0xc0000034 && memory.load<uint32_t>(io+4)==0 && files.open_count()==0,
                "async missing asset reports actual error without publishing a handle");
        std::cout << "Guest file open checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
