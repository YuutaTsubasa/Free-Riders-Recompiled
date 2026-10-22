// The Kinect's menu steadying (kinect_stability.h) on a real sensor, without
// the game: four panels, the colour and depth images twice, the skeleton as
// the SDK gives it above and as the game would be given it below. Keys: M
// switches race and menu (the steadying only acts in menus, and the right
// hand's cursor trail is drawn there); 1 turns the steadying on or off below.
// The sensor opens for one program at a time: close the game first.
#include "kinect_preview.h"
#include "kinect_stability.h"

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
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr COLORREF background = RGB(19, 24, 33), foreground = RGB(221, 228, 238), dim = RGB(150, 164, 182);
constexpr COLORREF tracked_colour = RGB(235, 240, 248), inferred_colour = RGB(255, 205, 119), changed_colour = RGB(70, 207, 236);
constexpr COLORREF off_colour = RGB(255, 150, 130), on_colour = RGB(128, 225, 167);
constexpr COLORREF body_tints[6] = {RGB(70, 207, 236), RGB(255, 176, 87), RGB(128, 225, 167),
                                    RGB(236, 112, 176), RGB(186, 150, 255), RGB(240, 230, 110)};

struct Compare {
    std::unique_ptr<sfr::KinectSensor> sensor;
    std::string failure;
    bool chinese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
    bool menu = true, steady_menu = true;
    sfr::KinectBodySteadying steadying{true};
    sfr::KinectFrame raw;
    std::vector<sfr::KinectBody> steadied;
    sfr::KinectImage colour, depth;
    std::vector<uint32_t> colour_pixels, depth_pixels;
    
    // The right hand's place in the colour image, a second of it, per row.
    std::deque<POINT> trail_raw, trail_steadied;

    std::wstring say(const wchar_t* english, const wchar_t* chinese_words) const { return chinese ? chinese_words : english; }

    void rebuild() {
        steadying = sfr::KinectBodySteadying(steady_menu);
        trail_raw.clear();
        trail_steadied.clear();
    }

    void refresh() {
        if (!sensor) return;
        if (sensor->image(sfr::KinectImageKind::colour, colour) && colour.bytes_per_pixel == 4) {
            colour_pixels.resize(size_t(colour.width) * colour.height);
            std::memcpy(colour_pixels.data(), colour.pixels.data(), colour_pixels.size() * 4);
        }
        if (sensor->image(sfr::KinectImageKind::depth_and_player, depth) && depth.bytes_per_pixel == 2) {
            depth_pixels.resize(size_t(depth.width) * depth.height);
            for (size_t i = 0; i < depth_pixels.size(); ++i) {
                const uint16_t value = uint16_t(depth.pixels[2 * i] | depth.pixels[2 * i + 1] << 8);
                const uint32_t millimetres = value >> 3, player = value & 7;
                const uint32_t shade = millimetres == 0 ? 0 :
                    255 - std::min<uint32_t>(255, uint32_t(std::max(0, int(millimetres) - 500)) * 255 / 3500);
                uint32_t r = shade, g = shade, b = shade;
                if (player) {
                    const COLORREF tint = body_tints[(player - 1) % 6];
                    r = GetRValue(tint) * (shade + 64) / 320;
                    g = GetGValue(tint) * (shade + 64) / 320;
                    b = GetBValue(tint) * (shade + 64) / 320;
                }
                depth_pixels[i] = b | g << 8 | r << 16;
            }
        }
        if (sensor->next(raw)) {
            // As the game does it: the frame steadied as it arrives.
            steadied = raw.bodies;
            steadying.steady(steadied, menu);
        }
    }

    static void text(HDC dc, int x, int y, const std::wstring& words, COLORREF colour = foreground) {
        SetTextColor(dc, colour);
        TextOutW(dc, x, y, words.c_str(), int(words.size()));
    }

    static void image(HDC dc, const RECT& area, const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height) {
        if (pixels.empty() || pixels.size() != size_t(width) * height) {
            HBRUSH brush = CreateSolidBrush(RGB(8, 10, 14));
            FillRect(dc, &area, brush);
            DeleteObject(brush);
            return;
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = int(width);
        info.bmiHeader.biHeight = -int(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, area.left, area.top, area.right - area.left, area.bottom - area.top, 0, 0, int(width), int(height),
                      pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    }

    // One skeleton set over one image. Tracked joints white, inferred ones
    // yellow and smaller; below, the ones steadying moved are blue.
    void skeletons(HDC dc, const RECT& area, bool colour_view, const std::vector<sfr::KinectBody>& bodies, bool below,
                   std::deque<POINT>* trail) {
        const float sx = float(area.right - area.left) / (colour_view ? 640.0f : 320.0f);
        const float sy = float(area.bottom - area.top) / (colour_view ? 480.0f : 240.0f);
        const auto place = [&](const std::array<float, 3>& p) {
            // A sensor that places points in its own images (a v2) first.
            sfr::KinectImagePoint point;
            const auto kind = colour_view ? sfr::KinectImageKind::colour : sfr::KinectImageKind::depth_and_player;
            if (!sensor || !sensor->image_point(kind, p, point))
                point = colour_view ? sfr::kinect_colour_point(p) : sfr::kinect_depth_point(p);
            return POINT{area.left + LONG(point.x * sx), area.top + LONG(point.y * sy)};
        };
        HRGN clip = CreateRectRgn(area.left, area.top, area.right, area.bottom);
        SelectClipRgn(dc, clip);
        const auto old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
        for (size_t b = 0; b < bodies.size(); ++b) {
            const sfr::KinectBody& body = bodies[b];
            const sfr::KinectBody* given = nullptr;
            for (const sfr::KinectBody& r : raw.bodies)
                if (r.tracking_id == body.tracking_id) given = &r;
            const auto moved = [&](uint32_t j) {
                if (!below || !given) return false;
                for (int k = 0; k < 3; ++k)
                    if (std::fabs(body.joints[j][k] - given->joints[j][k]) > 1e-3f) return true;
                return false;
            };
            for (const auto bone : sfr::camera_debug_bones()) {
                if (!body.joint_states[bone.from] || !body.joint_states[bone.to]) continue;
                const COLORREF colour = moved(bone.from) || moved(bone.to) ? changed_colour
                    : body.joint_states[bone.from] < 2 || body.joint_states[bone.to] < 2 ? inferred_colour : tracked_colour;
                HPEN pen = CreatePen(PS_SOLID, 3, colour);
                const auto old_pen = SelectObject(dc, pen);
                const POINT a = place(body.joints[bone.from]), c = place(body.joints[bone.to]);
                MoveToEx(dc, a.x, a.y, nullptr);
                LineTo(dc, c.x, c.y);
                SelectObject(dc, old_pen);
                DeleteObject(pen);
            }
            for (uint32_t j = 0; j < sfr::nui_joint_count; ++j) {
                if (!body.joint_states[j]) continue;
                const COLORREF colour = moved(j) ? changed_colour : body.joint_states[j] < 2 ? inferred_colour : tracked_colour;
                SetDCBrushColor(dc, colour);
                const POINT p = place(body.joints[j]);
                const int r = body.joint_states[j] >= 2 || moved(j) ? 4 : 3;
                SelectObject(dc, GetStockObject(NULL_PEN));
                Ellipse(dc, p.x - r, p.y - r, p.x + r + 1, p.y + r + 1);
            }
            if (trail && b == 0 && menu) {
                trail->push_back(place(body.joints[sfr::nui_joint::hand_right]));
                while (trail->size() > 30) trail->pop_front();
            }
        }
        if (trail && menu && trail->size() > 1) {
            HPEN pen = CreatePen(PS_SOLID, 2, below ? on_colour : off_colour);
            const auto old_pen = SelectObject(dc, pen);
            Polyline(dc, std::vector<POINT>(trail->begin(), trail->end()).data(), int(trail->size()));
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }
        SelectObject(dc, old_brush);
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
    }

    void paint(HDC target, const RECT& rect) {
        const int width = rect.right - rect.left, height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return;
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        const auto old_bitmap = SelectObject(dc, bitmap);
        HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, chinese ? L"Microsoft JhengHei UI" : L"Segoe UI");
        const auto old_font = SelectObject(dc, font);
        HBRUSH brush = CreateSolidBrush(background);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT);

        if (!sensor) {
            text(dc, 20, 20, say(L"No Kinect: ", L"沒有 Kinect：") + std::wstring(failure.begin(), failure.end()), off_colour);
            text(dc, 20, 44, say(L"Close the game and other programs using the Kinect, then start this again.",
                                 L"請先關閉遊戲和其他使用 Kinect 的程式，再重新開啟。"), dim);
        } else {
            wchar_t line[256];
            std::swprintf(line, 256, chinese ? L"模式：%ls（M 切換）   下排：1 游標穩定 %ls%ls"
                                             : L"Mode: %ls (M)   Below: 1 menu steadying %ls%ls",
                          menu ? say(L"menu", L"選單").c_str() : say(L"race", L"比賽").c_str(),
                          steady_menu ? say(L"on", L"開").c_str() : say(L"off", L"關").c_str(),
                          menu ? L"" : say(L"   (it only acts in menus)", L"   （只在選單作用）").c_str());
            text(dc, 20, 12, line);
            text(dc, 20, 34, say(L"Hold the right hand still as a cursor and compare the two trails.",
                                 L"右手舉起當游標停住，比較上下兩條軌跡。"), dim);
            // Two rows of two 4:3 panels.
            const int top = 64, gap = 14, label = 22;
            const int panel_w = std::max(0, std::min((width - 3 * gap) / 2, ((height - top - 30 - 2 * label - gap) / 2) * 4 / 3));
            const int panel_h = panel_w * 3 / 4;
            const int left = (width - 2 * panel_w - gap) / 2;
            for (int row = 0; row < 2; ++row) {
                const int y = top + row * (panel_h + label + gap) + label;
                const bool below = row == 1;
                text(dc, left, y - label, below ? say(L"NEW: menu steadying", L"新技術：游標穩定")
                                                : say(L"BEFORE: the SDK's skeleton as it is", L"未改：SDK 原樣"),
                     below ? on_colour : off_colour);
                for (int col = 0; col < 2; ++col) {
                    const RECT area{left + col * (panel_w + gap), y, left + col * (panel_w + gap) + panel_w, y + panel_h};
                    if (col == 0) image(dc, area, colour_pixels, colour.width, colour.height);
                    else image(dc, area, depth_pixels, depth.width, depth.height);
                    skeletons(dc, area, col == 0, below ? steadied : raw.bodies, below,
                              col == 0 ? (below ? &trail_steadied : &trail_raw) : nullptr);
                }
            }
            text(dc, 20, height - 26, say(L"White tracked, yellow inferred (guessed), blue moved by the steadying. Close the window to stop.",
                                          L"白：追蹤中　黃：SDK 推測（被擋住）　藍：被游標穩定移動的點。關閉視窗即結束。"), dim);
        }
        BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old_font);
        SelectObject(dc, old_bitmap);
        DeleteObject(font);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
};

LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Compare*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Compare*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wp, lp);
    switch (message) {
    case WM_TIMER:
        self->refresh();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == 'M') { self->menu = !self->menu; self->trail_raw.clear(); self->trail_steadied.clear(); }
        else if (wp == '1') { self->steady_menu = !self->steady_menu; self->rebuild(); }
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
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}

int main() {
    Compare compare;
    compare.sensor = sfr::KinectSensor::open(&compare.failure);
    std::cerr << "sfr_kinect_compare sensor=" << (compare.sensor ? compare.sensor->model() : "none") << '\n';
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls{};
    cls.lpfnWndProc = procedure;
    cls.hInstance = instance;
    cls.lpszClassName = L"SfrKinectCompare";
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&cls);
    HWND window = CreateWindowExW(0, cls.lpszClassName,
                                  compare.chinese ? L"Free Riders - Kinect 游標穩定比較" : L"Free Riders - Kinect steadying compared",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 1040, nullptr, nullptr, instance, &compare);
    if (!window) return 1;
    SetTimer(window, 1, 15, nullptr);
    ShowWindow(window, SW_SHOW);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
#else
#include <iostream>
int main() {
    std::cerr << "sfr_kinect_compare needs Windows and a Kinect\n";
    return 1;
}
#endif
