// The webcam on macOS and the device's own cameras on iOS, through
// AVFoundation. The capture output is asked for BGRA, so pictures arrive in
// the frame's own order and only the rows are copied.
#include "camera_capture.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sfr {
namespace {
// The newest picture the capture queue handed over, waiting for next().
struct LatestPicture {
    std::mutex lock;
    CameraFrame frame;
    uint64_t number = 0, taken = 0;
};
}
}

@interface SfrCameraReceiver : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
- (instancetype)initWithLatest:(std::shared_ptr<sfr::LatestPicture>)latest;
@end

@implementation SfrCameraReceiver {
    std::shared_ptr<sfr::LatestPicture> _latest;
}

- (instancetype)initWithLatest:(std::shared_ptr<sfr::LatestPicture>)latest {
    if ((self = [super init])) _latest = std::move(latest);
    return self;
}

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection {
    CVImageBufferRef image = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!image || CVPixelBufferGetPixelFormatType(image) != kCVPixelFormatType_32BGRA) return;
    if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    const uint32_t width = uint32_t(CVPixelBufferGetWidth(image)), height = uint32_t(CVPixelBufferGetHeight(image));
    const size_t stride = CVPixelBufferGetBytesPerRow(image);
    const auto* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));
    if (base && width && height) {
        std::lock_guard guard(_latest->lock);
        if (sfr::convert_camera_pixels(sfr::CameraPixels::bgra, {base, stride * height}, width, height,
                                       uint32_t(stride), _latest->frame))
            _latest->frame.number = ++_latest->number;
    }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
}
@end

namespace sfr {
namespace {
// What the host offers to capture from: on a Mac its own camera, USB
// webcams and an iPhone as Continuity Camera; on iOS each lens on its own
// (so the ultra-wide, which lets the player stand closest, can be chosen).
NSArray<AVCaptureDevice*>* capture_devices() {
    NSMutableArray<AVCaptureDeviceType>* types = [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
#if TARGET_OS_IOS
    [types addObject:AVCaptureDeviceTypeBuiltInUltraWideCamera];
    [types addObject:AVCaptureDeviceTypeBuiltInTelephotoCamera];
    if (@available(iOS 17.0, *)) [types addObject:AVCaptureDeviceTypeExternal];  // a USB camera on an iPad
#else
    if (@available(macOS 14.0, *)) {
        [types addObject:AVCaptureDeviceTypeExternal];
        [types addObject:AVCaptureDeviceTypeContinuityCamera];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [types addObject:AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
    }
#endif
    AVCaptureDeviceDiscoverySession* discovery =
        [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                               mediaType:AVMediaTypeVideo
                                                                position:AVCaptureDevicePositionUnspecified];
    return discovery.devices;
}

std::string device_name(AVCaptureDevice* device) {
    const char* text = device.localizedName.UTF8String;
    return text ? text : "Camera";
}

// Whether the player lets the game use the camera, asking the first time.
// The system shows the question itself; a wait on the main thread keeps its
// run loop turning, which iOS needs to show it.
bool camera_allowed() {
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
    case AVAuthorizationStatusAuthorized: return true;
    case AVAuthorizationStatusDenied:
    case AVAuthorizationStatusRestricted: return false;
    default: break;
    }
    dispatch_semaphore_t answered = dispatch_semaphore_create(0);
    __block bool granted = false;
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                             completionHandler:^(BOOL allowed) {
                                 granted = allowed;
                                 dispatch_semaphore_signal(answered);
                             }];
    if ([NSThread isMainThread]) {
        while (dispatch_semaphore_wait(answered, DISPATCH_TIME_NOW) != 0)
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    } else {
        dispatch_semaphore_wait(answered, DISPATCH_TIME_FOREVER);
    }
    return granted;
}

class AppleCamera final : public CameraCapture {
public:
    AppleCamera(AVCaptureSession* session, SfrCameraReceiver* receiver, dispatch_queue_t queue,
                std::shared_ptr<LatestPicture> latest)
        : session_(session), receiver_(receiver), queue_(queue), latest_(std::move(latest)) {}

    ~AppleCamera() override {
        [session_ stopRunning];
        // The last picture handed over may still be in the receiver.
        dispatch_sync(queue_, ^{});
    }

    bool next(CameraFrame& frame) override {
        std::lock_guard guard(latest_->lock);
        if (latest_->number == latest_->taken) return false;
        latest_->taken = latest_->number;
        frame = latest_->frame;
        return true;
    }

private:
    AVCaptureSession* session_;
    SfrCameraReceiver* receiver_;
    dispatch_queue_t queue_;
    std::shared_ptr<LatestPicture> latest_;
};
}

std::vector<std::string> CameraCapture::devices() {
    std::vector<std::string> names;
    @autoreleasepool {
        for (AVCaptureDevice* device in capture_devices()) names.push_back(device_name(device));
    }
    return names;
}

std::unique_ptr<CameraCapture> CameraCapture::open(uint32_t width, uint32_t height, const std::string& wanted_device) {
    @autoreleasepool {
        // Listing the cameras needs no permission, so a Mac or iPad without
        // one is never asked about it.
        NSArray<AVCaptureDevice*>* found = capture_devices();
        if (found.count == 0) {
            std::cerr << "NATIVE_CAMERA apple=no-camera\n";
            return nullptr;
        }
        if (!camera_allowed()) {
            std::cerr << "NATIVE_CAMERA apple=not-allowed\n";
            return nullptr;
        }
        std::vector<std::string> names;
        for (AVCaptureDevice* device in found) names.push_back(device_name(device));
        AVCaptureDevice* device = found[chosen_camera_device(names, wanted_device)];

        NSError* error = nil;
        AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
        if (!input) {
            std::cerr << "NATIVE_CAMERA apple=no-input device=\"" << device_name(device) << "\" error=\""
                      << (error.localizedDescription.UTF8String ?: "") << "\"\n";
            return nullptr;
        }
        AVCaptureSession* session = [[AVCaptureSession alloc] init];
        [session beginConfiguration];
        // 640x480 when the camera offers it; otherwise its own size, which
        // the pose estimator scales down anyway.
        if (width == 640 && height == 480 && [session canSetSessionPreset:AVCaptureSessionPreset640x480])
            session.sessionPreset = AVCaptureSessionPreset640x480;
        if (![session canAddInput:input]) {
            [session commitConfiguration];
            std::cerr << "NATIVE_CAMERA apple=input-refused\n";
            return nullptr;
        }
        [session addInput:input];

        AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
        output.videoSettings = @{(NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
        output.alwaysDiscardsLateVideoFrames = YES;
        auto latest = std::make_shared<LatestPicture>();
        SfrCameraReceiver* receiver = [[SfrCameraReceiver alloc] initWithLatest:latest];
        dispatch_queue_t queue = dispatch_queue_create("sfr.camera", DISPATCH_QUEUE_SERIAL);
        [output setSampleBufferDelegate:receiver queue:queue];
        if (![session canAddOutput:output]) {
            [session commitConfiguration];
            std::cerr << "NATIVE_CAMERA apple=output-refused\n";
            return nullptr;
        }
        [session addOutput:output];
        AVCaptureConnection* connection = [output connectionWithMediaType:AVMediaTypeVideo];
#if TARGET_OS_IOS
        // The game runs in landscape: the picture stands the way the player
        // does when it is turned as the screen is. A front camera's picture
        // is not mirrored, as with Camera2 on Android.
        if (@available(iOS 17.0, *)) {
            AVCaptureDeviceRotationCoordinator* rotation =
                [[AVCaptureDeviceRotationCoordinator alloc] initWithDevice:device previewLayer:nil];
            const CGFloat angle = rotation.videoRotationAngleForHorizonLevelCapture;
            if ([connection isVideoRotationAngleSupported:angle]) connection.videoRotationAngle = angle;
        } else if (connection.isVideoOrientationSupported) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            connection.videoOrientation = AVCaptureVideoOrientationLandscapeRight;
#pragma clang diagnostic pop
        }
        if (connection.isVideoMirroringSupported) {
            connection.automaticallyAdjustsVideoMirroring = NO;
            connection.videoMirrored = NO;
        }
#endif
        [session commitConfiguration];
        [session startRunning];
        if (!session.isRunning) {
            std::cerr << "NATIVE_CAMERA apple=not-running\n";
            return nullptr;
        }
        const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(device.activeFormat.formatDescription);
        std::cerr << "NATIVE_CAMERA device=\"" << device_name(device) << "\" size=" << size.width << 'x' << size.height
                  << " format=bgra backend=avfoundation\n";
        return std::make_unique<AppleCamera>(session, receiver, queue, std::move(latest));
    }
}
}
