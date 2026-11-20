#include "update_check.h"

#include <charconv>
#include <cstdio>

#ifndef SFR_VERSION
#define SFR_VERSION ""
#endif

namespace sfr::update {
namespace {
// The project's own addresses: GitHub's answer is trusted for nothing else.
constexpr std::string_view project_prefix = "https://github.com/YuutaTsubasa/Free-Riders-Recompiled/";

// Just enough JSON for a release: objects, arrays, strings, and numbers,
// true, false and null read as their text. Depth is bounded: the answer
// comes from the network.
struct Value {
    enum Kind { null, boolean, number, string, array, object } kind = null;
    bool flag = false;
    std::string text;
    std::vector<Value> items;
    // An object's keys and their values, side by side: a vector of pairs
    // holding Value would need Value complete here (libstdc++ checks).
    std::vector<std::string> keys;
    std::vector<Value> fields;

    const Value* field(std::string_view name) const {
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == name) return &fields[i];
        return nullptr;
    }
    std::string string_field(std::string_view name) const {
        const Value* value = field(name);
        return value && value->kind == string ? value->text : std::string();
    }
    bool bool_field(std::string_view name) const {
        const Value* value = field(name);
        return value && value->kind == boolean && value->flag;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : at_(text.data()), end_(text.data() + text.size()) {}

    std::optional<Value> document() {
        Value value;
        if (!parse(value, 0)) return std::nullopt;
        skip_space();
        if (at_ != end_) return std::nullopt;
        return value;
    }

private:
    const char* at_;
    const char* end_;

    void skip_space() {
        while (at_ != end_ && (*at_ == ' ' || *at_ == '\t' || *at_ == '\n' || *at_ == '\r')) ++at_;
    }
    bool take(char c) {
        skip_space();
        if (at_ == end_ || *at_ != c) return false;
        ++at_;
        return true;
    }
    bool word(std::string_view w) {
        if (size_t(end_ - at_) < w.size() || std::string_view(at_, w.size()) != w) return false;
        at_ += w.size();
        return true;
    }
    static void put_utf8(std::string& out, uint32_t c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    bool hex4(uint32_t& out) {
        if (end_ - at_ < 4) return false;
        out = 0;
        for (int i = 0; i < 4; ++i, ++at_) {
            const char c = *at_;
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) return false;
            out = out * 16 + uint32_t(digit);
        }
        return true;
    }
    bool parse_string(std::string& out) {
        if (!take('"')) return false;
        while (at_ != end_) {
            const char c = *at_++;
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (at_ == end_) return false;
            const char e = *at_++;
            switch (e) {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t c1 = 0;
                if (!hex4(c1)) return false;
                if (c1 >= 0xD800 && c1 < 0xDC00) {
                    uint32_t c2 = 0;
                    if (!word("\\u") || !hex4(c2) || c2 < 0xDC00 || c2 >= 0xE000) return false;
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                } else if (c1 >= 0xDC00 && c1 < 0xE000) {
                    return false;
                }
                put_utf8(out, c1);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool parse(Value& value, int depth) {
        if (depth > 32) return false;
        skip_space();
        if (at_ == end_) return false;
        const char c = *at_;
        if (c == '{') {
            ++at_;
            value.kind = Value::object;
            if (take('}')) return true;
            do {
                std::string key;
                Value item;
                if (!parse_string(key) || !take(':') || !parse(item, depth + 1)) return false;
                value.keys.push_back(std::move(key));
                value.fields.push_back(std::move(item));
            } while (take(','));
            return take('}');
        }
        if (c == '[') {
            ++at_;
            value.kind = Value::array;
            if (take(']')) return true;
            do {
                Value item;
                if (!parse(item, depth + 1)) return false;
                value.items.push_back(std::move(item));
            } while (take(','));
            return take(']');
        }
        if (c == '"') {
            value.kind = Value::string;
            return parse_string(value.text);
        }
        if (word("true")) { value.kind = Value::boolean; value.flag = true; return true; }
        if (word("false")) { value.kind = Value::boolean; return true; }
        if (word("null")) return true;
        const char* start = at_;
        while (at_ != end_ && (*at_ == '-' || *at_ == '+' || *at_ == '.' || *at_ == 'e' || *at_ == 'E' ||
                               (*at_ >= '0' && *at_ <= '9')))
            ++at_;
        if (at_ == start) return false;
        value.kind = Value::number;
        value.text.assign(start, at_);
        return true;
    }
};

bool from_project(std::string_view url) { return url.substr(0, project_prefix.size()) == project_prefix; }

std::optional<Release> release_from(const Value& value) {
    if (value.kind != Value::object) return std::nullopt;
    Release release;
    release.tag = value.string_field("tag_name");
    if (release.tag.empty()) return std::nullopt;
    release.page = value.string_field("html_url");
    release.draft = value.bool_field("draft");
    release.prerelease = value.bool_field("prerelease");
    if (const Value* assets = value.field("assets"); assets && assets->kind == Value::array) {
        for (const Value& item : assets->items) {
            if (item.kind != Value::object) continue;
            Asset asset{item.string_field("name"), item.string_field("browser_download_url")};
            if (!asset.name.empty() && !asset.url.empty()) release.assets.push_back(std::move(asset));
        }
    }
    return release;
}
}

// The version inside a marker that stays in the program as one string:
// scripts/package_release.py looks for it, so a launcher built before the
// version was bumped (which would announce its own release as an update)
// never goes into a release.
const char* build_version() {
    static constexpr char marker[] = "FreeRidersRecompiled-launcher-version=" SFR_VERSION;
    return marker + sizeof("FreeRidersRecompiled-launcher-version=") - 1;
}

std::string user_agent(std::string_view version) {
    return "FreeRidersRecompiled-launcher/" + std::string(version.empty() ? "unknown" : version);
}

std::optional<Version> parse_version(std::string_view text) {
    Version version;
    uint32_t* parts[3] = {&version.major, &version.minor, &version.patch};
    const char* at = text.data();
    const char* end = text.data() + text.size();
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            if (at == end || *at != '.') return std::nullopt;
            ++at;
        }
        // Digits only: from_chars would take no sign, but check the first
        // character anyway so "+1" or " 1" are refused.
        if (at == end || *at < '0' || *at > '9') return std::nullopt;
        const auto [next, error] = std::from_chars(at, end, *parts[i]);
        if (error != std::errc()) return std::nullopt;
        at = next;
    }
    if (at != end) return std::nullopt;
    return version;
}

std::optional<Version> parse_release_tag(std::string_view tag) {
    if (tag.size() < 2 || tag[0] != 'v') return std::nullopt;
    return parse_version(tag.substr(1));
}

std::string format_version(const Version& version) {
    char text[40];
    std::snprintf(text, sizeof text, "%u.%u.%u", version.major, version.minor, version.patch);
    return text;
}

std::optional<Release> parse_release(std::string_view json) {
    const auto document = Parser(json).document();
    return document ? release_from(*document) : std::nullopt;
}

std::optional<std::vector<Release>> parse_releases(std::string_view json) {
    const auto document = Parser(json).document();
    if (!document || document->kind != Value::array) return std::nullopt;
    std::vector<Release> releases;
    for (const Value& item : document->items)
        if (auto release = release_from(item)) releases.push_back(std::move(*release));
    return releases;
}

std::optional<Found> newest_release(const std::vector<Release>& releases, const Platform& platform) {
    std::optional<Found> newest;
    for (const Release& release : releases) {
        auto found = found_release(release, platform);
        if (found && (!newest || is_newer(found->version, newest->version))) newest = std::move(found);
    }
    return newest;
}

Platform this_platform() {
    Platform platform;
#if defined(_WIN32)
    platform.os = Os::windows;
#elif defined(__ANDROID__)
    platform.os = Os::android;
    platform.android_api = __ANDROID_API__;  // the build's minimum: 28 or 29 (scripts/build_android.sh)
#elif defined(SFR_IOS)
    platform.os = Os::ios;
#elif defined(__APPLE__)
    platform.os = Os::macos;
#elif defined(__linux__)
    platform.os = Os::linux_desktop;
#endif
#if defined(_M_X64) || defined(__x86_64__)
    platform.arch = Arch::x64;
#elif defined(_M_ARM64) || defined(__aarch64__)
    platform.arch = Arch::arm64;
#endif
    return platform;
}

std::string asset_suffix(const Platform& platform) {
    switch (platform.os) {
    case Os::windows:
        // x64 only; Windows on ARM runs it emulated.
        return platform.arch == Arch::x64 || platform.arch == Arch::arm64 ? "windows-x64.zip" : "";
    case Os::linux_desktop: return platform.arch == Arch::x64 ? "linux-x64.tar.gz" : "";
    case Os::android:
        if (platform.arch != Arch::arm64) return "";
        return platform.android_api >= 29 ? "android10-arm64.apk" : "android-arm64.apk";
    case Os::macos: return platform.arch == Arch::arm64 ? "macos-arm64.zip" : "";
    case Os::ios: return "ios-arm64.ipa";
    case Os::other: break;
    }
    return "";
}

std::string download_url(const Release& release, const Platform& platform) {
    const auto version = parse_release_tag(release.tag);
    const std::string suffix = asset_suffix(platform);
    if (version && !suffix.empty()) {
        const std::string name = "FreeRidersRecompiled-" + format_version(*version) + "-" + suffix;
        for (const Asset& asset : release.assets)
            if (asset.name == name && from_project(asset.url)) return asset.url;
    }
    return from_project(release.page) ? release.page : releases_page_url;
}

std::optional<Found> found_release(const Release& release, const Platform& platform) {
    if (release.draft || release.prerelease) return std::nullopt;
    const auto version = parse_release_tag(release.tag);
    if (!version) return std::nullopt;
    return Found{format_version(*version), download_url(release, platform)};
}

bool check_due(int64_t last_checked, int64_t now) {
    return last_checked <= 0 || now < last_checked || now - last_checked >= check_interval_seconds;
}

bool is_newer(std::string_view found, std::string_view current) {
    const auto a = parse_version(found), b = parse_version(current);
    return a && b && *a > *b;
}

const char* outcome_name(Outcome outcome) {
    switch (outcome) {
    case Outcome::found: return "found";
    case Outcome::no_answer: return "no_answer";
    case Outcome::unreadable: return "unreadable";
    case Outcome::not_a_release: return "not_a_release";
    }
    return "?";
}

CheckResult run_check(const Fetch& fetch, std::string_view current_version, const Platform& platform) {
    CheckResult result;
    const auto body = fetch ? fetch(latest_release_url, user_agent(current_version), timeout_ms) : std::nullopt;
    if (!body) return result;
    const auto release = parse_release(*body);
    if (!release) {
        result.outcome = Outcome::unreadable;
        return result;
    }
    result.found = found_release(*release, platform);
    if (!result.found) {
        // GitHub's latest is not a game release (another kind of release
        // published as a full one): the newest game release among the recent
        // ones, so the notice does not go quiet until the next game release.
        const auto list = fetch(recent_releases_url, user_agent(current_version), timeout_ms);
        if (const auto releases = list ? parse_releases(*list) : std::nullopt) result.found = newest_release(*releases, platform);
    }
    result.outcome = result.found ? Outcome::found : Outcome::not_a_release;
    return result;
}
}
