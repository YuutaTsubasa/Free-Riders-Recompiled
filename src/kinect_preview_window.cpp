#include "kinect_preview.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "camera_debug.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sfr {
namespace {
constexpr COLORREF background = RGB(19, 24, 33), foreground = RGB(221, 228, 238), dim = RGB(150, 164, 182);
// One colour a body, by the sensor's skeleton slot (the depth image's
// player index minus one).
constexpr COLORREF body_colours[6] = {RGB(70, 207, 236), RGB(255, 176, 87), RGB(128, 225, 167),
                                      RGB(236, 112, 176), RGB(186, 150, 255), RGB(240, 230, 110)};

void text(HDC dc, int x, int y, const std::wstring& words, COLORREF colour = foreground) {
    SetTextColor(dc, colour);
    TextOutW(dc, x, y, words.c_str(), int(words.size()));
}

void draw_image(HDC dc, const RECT& area, const std::vector<uint32_t>& pixels, int width, int height) {
    if (pixels.size() != size_t(width) * size_t(height)) return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, area.left, area.top, area.right - area.left, area.bottom - area.top, 0, 0, width, height,
                  pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
}
}

struct KinectPreviewWindow::Impl {
    bool chinese = false;
    mutable std::mutex lock;
    std::shared_ptr<KinectSensor> sensor;  // under lock
    std::string failure;                   // why no sensor opened
    std::atomic<bool> is_closed{false};
    std::stop_token stopping;
    KinectFrame frame;
    KinectImage colour, depth;
    std::vector<uint32_t> colour_pixels, depth_pixels;  // BGRX for GDI
    int angle = 0;
    bool has_angle = false;
    std::chrono::steady_clock::time_point angle_read{};
    // The tilt motor, turned from the window: a turn at a time, a second
    // or so each (the SDK asks for no more than a turn a second).
    static constexpr int up_id = 101, down_id = 102, step = 5;
    HWND up_button = nullptr, down_button = nullptr;
    HFONT button_font = nullptr;
    std::atomic<bool> turning{false}, reread_angle{false};
    std::jthread turner;
    // Last member: joins before everything above is destroyed.
    std::jthread worker;

    void turn(int degrees) {
        std::shared_ptr<KinectSensor> source;
        { std::lock_guard guard(lock); source = sensor; }
        if (!source || turning.exchange(true)) return;
        EnableWindow(up_button, FALSE);
        EnableWindow(down_button, FALSE);
        turner = std::jthread([this, source, degrees] {
            int now = 0;
            if (source->elevation(now)) source->set_elevation(now + degrees);
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            reread_angle.store(true);
            turning.store(false);
        });
    }

    void place_buttons(HWND window) {
        RECT rect;
        GetClientRect(window, &rect);
        const int width = 96, height = 32, gap = 10, top = 12;
        MoveWindow(down_button, rect.right - gap - width, top, width, height, TRUE);
        MoveWindow(up_button, rect.right - 2 * (gap + width), top, width, height, TRUE);
    }

    bool sensor_ready() const {
        std::lock_guard guard(lock);
        return sensor != nullptr;
    }

    std::wstring say(const wchar_t* english, const wchar_t* chinese_words) const {
        return chinese ? chinese_words : english;
    }

    void refresh() {
        std::shared_ptr<KinectSensor> source;
        { std::lock_guard guard(lock); source = sensor; }
        if (!source) return;
        source->next(frame);
        if (source->image(KinectImageKind::colour, colour) && colour.bytes_per_pixel == 4) {
            // B, G, R, unused: GDI's own order.
            colour_pixels.resize(size_t(colour.width) * colour.height);
            std::memcpy(colour_pixels.data(), colour.pixels.data(), colour_pixels.size() * 4);
        }
        if (source->image(KinectImageKind::depth_and_player, depth) && depth.bytes_per_pixel == 2) {
            depth_pixels.resize(size_t(depth.width) * depth.height);
            for (size_t i = 0; i < depth_pixels.size(); ++i) {
                const uint16_t value = uint16_t(depth.pixels[2 * i] | depth.pixels[2 * i + 1] << 8);
                const uint32_t millimetres = value >> 3, player = value & 7;
                // Near is bright; nothing measured is black.
                const uint32_t shade = millimetres == 0 ? 0 :
                    255 - std::min<uint32_t>(255, uint32_t(std::max(0, int(millimetres) - 500)) * 255 / 3500);
                uint32_t r = shade, g = shade, b = shade;
                if (player) {
                    const COLORREF tint = body_colours[(player - 1) % 6];
                    r = GetRValue(tint) * (shade + 64) / 320;
                    g = GetGValue(tint) * (shade + 64) / 320;
                    b = GetBValue(tint) * (shade + 64) / 320;
                }
                depth_pixels[i] = b | g << 8 | r << 16;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (reread_angle.exchange(false) || now - angle_read > std::chrono::seconds(1)) {
            has_angle = source->elevation(angle);
            angle_read = now;
        }
    }

    void skeletons(HDC dc, const RECT& area, bool colour_view) {
        const float sx = float(area.right - area.left) / (colour_view ? 640.0f : 320.0f);
        const float sy = float(area.bottom - area.top) / (colour_view ? 480.0f : 240.0f);
        const auto place = [&](const std::array<float, 3>& p) {
            const KinectImagePoint point = colour_view ? kinect_colour_point(p) : kinect_depth_point(p);
            return POINT{area.left + LONG(point.x * sx), area.top + LONG(point.y * sy)};
        };
        const auto old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
        for (const KinectBody& body : frame.bodies) {
            const COLORREF colour = body_colours[body.sensor_index % 6];
            HPEN pen = CreatePen(PS_SOLID, 3, colour);
            const auto old_pen = SelectObject(dc, pen);
            SetDCBrushColor(dc, colour);
            for (const auto bone : camera_debug_bones()) {
                if (!body.joint_states[bone.from] || !body.joint_states[bone.to]) continue;
                const POINT a = place(body.joints[bone.from]), b = place(body.joints[bone.to]);
                MoveToEx(dc, a.x, a.y, nullptr);
                LineTo(dc, b.x, b.y);
            }
            for (uint32_t j = 0; j < nui_joint_count; ++j) {
                if (!body.joint_states[j]) continue;
                const POINT p = place(body.joints[j]);
                const int r = body.joint_states[j] >= 2 ? 4 : 2;  // inferred joints smaller
                Ellipse(dc, p.x - r, p.y - r, p.x + r + 1, p.y + r + 1);
            }
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }
        SelectObject(dc, old_brush);
    }

    void paint(HDC target, const RECT& rect) {
        const int width = rect.right - rect.left, height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return;
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        if (!dc || !bitmap) { if (dc) DeleteDC(dc); if (bitmap) DeleteObject(bitmap); return; }
        const auto old_bitmap = SelectObject(dc, bitmap);
        HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                 chinese ? L"Microsoft JhengHei UI" : L"Segoe UI");
        const auto old_font = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
        HBRUSH brush = CreateSolidBrush(background);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT);

        std::shared_ptr<KinectSensor> source;
        std::string why;
        { std::lock_guard guard(lock); source = sensor; why = failure; }
        wchar_t line[256];
        if (!source) {
            text(dc, 20, 16, why.empty() ? say(L"Opening the Kinect...", L"正在開啟 Kinect…")
                                         : say(L"No Kinect: ", L"找不到 Kinect：") + std::wstring(why.begin(), why.end()),
                 why.empty() ? foreground : RGB(255, 150, 130));
        } else {
            const std::string name = source->model();
            const std::wstring model_name(name.begin(), name.end());
            if (has_angle)
                std::swprintf(line, 256, chinese ? L"%ls  追蹤到 %zu 人  感應器角度 %d°" : L"%ls  %zu tracked  Sensor angle %d°",
                              model_name.c_str(), frame.bodies.size(), angle);
            else
                std::swprintf(line, 256, chinese ? L"%ls  追蹤到 %zu 人" : L"%ls  %zu tracked", model_name.c_str(),
                              frame.bodies.size());
            std::wstring model(line);
            text(dc, 20, 16, model);
            // Per body: its id, distance, and whether its feet are really seen
            // (a foot the sensor only guesses is out of view).
            int y = 40;
            for (const KinectBody& body : frame.bodies) {
                const bool feet = body.joint_states[nui_joint::foot_left] >= 2 && body.joint_states[nui_joint::foot_right] >= 2;
                std::swprintf(line, 256, chinese ? L"編號 %u  位置 %u  距離 %.2f m  %ls" : L"id %u  slot %u  %.2f m away  %ls",
                              body.tracking_id, body.sensor_index, body.position[2],
                              feet ? say(L"feet in view", L"腳在畫面內").c_str()
                                   : say(L"feet out of view: tilt down or step back", L"腳不在畫面內：角度往下或往後站").c_str());
                text(dc, 20, y, line, feet ? body_colours[body.sensor_index % 6] : RGB(255, 205, 119));
                y += 20;
            }
        }
        const std::wstring keys = say(L"Tilt: buttons or Up / Down keys, 5 degrees a turn", L"調整角度：按鈕或鍵盤 ↑ ↓，每次 5 度");
        SIZE extent{};
        GetTextExtentPoint32W(dc, keys.c_str(), int(keys.size()), &extent);
        text(dc, width - 20 - extent.cx, 52, turning.load() ? say(L"Turning...", L"轉動中…") : keys, dim);
        // The two cameras side by side, 4:3 each, under the text.
        const int top = 110, gap = 20;
        const int panel_width = std::max(0, (width - 3 * gap) / 2);
        const int panel_height = std::min(std::max(0, height - top - 40), panel_width * 3 / 4);
        const RECT colour_area{gap, top, gap + panel_width, top + panel_height};
        const RECT depth_area{2 * gap + panel_width, top, 2 * gap + 2 * panel_width, top + panel_height};
        text(dc, colour_area.left, top - 22, say(L"COLOUR (skeleton roughly aligned)", L"彩色影像（骨架為近似對齊）"), dim);
        text(dc, depth_area.left, top - 22, say(L"DEPTH (players tinted)", L"深度影像（玩家上色）"), dim);
        draw_image(dc, colour_area, colour_pixels, int(colour.width), int(colour.height));
        draw_image(dc, depth_area, depth_pixels, int(depth.width), int(depth.height));
        skeletons(dc, colour_area, true);
        skeletons(dc, depth_area, false);
        text(dc, gap, height - 28, say(L"Close this window before starting the game: the Kinect opens for one program at a time.",
                                       L"開始遊戲前會自動關閉此視窗：Kinect 一次只能給一個程式使用。"), dim);
        BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old_font);
        SelectObject(dc, old_bitmap);
        if (font) DeleteObject(font);
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
                const bool ready = !self->turning.load() && self->sensor_ready();
                EnableWindow(self->up_button, ready);
                EnableWindow(self->down_button, ready);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == up_id) self->turn(step);
            else if (LOWORD(wp) == down_id) self->turn(-step);
            SetFocus(window);  // keep the arrow keys for the window
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_UP) self->turn(step);
            else if (wp == VK_DOWN) self->turn(-step);
            return 0;
        case WM_SIZE:
            self->place_buttons(window);
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
            limits->ptMinTrackSize = {760, 420};
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
        cls.lpszClassName = L"SfrKinectPreview";
        cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { is_closed.store(true); return; }
        HWND window = CreateWindowExW(0, cls.lpszClassName,
                                      chinese ? L"Free Riders - Kinect 預覽" : L"Free Riders - Kinect preview",
                                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1360, 640, nullptr, nullptr,
                                      instance, this);
        if (!window) { is_closed.store(true); return; }
        up_button = CreateWindowExW(0, L"BUTTON", chinese ? L"往上" : L"Up", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(up_id)), instance, nullptr);
        down_button = CreateWindowExW(0, L"BUTTON", chinese ? L"往下" : L"Down", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
                                      0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(down_id)), instance, nullptr);
        button_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                  chinese ? L"Microsoft JhengHei UI" : L"Segoe UI");
        for (HWND button : {up_button, down_button})
            if (button && button_font) SendMessageW(button, WM_SETFONT, WPARAM(button_font), TRUE);
        place_buttons(window);
        ShowWindow(window, SW_SHOW);
        // The sensor opens once the window shows, so the window says so
        // meanwhile; a second or so for a Kinect v1.
        std::jthread opener([this](std::stop_token) {
            std::string why;
            std::shared_ptr<KinectSensor> opened = KinectSensor::open(&why);
            std::lock_guard guard(lock);
            sensor = std::move(opened);
            failure = sensor ? "" : why;
        });
        if (!SetTimer(window, 1, 33, nullptr)) { DestroyWindow(window); return; }
        MSG message;
        while (!stop.stop_requested() && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(window)) DestroyWindow(window);
        if (button_font) DeleteObject(button_font);
        opener.join();
        // The sensor closes now unless a tilt is still using it.
        { std::lock_guard guard(lock); sensor.reset(); }
        is_closed.store(true);
    }
};

KinectPreviewWindow::KinectPreviewWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
KinectPreviewWindow::~KinectPreviewWindow() = default;

std::unique_ptr<KinectPreviewWindow> KinectPreviewWindow::open(bool chinese) {
    try {
        auto impl = std::make_unique<Impl>();
        impl->chinese = chinese;
        auto* raw = impl.get();
        impl->worker = std::jthread([raw](std::stop_token stop) { raw->run(stop); });
        return std::unique_ptr<KinectPreviewWindow>(new KinectPreviewWindow(std::move(impl)));
    } catch (const std::exception& error) {
        std::cerr << "NATIVE_KINECT_PREVIEW unavailable: " << error.what() << '\n';
        return nullptr;
    }
}

bool KinectPreviewWindow::closed() const { return impl_->is_closed.load(); }

std::shared_ptr<KinectSensor> KinectPreviewWindow::sensor() const {
    std::lock_guard guard(impl_->lock);
    return impl_->sensor;
}

std::string KinectPreviewWindow::failure() const {
    std::lock_guard guard(impl_->lock);
    return impl_->failure;
}
}
#else
namespace sfr {
struct KinectPreviewWindow::Impl {};
KinectPreviewWindow::KinectPreviewWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
KinectPreviewWindow::~KinectPreviewWindow() = default;
std::unique_ptr<KinectPreviewWindow> KinectPreviewWindow::open(bool) { return nullptr; }
bool KinectPreviewWindow::closed() const { return true; }
std::shared_ptr<KinectSensor> KinectPreviewWindow::sensor() const { return nullptr; }
std::string KinectPreviewWindow::failure() const { return {}; }
}
#endif
