// Plume's Apple glue on iOS, in place of tools/Plume/plume_apple.mm (AppKit
// and IOKit, which iOS lacks). Plume's Vulkan backend draws into SDL's Metal
// layer and asks CocoaWindow for the window's size in pixels; here the window
// is SDL's UIWindow. As on macOS, UIKit is read on the main thread, and other
// threads get the last size read while a fresh one is fetched.
#include "plume_apple.h"

#import <UIKit/UIKit.h>
#include <cmath>

namespace plume {
    RenderDeviceVendor getRenderDeviceVendor(uint64_t) { return RenderDeviceVendor::APPLE; }

    namespace {
        CocoaWindowAttributes readAttributes(void* handle) {
            UIWindow* window = (__bridge UIWindow*)handle;
            const CGRect bounds = window.bounds;
            const CGFloat scale = window.screen ? window.screen.nativeScale : UIScreen.mainScreen.nativeScale;
            return {int(std::lround(bounds.origin.x)), int(std::lround(bounds.origin.y)),
                    int(std::lround(bounds.size.width * scale)), int(std::lround(bounds.size.height * scale))};
        }

        int readRefreshRate(void* handle) {
            UIWindow* window = (__bridge UIWindow*)handle;
            UIScreen* screen = window.screen ? window.screen : UIScreen.mainScreen;
            return int(screen.maximumFramesPerSecond);
        }
    }

    CocoaWindow::CocoaWindow(void* window) : windowHandle(window), cachedRefreshRate(0) {
        cachedAttributes = {0, 0, 0, 0};
        if ([NSThread isMainThread]) {
            cachedAttributes = readAttributes(windowHandle);
            cachedRefreshRate.store(readRefreshRate(windowHandle));
        } else {
            updateWindowAttributesInternal(true);
            updateRefreshRateInternal(true);
        }
    }

    CocoaWindow::~CocoaWindow() {}

    void CocoaWindow::updateWindowAttributesInternal(bool forceSync) {
        auto update = ^{
            const CocoaWindowAttributes attributes = readAttributes(windowHandle);
            std::lock_guard<std::mutex> lock(attributesMutex);
            cachedAttributes = attributes;
        };
        if (forceSync) dispatch_sync(dispatch_get_main_queue(), update);
        else dispatch_async(dispatch_get_main_queue(), update);
    }

    void CocoaWindow::updateRefreshRateInternal(bool forceSync) {
        auto update = ^{ cachedRefreshRate.store(readRefreshRate(windowHandle)); };
        if (forceSync) dispatch_sync(dispatch_get_main_queue(), update);
        else dispatch_async(dispatch_get_main_queue(), update);
    }

    void CocoaWindow::getWindowAttributes(CocoaWindowAttributes* attributes) const {
        if ([NSThread isMainThread]) {
            const CocoaWindowAttributes fresh = readAttributes(windowHandle);
            std::lock_guard<std::mutex> lock(attributesMutex);
            const_cast<CocoaWindow*>(this)->cachedAttributes = fresh;
            *attributes = fresh;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(attributesMutex);
            *attributes = cachedAttributes;
        }
        const_cast<CocoaWindow*>(this)->updateWindowAttributesInternal(false);
    }

    int CocoaWindow::getRefreshRate() const {
        if ([NSThread isMainThread]) {
            const int rate = readRefreshRate(windowHandle);
            const_cast<CocoaWindow*>(this)->cachedRefreshRate.store(rate);
            return rate;
        }
        const int rate = cachedRefreshRate.load();
        const_cast<CocoaWindow*>(this)->updateRefreshRateInternal(false);
        return rate;
    }

    void CocoaWindow::toggleFullscreen() {}  // an iOS window always fills its screen
}
