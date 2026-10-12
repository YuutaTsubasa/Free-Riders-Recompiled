#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>

namespace sfr {
// The game's own window in this process (native_presentation.cpp's class),
// or null: the launcher's preview has none.
inline HWND find_game_window() {
    for (HWND window = nullptr; (window = FindWindowExW(nullptr, window, L"SonicFreeRidersNativePresentation", nullptr));) {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() && IsWindowVisible(window) && !IsIconic(window)) return window;
    }
    return nullptr;
}

// Moves a window that is about to be shown to the middle of the game's
// window (kept on the desktop, should it be larger than the game's window).
// Without a game window it stays where Windows put it.
inline void center_over_game(HWND window) {
    const HWND game = find_game_window();
    RECT over{}, own{};
    if (!game || !window || !GetWindowRect(game, &over) || !GetWindowRect(window, &own)) return;
    const LONG width = own.right - own.left, height = own.bottom - own.top;
    LONG x = (over.left + over.right) / 2 - width / 2, y = (over.top + over.bottom) / 2 - height / 2;
    const LONG left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    x = (std::max)(left, (std::min)(x, left + GetSystemMetrics(SM_CXVIRTUALSCREEN) - width));
    y = (std::max)(top, (std::min)(y, top + GetSystemMetrics(SM_CYVIRTUALSCREEN) - height));
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
}

// Puts a window in front with the keyboard, also when another program's
// window (or the game's, from another thread) is in front: Windows only lets
// the thread that has the foreground hand it over, so this thread joins that
// one's input for the moment it takes.
inline void bring_to_front(HWND window) {
    if (!window || !IsWindow(window)) return;
    const HWND front = GetForegroundWindow();
    const DWORD own = GetCurrentThreadId();
    const DWORD theirs = front ? GetWindowThreadProcessId(front, nullptr) : 0;
    const bool joined = theirs && theirs != own && AttachThreadInput(theirs, own, TRUE);
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(window);
    SetForegroundWindow(window);
    SetFocus(window);
    if (joined) AttachThreadInput(theirs, own, FALSE);
}
}
#endif
