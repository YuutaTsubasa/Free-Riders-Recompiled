#include "camera_preview_window.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "camera_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sfr {
namespace {
constexpr COLORREF background = RGB(19, 24, 33), foreground = RGB(221, 228, 238), dim = RGB(150, 164, 182);

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
    return out;
}
}  // namespace

struct CameraPreviewWindow::Impl {
    std::string device;
    bool chinese = false;
    std::atomic<bool> is_closed{false};
    std::atomic<HWND> shown{nullptr};
    std::stop_token stopping;
    // Opened on its own thread (a camera can take a second or two).
    std::mutex lock;
    std::unique_ptr<CameraCapture> camera;  // under lock
    bool opened = false;                    // under lock: the attempt is over
    CameraFrame frame;                      // the window's thread only
    std::chrono::steady_clock::time_point opened_at{}, rate_since{};
    uint64_t rate_first = 0;
    double rate = 0;
    std::jthread worker;  // last: joins before the rest goes

    void refresh() {
        std::lock_guard guard(lock);
        if (!camera || !camera->next(frame)) return;
        const auto now = std::chrono::steady_clock::now();
        if (!rate_first) {
            rate_first = frame.number;
            rate_since = now;
        } else if (const double seconds = std::chrono::duration<double>(now - rate_since).count(); seconds >= 1.0) {
            rate = double(frame.number - rate_first) / seconds;
            rate_first = frame.number;
            rate_since = now;
        }
    }

    void paint(HDC target, const RECT& rect) {
        const int width = rect.right - rect.left, height = rect.bottom - rect.top;
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        if (!dc || !bitmap) {
            if (dc) DeleteDC(dc);
            if (bitmap) DeleteObject(bitmap);
            return;
        }
        const auto old_bitmap = SelectObject(dc, bitmap);
        HBRUSH brush = CreateSolidBrush(background);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
        HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                 chinese ? L"Microsoft JhengHei UI" : L"Segoe UI");
        const auto old_font = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc, TRANSPARENT);
        const auto say = [&](int y, const std::wstring& words, COLORREF colour) {
            SetTextColor(dc, colour);
            TextOutW(dc, 12, y, words.c_str(), int(words.size()));
        };
        bool is_opened = false, has_camera = false;
        {
            std::lock_guard guard(lock);
            is_opened = opened;
            has_camera = camera != nullptr;
        }
        const std::wstring name = wide(device);
        if (frame.number && frame.bgra.size() == size_t(frame.width) * frame.height * 4) {
            // The whole picture, as large as fits under the status line.
            const int top = 36, room_w = width, room_h = (std::max)(1, height - top);
            const double scale = (std::min)(double(room_w) / frame.width, double(room_h) / frame.height);
            const int w = int(frame.width * scale), h = int(frame.height * scale);
            const int x = (room_w - w) / 2, y = top + (room_h - h) / 2;
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = int(frame.width);
            info.bmiHeader.biHeight = -int(frame.height);  // top-down rows
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, x, y, w, h, 0, 0, int(frame.width), int(frame.height), frame.bgra.data(), &info,
                          DIB_RGB_COLORS, SRCCOPY);
            wchar_t line[256];
            std::swprintf(line, 256, chinese ? L"%ls　%u×%u　%.1f fps　第 %llu 張" : L"%ls   %u×%u   %.1f fps   picture %llu",
                          name.c_str(), frame.width, frame.height, rate, static_cast<unsigned long long>(frame.number));
            say(8, line, foreground);
        } else if (!is_opened) {
            say(8, (chinese ? L"正在開啟攝影機：" : L"Opening the camera: ") + name, foreground);
        } else if (!has_camera) {
            say(8, (chinese ? L"攝影機打不開：" : L"The camera will not open: ") + name, foreground);
            say(36, chinese ? L"可能被其他程式使用中（視訊會議、相機 App、另一個預覽），或未允許存取"
                            : L"Another program may be using it (a video call, the Camera app, another preview),",
                dim);
            say(60, chinese ? L"（Windows 設定 > 隱私權與安全性 > 相機，允許桌面應用程式存取相機）。"
                            : L"or access is not allowed (Windows Settings > Privacy & security > Camera, desktop apps).",
                dim);
        } else {
            const double waited =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - opened_at).count();
            wchar_t line[256];
            std::swprintf(line, 256, chinese ? L"%ls 已開啟，等待畫面中（%.0f 秒）" : L"%ls is open; waiting for a picture (%.0f s)",
                          name.c_str(), waited);
            say(8, line, foreground);
            if (waited > 3)
                say(36, chinese ? L"開得起來卻沒有畫面：鏡頭可能有實體開關或遮蓋，或驅動程式沒有送出影像。"
                                : L"It opens but sends nothing: check a privacy shutter or switch, or the camera's driver.",
                    dim);
        }
        BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old_font);
        if (font) DeleteObject(font);
        SelectObject(dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        switch (message) {
        case WM_TIMER:
            if (self->stopping.stop_requested()) DestroyWindow(window);
            else {
                self->refresh();
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window, &ps);
            RECT rect;
            GetClientRect(window, &rect);
            self->paint(dc, rect);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
            limits->ptMinTrackSize = {480, 360};
            return 0;
        }
        case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_DESTROY: self->is_closed.store(true); PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    void run(std::stop_token stop) {
        stopping = stop;
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW cls{};
        cls.lpfnWndProc = procedure;
        cls.hInstance = instance;
        cls.lpszClassName = L"SfrCameraPreview";
        cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { is_closed.store(true); return; }
        HWND window = CreateWindowExW(0, cls.lpszClassName,
                                      chinese ? L"Free Riders - 攝影機畫面" : L"Free Riders - Camera picture",
                                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 860, 560, nullptr, nullptr,
                                      instance, this);
        if (!window) { is_closed.store(true); return; }
        shown.store(window);
        ShowWindow(window, SW_SHOW);
        std::jthread opener([this](std::stop_token) {
            auto made = CameraCapture::open(640, 480, device);
            std::lock_guard guard(lock);
            camera = std::move(made);
            opened = true;
            opened_at = std::chrono::steady_clock::now();
        });
        if (!SetTimer(window, 1, 33, nullptr)) DestroyWindow(window);
        MSG message;
        while (!stop.stop_requested() && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(window)) DestroyWindow(window);
        opener.join();
        { std::lock_guard guard(lock); camera.reset(); }  // the camera is free again
        is_closed.store(true);
    }
};

CameraPreviewWindow::CameraPreviewWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

CameraPreviewWindow::~CameraPreviewWindow() {
    if (const HWND window = impl_->shown.load(); window && !impl_->is_closed.load()) PostMessageW(window, WM_CLOSE, 0, 0);
}

std::unique_ptr<CameraPreviewWindow> CameraPreviewWindow::open(const std::string& device, bool chinese) {
    try {
        auto impl = std::make_unique<Impl>();
        impl->device = device;
        impl->chinese = chinese;
        auto* raw = impl.get();
        impl->worker = std::jthread([raw](std::stop_token stop) { raw->run(stop); });
        return std::unique_ptr<CameraPreviewWindow>(new CameraPreviewWindow(std::move(impl)));
    } catch (const std::exception& error) {
        std::cerr << "NATIVE_CAMERA_PREVIEW unavailable: " << error.what() << '\n';
        return nullptr;
    }
}

bool CameraPreviewWindow::closed() const { return impl_->is_closed.load(); }

}  // namespace sfr
#else
namespace sfr {
struct CameraPreviewWindow::Impl {};
CameraPreviewWindow::CameraPreviewWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CameraPreviewWindow::~CameraPreviewWindow() = default;
std::unique_ptr<CameraPreviewWindow> CameraPreviewWindow::open(const std::string&, bool) { return nullptr; }
bool CameraPreviewWindow::closed() const { return true; }
}  // namespace sfr
#endif
