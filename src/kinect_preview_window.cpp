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
#include "kinect_preview_layout.h"
#include "kinect_v2_depth.h"
#include "kinect_v2_image.h"
#include "window_placement.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
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

// right: the x the text may not pass (0 for no limit), so a long line never
// runs under the buttons, the hint or the next caption.
void text(HDC dc, int x, int y, const std::wstring& words, COLORREF colour = foreground, int right = 0) {
    SetTextColor(dc, colour);
    if (right > 0) {
        if (right <= x) return;
        const RECT clip{x, y, right, y + 24};
        ExtTextOutW(dc, x, y, ETO_CLIPPED, &clip, words.c_str(), UINT(words.size()), nullptr);
    } else {
        TextOutW(dc, x, y, words.c_str(), int(words.size()));
    }
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

// A depth and player index image in grey, near bright, each player tinted.
void shade_depth(const KinectImage& depth, std::vector<uint32_t>& out) {
    out.resize(size_t(depth.width) * depth.height);
    for (size_t i = 0; i < out.size(); ++i) {
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
        out[i] = b | g << 8 | r << 16;
    }
}

// The diagram's colours: the real v2 blue, the virtual v1 orange, 1P purple,
// 2P green, anyone else grey; each fill a mix with the panel's background.
constexpr COLORREF diagram_background = RGB(26, 32, 44), v2_colour = RGB(86, 156, 255), v1_colour = RGB(255, 140, 80);
constexpr COLORREF player_colours[3] = {RGB(176, 128, 255), RGB(110, 214, 140), RGB(160, 168, 180)};
constexpr COLORREF warning = RGB(255, 150, 130);
COLORREF mix(COLORREF colour, float amount) {
    const auto part = [&](int a, int b) { return int(std::lround(a + (b - a) * amount)); };
    return RGB(part(GetRValue(diagram_background), GetRValue(colour)), part(GetGValue(diagram_background), GetGValue(colour)),
               part(GetBValue(diagram_background), GetBValue(colour)));
}
}

struct KinectPreviewWindow::Impl {
    bool chinese = false;
    int model = 1;  // 1 Xbox 360 (a v2 stands in), 2 Kinect v2 only
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
    static constexpr int up_id = 101, down_id = 102, view_id = 103, step = 5;
    static constexpr int button_width = 96, view_width = 128;
    HWND up_button = nullptr, down_button = nullptr, view_box = nullptr;
    // The view list (a Kinect v2's): Recompiled, Simulated, Original, in
    // KinectDepthChoiceView's order. It starts on the launcher's view; one
    // picked here is this window's own, never written back. On the window's
    // thread: choice (the view shown and the launcher's tuning), and the
    // launcher's view last handed over, so that a view changed there shows.
    // - Original: each v2 camera's whole picture (the old "Original" box),
    //   and the sensor's depth for the game made as sensor.
    // - Simulated: the view both cameras share, and fit's depth.
    // - Recompiled: ideal's depth, and the four pictures and the diagram.
    KinectDepthChoice choice;
    KinectDepthChoiceView launcher_view = KinectDepthChoiceView::ideal;
    std::mutex choice_lock;
    KinectDepthChoice handed_choice;  // under choice_lock
    bool choice_new = false;          // under choice_lock
    // The sensor the choice was last given to (set_depth_settings), so a new
    // choice, or the sensor once it opens, is given it. A borrowed sensor
    // (the game's) is never given one: the window shows the game's view.
    const KinectSensor* settings_for = nullptr;
    bool settings_sent = false;
    bool game_ideal = false;  // over the game: its own view is Recompiled
    // Recompiled: the depth the game gets, and the diagram's snapshot.
    KinectImage game_depth;
    std::vector<uint32_t> game_depth_pixels;
    // The virtual v1's colour, pixel for pixel with that depth: each depth
    // pixel's point (kinect_v1_depth_points) and where it lands in the v2's
    // colour image, sampled again whenever either image is new.
    std::vector<float> v1_points, v1_colour_positions;
    std::vector<uint32_t> v1_colour_pixels;
    KinectIdealSnapshot snapshot;
    bool has_snapshot = false;
    HFONT button_font = nullptr, small_font = nullptr, ruler_font = nullptr;
    std::atomic<bool> turning{false}, reread_angle{false};
    std::jthread turner;
    // Last member: joins before everything above is destroyed.
    std::jthread worker;

    void turn(int degrees) {
        std::shared_ptr<KinectSensor> source;
        { std::lock_guard guard(lock); source = sensor; }
        if (!source || std::string_view(source->model()) == "Kinect v2" || turning.exchange(true)) return;
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
        const int width = button_width, height = 32, gap = 10, top = 12;
        MoveWindow(down_button, rect.right - gap - width, top, width, height, TRUE);
        MoveWindow(up_button, rect.right - 2 * (gap + width), top, width, height, TRUE);
        // A drop-down list's height is its open list's; its field is set to
        // the buttons' height (CB_SETITEMHEIGHT).
        MoveWindow(view_box, rect.right - 2 * (gap + width) - gap - view_width, top, view_width, 200, TRUE);
    }

    // The four pictures and the diagram (a Kinect v2 in Recompiled; over the
    // game only when the game itself makes Recompiled).
    bool recompiled_layout() const {
        return choice.view == KinectDepthChoiceView::ideal && sensor_is_v2() && (!borrowed || game_ideal);
    }
    // Each v2 camera's whole picture: Original, and the top row of Recompiled.
    bool uncropped() const { return choice.view == KinectDepthChoiceView::sensor || recompiled_layout(); }

    // The launcher's newest choice: its tuning always, its view when it has
    // changed there. Over the game the game's own view stays.
    void take_choice() {
        KinectDepthChoice given;
        {
            std::lock_guard guard(choice_lock);
            if (!choice_new) return;
            given = handed_choice;
            choice_new = false;
        }
        if (borrowed) return;
        KinectDepthChoice next = given;
        next.view = given.view != launcher_view ? given.view : choice.view;
        launcher_view = given.view;
        if (next.view != choice.view && view_box) SendMessageW(view_box, CB_SETCURSEL, WPARAM(int(next.view)), 0);
        if (!(next == choice)) {
            choice = next;
            settings_sent = false;
        }
    }

    // The window's title names the sensor: Kinect or Kinect v2.
    std::wstring title_for(bool v2) const {
        if (borrowed) return v2 ? (chinese ? L"Free Riders - Kinect v2 調整" : L"Free Riders - Kinect v2 adjustment")
                                : (chinese ? L"Free Riders - Kinect 調整" : L"Free Riders - Kinect adjustment");
        return v2 ? (chinese ? L"Free Riders - Kinect v2 預覽" : L"Free Riders - Kinect v2 preview")
                  : (chinese ? L"Free Riders - Kinect 預覽" : L"Free Riders - Kinect preview");
    }
    bool title_is_v2 = false;
    // Once a sensor is open, the title follows the one really in use (the
    // Xbox 360 choice falls back to a v2).
    void update_title(HWND window) {
        std::shared_ptr<KinectSensor> source;
        { std::lock_guard guard(lock); source = sensor; }
        const KinectSensor* used = borrowed ? borrowed : source.get();
        if (!used) return;
        const bool v2 = std::string_view(used->model()) == "Kinect v2";
        if (v2 == title_is_v2) return;
        title_is_v2 = v2;
        SetWindowTextW(window, title_for(v2).c_str());
    }

    bool sensor_is_v2() const {
        std::lock_guard guard(lock);
        return sensor != nullptr && std::string_view(sensor->model()) == "Kinect v2";
    }

    // A Kinect v2 has no tilt motor: the buttons stay greyed out for it.
    bool sensor_ready() const {
        std::lock_guard guard(lock);
        return sensor != nullptr && std::string_view(sensor->model()) != "Kinect v2";
    }

    std::wstring say(const wchar_t* english, const wchar_t* chinese_words) const {
        return chinese ? chinese_words : english;
    }

    void refresh() {
        std::shared_ptr<KinectSensor> source;
        { std::lock_guard guard(lock); source = sensor; }
        if (!source) return;
        const bool v2 = std::string_view(source->model()) == "Kinect v2";
        // The view's depth for the game, made by this window's own sensor.
        if (v2 && !borrowed && (!settings_sent || settings_for != source.get())) {
            source->set_depth_settings(kinect_depth_settings(choice));
            settings_sent = true;
            settings_for = source.get();
        }
        source->set_preview_common_view(!uncropped());  // a v2's; others ignore it
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
        if (new_depth && depth.bytes_per_pixel == 2) shade_depth(depth, depth_pixels);
        if (v2 && recompiled_layout()) {
            // The depth the game gets (game_image copies it; the game's own
            // reads are unaffected), and where the ideal camera stands.
            bool resample = new_colour;
            if (source->game_image(KinectImageKind::depth_and_player, game_depth) && game_depth.bytes_per_pixel == 2) {
                shade_depth(game_depth, game_depth_pixels);
                // Back through the very view it was made with, into the
                // sensor's space, and on into its colour image.
                kinect_v1_depth_points(game_depth, kinect_image_view(game_depth), v1_points);
                if (!source->colour_image_positions(v1_points, v1_colour_positions)) v1_colour_positions.clear();
                resample = true;
            }
            const size_t pixels = size_t(game_depth.width) * game_depth.height;
            if (resample && pixels && v1_colour_positions.size() == 2 * pixels &&
                colour_pixels.size() == size_t(colour.width) * colour.height)
                kinect_colour_from_positions(v1_colour_positions.data(), pixels, colour_pixels.data(), colour.width,
                                             colour.height, v1_colour_pixels);
            has_snapshot = source->ideal_snapshot(snapshot);
        }
        if (reread_angle.exchange(false) || now - angle_read > std::chrono::seconds(1)) {
            has_angle = source->elevation(angle);
            angle_read = now;
        }
    }

    void skeletons(HDC dc, const RECT& area, bool colour_view, KinectSensor* source) {
        const KinectImageKind kind = colour_view ? KinectImageKind::colour : KinectImageKind::depth_and_player;
        // The picture is the sensor's rectangle in the preview image's pixels
        // (a Kinect v2's: the whole image, which follows the picture's shape
        // and fills the panel), or a v1's whole fixed-size image. The panel
        // shows that image whole, so its size is the scale.
        KinectImageRect picture{0, 0, colour_view ? 640.0f : 320.0f, colour_view ? 480.0f : 240.0f};
        if (source) {
            KinectImageRect given;
            if (source->picture_rect(kind, given)) picture = given;
        }
        const float image_width = picture.x + picture.width, image_height = picture.y + picture.height;
        if (!(image_width > 0) || !(image_height > 0)) return;
        const float sx = float(area.right - area.left) / image_width;
        const float sy = float(area.bottom - area.top) / image_height;
        // A sensor that places points in its own images (a Kinect v2, whose
        // images are fitted into the v1 sizes) is asked first, and a point it
        // cannot place is left out; a v1's lens places every point. A point
        // placed outside the picture still counts (its bone is clipped).
        const auto place = [&](const std::array<float, 3>& p, POINT& at) {
            KinectImagePoint point;
            bool shown = true;
            if (source && source->image_point(kind, p, point)) shown = point.visible;
            else point = colour_view ? kinect_colour_point(p) : kinect_depth_point(p);
            at = POINT{area.left + LONG(point.x * sx), area.top + LONG(point.y * sy)};
            return shown;
        };
        // Everything is clipped to the picture, so that a bone leaving it is
        // cut at its edge (never into the black bars around a fitted picture
        // or the next panel); a joint is drawn only when its centre is inside
        // the picture (all of the panel).
        const RECT clip{std::max<LONG>(area.left, area.left + LONG(std::floor(picture.x * sx))),
                        std::max<LONG>(area.top, area.top + LONG(std::floor(picture.y * sy))),
                        std::min<LONG>(area.right, area.left + LONG(std::ceil((picture.x + picture.width) * sx))),
                        std::min<LONG>(area.bottom, area.top + LONG(std::ceil((picture.y + picture.height) * sy)))};
        bodies(dc, clip, place);
    }

    // The skeletons with place putting each point in the window (false for a
    // point it cannot place), clipped to clip.
    using Placer = std::function<bool(const std::array<float, 3>&, POINT&)>;
    void bodies(HDC dc, const RECT& clip, const Placer& place) {
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, clip.left, clip.top, clip.right, clip.bottom);
        const auto inside = [&](const POINT& p) {
            return p.x >= clip.left && p.x < clip.right && p.y >= clip.top && p.y < clip.bottom;
        };
        const auto old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
        for (const KinectBody& body : frame.bodies) {
            const COLORREF colour = body_colours[body.sensor_index % 6];
            HPEN pen = CreatePen(PS_SOLID, 3, colour);
            const auto old_pen = SelectObject(dc, pen);
            SetDCBrushColor(dc, colour);
            for (const auto bone : camera_debug_bones()) {
                if (!body.joint_states[bone.from] || !body.joint_states[bone.to]) continue;
                POINT a{}, b{};
                if (!place(body.joints[bone.from], a) || !place(body.joints[bone.to], b)) continue;
                MoveToEx(dc, a.x, a.y, nullptr);
                LineTo(dc, b.x, b.y);
            }
            for (uint32_t j = 0; j < nui_joint_count; ++j) {
                if (!body.joint_states[j]) continue;
                POINT p{};
                if (!place(body.joints[j], p) || !inside(p)) continue;
                const int r = body.joint_states[j] >= 2 ? 4 : 2;  // inferred joints smaller
                Ellipse(dc, p.x - r, p.y - r, p.x + r + 1, p.y + r + 1);
            }
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }
        SelectObject(dc, old_brush);
        RestoreDC(dc, saved);
    }

    // The frames' skeletons are in the skeleton's view, which Recompiled may
    // put at its own distance (SFR_KINECT_V2_SKELETON_DISTANCE): a point of
    // it is this much nearer along z than in the depth's view.
    float skeleton_shift() const {
        return choice.skeleton_distance > 0 ? choice.distance - choice.skeleton_distance : 0.0f;
    }

    // Recompiled: the real v2's whole pictures on top, the virtual v1's
    // below, the diagram on the right (kinect_preview_recompiled_layout).
    void paint_recompiled(HDC dc, int width, int height, KinectSensor* source) {
        KinectRecompiledLayoutInput input;
        input.client_width = width;
        input.client_height = height;
        if (source) {
            KinectImageRect rect;
            if (source->picture_rect(KinectImageKind::colour, rect) && rect.height > 0)
                input.v2_colour_aspect = rect.width / rect.height;
            if (source->picture_rect(KinectImageKind::depth_and_player, rect) && rect.height > 0)
                input.v2_depth_aspect = rect.width / rect.height;
        }
        const KinectRecompiledLayout layout = kinect_preview_recompiled_layout(input);
        const auto win = [](const KinectPanelRect& r) { return RECT{r.left, r.top, r.right, r.bottom}; };
        // One short caption above each picture, stopping at the next cell.
        const auto caption = [&](const KinectPanelRect& picture, int right, const wchar_t* english, const wchar_t* chinese_words) {
            if (picture.width() > 0) text(dc, picture.left, picture.top - 22, say(english, chinese_words), dim, right - 8);
        };
        caption(layout.v2_colour, layout.depth_cells[0].left, L"V2 colour", L"V2 彩色影像");
        caption(layout.v2_depth, layout.diagram.left, L"V2 depth", L"V2 深度影像");
        caption(layout.v1_colour, layout.depth_cells[1].left, L"Simulated colour", L"模擬彩色影像");
        caption(layout.v1_depth, layout.diagram.left, L"Simulated depth", L"模擬深度影像");
        caption(layout.diagram, width, L"Positions from above", L"俯視位置");
        const RECT v2_colour_area = win(layout.v2_colour), v2_depth_area = win(layout.v2_depth);
        const RECT v1_colour_area = win(layout.v1_colour), v1_depth_area = win(layout.v1_depth);
        draw_image(dc, v2_colour_area, colour_pixels, int(colour.width), int(colour.height));
        draw_image(dc, v2_depth_area, depth_pixels, int(depth.width), int(depth.height));
        if (layout.v2_colour.width() > 0) skeletons(dc, v2_colour_area, true, source);
        if (layout.v2_depth.width() > 0) skeletons(dc, v2_depth_area, false, source);
        // The v1's colour and depth, pixel for pixel the same (the colour is
        // sampled for each depth pixel's own point), both with the skeleton
        // through the v1's own lens (NuiTransformSkeletonToDepthImage) in the
        // depth's view.
        const auto v1_panel = [&](const RECT& area, const std::vector<uint32_t>& pixels) {
            if (area.right <= area.left) return;
            if (pixels.size() != size_t(game_depth.width) * game_depth.height || pixels.empty())
                FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            else draw_image(dc, area, pixels, int(game_depth.width), int(game_depth.height));
            const float sx = float(area.right - area.left) / float(kinect_v1_depth_width),
                        sy = float(area.bottom - area.top) / float(kinect_v1_depth_height);
            const float shift = skeleton_shift();
            bodies(dc, area, [&](const std::array<float, 3>& p, POINT& at) {
                const KinectImagePoint point = kinect_depth_point({p[0], p[1], p[2] + shift});
                if (!point.visible) return false;
                at = POINT{area.left + LONG(point.x * sx), area.top + LONG(point.y * sy)};
                return true;
            });
        };
        v1_panel(v1_colour_area, v1_colour_pixels);
        v1_panel(v1_depth_area, game_depth_pixels);
        if (layout.diagram.width() > 0) paint_diagram(dc, win(layout.diagram));
    }

    // A label's small rounded box, its top centre at (x, y), kept inside area.
    static RECT tag_box(HDC dc, const RECT& area, int x, int y, const std::wstring& words) {
        SIZE size{};
        GetTextExtentPoint32W(dc, words.c_str(), int(words.size()), &size);
        const int w = size.cx + 12, h = size.cy + 4;
        RECT box{x - w / 2, y, x - w / 2 + w, y + h};
        if (box.right > area.right - 2) OffsetRect(&box, area.right - 2 - box.right, 0);
        if (box.left < area.left + 2) OffsetRect(&box, area.left + 2 - box.left, 0);
        if (box.bottom > area.bottom - 2) OffsetRect(&box, 0, area.bottom - 2 - box.bottom);
        return box;
    }
    void tag(HDC dc, const RECT& box, const std::wstring& words, COLORREF colour) {
        HPEN pen = CreatePen(PS_SOLID, 1, colour);
        HBRUSH brush = CreateSolidBrush(mix(colour, 0.25f));
        const auto old_pen = SelectObject(dc, pen);
        const auto old_brush = SelectObject(dc, brush);
        RoundRect(dc, box.left, box.top, box.right, box.bottom, 8, 8);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
        SetTextColor(dc, foreground);
        RECT inner = box;
        DrawTextW(dc, words.c_str(), int(words.size()), &inner, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // The top-down sketch (kinect_ideal_diagram): the real v2 at the bottom
    // looking up, its field of view filled blue; the virtual v1, its view
    // dashed orange, where the ideal view puts it; the players' depth line,
    // each player with the part of the row both cameras see, its distance to
    // each camera, and what it is outside of.
    void paint_diagram(HDC dc, const RECT& area) {
        HBRUSH back = CreateSolidBrush(diagram_background);
        FillRect(dc, &area, back);
        DeleteObject(back);
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
        if (small_font) SelectObject(dc, small_font);
        const int margin = 10;
        const RECT inner{area.left + margin, area.top + margin, area.right - margin, area.bottom - margin};
        KinectIdealSnapshot shown;
        if (has_snapshot) shown = snapshot;
        else shown.distance = choice.distance;
        // Strips for the rulers' labels: the depth ruler on the left, the
        // lateral one along the bottom; the drawing is the rest.
        SIZE label_size{};
        if (ruler_font) SelectObject(dc, ruler_font);
        GetTextExtentPoint32W(dc, L"-8 m", 4, &label_size);
        const LONG ruler_left = label_size.cx + 10, ruler_bottom = label_size.cy + 8;
        if (small_font) SelectObject(dc, small_font);
        const KinectIdealDiagram diagram = kinect_ideal_diagram(shown, float(inner.right - inner.left), float(inner.bottom - inner.top),
                                                                float(ruler_left), float(ruler_bottom));
        if (diagram.scale <= 0) { RestoreDC(dc, saved); return; }
        const RECT drawing{inner.left + ruler_left, inner.top, inner.right, inner.bottom - ruler_bottom};
        const auto at = [&](const KinectDiagramPoint& p) { return POINT{inner.left + LONG(p.x), inner.top + LONG(p.y)}; };
        const auto x_of = [&](float x) { return inner.left + LONG(x); };
        const auto y_of = [&](float y) { return inner.top + LONG(y); };
        const auto triangle = [&](const std::array<KinectDiagramPoint, 3>& corners, COLORREF fill, HPEN outline) {
            const POINT points[3] = {at(corners[0]), at(corners[1]), at(corners[2])};
            HBRUSH brush = CreateSolidBrush(fill);
            const auto old_brush = SelectObject(dc, brush);
            const auto old_pen = SelectObject(dc, outline ? outline : GetStockObject(NULL_PEN));
            Polygon(dc, points, 3);
            SelectObject(dc, old_pen);
            SelectObject(dc, old_brush);
            DeleteObject(brush);
        };
        const auto outline = [&](const std::array<KinectDiagramPoint, 3>& corners, HPEN pen) {
            const auto old_pen = SelectObject(dc, pen);
            const POINT apex = at(corners[0]), left = at(corners[1]), right = at(corners[2]);
            MoveToEx(dc, left.x, left.y, nullptr);
            LineTo(dc, apex.x, apex.y);
            LineTo(dc, right.x, right.y);
            SelectObject(dc, old_pen);
        };
        const auto line = [&](LONG x0, LONG y0, LONG x1, LONG y1, HPEN pen) {
            const auto old_pen = SelectObject(dc, pen);
            MoveToEx(dc, x0, y0, nullptr);
            LineTo(dc, x1, y1);
            SelectObject(dc, old_pen);
        };
        HPEN v2_pen = CreatePen(PS_SOLID, 2, v2_colour);
        LOGBRUSH dash_brush{BS_SOLID, v1_colour, 0};
        HPEN v1_pen = ExtCreatePen(PS_GEOMETRIC | PS_DASH | PS_ENDCAP_FLAT, 2, &dash_brush, 0, nullptr);
        HPEN v1_dots = CreatePen(PS_DOT, 1, mix(v1_colour, 0.7f));
        HPEN row_dots = CreatePen(PS_DOT, 1, dim);
        SetBkMode(dc, TRANSPARENT);
        // The rulers: depth from the real v2 up the left edge, across along
        // the bottom (0 on the v2's axis); a mark every half metre, a label
        // every metre where there is room for it.
        HPEN ruler_pen = CreatePen(PS_SOLID, 1, dim);
        HPEN grid_pen = CreatePen(PS_SOLID, 1, mix(foreground, 0.10f));
        if (ruler_font) SelectObject(dc, ruler_font);
        SetTextColor(dc, dim);
        const LONG axis_x = drawing.left - 2, axis_y = drawing.bottom + 2;
        line(axis_x, drawing.top, axis_x, axis_y, ruler_pen);
        line(axis_x, axis_y, drawing.right, axis_y, ruler_pen);
        const auto metres_label = [](float metres) {
            wchar_t words[16];
            if (metres == 0) std::swprintf(words, 16, L"0");
            else std::swprintf(words, 16, L"%d m", int(std::lround(metres)));
            return std::wstring(words);
        };
        const bool depth_labels = diagram.scale >= float(label_size.cy + 4);
        for (const KinectDiagramTick& tick : diagram.depth_ticks) {
            const LONG y = y_of(tick.at);
            if (y < drawing.top || y > drawing.bottom) continue;
            line(axis_x - (tick.whole ? 6 : 3), y, axis_x, y, ruler_pen);
            if (!tick.whole || !depth_labels) continue;
            const std::wstring words = metres_label(tick.metres);
            RECT box{inner.left, y - label_size.cy / 2, axis_x - 8, y + label_size.cy / 2 + 1};
            DrawTextW(dc, words.c_str(), int(words.size()), &box, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        const bool lateral_labels = diagram.scale >= float(label_size.cx + 6);
        for (const KinectDiagramTick& tick : diagram.lateral_ticks) {
            const LONG x = x_of(tick.at);
            if (x < drawing.left || x > drawing.right) continue;
            line(x, axis_y, x, axis_y + (tick.whole ? 5 : 3), ruler_pen);
            if (!tick.whole || !lateral_labels) continue;
            const std::wstring words = metres_label(tick.metres);
            RECT box{x - label_size.cx, axis_y + 5, x + label_size.cx, inner.bottom + 2};
            DrawTextW(dc, words.c_str(), int(words.size()), &box, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (small_font) SelectObject(dc, small_font);
        // The drawing itself stays inside its own part of the panel.
        const int drawing_saved = SaveDC(dc);
        IntersectClipRect(dc, drawing.left, drawing.top, drawing.right, drawing.bottom);
        triangle(diagram.v2_view, mix(v2_colour, 0.22f), nullptr);
        triangle(diagram.v1_view, mix(v1_colour, 0.22f), nullptr);
        // A faint line across at every whole metre from the v2.
        for (const KinectDiagramTick& tick : diagram.depth_ticks)
            if (tick.whole) line(drawing.left, y_of(tick.at), drawing.right, y_of(tick.at), grid_pen);
        outline(diagram.v2_view, v2_pen);
        outline(diagram.v1_view, v1_pen ? v1_pen : v2_pen);
        // The v1's usable depths, 0.8 and 4.0 m from it: dotted across its
        // view, and marked and labelled on its own axis.
        line(x_of(diagram.v1_near_x[0]), y_of(diagram.v1_near_y), x_of(diagram.v1_near_x[1]), y_of(diagram.v1_near_y), v1_dots);
        line(x_of(diagram.v1_far_x[0]), y_of(diagram.v1_far_y), x_of(diagram.v1_far_x[1]), y_of(diagram.v1_far_y), v1_dots);
        HPEN mark_pen = CreatePen(PS_SOLID, 2, v1_colour);
        if (ruler_font) SelectObject(dc, ruler_font);
        SetTextColor(dc, v1_colour);
        for (const auto& [mark, words] : {std::pair{diagram.v1_near_mark, L"0.8 m"}, std::pair{diagram.v1_far_mark, L"4.0 m"}}) {
            const POINT p = at(mark);
            line(p.x - 6, p.y, p.x + 7, p.y, mark_pen);
            TextOutW(dc, p.x + 9, p.y - label_size.cy, words, int(std::wcslen(words)));
        }
        if (small_font) SelectObject(dc, small_font);
        // The players' depth line, distance in front of the virtual camera: drawn
        // with nobody there too, so the launcher's distance slider shows at once.
        line(drawing.left, y_of(diagram.players_y), drawing.right, y_of(diagram.players_y), row_dots);
        const LONG radius = std::clamp<LONG>(LONG(0.2f * diagram.scale), 9, 15);
        for (const KinectDiagramPlayer& player : diagram.players) {
            const COLORREF colour = player_colours[player.slot == 0 ? 0 : player.slot == 1 ? 1 : 2];
            const POINT centre = at(player.at);
            if (player.band_right > player.band_left) {
                const RECT band{x_of(player.band_left), centre.y - radius * 2 / 3, x_of(player.band_right), centre.y + radius * 2 / 3};
                HBRUSH brush = CreateSolidBrush(mix(colour, 0.2f));
                FillRect(dc, &band, brush);
                DeleteObject(brush);
            }
        }
        for (const KinectDiagramPlayer& player : diagram.players) {
            const COLORREF colour = player_colours[player.slot == 0 ? 0 : player.slot == 1 ? 1 : 2];
            const POINT centre = at(player.at);
            HBRUSH brush = CreateSolidBrush(colour);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            const auto old_brush = SelectObject(dc, brush);
            const auto old_pen = SelectObject(dc, pen);
            Ellipse(dc, centre.x - radius, centre.y - radius, centre.x + radius + 1, centre.y + radius + 1);
            SelectObject(dc, old_pen);
            SelectObject(dc, old_brush);
            DeleteObject(pen);
            DeleteObject(brush);
            const std::wstring name = player.slot == 0 ? L"1P" : player.slot == 1 ? L"2P" : L"";
            if (!name.empty()) {
                RECT label{centre.x - radius, centre.y - radius, centre.x + radius + 1, centre.y + radius + 1};
                SetTextColor(dc, RGB(255, 255, 255));
                DrawTextW(dc, name.c_str(), int(name.size()), &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            // Its distances, and what it is outside of, beside it.
            wchar_t numbers[96];
            std::swprintf(numbers, 96, chinese ? L"遊戲 %.2f m  v2 %.2f m" : L"game %.2f m  v2 %.2f m", double(player.v1_depth),
                         double(player.v2_depth));
            std::vector<std::pair<std::wstring, COLORREF>> lines{{numbers, foreground}};
            if (!player.in_v1_view) lines.push_back({say(L"Outside the game's view", L"超出遊戲視野"), warning});
            else if (!player.in_v1_range) lines.push_back({say(L"Outside the game's 0.8-4.0 m", L"超出遊戲的 0.8–4.0 公尺"), warning});
            if (!player.in_v2_view) lines.push_back({say(L"Outside the v2's view", L"超出 v2 視野"), warning});
            LONG widest = 0;
            for (const auto& [words, unused] : lines) {
                SIZE size{};
                GetTextExtentPoint32W(dc, words.c_str(), int(words.size()), &size);
                widest = std::max(widest, size.cx);
            }
            const LONG right_side = centre.x + radius + 6;
            const LONG x = right_side + widest <= drawing.right ? right_side : std::max<LONG>(drawing.left, centre.x - radius - 6 - widest);
            LONG y = centre.y - LONG(lines.size()) * 8;
            for (const auto& [words, colour_of_line] : lines) {
                text(dc, x, y, words, colour_of_line);
                y += 16;
            }
        }
        // The cameras: a dot at each, and its name in a box under it (side
        // by side when the two stand together).
        const POINT v2_at = at(diagram.v2_view[0]), v1_at = at(diagram.v1_view[0]);
        for (const auto& [where, colour] : {std::pair{v2_at, v2_colour}, std::pair{v1_at, v1_colour}}) {
            HBRUSH brush = CreateSolidBrush(colour);
            const auto old_brush = SelectObject(dc, brush);
            const auto old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, where.x - 4, where.y - 4, where.x + 5, where.y + 5);
            SelectObject(dc, old_pen);
            SelectObject(dc, old_brush);
            DeleteObject(brush);
        }
        const std::wstring v2_name = say(L"Physical v2", L"實體 v2"), v1_name = say(L"Virtual Kinect", L"虛擬 Kinect");
        const RECT v2_box = tag_box(dc, drawing, v2_at.x, v2_at.y + 6, v2_name);
        RECT v1_box = tag_box(dc, drawing, v1_at.x, v1_at.y + 6, v1_name);
        RECT overlap{};
        if (IntersectRect(&overlap, &v2_box, &v1_box)) {
            // Beside the v2's instead, on the side with more room.
            const LONG w = v1_box.right - v1_box.left;
            const bool left = v2_box.left - drawing.left > drawing.right - v2_box.right;
            const LONG centre_x = left ? v2_box.left - 4 - w / 2 : v2_box.right + 4 + w / 2;
            v1_box = tag_box(dc, drawing, centre_x, v2_box.top, v1_name);
        }
        tag(dc, v2_box, v2_name, v2_colour);
        tag(dc, v1_box, v1_name, v1_colour);
        // Nothing to place yet: say why.
        const std::wstring hint = !has_snapshot ? say(L"Waiting for the depth image", L"等待深度影像")
                                  : shown.players.empty() ? say(L"No players tracked", L"沒有追蹤到玩家")
                                                          : std::wstring();
        if (!hint.empty()) text(dc, drawing.left + 4, drawing.top, hint, dim, drawing.right);
        // Recompiled's tuning as the launcher has it now, so a slider's move
        // shows here at once.
        if (recompiled_layout()) {
            wchar_t first[96], second[96];
            std::swprintf(first, 96, chinese ? L"距離 %.2f m  高度 %.2f m" : L"Distance %.2f m  Height %.2f m",
                          double(choice.distance), double(choice.height));
            std::swprintf(second, 96, chinese ? L"跟隨 %.1f s  死區 %.2f m" : L"Follow %.1f s  Dead band %.2f m",
                          double(choice.follow), double(choice.deadband));
            const int step = int(label_size.cy) + 2;
            int row_y = drawing.top + (hint.empty() ? 0 : step);
            text(dc, drawing.left + 4, row_y, first, dim, drawing.right);
            text(dc, drawing.left + 4, row_y + step, second, dim, drawing.right);
        }
        RestoreDC(dc, drawing_saved);
        DeleteObject(v2_pen);
        DeleteObject(ruler_pen);
        DeleteObject(grid_pen);
        DeleteObject(mark_pen);
        if (v1_pen) DeleteObject(v1_pen);
        DeleteObject(v1_dots);
        DeleteObject(row_dots);
        RestoreDC(dc, saved);
    }

    // The one place the panels are placed: painting and the skeleton overlay
    // both use these rectangles.
    static KinectPreviewLayout panel_layout(int width, int height, double colour_aspect, double depth_aspect,
                                            bool same_size) {
        KinectPreviewLayoutInput input;
        input.client_width = width;
        input.client_height = height;
        input.colour_aspect = colour_aspect;
        input.depth_aspect = depth_aspect;
        input.same_size = same_size;
        return kinect_preview_layout(input);
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
        // Where text may run to: the first row stops short of the buttons,
        // the next rows (to y 76) of the tilt hint under them, the rest of
        // the window's edge; so nothing overlaps at the smallest window.
        const std::wstring keys = say(L"Tilt: buttons or Up / Down keys, 5 degrees a turn", L"調整角度：按鈕或鍵盤 ↑ ↓，每次 5 度");
        SIZE extent{};
        GetTextExtentPoint32W(dc, keys.c_str(), int(keys.size()), &extent);
        const int buttons_left = width - 2 * (10 + button_width) - (10 + view_width) - 10;
        const int hint_left = width - 20 - extent.cx;
        const auto limit_at = [&](int y) { return y < 40 ? buttons_left : y < 76 ? hint_left - 12 : width - 20; };
        if (!readiness.ready()) {
            // Not playable yet: each step with its state, then what to do
            // about the first one that failed.
            // The SDK of the sensor in use: 2.0 for a Kinect v2, 1.8 for a v1;
            // before one opens, the one the launcher chose.
            const bool v2_sdk = source ? std::string_view(source->model()) == "Kinect v2" : model == 2;
            const wchar_t* const names[] = {v2_sdk ? L"Runtime (SDK 2.0)" : L"Runtime (SDK 1.8)",
                                            v2_sdk ? L"執行環境（SDK 2.0）" : L"執行環境（SDK 1.8）", L"Sensor", L"感應器",
                                            L"Frames", L"資料傳送", L"Body", L"人體追蹤"};
            int x = 20;
            for (size_t step = 0; step < size_t(KinectStep::count); ++step) {
                const KinectStepState state = readiness.steps[step];
                const wchar_t* const mark = state == KinectStepState::passed ? L"\u2713 " :
                                            state == KinectStepState::failed ? L"\u2717 " : L"\u2026 ";
                const std::wstring item = mark + say(names[2 * step], names[2 * step + 1]);
                text(dc, x, 16, item, state == KinectStepState::passed ? RGB(128, 225, 167) :
                                      state == KinectStepState::failed ? RGB(255, 150, 130) : dim, limit_at(16));
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
            text(dc, 20, 40, hint, readiness.first_failed == KinectStep::count ? foreground : RGB(255, 205, 119), limit_at(40));
        }
        if (source && readiness.ready()) {
            const std::string name = source->model();
            const std::wstring model_name(name.begin(), name.end());
            if (has_angle)
                std::swprintf(line, 256, chinese ? L"%ls  追蹤到 %zu 人  感應器角度 %d°" : L"%ls  %zu tracked  Sensor angle %d°",
                              model_name.c_str(), frame.bodies.size(), angle);
            else
                std::swprintf(line, 256, chinese ? L"%ls  追蹤到 %zu 人" : L"%ls  %zu tracked", model_name.c_str(),
                              frame.bodies.size());
            std::wstring model(line);
            text(dc, 20, 16, model, foreground, limit_at(16));
            // No depth stream for the game (it would get none, and its own
            // detectors see nothing): say so, with why.
            if (const std::string why = source->depth_failure(); !why.empty()) {
                SIZE size{};
                GetTextExtentPoint32W(dc, model.c_str(), int(model.size()), &size);
                const std::wstring reason(why.begin(), why.end());
                text(dc, 20 + size.cx + 24, 16,
                     say(L"\u2717 No depth for the game: ", L"\u2717 遊戲沒有深度影像：") + reason, RGB(255, 150, 130), limit_at(16));
            }
            // Per body: its id, distance, and whether its feet are really seen
            // (a foot the sensor only guesses is out of view).
            int y = 40;
            for (const KinectBody& body : frame.bodies) {
                const bool feet = body.joint_states[nui_joint::foot_left] >= 2 && body.joint_states[nui_joint::foot_right] >= 2;
                std::swprintf(line, 256, chinese ? L"編號 %u  位置 %u  距離 %.2f m  %ls" : L"id %u  slot %u  %.2f m away  %ls",
                              body.tracking_id, body.sensor_index, body.position[2],
                              feet ? say(L"feet in view", L"腳在畫面內").c_str()
                                   : say(L"feet out of view: tilt down or step back", L"腳不在畫面內：角度往下或往後站").c_str());
                text(dc, 20, y, line, feet ? body_colours[body.sensor_index % 6] : RGB(255, 205, 119), limit_at(y));
                y += 20;
            }
            // A body cut by the picture's edge (a Kinect v2 says which; a v1
            // says nothing). The edges are the picture's, as drawn below,
            // not mirrored: its left is the sensor's left, which is the
            // PLAYER's right while facing the sensor. So a cut on the
            // picture's left asks the player to step to their own left,
            // and the reverse (kinect_edges_move). Display only.
            for (const KinectBody& body : frame.bodies) {
                if (!body.clipped_edges || y > 90) continue;
                const uint8_t move = kinect_edges_move(body.clipped_edges);
                std::wstring edges, what;
                const auto add = [&](std::wstring& list, const wchar_t* en, const wchar_t* zh, const wchar_t* join) {
                    if (!list.empty()) list += join;
                    list += say(en, zh);
                };
                if (body.clipped_edges & kinect_edge::top) add(edges, L"top", L"上方", L"、");
                if (body.clipped_edges & kinect_edge::bottom) add(edges, L"bottom", L"下方", L"、");
                if (body.clipped_edges & kinect_edge::left) add(edges, L"picture left", L"畫面左側", L"、");
                if (body.clipped_edges & kinect_edge::right) add(edges, L"picture right", L"畫面右側", L"、");
                if (move & kinect_edge_move::player_left) add(what, L"step to your left", L"請往你的左邊移動", L", ");
                if (move & kinect_edge_move::player_right) add(what, L"step to your right", L"請往你的右邊移動", L", ");
                if (move & kinect_edge_move::step_back) {
                    if (move & kinect_edge_move::tilt_up) add(what, L"step back or tilt the sensor up", L"請往後退，或把感應器角度往上", L", ");
                    else if (move & kinect_edge_move::tilt_down) add(what, L"step back or tilt the sensor down", L"請往後退，或把感應器角度往下", L", ");
                    else add(what, L"step back", L"請往後退", L", ");
                }
                std::wstring who;
                if (frame.bodies.size() > 1) {
                    wchar_t slot[32];
                    std::swprintf(slot, 32, chinese ? L"（位置 %u）" : L" (slot %u)", body.sensor_index);
                    who = slot;
                }
                text(dc, 20, y, say(L"✗ Body cut off", L"✗ 身體被畫面切到") + who + say(L": ", L"：") + edges +
                                    say(L" — ", L"，") + what, RGB(255, 150, 130), limit_at(y));
                y += 20;
            }
        }
        text(dc, hint_left, 52, turning.load() ? say(L"Turning...", L"轉動中…") : keys, dim);
        const int gap = 20;
        if (source && recompiled_layout()) paint_recompiled(dc, width, height, source.get());
        else paint_pair(dc, width, height, source.get());
        // Name the sensor in use: a v2 says Kinect v2.
        const bool v2_sensor = source ? std::string_view(source->model()) == "Kinect v2" : model == 2;
        text(dc, gap, height - 28, borrowed ? say(L"Close this window to go back to the game.", L"關閉此視窗即可回到遊戲。")
                                            : v2_sensor
                                                  ? say(L"Close this window before starting the game: the Kinect v2 opens for one program at a time.",
                                                        L"開始遊戲前會自動關閉此視窗：Kinect v2 一次只能給一個程式使用。")
                                                  : say(L"Close this window before starting the game: the Kinect opens for one program at a time.",
                                                        L"開始遊戲前會自動關閉此視窗：Kinect 一次只能給一個程式使用。"),
             dim, width - gap);
        BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old_font);
        SelectObject(dc, old_bitmap);
        if (font) DeleteObject(font);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }

    // Simulated and Original (and a v1): colour and depth side by side.
    void paint_pair(HDC dc, int width, int height, KinectSensor* source) {
        // The two cameras side by side under the text, each panel following
        // its picture's shape (kinect_preview_layout). A Kinect v2 shows the
        // view both its cameras have unless Original is chosen; its sensor
        // says so by giving the picture a rectangle, and both panels then
        // have the same shape and size.
        bool cropped = false;
        double colour_aspect = 4.0 / 3.0, depth_aspect = 4.0 / 3.0;
        if (source) {
            KinectImageRect colour_rect, depth_rect;
            const bool has_colour = source->picture_rect(KinectImageKind::colour, colour_rect);
            const bool has_depth = source->picture_rect(KinectImageKind::depth_and_player, depth_rect);
            cropped = has_colour && !uncropped();
            if (has_colour && colour_rect.height > 0) colour_aspect = colour_rect.width / colour_rect.height;
            else if (colour.width && colour.height) colour_aspect = double(colour.width) / colour.height;
            if (has_depth && depth_rect.height > 0) depth_aspect = depth_rect.width / depth_rect.height;
            else if (depth.width && depth.height) depth_aspect = double(depth.width) / depth.height;
        }
        const KinectPreviewLayout layout = panel_layout(width, height, colour_aspect, depth_aspect, cropped);
        const RECT colour_area{layout.colour.left, layout.colour.top, layout.colour.right, layout.colour.bottom};
        const RECT depth_area{layout.depth.left, layout.depth.top, layout.depth.right, layout.depth.bottom};
        const int gap = 20, top = 110;
        // A caption stops short of the next one (the depth panel's, or the window's edge).
        text(dc, gap, top - 22,
             cropped ? say(L"COLOUR (cropped)", L"彩色影像（裁切）") : say(L"COLOUR", L"彩色影像"), dim,
             depth_area.right > 0 ? depth_area.left - 8 : width - gap);
        if (depth_area.right > 0)
            text(dc, depth_area.left, top - 22, say(L"DEPTH", L"深度影像"), dim, width - gap);
        draw_image(dc, colour_area, colour_pixels, int(colour.width), int(colour.height));
        draw_image(dc, depth_area, depth_pixels, int(depth.width), int(depth.height));
        if (colour_area.right > colour_area.left) skeletons(dc, colour_area, true, source);
        if (depth_area.right > depth_area.left) skeletons(dc, depth_area, false, source);
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
                self->take_choice();
                self->refresh();
                self->update_title(window);
                const bool ready = !self->turning.load() && self->sensor_ready();
                // Only on a change: enabling repaints a control.
                const auto enable = [](HWND control, bool on) {
                    if (control && bool(IsWindowEnabled(control)) != on) EnableWindow(control, on);
                };
                enable(self->up_button, ready);
                enable(self->down_button, ready);
                enable(self->view_box, self->sensor_is_v2());  // a v1 has one view only
                // The window is WS_CLIPCHILDREN, so this repaint never draws
                // over the list and buttons.
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == view_id) {
                // The list keeps the focus while it is open; the window takes
                // the arrow keys back once it closes.
                if (HIWORD(wp) == CBN_SELCHANGE) {
                    const LRESULT picked = SendMessageW(self->view_box, CB_GETCURSEL, 0, 0);
                    if (picked >= 0 && picked < 3 && KinectDepthChoiceView(picked) != self->choice.view) {
                        self->choice.view = KinectDepthChoiceView(picked);
                        self->settings_sent = false;
                        InvalidateRect(window, nullptr, FALSE);
                    }
                } else if (HIWORD(wp) == CBN_CLOSEUP) {
                    SetFocus(window);
                }
                return 0;
            }
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
        title_is_v2 = model == 2;
        HWND window = CreateWindowExW(0, cls.lpszClassName,
                                      title_for(model == 2).c_str(),
                                      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1360,
                                      // Recompiled's two rows of pictures want more height.
                                      choice.view == KinectDepthChoiceView::ideal && (model == 2 || game_ideal) ? 820 : 640,
                                      nullptr, nullptr, instance, this);
        if (!window) { is_closed.store(true); return; }
        shown.store(window);
        up_button = CreateWindowExW(0, L"BUTTON", chinese ? L"往上" : L"Up", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(up_id)), instance, nullptr);
        down_button = CreateWindowExW(0, L"BUTTON", chinese ? L"往下" : L"Down", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
                                      0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(down_id)), instance, nullptr);
        button_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                  chinese ? L"Microsoft JhengHei UI" : L"Segoe UI");
        for (HWND button : {up_button, down_button})
            if (button_font) SendMessageW(button, WM_SETFONT, WPARAM(button_font), TRUE);
        small_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                 chinese ? L"Microsoft JhengHei UI" : L"Segoe UI");
        ruler_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                 chinese ? L"Microsoft JhengHei UI" : L"Segoe UI");
        // The depth views, as the launcher names and orders them.
        view_box = CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_VSCROLL | CBS_DROPDOWNLIST,
                                   0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(view_id)), instance, nullptr);
        if (button_font) SendMessageW(view_box, WM_SETFONT, WPARAM(button_font), TRUE);
        for (const wchar_t* name : {L"Recompiled", chinese ? L"模擬" : L"Simulated", chinese ? L"原始" : L"Original"})
            SendMessageW(view_box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        SendMessageW(view_box, CB_SETCURSEL, WPARAM(int(choice.view)), 0);
        // The field as tall as the buttons beside it (its border adds about 6).
        SendMessageW(view_box, CB_SETITEMHEIGHT, WPARAM(-1), 26);
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
                                                            : KinectSensor::open_selected(model, &why);
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
        if (small_font) DeleteObject(small_font);
        if (ruler_font) DeleteObject(ruler_font);
        opener.join();
        // The sensor closes now unless a tilt is still using it.
        { std::lock_guard guard(lock); sensor.reset(); }
        is_closed.store(true);
    }
};

KinectPreviewWindow::KinectPreviewWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
KinectPreviewWindow::~KinectPreviewWindow() = default;

std::unique_ptr<KinectPreviewWindow> KinectPreviewWindow::open(bool chinese, int model, const KinectDepthChoice& depth) {
    try {
        auto impl = std::make_unique<Impl>();
        impl->chinese = chinese;
        impl->model = model;
        impl->choice = depth;
        impl->launcher_view = depth.view;
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
        // The game's own view and tuning (its environment), which this
        // window never changes: picking another view only changes the
        // pictures' crop.
        const KinectDepthSettings game = KinectDepthSettings::from_environment();
        impl->game_ideal = game.mode == KinectDepthViewMode::ideal;
        impl->choice.view = impl->game_ideal                              ? KinectDepthChoiceView::ideal
                            : game.mode == KinectDepthViewMode::sensor ? KinectDepthChoiceView::sensor
                                                                       : KinectDepthChoiceView::fit;
        impl->choice.distance = game.distance;
        impl->choice.height = game.height;
        impl->choice.follow = game.follow;
        impl->choice.deadband = game.deadband;
        impl->choice.skeleton_distance = game.skeleton_distance;
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

void KinectPreviewWindow::set_depth_choice(const KinectDepthChoice& depth) {
    // Sent every frame; take_choice compares with what is shown.
    std::lock_guard guard(impl_->choice_lock);
    impl_->handed_choice = depth;
    impl_->choice_new = true;
}

bool KinectPreviewWindow::ideal_snapshot(KinectIdealSnapshot& snapshot) const {
    const std::shared_ptr<KinectSensor> source = sensor();
    return source && source->ideal_snapshot(snapshot);
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
}
#else
namespace sfr {
struct KinectPreviewWindow::Impl {};
KinectPreviewWindow::KinectPreviewWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
KinectPreviewWindow::~KinectPreviewWindow() = default;
std::unique_ptr<KinectPreviewWindow> KinectPreviewWindow::open(bool, int, const KinectDepthChoice&) { return nullptr; }
std::unique_ptr<KinectPreviewWindow> KinectPreviewWindow::open_over(KinectSensor&) { return nullptr; }
void KinectPreviewWindow::show_frame(const KinectFrame&) {}
void KinectPreviewWindow::set_depth_choice(const KinectDepthChoice&) {}
bool KinectPreviewWindow::ideal_snapshot(KinectIdealSnapshot&) const { return false; }
bool KinectPreviewWindow::closed() const { return true; }
void KinectPreviewWindow::close() {}
void KinectPreviewWindow::raise() {}
std::shared_ptr<KinectSensor> KinectPreviewWindow::sensor() const { return nullptr; }
std::string KinectPreviewWindow::failure() const { return {}; }
}
#endif
