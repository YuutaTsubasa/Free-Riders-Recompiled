#pragma once

#include <memory>
#include <string>

namespace sfr {

// The launcher's look at a webcam: a window of its own showing the pictures
// the camera sends, with their size and rate, or why none come. The camera's
// Test button says only whether pictures arrive; this shows what they are
// (pointed the right way, lit, the camera the player meant). It holds the
// camera while open, so it closes before the game starts. Windows only;
// elsewhere open() gives null.
class CameraPreviewWindow {
public:
    static std::unique_ptr<CameraPreviewWindow> open(const std::string& device, bool chinese);
    ~CameraPreviewWindow();
    bool closed() const;
    struct Impl;

private:
    explicit CameraPreviewWindow(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

}  // namespace sfr
