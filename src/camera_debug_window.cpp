#include "camera_debug.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

namespace sfr {
namespace {
constexpr COLORREF background = RGB(19,24,33), foreground = RGB(221,228,238);
COLORREF side_color(int side) { return side < 0 ? RGB(70,207,236) : side > 0 ? RGB(255,176,87) : foreground; }
void label(HDC dc, int x, int y, const char* text, COLORREF color = foreground) {
    SetTextColor(dc,color);
    TextOutA(dc,x,y,text,static_cast<int>(std::strlen(text)));
}
void line(HDC dc, int x, int y, int xx, int yy) { MoveToEx(dc,x,y,nullptr); LineTo(dc,xx,yy); }
}

struct CameraDebugWindow::Impl {
    std::mutex lock;
    CameraDebugFrame latest{};
    std::atomic<bool> is_closed{false};
    std::stop_token stopping;
    // Last member: joins before the mailbox and its lock are destroyed.
    std::jthread worker;

    void paint(HDC target, const RECT& rect) {
        const int width = rect.right-rect.left, height = rect.bottom-rect.top;
        if (width <= 0 || height <= 0) return;
        CameraDebugFrame frame;
        { std::lock_guard guard(lock); frame = latest; }
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target,width,height);
        if (!dc || !bitmap) { if (dc) DeleteDC(dc); if (bitmap) DeleteObject(bitmap); return; }
        const auto old_bitmap = SelectObject(dc,bitmap);
        HFONT font = CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        const auto old_font = SelectObject(dc,font ? font : GetStockObject(DEFAULT_GUI_FONT));
        HBRUSH brush = CreateSolidBrush(background);
        FillRect(dc,&rect,brush); DeleteObject(brush);
        SetBkMode(dc,TRANSPARENT);
        const auto now = CameraDebugClock::now();
        const auto state = camera_debug_state(frame,now);
        const double elapsed = frame.game_frame ? std::max(0.0,std::chrono::duration<double,std::milli>(now-frame.submitted).count()) : 0;
        const char* status = "Waiting for a submitted P1 skeleton";
        if (state == CameraDebugState::unavailable) status = "Camera or pose model unavailable";
        if (state == CameraDebugState::live) status = "Live camera skeleton delivered to game";
        if (state == CameraDebugState::held) status = "Tracking lost / delayed - game holds the last pose";
        if (state == CameraDebugState::stale) status = "Guest skeleton updates paused - showing last submission";
        if (state == CameraDebugState::controller) {
            if (!frame.camera_available) status = "Controller controls P1 - camera unavailable";
            else if (frame.pose_age_ms < 0) status = "Controller controls P1 - waiting for camera pose";
            else if (frame.pose_age_ms + elapsed > 500) status = "Controller controls P1 - camera tracking lost / delayed";
            else status = "Controller controls P1 - camera pose ready";
        }
        label(dc,20,16,"INPUT SOURCE / GAME P1 SKELETON");
        label(dc,20,40,status,state == CameraDebugState::live || state == CameraDebugState::controller ? RGB(128,225,167) : RGB(255,205,119));
        char details[180];
        std::snprintf(details,sizeof(details),"Guest frame: %llu   Camera pose age: %.0f ms   Guest update age: %.0f ms",
            static_cast<unsigned long long>(frame.game_frame),frame.pose_age_ms >= 0 ? frame.pose_age_ms+elapsed : -1,elapsed);
        label(dc,20,62,details);
        const int panel_width = std::max(0,(width-60)/2), panel_height = std::max(0,height-166);
        for (int panel=0; panel<2; ++panel) {
            const int left=20+panel*(panel_width+20), top=110;
            label(dc,left,88,panel == 0 ? "FRONT  (X / Y)" : "SIDE  (Z / Y)");
            const int saved = SaveDC(dc);
            IntersectClipRect(dc,left,top,left+panel_width,top+panel_height);
            HPEN grid=CreatePen(PS_SOLID,1,RGB(43,53,68));
            const auto old_pen=SelectObject(dc,grid);
            for (int n=0;n<=4;++n) {
                const int gx=left+n*panel_width/4, gy=top+n*panel_height/4;
                line(dc,gx,top,gx,top+panel_height); line(dc,left,gy,left+panel_width,gy);
            }
            SelectObject(dc,old_pen); DeleteObject(grid);
            if (frame.has_pose) {
                std::array<CameraDebugPoint,nui_joint_count> points;
                for (uint32_t n=0;n<nui_joint_count;++n)
                    points[n]=camera_debug_project(frame.joints[n],panel == 0 ? CameraDebugView::front : CameraDebugView::side,
                                                   float(panel_width),float(panel_height));
                const auto old_brush=SelectObject(dc,GetStockObject(DC_BRUSH));
                for (const auto bone : camera_debug_bones()) {
                    const auto a=points[bone.from], b=points[bone.to];
                    const COLORREF color=side_color(bone.side);
                    HPEN pen=CreatePen(PS_SOLID,3,color);
                    const auto previous=SelectObject(dc,pen);
                    if (a.visible && b.visible) line(dc,left+int(a.x),top+int(a.y),left+int(b.x),top+int(b.y));
                    SetDCBrushColor(dc,color);
                    for (const auto point : {a,b}) if (point.visible) {
                        const int x=left+int(point.x), y=top+int(point.y);
                        Ellipse(dc,x-4,y-4,x+5,y+5);
                    }
                    SelectObject(dc,previous); DeleteObject(pen);
                }
                SelectObject(dc,old_brush);
            }
            RestoreDC(dc,saved);
        }
        label(dc,20,height-44,"Left joints",side_color(-1));
        label(dc,110,height-44,"Right joints",side_color(1));
        label(dc,210,height-44,"20 submitted P1 joints / metres");
        label(dc,20,height-24,"Webcam depth is estimated. Closing this window keeps camera input and the game running.",RGB(162,176,194));
        BitBlt(target,0,0,width,height,dc,0,0,SRCCOPY);
        SelectObject(dc,old_font); SelectObject(dc,old_bitmap);
        if (font) DeleteObject(font);
        DeleteObject(bitmap); DeleteDC(dc);
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window,message,wp,lp);
        switch (message) {
        case WM_TIMER:
            // Also runs inside Windows' move/resize and system-menu modal loops.
            // Checking only the outer message loop would block shutdown there.
            if (self->stopping.stop_requested()) DestroyWindow(window);
            else InvalidateRect(window,nullptr,FALSE);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc=BeginPaint(window,&ps); RECT rect; GetClientRect(window,&rect);
            self->paint(dc,rect); EndPaint(window,&ps); return 0;
        }
        case WM_PRINTCLIENT: { RECT rect; GetClientRect(window,&rect); self->paint(reinterpret_cast<HDC>(wp),rect); return 0; }
        case WM_GETMINMAXINFO: {
            auto* limits=reinterpret_cast<MINMAXINFO*>(lp); limits->ptMinTrackSize={680,440}; return 0;
        }
        case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_DESTROY: self->is_closed.store(true); PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window,message,wp,lp);
    }
    void run(std::stop_token stop) {
        stopping=stop;
        const HINSTANCE instance=GetModuleHandleW(nullptr);
        WNDCLASSW cls{};
        cls.lpfnWndProc=procedure; cls.hInstance=instance; cls.lpszClassName=L"SfrCameraSkeletonDebug";
        cls.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
        if (!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) { is_closed.store(true); return; }
        HWND window=CreateWindowExW(WS_EX_NOACTIVATE,cls.lpszClassName,L"Free Riders - Camera Skeleton Debug",
            WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,920,660,nullptr,nullptr,instance,this);
        if (!window) { is_closed.store(true); return; }
        ShowWindow(window,SW_SHOWNOACTIVATE);
        if (!SetTimer(window,1,50,nullptr)) { DestroyWindow(window); return; }
        MSG message;
        while (!stop.stop_requested() && GetMessageW(&message,nullptr,0,0)>0) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (IsWindow(window)) DestroyWindow(window);
        is_closed.store(true);
    }
};
CameraDebugWindow::CameraDebugWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CameraDebugWindow::~CameraDebugWindow() = default;
std::unique_ptr<CameraDebugWindow> CameraDebugWindow::start() {
    const char* enabled=std::getenv("SFR_CAMERA_DEBUG"), *mode=std::getenv("SFR_CAMERA");
    if (!enabled || std::strcmp(enabled,"1") || !mode || std::strcmp(mode,"motion")) return nullptr;
    try {
        auto impl=std::make_unique<Impl>();
        auto* raw=impl.get();
        impl->worker=std::jthread([raw](std::stop_token stop) { raw->run(stop); });
        return std::unique_ptr<CameraDebugWindow>(new CameraDebugWindow(std::move(impl)));
    } catch (const std::exception& error) {
        std::cerr << "NATIVE_CAMERA_DEBUG unavailable: " << error.what() << '\n'; return nullptr;
    }
}
void CameraDebugWindow::publish(const CameraDebugFrame& frame) {
    if (closed()) return;
    std::unique_lock guard(impl_->lock,std::try_to_lock);
    if (guard.owns_lock()) impl_->latest=frame;
}
bool CameraDebugWindow::closed() const { return impl_->is_closed.load(); }
}
#else
namespace sfr {
struct CameraDebugWindow::Impl {};
CameraDebugWindow::CameraDebugWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CameraDebugWindow::~CameraDebugWindow() = default;
std::unique_ptr<CameraDebugWindow> CameraDebugWindow::start() { return nullptr; }
void CameraDebugWindow::publish(const CameraDebugFrame&) {}
bool CameraDebugWindow::closed() const { return true; }
}
#endif
