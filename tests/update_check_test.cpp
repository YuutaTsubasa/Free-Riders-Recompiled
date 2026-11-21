#include "update_check.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace up = sfr::update;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// GitHub's answer for a release, cut down to what the launcher reads plus a
// few of the fields around them (with escapes, nested objects and numbers,
// which the reader must step over).
std::string release_json(const std::string& tag, bool draft = false, bool prerelease = false) {
    const std::string v = tag.size() > 1 && tag[0] == 'v' ? tag.substr(1) : tag;
    const std::string base = "https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/download/" + tag + "/";
    auto asset = [&](const std::string& name) {
        return "{\"url\":\"https://api.github.com/x\",\"id\":123,\"name\":\"" + name +
               "\",\"uploader\":{\"login\":\"YuutaTsubasa\",\"site_admin\":false},\"size\":1048576,"
               "\"browser_download_url\":\"" + base + name + "\"}";
    };
    std::string assets;
    for (const char* suffix : {"android-arm64-licenses.zip", "android-arm64.apk", "android-arm64.apk.idsig",
                               "android10-arm64.apk", "android10-arm64.apk.idsig", "ios-arm64.ipa", "linux-x64.tar.gz",
                               "macos-arm64.zip", "windows-x64.zip"}) {
        if (!assets.empty()) assets += ",";
        assets += asset("FreeRidersRecompiled-" + v + "-" + suffix);
    }
    if (!assets.empty()) assets += ",";
    assets += asset("SHA256SUMS.txt");
    return "{\n  \"url\": \"https://api.github.com/repos/YuutaTsubasa/Free-Riders-Recompiled/releases/1\",\n"
           "  \"html_url\": \"https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/" + tag + "\",\n"
           "  \"id\": 271828, \"tag_name\": \"" + tag + "\", \"target_commitish\": \"main\",\n"
           "  \"name\": \"" + tag + " \\u2014 Older processors \\\"without\\\" AVX \\ud83d\\ude80\",\n"
           "  \"draft\": " + (draft ? "true" : "false") + ", \"prerelease\": " + (prerelease ? "true" : "false") + ",\n"
           "  \"created_at\": \"2026-11-19T19:51:27Z\", \"reactions\": {\"+1\": 2, \"total_count\": 2.5e0},\n"
           "  \"assets\": [" + assets + "],\n"
           "  \"body\": \"Line one\\nLine two with a path C:\\\\Games\\\\ and [a link](https://example.com)\", \"mentions_count\": null\n}";
}

up::Platform platform(up::Os os, up::Arch arch, int api = 0) {
    up::Platform p;
    p.os = os;
    p.arch = arch;
    p.android_api = api;
    return p;
}

void versions() {
    require(up::parse_version("0.10.69") == up::Version{0, 10, 69}, "plain version");
    require(up::parse_version("1.2.3") < up::parse_version("1.2.10"), "patch compares as a number");
    require(*up::parse_version("0.10.9") < *up::parse_version("0.10.10"), "0.10.9 < 0.10.10");
    require(*up::parse_version("0.9.99") < *up::parse_version("0.10.0"), "minor first");
    require(*up::parse_version("1.0.0") > *up::parse_version("0.99.99"), "major first");
    for (const char* bad : {"", "0.10", "0.10.69.1", "v0.10.69", "0.10.69-rc1", "0.10.x", " 0.10.69", "0.10.69 ", "+0.10.69",
                            "0..69", "0.10.99999999999"})
        require(!up::parse_version(bad), bad);
    require(up::format_version({0, 10, 70}) == "0.10.70", "format");

    require(up::is_newer("0.10.10", "0.10.9"), "0.10.10 is newer than 0.10.9");
    require(!up::is_newer("0.10.9", "0.10.10"), "an older release is not newer");
    require(!up::is_newer("0.10.69", "0.10.69"), "the same version is not newer");
    require(!up::is_newer("0.10.70", ""), "no current version: no notice");
    require(!up::is_newer("", "0.10.69"), "nothing found: no notice");
    require(!up::is_newer("garbage", "0.10.69"), "an unreadable cache: no notice");
}

void tags() {
    require(up::parse_release_tag("v0.10.70") == up::Version{0, 10, 70}, "a release tag");
    for (const char* bad : {"deps-onnxruntime-1.30.0", "0.10.70", "v", "V0.10.70", "v0.10.70-rc1", "v0.10", "vv0.10.70", ""})
        require(!up::parse_release_tag(bad), bad);

    const auto windows = platform(up::Os::windows, up::Arch::x64);
    require(up::found_release(*up::parse_release(release_json("v0.10.70")), windows).has_value(), "a release is found");
    require(!up::found_release(*up::parse_release(release_json("deps-onnxruntime-1.30.0")), windows), "the ONNX mirror is not a release");
    require(!up::found_release(*up::parse_release(release_json("v0.10.70", true)), windows), "a draft is not a release");
    require(!up::found_release(*up::parse_release(release_json("v0.10.70", false, true)), windows), "a prerelease is not a release");
    require(!up::found_release(*up::parse_release(release_json("v0.10.70-beta")), windows), "a suffixed tag is not a release");
}

void reading() {
    const auto release = up::parse_release(release_json("v0.10.70"));
    require(release.has_value(), "the answer reads");
    require(release->tag == "v0.10.70", "tag");
    require(release->page == "https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.10.70", "page");
    require(!release->draft && !release->prerelease, "flags");
    require(release->assets.size() == 10, "every asset");
    require(release->assets[0].name == "FreeRidersRecompiled-0.10.70-android-arm64-licenses.zip", "the first asset's name");

    for (const char* bad : {"", "not json", "[]", "{}", "{\"tag_name\": 7}", "{\"tag_name\": \"v0.10.70\"", "{\"tag_name\": \"v0.10.70\"} x",
                            "{\"tag_name\": \"v0.10.70\", \"name\": \"\\ud83d\"}", "{\"tag_name\": \"\\x\"}"})
        require(!up::parse_release(bad), bad);
    // Nested far deeper than any release: refused rather than recursed into.
    require(!up::parse_release("{\"tag_name\":\"v1.0.0\",\"x\":" + std::string(200, '[') + std::string(200, ']') + "}"), "depth");
    // Assets without a name or address are skipped, the rest kept.
    const auto partial = up::parse_release(
        "{\"tag_name\":\"v1.0.0\",\"assets\":[{\"name\":\"a\"},7,{\"name\":\"b\",\"browser_download_url\":\"u\"}]}");
    require(partial && partial->assets.size() == 1 && partial->assets[0].name == "b", "partial assets");
}

void picking() {
    const auto release = *up::parse_release(release_json("v0.10.70"));
    const std::string base = "https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/download/v0.10.70/FreeRidersRecompiled-0.10.70-";
    const std::string page = "https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.10.70";
    require(up::download_url(release, platform(up::Os::windows, up::Arch::x64)) == base + "windows-x64.zip", "Windows");
    require(up::download_url(release, platform(up::Os::windows, up::Arch::arm64)) == base + "windows-x64.zip", "Windows on ARM");
    require(up::download_url(release, platform(up::Os::linux_desktop, up::Arch::x64)) == base + "linux-x64.tar.gz", "Linux");
    require(up::download_url(release, platform(up::Os::linux_desktop, up::Arch::arm64)) == page, "Linux on ARM: the page");
    require(up::download_url(release, platform(up::Os::android, up::Arch::arm64, 29)) == base + "android10-arm64.apk",
            "Android 10 build: the android10 APK");
    require(up::download_url(release, platform(up::Os::android, up::Arch::arm64, 30)) == base + "android10-arm64.apk",
            "a newer API is still the android10 flavour");
    require(up::download_url(release, platform(up::Os::android, up::Arch::arm64, 28)) == base + "android-arm64.apk",
            "Android 9 build: the older APK, never its .idsig or licences");
    require(up::download_url(release, platform(up::Os::android, up::Arch::x64, 29)) == page, "Android x86-64: the page");
    require(up::download_url(release, platform(up::Os::macos, up::Arch::arm64)) == base + "macos-arm64.zip", "macOS");
    require(up::download_url(release, platform(up::Os::macos, up::Arch::x64)) == page, "an Intel Mac: the page");
    require(up::download_url(release, platform(up::Os::ios, up::Arch::arm64)) == base + "ios-arm64.ipa", "iOS");
    require(up::download_url(release, platform(up::Os::other, up::Arch::x64)) == page, "another system: the page");

    // A release that lacks this platform's file: its page.
    auto missing = release;
    missing.assets.clear();
    require(up::download_url(missing, platform(up::Os::windows, up::Arch::x64)) == page, "no file: the page");
    // Addresses off the project's GitHub are never opened.
    auto elsewhere = release;
    for (auto& asset : elsewhere.assets) asset.url = "https://example.com/" + asset.name;
    elsewhere.page = "https://example.com/release";
    require(up::download_url(elsewhere, platform(up::Os::windows, up::Arch::x64)) == up::releases_page_url,
            "foreign addresses: the project's releases page");
    auto lookalike = release;
    for (auto& asset : lookalike.assets) asset.url = "https://github.com/YuutaTsubasa/Free-Riders-Recompiled-evil/" + asset.name;
    require(up::download_url(lookalike, platform(up::Os::windows, up::Arch::x64)) == page, "a look-alike repository is foreign");

    const auto here = up::this_platform();
#if defined(_WIN32)
    require(here.os == up::Os::windows, "this build is Windows");
#elif defined(__ANDROID__)
    require(here.os == up::Os::android && here.android_api >= 28, "this build is Android");
#elif defined(__APPLE__)
    require(here.os == up::Os::macos || here.os == up::Os::ios, "this build is Apple");
#elif defined(__linux__)
    require(here.os == up::Os::linux_desktop, "this build is Linux");
#endif
}

void throttle() {
    const int64_t hour = up::check_interval_seconds, t = 1'790'000'000;
    require(hour == 60 * 60, "the interval is an hour (Issue #365)");
    require(up::check_due(0, t), "never checked");
    require(!up::check_due(t, t), "just checked");
    require(!up::check_due(t, t + 59 * 60), "59 minutes later");
    require(!up::check_due(t, t + hour - 1), "within the hour");
    require(up::check_due(t, t + hour), "an hour later");
    require(up::check_due(t, t + hour + 60), "an hour and a minute later (an open launcher's next look)");
    require(up::check_due(t, t + 24 * hour), "a day later");
    require(up::check_due(t, t - 60), "the clock went back: ask again");
}

void checking() {
    int calls = 0;
    std::string asked_url, asked_agent;
    int asked_timeout = 0;
    const auto answer = [&](std::optional<std::string> body) {
        return [&, body](const std::string& url, const std::string& agent, int timeout) {
            ++calls;
            asked_url = url;
            asked_agent = agent;
            asked_timeout = timeout;
            return body;
        };
    };
    const auto android10 = platform(up::Os::android, up::Arch::arm64, 29);

    // The reporter's case: 0.10.49 on the android10 APK sees 0.10.50's.
    auto result = up::run_check(answer(release_json("v0.10.50")), "0.10.49", android10);
    require(calls == 1, "one request");
    require(asked_url == "https://api.github.com/repos/YuutaTsubasa/Free-Riders-Recompiled/releases/latest", "the latest release");
    require(asked_agent == "FreeRidersRecompiled-launcher/0.10.49", "the User-Agent names the launcher and its version only");
    require(asked_timeout > 0 && asked_timeout <= 5000, "a short timeout");
    require(result.outcome == up::Outcome::found && result.found, "found");
    require(result.found->version == "0.10.50", "the version found");
    require(result.found->url == "https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/download/v0.10.50/"
                                 "FreeRidersRecompiled-0.10.50-android10-arm64.apk", "the same flavour's APK");
    require(up::is_newer(result.found->version, "0.10.49"), "newer than the installed one");
    require(!up::is_newer(result.found->version, "0.10.50"), "the same version: no notice");

    // GitHub's latest is not a game release: the recent releases are read
    // and the newest game release among them is the answer.
    {
        std::vector<std::string> urls;
        const std::string list = "[" + release_json("deps-onnxruntime-1.30.0") + "," + release_json("v0.10.51", false, true) + "," +
                                 release_json("v0.10.50") + "," + release_json("v0.10.48") + ",{\"id\":1}]";
        const auto fallback = up::run_check([&](const std::string& url, const std::string&, int) -> std::optional<std::string> {
            urls.push_back(url);
            return urls.size() == 1 ? release_json("deps-onnxruntime-1.30.0") : list;
        }, "0.10.49", android10);
        require(urls.size() == 2 && urls[1] == up::recent_releases_url, "a second request for the recent releases");
        require(fallback.outcome == up::Outcome::found && fallback.found && fallback.found->version == "0.10.50",
                "the newest game release, not the prerelease or the mirror");
        require(fallback.found->url.ends_with("/v0.10.50/FreeRidersRecompiled-0.10.50-android10-arm64.apk"), "its APK");
        // The list unreadable or without a game release: nothing found.
        require(up::run_check([&](const std::string& url, const std::string&, int) -> std::optional<std::string> {
                    return url == up::latest_release_url ? release_json("deps-onnxruntime-1.30.0") : std::string("[]");
                }, "0.10.49", android10).outcome == up::Outcome::not_a_release, "no game release at all");
        require(!up::parse_releases(release_json("v0.10.50")), "an object is not a list");
    }
    // A latest release that is the game's needs one request only.
    calls = 0;
    up::run_check(answer(release_json("v0.10.50")), "0.10.49", android10);
    require(calls == 1, "one request when the latest is a game release");
    require(up::run_check(answer(std::nullopt), "0.10.49", android10).outcome == up::Outcome::no_answer, "no network");
    require(!up::run_check(answer(std::nullopt), "0.10.49", android10).found, "no network: nothing found");
    require(up::run_check(answer(std::string("<html>rate limited</html>")), "0.10.49", android10).outcome ==
                up::Outcome::unreadable, "not JSON");
    require(up::run_check(answer(release_json("deps-onnxruntime-1.30.0")), "0.10.49", android10).outcome ==
                up::Outcome::not_a_release, "the mirror's tag (and the same answer for the list, which is no list)");
    require(up::run_check(nullptr, "0.10.49", android10).outcome == up::Outcome::no_answer, "no fetch at all");
    require(up::user_agent("") == "FreeRidersRecompiled-launcher/unknown", "no version");
    require(up::outcome_name(up::Outcome::found) == std::string("found"), "outcome names");
}
}

int main() {
    try {
        versions();
        tags();
        reading();
        picking();
        throttle();
        checking();
        // The build's own version is a release's (scripts/package_android.py).
        require(std::string(up::build_version()).empty() || up::parse_version(up::build_version()), "build version");
    } catch (const std::exception& error) {
        std::cerr << "update_check: " << error.what() << '\n';
        return 1;
    }
    std::cout << "update_check: ok (build version " << up::build_version() << ")\n";
    return 0;
}
