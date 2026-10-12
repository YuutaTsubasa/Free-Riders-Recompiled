#include "window_title.h"

#include "native_graphics.h"
#include "title_stats.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace sfr {
// Frames presented so far (guest_graphics_hooks.cpp).
extern std::atomic<uint32_t> present_count;

namespace {
// The title is changed on a thread of its own: setting another thread's
// window text waits for that thread (the presentation's) to answer, and the
// input update must never wait on it -- the presentation may be waiting on
// the game. Each change gives up after a quarter of a second.
//
// Every half second it says the frame rate and the frame times, the
// graphics, and, when a body drives the game, how often it arrives.
// Never freed: the worker is detached and may still be waiting on them
// while the process's statics are destroyed.
std::mutex& title_lock = *new std::mutex;
std::condition_variable& title_wake = *new std::condition_variable;
HWND title_window = nullptr;
bool title_wanted = false;

// Which body drives the game, from the launcher's SFR_CAMERA: 'k' a Kinect,
// 'w' a webcam, 0 none (a pad).
char body_source() {
    const char* const camera = std::getenv("SFR_CAMERA");
    if (camera && std::strcmp(camera, "kinect") == 0) return 'k';
    if (camera && std::strcmp(camera, "motion") == 0) return 'w';
    return 0;
}

// "Vulkan 100%": the graphics the frames are drawn with, as the launcher
// chose them (native_presentation.cpp reads the same scale).
std::wstring graphics_label() {
    std::wstring label = selected_graphics_backend() == GraphicsBackend::vulkan ? L"Vulkan" : L"D3D12";
    const char* const scale = std::getenv("SFR_RENDER_SCALE");
    const std::string value = scale ? scale : "";
    const bool known = value == "50" || value == "75" || value == "100" || value == "150" || value == "200" ||
                       value == "300";
    label += L' ';
    label += known ? std::wstring(value.begin(), value.end()) : L"100";
    label += L'%';
    return label;
}

void title_worker() {
    using clock = std::chrono::steady_clock;
    const char body = body_source();
    const std::wstring graphics = graphics_label();
    std::wstring original;
    HWND original_of = nullptr;
    uint32_t counted = present_count.load(std::memory_order_relaxed);
    clock::time_point since = clock::now();
    take_title_stats();
    double fps = 0, worst = 0, body_rate = 0, pose_ms = 0;
    std::unique_lock guard(title_lock);
    for (;;) {
        title_wake.wait_for(guard, std::chrono::milliseconds(500), [] { return title_wanted; });
        title_wanted = false;
        HWND window = title_window;
        guard.unlock();
        const clock::time_point now = clock::now();
        const double seconds = std::chrono::duration<double>(now - since).count();
        if (seconds >= 0.45) {
            const uint32_t presents = present_count.load(std::memory_order_relaxed);
            const TitleStats stats = take_title_stats();
            fps = (presents - counted) / seconds;
            worst = stats.worst_frame_ms;
            body_rate = (body == 'k' ? stats.kinect_frames : stats.pose_estimates) / seconds;
            pose_ms = stats.pose_estimates ? stats.pose_ms / stats.pose_estimates : 0;
            counted = presents;
            since = now;
        }
        if (window && IsWindow(window)) {
            DWORD_PTR result = 0;
            if (window != original_of) {
                wchar_t text[256]{};
                if (SendMessageTimeoutW(window, WM_GETTEXT, 256, reinterpret_cast<LPARAM>(text),
                                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &result)) {
                    original = text;
                    original_of = window;
                }
            }
            if (window == original_of) {
                // [FPS 59.8 | 16.7/41.0 ms]: the frame rate, the average
                // frame and the longest one, which is where a stutter shows.
                wchar_t part[96];
                swprintf(part, 96, L"  [FPS %.1f | %.1f/%.1f ms]", fps, fps > 0 ? 1000.0 / fps : 0.0, worst);
                std::wstring title = original + part;
                if (body == 'k') {
                    swprintf(part, 96, L"  [Kinect %.0f]", body_rate);
                    title += part;
                } else if (body == 'w') {
                    swprintf(part, 96, L"  [Webcam %.0f | %.0f ms]", body_rate, pose_ms);
                    title += part;
                }
                title += L"  [" + graphics + L"]";
                const bool set = SendMessageTimeoutW(window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(title.c_str()),
                                                     SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &result) != 0;
                static bool told = false;
                if (set && !told) {
                    told = true;
                    std::cerr << "NATIVE_WINDOW_TITLE stats=1\n";
                }
            }
        }
        guard.lock();
    }
}

void show(HWND window) {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(title_worker).detach(); });
    std::lock_guard guard(title_lock);
    title_window = window;
    title_wanted = true;
    title_wake.notify_one();
}

HWND shown_on = nullptr;

// SFR_TITLE_STATS=0 (the launcher's Performance switch) leaves the title alone.
bool wanted() {
    static const bool on = [] {
        const char* const text = std::getenv("SFR_TITLE_STATS");
        return !(text && *text == '0');
    }();
    return on;
}
}

void poll_window_title() {
    if (!wanted()) return;
    // The game's window, as the presentation made it (not any window of
    // ours in front: an adjustment window would keep a frozen title).
    HWND game = static_cast<HWND>(game_window());
    if (game && game != shown_on) {
        shown_on = game;
        show(game);
    }
}
}
#else
namespace sfr {
void poll_window_title() {}
}
#endif
