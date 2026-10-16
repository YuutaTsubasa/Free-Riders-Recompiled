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
#include "window_placement.h"

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
#include <utility>
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
    // Over the game's sensor (open_over): borrowed, never opened or closed
    // here, and its skeleton frames come from the game (handed).
    KinectSensor* borrowed = nullptr;
    std::mutex handed_lock;
    KinectFrame handed;
    bool handed_new = false;
    mutable std::mutex lock;
    std::shared_ptr<KinectSensor> sensor;  // under lock
    std::string failure;                   // why no sensor opened
    std::atomic<bool> is_closed{false};
    std::atomic<HWND> shown{nullptr};  // for close()
    std::stop_token stopping;
    KinectFrame frame;
    KinectImage colour, depth;
    std::vector<uint32_t> colour_pixels, depth_pixels;  // BGRX for GDI
    int angle = 0;
    bool has_angle = false;
    std::chrono::steady_clock::time_point angle_read{};
    // When the sensor opened (under lock), and when it last sent a skeleton
    // frame or an image: what kinect_readiness's frames step asks.
    std::chrono::steady_clock::time_point opened_at{}, last_frame{};
    // The tilt motor, turned from the window: a turn at a time, a second
    // or so each (the SDK asks for no more than a turn a second).
    static constexpr int up_id = 101, down_id = 102, calibrate_id = 103, step = 5;
    HWND up_button = nullptr, down_button = nullptr, calibrate_button = nullptr;
    // The calibration (kinect_calibration.h), on this window's thread; its
    // finished result waits under lock for the launcher.
    KinectCalibration calibration;
    bool calibration_saved = false;
    std::optional<KinectCalibrationResult> finished;  // under lock
    bool new_frame = false;
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
        if (calibrate_button) MoveWindow(calibrate_button, rect.right - 3 * (gap + width) - 40, top, width + 40, height, TRUE);
    }

    void calibrate() {
        if (calibration.step() == KinectCalibrationStep::idle || calibration.step() == KinectCalibrationStep::done)
            calibration.start();
        else
            calibration.cancel();
        calibration_saved = false;
        update_calibrate_label();
    }

    void update_calibrate_label() {
        if (!calibrate_button) return;
        const auto now = calibration.step();
        const std::wstring label = now == KinectCalibrationStep::idle ? say(L"Calibrate", L"校正")
                                   : now == KinectCalibrationStep::done ? say(L"Calibrate again", L"重新校正")
                                                                         : say(L"Stop calibrating", L"停止校正");
        SetWindowTextW(calibrate_button, label.c_str());
    }

    // The player calibrated: the first tracked body (the nearest when there
    // are several), as the race takes P1.
    void feed_calibration() {
        if (!new_frame) return;
        new_frame = false;
        const auto before = calibration.step();
        if (before == KinectCalibrationStep::idle || before == KinectCalibrationStep::done) return;
        const KinectBody* player = nullptr;
        for (const KinectBody& body : frame.bodies)
            if (!player || body.position[2] < player->position[2]) player = &body;
        const double seconds = std::chrono::duration<double>(frame.captured_at.time_since_epoch()).count();
        calibration.observe(player, frame.floor_plane, seconds);
        if (calibration.step() == KinectCalibrationStep::done && !calibration_saved) {
            calibration_saved = true;
            std::lock_guard guard(lock);
            finished = calibration.result();
        }
        if (calibration.step() != before) update_calibrate_label();
    }

    // The current step's instruction, progress and live reading, over the top
    // of the window.
    void paint_calibration(HDC dc, int width) {
        const auto now = calibration.step();
        if (now == KinectCalibrationStep::idle) return;
        RECT panel{20, 8, width - 20, 100};
        HBRUSH brush = CreateSolidBrush(RGB(28, 38, 56));
        FillRect(dc, &panel, brush);
        DeleteObject(brush);
        HFONT big = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                chinese ? L"Microsoft JhengHei UI" : L"Segoe UI");
        const auto old = SelectObject(dc, big ? big : GetStockObject(DEFAULT_GUI_FONT));
        const KinectCalibrationResult& r = calibration.result();
        wchar_t line[320];
        std::wstring title, detail;
        switch (now) {
        case KinectCalibrationStep::stand:
            title = say(L"1/5  Stand up straight, arms down, and keep still", L"1/5  站直、雙手放下，保持不動");
            break;
        case KinectCalibrationStep::crouch:
            title = say(L"2/5  Crouch as you would in a race, then stand up", L"2/5  像比賽時那樣蹲下，再站起來");
            std::swprintf(line, 320, chinese ? L"現在低了 %.0f 公分" : L"Now %.0f cm lower", std::max(0.0f, calibration.drop()) * 100);
            detail = line;
            break;
        case KinectCalibrationStep::jump:
            title = say(L"3/5  Jump (both feet off the floor)", L"3/5  跳一下（雙腳離地）");
            std::swprintf(line, 320, chinese ? L"蹲下 %.0f 公分　現在高了 %.0f 公分" : L"Crouch %.0f cm   now %.0f cm higher",
                          r.crouch_drop * 100, std::max(0.0f, -calibration.drop()) * 100);
            detail = line;
            break;
        case KinectCalibrationStep::lean_left:
        case KinectCalibrationStep::lean_right:
            title = now == KinectCalibrationStep::lean_left
                        ? say(L"4/5  Lean your body to your left as far as you steer, then stand straight",
                              L"4/5  身體往左傾（像轉彎那樣），再站直")
                        : say(L"5/5  Now lean to your right, then stand straight", L"5/5  再往右傾，然後站直");
            std::swprintf(line, 320, chinese ? L"現在傾斜 %.0f°（遊戲在 30° 時轉到底）" : L"Leaning %.0f° (the race steers fully at 30°)",
                          std::fabs(calibration.lean()));
            detail = line;
            break;
        case KinectCalibrationStep::done:
            std::swprintf(line, 320, chinese ? L"校正完成：蹲下深度設為 %u 公分" : L"Calibrated: crouch depth set to %u cm", r.crouch_cm);
            title = line;
            std::swprintf(line, 320,
                          chinese ? L"蹲下 %.0f 公分　跳躍 %ls　左傾 %.0f°、右傾 %.0f°（30° 轉到底）"
                                  : L"Crouch %.0f cm   jump %ls   lean %.0f° left, %.0f° right (30° steers fully)",
                          r.crouch_drop * 100,
                          r.jump_rise > 0 ? (std::to_wstring(int(std::lround(r.jump_rise * 100))) + say(L" cm", L" 公分")).c_str()
                                          : say(L"not seen", L"沒有偵測到").c_str(),
                          r.lean_left, r.lean_right);
            detail = line;
            break;
        default: break;
        }
        text(dc, 32, 16, title, now == KinectCalibrationStep::done ? RGB(128, 225, 167) : foreground);
        SelectObject(dc, old);
        if (big) DeleteObject(big);
        if (!calibration.tracked() && now != KinectCalibrationStep::done)
            detail = say(L"Nobody in view: stand 1.5 to 3.5 m from the sensor, facing it", L"看不到你：請面向感應器，站在 1.5 到 3.5 公尺處");
        else if (calibration.waiting_long() && now != KinectCalibrationStep::done)
            detail += say(L"   (not seen yet: try it again, a little more)", L"　（還沒偵測到：請再做一次，動作大一點）");
        text(dc, 32, 52, detail, now == KinectCalibrationStep::done ? foreground : RGB(255, 205, 119));
        if (now != KinectCalibrationStep::done) {
            RECT bar{32, 80, 32 + int((width - 104) * calibration.progress()), 90};
            HBRUSH fill = CreateSolidBrush(RGB(70, 207, 236));
            FillRect(dc, &bar, fill);
            DeleteObject(fill);
        }
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
        const auto now = std::chrono::steady_clock::now();
        if (borrowed) {
            std::lock_guard guard(handed_lock);
            if (handed_new) {
                frame = handed;
                handed_new = false;
                last_frame = now;
            }
        } else if (source->next(frame)) {
            last_frame = now;
            new_frame = true;
        }
        const bool new_colour = source->image(KinectImageKind::colour, colour);
        if (new_colour) last_frame = now;
        if (new_colour && colour.bytes_per_pixel == 4) {
            // B, G, R, unused: GDI's own order.
            colour_pixels.resize(size_t(colour.width) * colour.height);
            std::memcpy(colour_pixels.data(), colour.pixels.data(), colour_pixels.size() * 4);
        }
        const bool new_depth = source->image(KinectImageKind::depth_and_player, depth);
        if (new_depth) last_frame = now;
        if (new_depth && depth.bytes_per_pixel == 2) {
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
        std::chrono::steady_clock::time_point since{};
        { std::lock_guard guard(lock); source = sensor; why = failure; since = opened_at; }
        const auto now = std::chrono::steady_clock::now();
        const auto seconds = [&](std::chrono::steady_clock::time_point from) {
            return std::chrono::duration<double>(now - from).count();
        };
        KinectReadinessInput input;
        input.opened = source != nullptr;
        input.failure = why;
        input.seconds_open = source ? seconds(since) : 0;
        input.seconds_since_frame = last_frame == std::chrono::steady_clock::time_point{} ? -1 : seconds(last_frame);
        input.bodies = frame.bodies.size();
        const KinectReadiness readiness = kinect_readiness(input);
        wchar_t line[256];
        if (!readiness.ready()) {
            // Not playable yet: each step with its state, then what to do
            // about the first one that failed.
            const wchar_t* const names[] = {L"Runtime (SDK)", L"執行環境（SDK）", L"Sensor", L"感應器",
                                            L"Frames", L"資料傳送", L"Body", L"人體追蹤"};
            int x = 20;
            for (size_t step = 0; step < size_t(KinectStep::count); ++step) {
                const KinectStepState state = readiness.steps[step];
                const wchar_t* const mark = state == KinectStepState::passed ? L"\u2713 " :
                                            state == KinectStepState::failed ? L"\u2717 " : L"\u2026 ";
                const std::wstring item = mark + say(names[2 * step], names[2 * step + 1]);
                text(dc, x, 16, item, state == KinectStepState::passed ? RGB(128, 225, 167) :
                                      state == KinectStepState::failed ? RGB(255, 150, 130) : dim);
                SIZE size{};
                GetTextExtentPoint32W(dc, item.c_str(), int(item.size()), &size);
                x += size.cx + 24;
            }
            std::wstring hint;
            switch (readiness.first_failed) {
            case KinectStep::runtime:
                hint = say(L"Install Kinect for Windows SDK 1.8 (Kinect for Xbox 360) or SDK 2.0 (Kinect v2), then open the preview again.",
                           L"請安裝 Kinect for Windows SDK 1.8（Xbox 360 Kinect）或 SDK 2.0（Kinect v2），再重新開啟預覽。");
                break;
            case KinectStep::sensor:
                hint = say(L"Check the Kinect's power adapter and USB cable, close other programs using it, then open the preview again",
                           L"請檢查 Kinect 的電源變壓器和 USB 線，關閉其他正在使用 Kinect 的程式，再重新開啟預覽") +
                       L" (" + std::wstring(why.begin(), why.end()) + L")";
                break;
            case KinectStep::frames:
                hint = say(L"The Kinect opened but sends nothing: close other programs using it, or plug its USB and power in again.",
                           L"Kinect 已開啟但沒有傳來資料：請關閉其他使用 Kinect 的程式，或重新插拔 USB 和電源。");
                break;
            case KinectStep::body:
                hint = say(L"Nobody tracked: stand 1.5 to 3.5 m from the sensor, facing it, with your whole body in view.",
                           L"沒有追蹤到人：請面向感應器，站在 1.5 到 3.5 公尺處，讓全身入鏡。");
                break;
            case KinectStep::count:
                hint = source ? say(L"Waiting for the Kinect's first frame...", L"正在等待 Kinect 傳來第一筆資料…")
                              : say(L"Opening the Kinect...", L"正在開啟 Kinect…");
                break;
            }
            text(dc, 20, 40, hint, readiness.first_failed == KinectStep::count ? foreground : RGB(255, 205, 119));
        }
        const bool calibrating = calibration.step() != KinectCalibrationStep::idle;
        if (source && readiness.ready() && !calibrating) {
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
        paint_calibration(dc, width);
        text(dc, gap, height - 28, borrowed ? say(L"Close this window to go back to the game.", L"關閉此視窗即可回到遊戲。")
                                            : say(L"Close this window before starting the game: the Kinect opens for one program at a time.",
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
                self->feed_calibration();
                const bool ready = !self->turning.load() && self->sensor_ready();
                EnableWindow(self->up_button, ready);
                EnableWindow(self->down_button, ready);
                if (self->calibrate_button) EnableWindow(self->calibrate_button, self->sensor_ready());
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == up_id) self->turn(step);
            else if (LOWORD(wp) == down_id) self->turn(-step);
            else if (LOWORD(wp) == calibrate_id) self->calibrate();
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
        case WM_CLOSE:
            // Back to the game it was opened over, which has the keyboard again.
            if (const HWND game = sfr::find_game_window()) SetForegroundWindow(game);
            DestroyWindow(window);
            return 0;
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
                                      borrowed ? (chinese ? L"Free Riders - Kinect 調整" : L"Free Riders - Kinect adjustment")
                                               : (chinese ? L"Free Riders - Kinect 預覽" : L"Free Riders - Kinect preview"),
                                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1360, 640, nullptr, nullptr,
                                      instance, this);
        if (!window) { is_closed.store(true); return; }
        shown.store(window);
        up_button = CreateWindowExW(0, L"BUTTON", chinese ? L"往上" : L"Up", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(up_id)), instance, nullptr);
        down_button = CreateWindowExW(0, L"BUTTON", chinese ? L"往下" : L"Down", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
                                      0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(down_id)), instance, nullptr);
        button_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                  chinese ? L"Microsoft JhengHei UI" : L"Segoe UI");
        // Calibrating stores the launcher's crouch depth, so only the
        // launcher's preview has it (not the one over the game's sensor).
        if (!borrowed) {
            calibrate_button = CreateWindowExW(0, L"BUTTON", say(L"Calibrate", L"校正").c_str(),
                                               WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON, 0, 0, 0, 0, window,
                                               reinterpret_cast<HMENU>(INT_PTR(calibrate_id)), instance, nullptr);
        }
        for (HWND button : {up_button, down_button, calibrate_button})
            if (button && button_font) SendMessageW(button, WM_SETFONT, WPARAM(button_font), TRUE);
        place_buttons(window);
        if (borrowed) sfr::center_over_game(window);  // over the game it adjusts
        ShowWindow(window, SW_SHOW);
        if (borrowed) sfr::bring_to_front(window);  // in front of the game, with the keyboard
        // The sensor opens once the window shows, so the window says so
        // meanwhile; a second or so for a Kinect v1.
        std::jthread opener([this](std::stop_token) {
            std::string why;
            // A borrowed sensor is the game's: shared without an owner, so
            // closing the window leaves it open.
            std::shared_ptr<KinectSensor> opened = borrowed ? std::shared_ptr<KinectSensor>(borrowed, [](KinectSensor*) {})
                                                            : KinectSensor::open(&why);
            std::lock_guard guard(lock);
            sensor = std::move(opened);
            failure = sensor ? "" : why;
            opened_at = std::chrono::steady_clock::now();
        });
        if (!SetTimer(window, 1, 33, nullptr)) { DestroyWindow(window); return; }
        MSG message;
        while (!stop.stop_requested() && GetMessageW(&message, nullptr, 0, 0) > 0) {
            // Esc closes it from anywhere in it, a slider or button included.
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
                PostMessageW(window, WM_CLOSE, 0, 0);
                continue;
            }
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

std::unique_ptr<KinectPreviewWindow> KinectPreviewWindow::open_over(KinectSensor& sensor) {
    try {
        auto impl = std::make_unique<Impl>();
        impl->chinese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
        impl->borrowed = &sensor;
        auto* raw = impl.get();
        impl->worker = std::jthread([raw](std::stop_token stop) { raw->run(stop); });
        return std::unique_ptr<KinectPreviewWindow>(new KinectPreviewWindow(std::move(impl)));
    } catch (const std::exception& error) {
        std::cerr << "NATIVE_KINECT_PREVIEW unavailable: " << error.what() << '\n';
        return nullptr;
    }
}

void KinectPreviewWindow::show_frame(const KinectFrame& frame) {
    std::lock_guard guard(impl_->handed_lock);
    impl_->handed = frame;
    impl_->handed_new = true;
}

bool KinectPreviewWindow::closed() const { return impl_->is_closed.load(); }
void KinectPreviewWindow::close() {
    if (const HWND window = impl_->shown.load(); window && !closed()) PostMessageW(window, WM_CLOSE, 0, 0);
}
void KinectPreviewWindow::raise() {
    if (const HWND window = impl_->shown.load(); window && !closed()) sfr::bring_to_front(window);
}

std::shared_ptr<KinectSensor> KinectPreviewWindow::sensor() const {
    std::lock_guard guard(impl_->lock);
    return impl_->sensor;
}

std::string KinectPreviewWindow::failure() const {
    std::lock_guard guard(impl_->lock);
    return impl_->failure;
}
std::optional<KinectCalibrationResult> KinectPreviewWindow::take_calibration() {
    std::lock_guard guard(impl_->lock);
    return std::exchange(impl_->finished, std::nullopt);
}
}
#else
namespace sfr {
struct KinectPreviewWindow::Impl {};
KinectPreviewWindow::KinectPreviewWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
KinectPreviewWindow::~KinectPreviewWindow() = default;
std::unique_ptr<KinectPreviewWindow> KinectPreviewWindow::open(bool) { return nullptr; }
std::unique_ptr<KinectPreviewWindow> KinectPreviewWindow::open_over(KinectSensor&) { return nullptr; }
void KinectPreviewWindow::show_frame(const KinectFrame&) {}
bool KinectPreviewWindow::closed() const { return true; }
void KinectPreviewWindow::close() {}
void KinectPreviewWindow::raise() {}
std::shared_ptr<KinectSensor> KinectPreviewWindow::sensor() const { return nullptr; }
std::string KinectPreviewWindow::failure() const { return {}; }
std::optional<KinectCalibrationResult> KinectPreviewWindow::take_calibration() { return std::nullopt; }
}
#endif
