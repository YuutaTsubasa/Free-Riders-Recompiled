// Synthetic window/lifetime probe. Never opens a camera or loads a pose model.
#include "camera_debug.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
namespace {
void require(bool value,const char* why) { if (!value) throw std::runtime_error(why); }
HWND own_window() {
    HWND window=nullptr;
    while ((window=FindWindowExW(nullptr,window,L"SfrCameraSkeletonDebug",nullptr))) {
        DWORD process=0; GetWindowThreadProcessId(window,&process);
        if (process==GetCurrentProcessId()) return window;
    }
    return nullptr;
}
void save(HWND window,const std::filesystem::path& path) {
    RECT rect; GetClientRect(window,&rect);
    const int w=rect.right,h=rect.bottom;
    HDC target=GetDC(window),dc=CreateCompatibleDC(target);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    require(bitmap!=nullptr,"snapshot bitmap creation failed");
    const auto old=SelectObject(dc,bitmap);
    SendMessageW(window,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
    GdiFlush();
    BITMAPFILEHEADER file{};file.bfType=0x4D42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
    file.bfSize=file.bfOffBits+w*h*4;
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file),sizeof(file));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(BITMAPINFOHEADER));
    output.write(static_cast<const char*>(pixels),w*h*4);
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window,target);
    require(bool(output),"snapshot write failed");
}
sfr::CameraDebugFrame pose() {
    sfr::CameraDebugFrame f;f.game_frame=1;f.camera_available=f.has_pose=f.detected=true;
    f.observation_age_ms=f.pose_age_ms=0;
    f.joints={{{0,-.15f,2.5f},{0,.15f,2.5f},{0,.48f,2.5f},{0,.78f,2.5f},
        {-.18f,.48f,2.5f},{-.4f,.3f,2.5f},{-.58f,.22f,2.5f},{-.68f,.2f,2.45f},
        {.18f,.48f,2.5f},{.36f,.7f,2.5f},{.43f,.9f,2.4f},{.46f,1.0f,2.35f},
        {-.13f,-.18f,2.5f},{-.14f,-.55f,2.5f},{-.14f,-.9f,2.5f},{-.14f,-.95f,2.4f},
        {.13f,-.18f,2.5f},{.15f,-.55f,2.5f},{.16f,-.9f,2.5f},{.16f,-.95f,2.4f}}};
    return f;
}
}
int main(int argc,char** argv) {
    try {
        const auto directory=argc>1?std::filesystem::path(argv[1]):std::filesystem::path(".");
        std::filesystem::create_directories(directory);
        _putenv_s("SFR_CAMERA","motion");_putenv_s("SFR_CAMERA_DEBUG","0");
        require(!sfr::CameraDebugWindow::start(),"disabled debug created a window");
        _putenv_s("SFR_CAMERA","picture");_putenv_s("SFR_CAMERA_DEBUG","1");
        require(!sfr::CameraDebugWindow::start(),"picture mode created a skeleton window");
        _putenv_s("SFR_CAMERA","motion");
        auto debug=sfr::CameraDebugWindow::start();require(bool(debug),"debug start failed");
        HWND window=nullptr;
        for(int n=0;n<100 && !(window=own_window());++n) std::this_thread::sleep_for(20ms);
        require(window!=nullptr,"window did not open");
        std::this_thread::sleep_for(60ms);
        save(window,directory/"waiting.bmp");
        auto frame=pose();frame.submitted=sfr::CameraDebugClock::now();
        for(int n=0;n<100;++n) debug->publish(frame);
        save(window,directory/"live.bmp");
        frame.controller_active=true;
        for(int n=0;n<100;++n) debug->publish(frame);
        save(window,directory/"controller.bmp");
        frame.pose_age_ms=1000;
        for(int n=0;n<100;++n) debug->publish(frame);
        save(window,directory/"controller-lost.bmp");
        frame.controller_active=false;
        frame.detected=false;frame.pose_age_ms=200;frame.submitted=sfr::CameraDebugClock::now();
        for(int n=0;n<100;++n) debug->publish(frame);
        save(window,directory/"held.bmp");
        std::this_thread::sleep_for(550ms);
        save(window,directory/"stale.bmp");
        PostMessageW(window,WM_CLOSE,0,0);
        for(int n=0;n<100 && !debug->closed();++n) std::this_thread::sleep_for(20ms);
        require(debug->closed(),"closing debug window did not stop its worker");
        debug->publish(frame);debug.reset();
        require(!own_window(),"closed window remained alive");
        MSG message;
        require(!PeekMessageW(&message,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE),"debug close quit the host thread");
        // Destruction also shuts down an open window, including immediate shutdown.
        for(int n=0;n<3;++n) { auto again=sfr::CameraDebugWindow::start();require(bool(again),"restart failed"); }
        require(!own_window(),"destruction left a window alive");
        // Moving the window runs its own message loop inside DefWindowProc.
        auto modal=sfr::CameraDebugWindow::start();require(bool(modal),"modal start failed");
        for(int n=0;n<100 && !(window=own_window());++n) std::this_thread::sleep_for(20ms);
        require(window!=nullptr,"modal window did not open");
        SetForegroundWindow(window);
        PostMessageW(window,WM_SYSCOMMAND,SC_MOVE,0);
        GUITHREADINFO gui{};gui.cbSize=sizeof(gui);
        for(int n=0;n<100;++n) {
            GetGUIThreadInfo(GetWindowThreadProcessId(window,nullptr),&gui);
            if(gui.flags&GUI_INMOVESIZE) break;
            std::this_thread::sleep_for(20ms);
        }
        require(bool(gui.flags&GUI_INMOVESIZE),"move-window probe did not enter modal state");
        const auto before=sfr::CameraDebugClock::now();
        modal.reset();
        require(sfr::CameraDebugClock::now()-before<2s,"modal shutdown took too long");
        require(!own_window(),"modal shutdown left a window alive");
        std::cout<<"Camera debug window probe passed: gating, render snapshots, independent close, normal/modal shutdown\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
