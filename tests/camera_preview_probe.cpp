// Opens the launcher's camera preview window (camera_preview_window.h) on its
// own and, after a few seconds, saves what the window shows: a check of the
// preview without the launcher. Not a test (it needs a camera).
//   sfr_camera_preview_probe [camera name] [seconds] [out.bmp]
#include "camera_capture.h"
#include "camera_preview_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    const auto names = sfr::CameraCapture::devices();
    for (const auto& name : names) std::cout << "camera: " << name << '\n';
    const std::string device = argc > 1 ? argv[1] : (names.empty() ? std::string() : names.front());
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 5;
    const char* out = argc > 3 ? argv[3] : "camera_preview.bmp";
    auto preview = sfr::CameraPreviewWindow::open(device, false);
    if (!preview) {
        std::cerr << "no preview window\n";
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    HWND window = FindWindowW(L"SfrCameraPreview", nullptr);
    if (!window) {
        std::cerr << "window not found (closed=" << preview->closed() << ")\n";
        return 1;
    }
    RECT rect;
    GetClientRect(window, &rect);
    const int width = rect.right, height = rect.bottom;
    HDC screen = GetDC(window);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    SelectObject(dc, bitmap);
    PrintWindow(window, dc, PW_CLIENTONLY);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(size_t(width) * height * 4);
    GetDIBits(dc, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader);
    file.bfSize = file.bfOffBits + DWORD(pixels.size());
    std::ofstream stream(out, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    stream.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size()));
    std::cout << "saved " << out << ' ' << width << 'x' << height << " closed=" << preview->closed() << '\n';
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(window, screen);
    return 0;
}
