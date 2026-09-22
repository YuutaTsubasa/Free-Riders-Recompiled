#include "content_files.h"
#include "guest_memory.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace sfr {
namespace fs = std::filesystem;

namespace {
constexpr uint32_t error_success = 0, error_file_not_found = 2, error_path_not_found = 3,
                   error_already_exists = 183, error_invalid_parameter = 87;
constexpr uint32_t status_success = 0, status_end_of_file = 0xC0000011, status_invalid_parameter = 0xC000000D,
                   status_invalid_handle = 0xC0000008, status_name_not_found = 0xC0000034,
                   status_name_collision = 0xC0000035, status_path_not_found = 0xC000003A,
                   status_access_denied = 0xC0000022, status_is_directory = 0xC00000BA,
                   status_not_directory = 0xC0000103, status_not_implemented = 0xC0000002,
                   status_disk_full = 0xC000007F;
constexpr uint32_t attribute_directory = 0x10, attribute_normal = 0x80;
constexpr uint32_t content_data_size = 0x134;

[[noreturn]] void unsupported(uint64_t value, const std::string& detail) {
    throw RuntimeStop("content-files", value, detail);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// FILETIME (100 ns since 1601) of a host timestamp.
uint64_t filetime(fs::file_time_type time) {
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const auto since_unix = std::chrono::duration_cast<std::chrono::microseconds>(system.time_since_epoch()).count();
    return uint64_t(since_unix) * 10 + 116444736000000000ull;
}

// Splits "\??\root:\a\b" into the lower-case root and "a/b"; false when the
// path names no root or climbs out of it.
bool split(const std::string& path, std::string& root, fs::path& relative) {
    std::string text = path;
    if (text.rfind("\\??\\", 0) == 0) text = text.substr(4);
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    root = lower(text.substr(0, colon));
    std::string rest = text.substr(colon + 1);
    while (!rest.empty() && (rest.front() == '\\' || rest.front() == '/')) rest.erase(rest.begin());
    relative.clear();
    size_t begin = 0;
    while (begin <= rest.size() && !rest.empty()) {
        const size_t end = std::min(rest.find_first_of("\\/", begin), rest.size());
        const std::string part = rest.substr(begin, end - begin);
        if (part == "..") return false;
        if (!part.empty() && part != ".") relative /= part;
        begin = end + 1;
        if (end == rest.size()) break;
    }
    return true;
}
}

struct ContentFiles::File {
    fs::path path;
    bool directory = false;
    std::fstream stream;
    uint64_t position = 0;
    bool delete_on_close = false;
};

ContentFiles::ContentFiles(GuestMemory& memory, fs::path root) : memory_(memory), root_(std::move(root)) {}
ContentFiles::~ContentFiles() = default;

fs::path ContentFiles::package_directory(uint64_t xuid, uint32_t content_type, const std::string& file_name) const {
    char owner[17], type[9];
    std::snprintf(owner, sizeof(owner), "%016llX", static_cast<unsigned long long>(xuid));
    std::snprintf(type, sizeof(type), "%08X", content_type);
    return root_ / owner / type / file_name;
}

std::string ContentFiles::read_root(uint32_t root_name) const {
    if (!root_name) unsupported(0, "content root name is required");
    std::string name;
    for (uint32_t i = 0; i < 16; ++i) {
        const char c = char(memory_.load<uint8_t>(uint64_t(root_name) + i));
        if (!c) break;
        name.push_back(c);
    }
    if (name.empty() || name.size() >= 16) unsupported(root_name, "content root name must be 1..15 characters");
    return lower(name);
}

uint32_t ContentFiles::create(uint64_t xuid, uint32_t root_name, uint32_t content_data, uint32_t flags,
                              uint32_t disposition_output, uint32_t license_output) {
    const std::string root = read_root(root_name);
    if (!content_data) return error_invalid_parameter;
    memory_.check(content_data, content_data_size);
    const uint32_t device = memory_.load<uint32_t>(content_data);
    const uint32_t content_type = memory_.load<uint32_t>(uint64_t(content_data) + 4);
    std::string file_name;
    for (uint32_t i = 0; i < 42; ++i) {
        const char c = char(memory_.load<uint8_t>(uint64_t(content_data) + 0x108 + i));
        if (!c) break;
        if (c == '/' || c == '\\' || c == ':' || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|')
            return error_invalid_parameter;
        file_name.push_back(c);
    }
    if (file_name.empty() || file_name == "." || file_name == "..") return error_invalid_parameter;
    if (device != device_id && device != 0) return error_path_not_found;
    const fs::path directory = package_directory(xuid, content_type, file_name);
    std::error_code error;
    const bool exists = fs::is_directory(directory, error);
    // XCONTENTFLAG_CREATENEW 1, CREATEALWAYS 2, OPENEXISTING 3, OPENALWAYS 4, TRUNCATEEXISTING 5.
    const uint32_t mode = flags & 0xF;
    if (mode < 1 || mode > 5) unsupported(flags, "unsupported content creation flags");
    if ((mode == 3 || mode == 5) && !exists) return error_file_not_found;
    if (mode == 1 && exists) return error_already_exists;
    if (exists && (mode == 2 || mode == 5)) {
        fs::remove_all(directory, error);
        if (error) return error_path_not_found;
    }
    const bool created = !exists || mode == 2 || mode == 5;
    fs::create_directories(directory, error);
    if (error) return error_path_not_found;
    if (created) {
        // The display name, for enumeration.
        std::ofstream name(fs::path(directory).concat(".name"), std::ios::binary | std::ios::trunc);
        for (uint32_t i = 0; i < 128; ++i) {
            const uint16_t c = memory_.load<uint16_t>(uint64_t(content_data) + 8 + i * 2);
            if (!c) break;
            name.put(char(c & 0xFF));
            name.put(char(c >> 8));
        }
    }
    mounts_[root] = Mount{directory};
    if (disposition_output) memory_.store<uint32_t>(disposition_output, created ? 1 : 2);
    if (license_output) memory_.store<uint32_t>(license_output, 0);
    return error_success;
}

uint32_t ContentFiles::close(uint32_t root_name) {
    const std::string root = read_root(root_name);
    if (!mounts_.erase(root)) return error_file_not_found;
    // Handles opened under the root stay usable until the title closes them.
    return error_success;
}

uint32_t ContentFiles::creator(uint64_t xuid, uint32_t content_data, uint32_t is_creator_output, uint32_t xuid_output) {
    if (!content_data) return error_invalid_parameter;
    if (is_creator_output) memory_.store<uint32_t>(is_creator_output, 1);
    if (xuid_output) memory_.store<uint64_t>(xuid_output, xuid);
    return error_success;
}

uint32_t ContentFiles::device_data(uint32_t device, uint32_t output) {
    if (device != device_id) return error_file_not_found;
    if (!output) return error_invalid_parameter;
    memory_.check_write(output, 0x50);
    std::error_code error;
    fs::create_directories(root_, error);
    const auto space = fs::space(root_, error);
    // A console hard drive reports at most its own capacity; keep the numbers
    // in the range titles expect (they compare free space with their size).
    const uint64_t total = error ? (uint64_t(20) << 30) : std::min<uint64_t>(space.capacity, uint64_t(250) << 30);
    const uint64_t free = error ? (uint64_t(16) << 30) : std::min<uint64_t>(space.available, total);
    memory_.store<uint32_t>(output, device_id);
    memory_.store<uint32_t>(uint64_t(output) + 4, 1);  // XCONTENTDEVICETYPE_HDD
    memory_.store<uint64_t>(uint64_t(output) + 8, total);
    memory_.store<uint64_t>(uint64_t(output) + 16, free);
    const std::u16string name = u"Hard Drive";
    for (uint32_t i = 0; i < 28; ++i)
        memory_.store<uint16_t>(uint64_t(output) + 24 + i * 2, i < name.size() ? uint16_t(name[i]) : 0);
    return error_success;
}

std::vector<ContentFiles::Package> ContentFiles::packages(uint64_t xuid, uint32_t content_type) const {
    std::vector<Package> result;
    const fs::path directory = package_directory(xuid, content_type, "").parent_path();
    std::error_code error;
    for (auto it = fs::directory_iterator(directory, error); !error && it != fs::directory_iterator(); it.increment(error)) {
        if (!it->is_directory()) continue;
        Package package{content_type, {}, it->path().filename().string()};
        if (package.file_name.size() >= 42) continue;
        std::ifstream name(fs::path(it->path()).concat(".name"), std::ios::binary);
        for (int low, high; (low = name.get()) != EOF && (high = name.get()) != EOF;)
            package.display_name.push_back(char16_t(low | high << 8));
        result.push_back(std::move(package));
    }
    std::sort(result.begin(), result.end(), [](const Package& a, const Package& b) { return a.file_name < b.file_name; });
    return result;
}

void ContentFiles::write_content_data(GuestMemory& memory, uint32_t address, const Package& package) {
    memory.check_write(address, content_data_size);
    memory.store<uint32_t>(address, device_id);
    memory.store<uint32_t>(uint64_t(address) + 4, package.content_type);
    for (uint32_t i = 0; i < 128; ++i)
        memory.store<uint16_t>(uint64_t(address) + 8 + i * 2,
                               i < package.display_name.size() && i < 127 ? uint16_t(package.display_name[i]) : 0);
    for (uint32_t i = 0; i < 42; ++i)
        memory.store<uint8_t>(uint64_t(address) + 0x108 + i, i < package.file_name.size() ? uint8_t(package.file_name[i]) : 0);
    memory.store<uint16_t>(uint64_t(address) + 0x132, 0);
}

bool ContentFiles::claims(const std::string& path) const {
    std::string root;
    fs::path relative;
    return split(path, root, relative) && mounts_.count(root);
}

fs::path ContentFiles::host_path(const std::string& path) const {
    std::string root;
    fs::path relative;
    if (!split(path, root, relative)) return {};
    const auto mount = mounts_.find(root);
    return mount == mounts_.end() ? fs::path{} : mount->second.directory / relative;
}

void ContentFiles::complete(uint32_t io_output, uint32_t status, uint32_t information) {
    if (!io_output) return;
    memory_.store<uint32_t>(io_output, status);
    memory_.store<uint32_t>(uint64_t(io_output) + 4, information);
}

uint32_t ContentFiles::open(const std::string& path, uint32_t handle_output, uint32_t access, uint32_t io_output,
                            uint32_t share_access, uint32_t disposition, uint32_t options) {
    if (!handle_output || !io_output) unsupported(handle_output, "file handle and IO outputs must be non-null");
    memory_.check_write(handle_output, 4);
    memory_.check_write(io_output, 8);
    (void)access;
    (void)share_access;
    const fs::path host = host_path(path);
    if (host.empty()) return status_path_not_found;
    const bool want_directory = options & 0x1;   // FILE_DIRECTORY_FILE
    const bool want_file = options & 0x40;       // FILE_NON_DIRECTORY_FILE
    std::error_code error;
    const bool exists = fs::exists(host, error);
    const bool is_directory = exists && fs::is_directory(host, error);
    if (!fs::is_directory(host.parent_path(), error)) return status_path_not_found;
    if (disposition > 5) return status_invalid_parameter;
    // FILE_SUPERSEDE 0, OPEN 1, CREATE 2, OPEN_IF 3, OVERWRITE 4, OVERWRITE_IF 5.
    if (!exists && (disposition == 1 || disposition == 4)) return status_name_not_found;
    if (exists && disposition == 2) return status_name_collision;
    if (exists && is_directory && want_file) return status_is_directory;
    if (exists && !is_directory && want_directory) return status_not_directory;
    if (next_handle_ >= handle_limit) unsupported(next_handle_, "content file handles exhausted");

    auto file = std::make_unique<File>();
    file->path = host;
    uint32_t information = 1;  // FILE_OPENED
    if (want_directory || is_directory) {
        if (!exists) {
            fs::create_directory(host, error);
            if (error) return status_access_denied;
            information = 2;  // FILE_CREATED
        }
        file->directory = true;
    } else {
        const bool truncate = !exists || disposition == 0 || disposition == 4 || disposition == 5;
        if (truncate) {
            std::ofstream(host, std::ios::binary | std::ios::trunc);
            information = !exists ? 2 : disposition == 0 ? 0 : 3;  // CREATED, SUPERSEDED, OVERWRITTEN
        }
        file->stream.open(host, std::ios::binary | std::ios::in | std::ios::out);
        if (!file->stream) return status_access_denied;
    }
    const uint32_t handle = next_handle_;
    next_handle_ += 4;
    files_.emplace(handle, std::move(file));
    memory_.store<uint32_t>(handle_output, handle);
    complete(io_output, status_success, information);
    return status_success;
}

bool ContentFiles::owns(uint32_t handle) const { return files_.count(handle) != 0; }

ContentFiles::File* ContentFiles::file(uint32_t handle) const {
    const auto found = files_.find(handle);
    return found == files_.end() ? nullptr : found->second.get();
}

uint32_t ContentFiles::read(uint32_t handle, uint32_t io_output, uint32_t buffer, uint32_t length, uint32_t offset_pointer) {
    File* f = file(handle);
    if (!f) return status_invalid_handle;
    if (f->directory) return status_invalid_parameter;
    uint64_t offset = f->position;
    if (offset_pointer) {
        const uint64_t requested = memory_.load<uint64_t>(offset_pointer);
        if (requested != 0xFFFFFFFFFFFFFFFEull) offset = requested;  // not FILE_USE_FILE_POINTER_POSITION
    }
    if (length) memory_.check_write(buffer, length);
    std::error_code error;
    const uint64_t size = fs::file_size(f->path, error);
    if (length && offset >= size) {
        complete(io_output, status_end_of_file, 0);
        return status_end_of_file;
    }
    const uint32_t count = uint32_t(std::min<uint64_t>(length, size - offset));
    std::vector<char> bytes(count);
    f->stream.clear();
    f->stream.seekg(std::streamoff(offset));
    f->stream.read(bytes.data(), count);
    const uint32_t transferred = uint32_t(f->stream.gcount());
    for (uint32_t i = 0; i < transferred; ++i) memory_.store<uint8_t>(uint64_t(buffer) + i, uint8_t(bytes[i]));
    f->position = offset + transferred;
    complete(io_output, status_success, transferred);
    return status_success;
}

uint32_t ContentFiles::write(uint32_t handle, uint32_t io_output, uint32_t buffer, uint32_t length, uint32_t offset_pointer) {
    File* f = file(handle);
    if (!f) return status_invalid_handle;
    if (f->directory) return status_invalid_parameter;
    uint64_t offset = f->position;
    if (offset_pointer) {
        const uint64_t requested = memory_.load<uint64_t>(offset_pointer);
        if (requested == 0xFFFFFFFFFFFFFFFFull) {  // FILE_WRITE_TO_END_OF_FILE
            std::error_code error;
            offset = fs::file_size(f->path, error);
        } else if (requested != 0xFFFFFFFFFFFFFFFEull) {
            offset = requested;
        }
    }
    if (length) memory_.check(buffer, length);
    std::vector<char> bytes(length);
    for (uint32_t i = 0; i < length; ++i) bytes[i] = char(memory_.load<uint8_t>(uint64_t(buffer) + i));
    f->stream.clear();
    f->stream.seekp(std::streamoff(offset));
    f->stream.write(bytes.data(), length);
    f->stream.flush();
    if (!f->stream) {
        complete(io_output, status_disk_full, 0);
        return status_disk_full;
    }
    f->position = offset + length;
    complete(io_output, status_success, length);
    return status_success;
}

uint32_t ContentFiles::query_information(uint32_t handle, uint32_t io_output, uint32_t output, uint32_t length,
                                         uint32_t info_class) {
    File* f = file(handle);
    if (!f) return status_invalid_handle;
    std::error_code error;
    const uint64_t size = f->directory ? 0 : fs::file_size(f->path, error);
    const uint64_t allocation = (size + 4095) & ~uint64_t(4095);
    const uint64_t time = filetime(fs::last_write_time(f->path, error));
    const uint32_t attributes = f->directory ? attribute_directory : attribute_normal;
    const auto need = [&](uint32_t bytes) {
        if (length < bytes) return false;
        memory_.check_write(output, bytes);
        return true;
    };
    switch (info_class) {
    case 27:  // Xbox XFileXctdCompressionInformation: saves are not XCTD-compressed
        if (need(4)) memory_.store<uint32_t>(output, 0);
        complete(io_output, status_invalid_parameter, 0);
        return status_invalid_parameter;
    case 4:  // FileBasicInformation
        if (!need(40)) return status_invalid_parameter;
        for (uint32_t i = 0; i < 4; ++i) memory_.store<uint64_t>(uint64_t(output) + i * 8, time);
        memory_.store<uint32_t>(uint64_t(output) + 32, attributes);
        memory_.store<uint32_t>(uint64_t(output) + 36, 0);
        complete(io_output, status_success, 40);
        return status_success;
    case 5:  // FileStandardInformation
        if (!need(24)) return status_invalid_parameter;
        memory_.store<uint64_t>(output, allocation);
        memory_.store<uint64_t>(uint64_t(output) + 8, size);
        memory_.store<uint32_t>(uint64_t(output) + 16, 1);
        memory_.store<uint8_t>(uint64_t(output) + 20, f->delete_on_close);
        memory_.store<uint8_t>(uint64_t(output) + 21, f->directory);
        memory_.store<uint16_t>(uint64_t(output) + 22, 0);
        complete(io_output, status_success, 24);
        return status_success;
    case 14:  // FilePositionInformation
        if (!need(8)) return status_invalid_parameter;
        memory_.store<uint64_t>(output, f->position);
        complete(io_output, status_success, 8);
        return status_success;
    case 34:  // FileNetworkOpenInformation
        if (!need(56)) return status_invalid_parameter;
        for (uint32_t i = 0; i < 4; ++i) memory_.store<uint64_t>(uint64_t(output) + i * 8, time);
        memory_.store<uint64_t>(uint64_t(output) + 32, allocation);
        memory_.store<uint64_t>(uint64_t(output) + 40, size);
        memory_.store<uint32_t>(uint64_t(output) + 48, attributes);
        memory_.store<uint32_t>(uint64_t(output) + 52, 0);
        complete(io_output, status_success, 56);
        return status_success;
    default:
        unsupported(info_class, "unsupported content file information class " + std::to_string(info_class));
    }
}

uint32_t ContentFiles::set_information(uint32_t handle, uint32_t io_output, uint32_t input, uint32_t length,
                                       uint32_t info_class) {
    File* f = file(handle);
    if (!f) return status_invalid_handle;
    std::error_code error;
    switch (info_class) {
    case 4:  // FileBasicInformation: times and attributes are not kept
        complete(io_output, status_success, 0);
        return status_success;
    case 13:  // FileDispositionInformation
        if (length < 1) return status_invalid_parameter;
        f->delete_on_close = memory_.load<uint8_t>(input) != 0;
        complete(io_output, status_success, 0);
        return status_success;
    case 14:  // FilePositionInformation
        if (length < 8) return status_invalid_parameter;
        f->position = memory_.load<uint64_t>(input);
        complete(io_output, status_success, 0);
        return status_success;
    case 19:  // FileAllocationInformation: space is not reserved
        complete(io_output, status_success, 0);
        return status_success;
    case 20:  // FileEndOfFileInformation
        if (length < 8 || f->directory) return status_invalid_parameter;
        f->stream.flush();
        fs::resize_file(f->path, memory_.load<uint64_t>(input), error);
        if (error) return status_disk_full;
        complete(io_output, status_success, 0);
        return status_success;
    default:
        unsupported(info_class, "unsupported content file set-information class " + std::to_string(info_class));
    }
}

uint32_t ContentFiles::query_full_attributes(const std::string& path, uint32_t output) {
    const fs::path host = host_path(path);
    std::error_code error;
    if (host.empty() || !fs::exists(host, error))
        return fs::is_directory(host.parent_path(), error) ? status_name_not_found : status_path_not_found;
    memory_.check_write(output, 56);
    const bool directory = fs::is_directory(host, error);
    const uint64_t size = directory ? 0 : fs::file_size(host, error);
    const uint64_t time = filetime(fs::last_write_time(host, error));
    for (uint32_t i = 0; i < 4; ++i) memory_.store<uint64_t>(uint64_t(output) + i * 8, time);
    memory_.store<uint64_t>(uint64_t(output) + 32, (size + 4095) & ~uint64_t(4095));
    memory_.store<uint64_t>(uint64_t(output) + 40, size);
    memory_.store<uint32_t>(uint64_t(output) + 48, directory ? attribute_directory : attribute_normal);
    memory_.store<uint32_t>(uint64_t(output) + 52, 0);
    return status_success;
}

uint32_t ContentFiles::flush(uint32_t handle, uint32_t io_output) {
    File* f = file(handle);
    if (!f) return status_invalid_handle;
    if (!f->directory) f->stream.flush();
    complete(io_output, status_success, 0);
    return status_success;
}

uint32_t ContentFiles::close_handle(uint32_t handle) {
    const auto found = files_.find(handle);
    if (found == files_.end()) return status_invalid_handle;
    File& f = *found->second;
    if (f.stream.is_open()) f.stream.close();
    if (f.delete_on_close) {
        std::error_code error;
        fs::remove(f.path, error);
    }
    files_.erase(found);
    return status_success;
}
}
