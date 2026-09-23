// The webcam on Windows, through Media Foundation's source reader. The
// reader is asked to deliver BGRA (it inserts a converter for whatever the
// camera offers); a camera that cannot is read in its own format and
// converted here.
#include "camera_capture.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace sfr {
namespace {
using Microsoft::WRL::ComPtr;

// Media Foundation starts and stops once for the process.
struct MediaFoundation {
    bool ready = false;
    MediaFoundation() { ready = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)); }
    ~MediaFoundation() { if (ready) MFShutdown(); }
};

class Win32Camera final : public CameraCapture {
public:
    Win32Camera(ComPtr<IMFSourceReader> reader, CameraPixels format, uint32_t width, uint32_t height, uint32_t stride)
        : reader_(std::move(reader)), format_(format), width_(width), height_(height), stride_(stride) {
        // Read on a thread of its own: a read blocks until the camera has a
        // picture, and the game asks for one whenever it draws.
        worker_ = std::jthread([this](std::stop_token stop) { read_frames(stop); });
    }

    bool next(CameraFrame& frame) override {
        std::lock_guard guard(lock_);
        if (latest_.number == taken_) return false;
        frame = latest_;
        taken_ = latest_.number;
        return true;
    }

private:
    void read_frames(std::stop_token stop) {
        uint64_t number = 0;
        while (!stop.stop_requested()) {
            DWORD stream_index = 0, flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            if (FAILED(reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream_index, &flags, &timestamp,
                                           sample.GetAddressOf())))
                break;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (!sample) continue;  // a timeout or a format change
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf()))) continue;
            BYTE* bytes = nullptr;
            DWORD length = 0;
            if (FAILED(buffer->Lock(&bytes, nullptr, &length))) continue;
            CameraFrame converted;
            const bool ok = convert_camera_pixels(format_, {bytes, length}, width_, height_, stride_, converted);
            buffer->Unlock();
            if (!ok) continue;
            converted.number = ++number;
            std::lock_guard guard(lock_);
            latest_ = std::move(converted);
        }
    }

    ComPtr<IMFSourceReader> reader_;
    CameraPixels format_;
    uint32_t width_, height_, stride_;
    std::mutex lock_;
    CameraFrame latest_;
    uint64_t taken_ = 0;
    std::jthread worker_;
};

// The format a reader settled on, as this file understands it.
bool pixels_of(const GUID& subtype, CameraPixels& format) {
    if (subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32) format = CameraPixels::bgra;
    else if (subtype == MFVideoFormat_YUY2) format = CameraPixels::yuy2;
    else if (subtype == MFVideoFormat_NV12) format = CameraPixels::nv12;
    else return false;
    return true;
}
}

std::unique_ptr<CameraCapture> CameraCapture::open(uint32_t width, uint32_t height) {
    static MediaFoundation media;
    if (!media.ready) {
        std::cerr << "NATIVE_CAMERA unavailable=media-foundation\n";
        return nullptr;
    }
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(attributes.GetAddressOf(), 1)) ||
        FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
        return nullptr;
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attributes.Get(), &devices, &count)) || !count) {
        if (devices) CoTaskMemFree(devices);
        std::cerr << "NATIVE_CAMERA unavailable=no-device\n";
        return nullptr;
    }
    // SFR_CAMERA_DEVICE=N picks another of the cameras the host lists.
    uint32_t wanted = 0;
    if (const char* text = std::getenv("SFR_CAMERA_DEVICE"); text && *text) wanted = uint32_t(std::strtoul(text, nullptr, 10));
    if (wanted >= count) wanted = 0;
    ComPtr<IMFMediaSource> source;
    const HRESULT activated = devices[wanted]->ActivateObject(IID_PPV_ARGS(source.GetAddressOf()));
    std::wstring name;
    {
        WCHAR* friendly = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(devices[wanted]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendly, &length))) {
            name.assign(friendly, length);
            CoTaskMemFree(friendly);
        }
    }
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);
    if (FAILED(activated)) return nullptr;

    ComPtr<IMFAttributes> reader_attributes;
    if (FAILED(MFCreateAttributes(reader_attributes.GetAddressOf(), 1)) ||
        FAILED(reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE)))
        return nullptr;
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), reader_attributes.Get(), reader.GetAddressOf())))
        return nullptr;

    // Ask for BGRA at the size wanted; the reader converts and scales where
    // it can, and otherwise keeps what the camera offers.
    ComPtr<IMFMediaType> wanted_type;
    if (SUCCEEDED(MFCreateMediaType(wanted_type.GetAddressOf()))) {
        wanted_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        wanted_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        MFSetAttributeSize(wanted_type.Get(), MF_MT_FRAME_SIZE, width, height);
        reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, wanted_type.Get());
    }
    ComPtr<IMFMediaType> settled;
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, settled.GetAddressOf())))
        return nullptr;
    GUID subtype{};
    UINT32 actual_width = 0, actual_height = 0;
    CameraPixels format = CameraPixels::bgra;
    if (FAILED(settled->GetGUID(MF_MT_SUBTYPE, &subtype)) || !pixels_of(subtype, format) ||
        FAILED(MFGetAttributeSize(settled.Get(), MF_MT_FRAME_SIZE, &actual_width, &actual_height))) {
        std::cerr << "NATIVE_CAMERA unavailable=unsupported-format\n";
        return nullptr;
    }
    // A contiguous buffer is packed unless the camera says otherwise; a
    // negative stride would mean a bottom-up picture, which the converter
    // does not handle, so those are refused.
    INT32 stride = 0;
    if (SUCCEEDED(settled->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride))) && stride < 0) {
        std::cerr << "NATIVE_CAMERA unavailable=bottom-up\n";
        return nullptr;
    }
    std::wcerr << L"NATIVE_CAMERA device=\"" << name << L"\"";
    std::cerr << " size=" << actual_width << 'x' << actual_height << " format="
              << (format == CameraPixels::bgra ? "bgra" : format == CameraPixels::yuy2 ? "yuy2" : "nv12")
              << " stride=" << stride << '\n';
    return std::make_unique<Win32Camera>(std::move(reader), format, actual_width, actual_height, uint32_t(stride));
}
}
