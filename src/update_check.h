#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The launcher's "a newer version is available" notice (Issue #365): at
// most once an hour it asks GitHub for the project's latest release and, when
// that is newer than this build, offers the release's file for this platform
// in the browser. Nothing is downloaded or installed by the launcher itself.
//
// This file is the part without a network: reading the answer, comparing
// versions, choosing the file and deciding when to ask. The request is
// sfr::launcher::fetch_https (launcher_platform.h), a function here so the
// tests hand in a made-up answer.
namespace sfr::update {

// The one request, carrying no cookie, account or other personal data: the
// User-Agent names the launcher and its version only.
inline constexpr const char* latest_release_url =
    "https://api.github.com/repos/YuutaTsubasa/Free-Riders-Recompiled/releases/latest";
// Asked only when the latest release is not the game's (a tag that is not
// vX.Y.Z): the recent releases, newest first.
inline constexpr const char* recent_releases_url =
    "https://api.github.com/repos/YuutaTsubasa/Free-Riders-Recompiled/releases?per_page=20";
// Opened when this platform has no file of its own in a release.
inline constexpr const char* releases_page_url = "https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/latest";
inline constexpr int timeout_ms = 5000;
inline constexpr int64_t check_interval_seconds = 60 * 60;  // an hour

// This build's version (scripts/package_android.py's VERSION_NAME, read by
// CMake), "" when the build was made without it.
const char* build_version();
// "FreeRidersRecompiled-launcher/<version>".
std::string user_agent(std::string_view version);

struct Version {
    uint32_t major = 0, minor = 0, patch = 0;
    friend bool operator==(const Version&, const Version&) = default;
    friend auto operator<=>(const Version&, const Version&) = default;
};
// "0.10.69": three numbers and nothing else.
std::optional<Version> parse_version(std::string_view text);
// A release tag, "v0.10.69"; anything else (deps-onnxruntime-1.30.0, a
// v0.11.0-rc1) is not a release of the game.
std::optional<Version> parse_release_tag(std::string_view tag);
std::string format_version(const Version& version);

struct Asset {
    std::string name, url;  // url: browser_download_url
};
struct Release {
    std::string tag, page;  // page: html_url
    bool draft = false, prerelease = false;
    std::vector<Asset> assets;
};
// GitHub's answer for one release; nullopt when it is not JSON or has no tag.
std::optional<Release> parse_release(std::string_view json);
// Its answer for a list of releases (entries without a tag are skipped).
std::optional<std::vector<Release>> parse_releases(std::string_view json);

// Which release file runs here. Android's two APKs are the API 28 build
// (Android 9) and the API 29 one ("android10"); the build knows its own.
enum class Os { windows, linux_desktop, android, macos, ios, other };
enum class Arch { x64, arm64, other };
struct Platform {
    Os os = Os::other;
    Arch arch = Arch::other;
    int android_api = 0;
};
Platform this_platform();
// "windows-x64.zip", "android10-arm64.apk", ...; "" when releases carry
// nothing for this platform (Linux on ARM, an Intel Mac, Android x86-64).
std::string asset_suffix(const Platform& platform);
// The file for this platform, FreeRidersRecompiled-<version>-<suffix>
// exactly (not its .idsig or licences), else the release's page. Only
// addresses on the project's GitHub are taken; anything else falls back to
// releases_page_url.
std::string download_url(const Release& release, const Platform& platform);

// What a check found: the newest release and where its file is. Kept in
// settings.ini, so the notice stays without asking again.
struct Found {
    std::string version;  // "0.10.70"
    std::string url;
};
// The release as a notice: nullopt for a draft, a prerelease or a tag that
// is not vX.Y.Z (GitHub's latest skips the first two already).
std::optional<Found> found_release(const Release& release, const Platform& platform);
// The highest-versioned game release of a list, if any.
std::optional<Found> newest_release(const std::vector<Release>& releases, const Platform& platform);

// Whether check_interval_seconds (an hour) has passed since the last check
// (or the clock went back).
bool check_due(int64_t last_checked, int64_t now);
// Whether found is newer than this build; false when either is unknown.
bool is_newer(std::string_view found, std::string_view current);

// The fetch: the body of a 200 answer, nullopt for anything else (no
// network, a timeout, a refusal, GitHub's rate limit).
using Fetch = std::function<std::optional<std::string>(const std::string& url, const std::string& user_agent, int timeout_ms)>;

enum class Outcome { found, no_answer, unreadable, not_a_release };
const char* outcome_name(Outcome outcome);
struct CheckResult {
    Outcome outcome = Outcome::no_answer;
    std::optional<Found> found;
};
// One check: the request, then reading it. When GitHub's latest release is
// not a game release, a second request reads the recent ones.
CheckResult run_check(const Fetch& fetch, std::string_view current_version, const Platform& platform);
}
