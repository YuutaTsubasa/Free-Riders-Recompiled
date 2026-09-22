#include "asset_files.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <vector>

namespace {
constexpr uint32_t success = 0;
constexpr uint32_t end_of_file = 0xc0000011u;
constexpr uint32_t cancelled = 0xc0000120u;
using Mode = sfr::AssetFiles::OpenMode;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template <typename F> void rejects(F&& operation, const char* message) {
    try { operation(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error(message);
}

struct Fixture {
    std::filesystem::path directory, mount, file;
    uint32_t sector = 0;
    std::vector<uint8_t> bytes;

    Fixture() {
        wchar_t temporary[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH, temporary) != 0, "temporary volume path is available");
        DWORD sectors_per_cluster = 0, bytes_per_sector = 0, free_clusters = 0, clusters = 0;
        require(GetDiskFreeSpaceW(temporary, &sectors_per_cluster, &bytes_per_sector,
                                  &free_clusters, &clusters) != FALSE && bytes_per_sector != 0,
                "actual filesystem sector size is available");
        sector = bytes_per_sector;
        for (unsigned attempt = 0; attempt < 100 && directory.empty(); ++attempt) {
            auto candidate = std::filesystem::temp_directory_path() /
                ("sfr-native-async-read-" + std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(GetTickCount64()) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) directory = std::move(candidate);
        }
        require(!directory.empty(), "isolated asynchronous read fixture creates");
        mount = directory / "mount";
        std::filesystem::create_directory(mount);
        file = mount / "FNT_SE";
        bytes.resize(uint64_t(sector) * 2 + 123);
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i * 29 + 7);
        std::ofstream stream(file, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        require(bool(stream), "asynchronous read fixture bytes write");
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

sfr::AssetFiles::AsyncReadResult finish(sfr::AssetFiles::AsyncRead& request,
                                        sfr::AssetFiles::SubmitResult submitted) {
    if (submitted == sfr::AssetFiles::SubmitResult::pending) {
        rejects([&] { (void)request.result(); }, "pending request has no terminal result");
        return request.wait();
    }
    return request.result();
}

void real_read_reports_os_completion_and_partial_eof_bytes() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    auto opened = files.open("game:/FNT_SE", 1, Mode::asynchronous_unbuffered);
    auto request = files.prepare_async_read(opened.handle, fixture.sector * 4, 0);
    rejects([&] { (void)request->result(); }, "prepared request is not terminal");
    const auto submitted = request->submit();
    const auto result = finish(*request, submitted);
    require(result.status == success && result.offset == 0 &&
                result.transferred == fixture.bytes.size() && result.bytes.size() == result.transferred &&
                std::equal(result.bytes.begin(), result.bytes.end(), fixture.bytes.begin()),
            "real unbuffered OVERLAPPED read returns exact partial-EOF bytes and count");
    rejects([&] { (void)request->submit(); }, "request cannot be submitted twice");
    require(request->result().transferred == result.transferred,
            "terminal result remains stable after completion");
}

void zero_and_exact_eof_reads_have_real_terminal_status() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    auto opened = files.open("game:/FNT_SE", 1, Mode::asynchronous_unbuffered);
    auto zero = files.prepare_async_read(opened.handle, 0, 0);
    auto zero_result = finish(*zero, zero->submit());
    require(zero_result.status == success && zero_result.transferred == 0 && zero_result.bytes.empty(),
            "zero-length native read completes successfully without bytes");
    const uint64_t eof_offset = (fixture.bytes.size() + fixture.sector - 1) / fixture.sector * fixture.sector;
    auto eof = files.prepare_async_read(opened.handle, fixture.sector, eof_offset);
    auto eof_result = finish(*eof, eof->submit());
    require(eof_result.status == end_of_file && eof_result.transferred == 0 && eof_result.bytes.empty() &&
                eof_result.offset == eof_offset,
            "aligned read beyond the real EOF reports EOF with no fabricated transfer");
}

void validation_and_request_ownership_precede_native_effects() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    const auto synchronous = files.open("game:/FNT_SE", 1);
    const auto asynchronous = files.open("game:/FNT_SE", 1, Mode::asynchronous_unbuffered);
    rejects([&] { (void)files.prepare_async_read(synchronous.handle, fixture.sector, 0); },
            "synchronous handle cannot prepare OVERLAPPED read");
    rejects([&] { (void)files.prepare_async_read(0x720ffffcu, fixture.sector, 0); },
            "unknown handle cannot retain asynchronous ownership");
    rejects([&] { (void)files.prepare_async_read(asynchronous.handle, fixture.sector - 1, 0); },
            "unbuffered length must follow actual storage granularity");
    rejects([&] { (void)files.prepare_async_read(asynchronous.handle, fixture.sector, 1); },
            "unbuffered offset must follow actual storage granularity");
    auto prepared = files.prepare_async_read(asynchronous.handle, fixture.sector, 0);
    rejects([&] { files.close(asynchronous.handle); },
            "prepared request retains file ownership before submission");
    prepared.reset();
    files.close(asynchronous.handle);
    require(!files.owns(asynchronous.handle), "destroying prepared request releases close guard");
    files.close(synchronous.handle);
}

void cancellation_race_and_destructor_always_drain_native_io() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    const auto opened = files.open("game:/FNT_SE", 1, Mode::asynchronous_unbuffered);
    {
        auto request = files.prepare_async_read(opened.handle, fixture.sector * 4, 0);
        const auto submitted = request->submit();
        rejects([&] { files.close(opened.handle); },
                "submitted request prevents file close before request destruction");
        if (submitted == sfr::AssetFiles::SubmitResult::pending) {
            std::stop_source stop;
            stop.request_stop();
            const auto result = request->wait(stop.get_token());
            require((result.status == cancelled && result.transferred == 0) ||
                        (result.status == success && result.transferred == fixture.bytes.size()),
                    "real cancellation race reports actual aborted or completed result");
        } else {
            require(request->result().status == success,
                    "real immediate completion is not relabeled pending for cancellation testing");
        }
    }
    files.close(opened.handle);

    const auto second = files.open("game:/FNT_SE", 1, Mode::asynchronous_unbuffered);
    {
        auto request = files.prepare_async_read(second.handle, fixture.sector * 4, 0);
        (void)request->submit();
    }
    files.close(second.handle);
    require(files.open_count() == 0, "request destructor cancels/drains before releasing retained ownership");
}

void native_submission_failure_remains_failed_on_every_result_access() {
    Fixture fixture;
    HANDLE lock_handle = CreateFileW(fixture.file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OVERLAPPED, nullptr);
    require(lock_handle != INVALID_HANDLE_VALUE, "independent native lock handle opens");
    OVERLAPPED lock_offset{};
    require(LockFileEx(lock_handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                       0, fixture.sector, 0, &lock_offset) != FALSE,
            "independent native byte-range lock creates a real read conflict");
    try {
        sfr::AssetFiles files(fixture.mount);
        const auto opened = files.open("game:/FNT_SE", 3, Mode::asynchronous_unbuffered);
        auto request = files.prepare_async_read(opened.handle, fixture.sector, 0);
        bool native_failure = false;
        try {
            const auto submitted = request->submit();
            if (submitted == sfr::AssetFiles::SubmitResult::pending) (void)request->wait();
        } catch (const std::runtime_error&) {
            native_failure = true;
        }
        require(native_failure,
                "overlapping native file lock produces a real immediate or delayed read failure");
        rejects([&] { (void)request->result(); },
                "failed asynchronous request never exposes a fabricated success result");
        rejects([&] { (void)request->wait(); },
                "wait consistently preserves the stored terminal request failure");
        {
            auto destructor_drain = files.prepare_async_read(opened.handle, fixture.sector, 0);
            try { (void)destructor_drain->submit(); }
            catch (const std::runtime_error&) {}
            // If Windows returned pending, destruction must drain its eventual
            // lock-violation completion without terminating or orphaning ownership.
        }
        request.reset();
        files.close(opened.handle);
    } catch (...) {
        UnlockFileEx(lock_handle, 0, fixture.sector, 0, &lock_offset);
        CloseHandle(lock_handle);
        throw;
    }
    require(UnlockFileEx(lock_handle, 0, fixture.sector, 0, &lock_offset) != FALSE,
            "independent native byte-range lock releases");
    CloseHandle(lock_handle);
}
}

int main() {
    try {
        real_read_reports_os_completion_and_partial_eof_bytes();
        zero_and_exact_eof_reads_have_real_terminal_status();
        validation_and_request_ownership_precede_native_effects();
        cancellation_race_and_destructor_always_drain_native_io();
        native_submission_failure_remains_failed_on_every_result_access();
        std::cout << "Native asynchronous read checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
