#include "guest_printf.h"
#include "guest_memory.h"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <string_view>

namespace sfr {
namespace {
std::string read_string(GuestMemory& memory, uint32_t address, int64_t limit) {
    if (!address) return "(null)";
    std::string text;
    for (uint64_t i = 0; (limit < 0 || int64_t(i) < limit) && i < 0x100000; ++i) {
        const char c = char(memory.load<uint8_t>(uint64_t(address) + i));
        if (!c) break;
        text += c;
    }
    return text;
}
}

std::string guest_format(GuestMemory& memory, uint32_t format, const std::function<uint64_t()>& next) {
    const std::string pattern = read_string(memory, format, -1);
    std::string out;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') { out += pattern[i]; continue; }
        std::string spec = "%";
        size_t j = i + 1;
        while (j < pattern.size() && std::string_view("-+ #0").find(pattern[j]) != std::string_view::npos) spec += pattern[j++];
        const auto number = [&](bool allow_star) {
            if (allow_star && j < pattern.size() && pattern[j] == '*') {
                spec += std::to_string(int32_t(next()));
                ++j;
                return;
            }
            while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9') spec += pattern[j++];
        };
        number(true);
        if (j < pattern.size() && pattern[j] == '.') { spec += pattern[j++]; number(true); }
        int size = 32;  // argument width for integers
        if (pattern.compare(j, 3, "I64") == 0) { size = 64; j += 3; }
        else if (pattern.compare(j, 2, "ll") == 0) { size = 64; j += 2; }
        else if (pattern.compare(j, 2, "hh") == 0) { size = 8; j += 2; }
        else if (j < pattern.size() && pattern[j] == 'h') { size = 16; ++j; }
        else if (j < pattern.size() && (pattern[j] == 'l' || pattern[j] == 'z')) ++j;  // 32-bit long/size_t
        if (j >= pattern.size()) throw RuntimeStop("guest-printf", format, "truncated conversion in format string");
        const char conversion = pattern[j];
        char buffer[512];
        switch (conversion) {
        case '%': out += '%'; break;
        case 'd': case 'i': {
            const uint64_t raw = next();
            const int64_t value = size == 64 ? int64_t(raw) : size == 16 ? int16_t(raw) : size == 8 ? int8_t(raw) : int32_t(raw);
            std::snprintf(buffer, sizeof buffer, (spec + "lld").c_str(), static_cast<long long>(value));
            out += buffer;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            const uint64_t raw = next();
            const uint64_t value = size == 64 ? raw : size == 16 ? uint16_t(raw) : size == 8 ? uint8_t(raw) : uint32_t(raw);
            std::snprintf(buffer, sizeof buffer, (spec + "ll" + conversion).c_str(), static_cast<unsigned long long>(value));
            out += buffer;
            break;
        }
        case 'p':
            std::snprintf(buffer, sizeof buffer, "%08X", uint32_t(next()));
            out += buffer;
            break;
        case 'c':
            std::snprintf(buffer, sizeof buffer, (spec + 'c').c_str(), char(next()));
            out += buffer;
            break;
        case 's': {
            // Precision limits the characters read from the guest string.
            const auto dot = spec.find('.');
            const int64_t limit = dot == std::string::npos ? -1 : std::stoll("0" + spec.substr(dot + 1));
            const std::string text = read_string(memory, uint32_t(next()), limit);
            std::snprintf(buffer, sizeof buffer, (spec + 's').c_str(), text.c_str());
            out += text.size() < sizeof buffer / 2 ? std::string(buffer) : text;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            std::snprintf(buffer, sizeof buffer, (spec + conversion).c_str(), std::bit_cast<double>(next()));
            out += buffer;
            break;
        default:
            throw RuntimeStop("guest-printf", format, std::string("unsupported printf conversion %") + conversion);
        }
        i = j;
    }
    return out;
}

int32_t guest_store_limited(GuestMemory& memory, uint32_t buffer, uint32_t count, const std::string& text) {
    const uint32_t copied = uint32_t(std::min<size_t>(text.size(), count));
    memory.check_write(buffer, copied + (text.size() < count ? 1 : 0));
    for (uint32_t i = 0; i < copied; ++i) memory.store<uint8_t>(uint64_t(buffer) + i, uint8_t(text[i]));
    if (text.size() < count) memory.store<uint8_t>(uint64_t(buffer) + copied, 0);
    return text.size() <= count ? int32_t(text.size()) : -1;
}

int32_t guest_store(GuestMemory& memory, uint32_t buffer, const std::string& text) {
    memory.check_write(buffer, text.size() + 1);
    for (size_t i = 0; i < text.size(); ++i) memory.store<uint8_t>(uint64_t(buffer) + i, uint8_t(text[i]));
    memory.store<uint8_t>(uint64_t(buffer) + text.size(), 0);
    return int32_t(text.size());
}
}
