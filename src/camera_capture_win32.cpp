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
#include <strmif.h>

#include <atomic>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>

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

// The controls the player set, kept beside the game (camera_controls.txt).
std::filesystem::path saved_controls_path() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return "camera_controls.txt";
    return std::filesystem::path(path).parent_path() / "camera_controls.txt";
}
std::mutex saved_controls_lock;
std::vector<SavedCameraControl> read_saved_controls() {
    std::ifstream file(saved_controls_path(), std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return parse_camera_controls(text.str());
}

// UVC's controls behind a Media Foundation camera: IAMCameraControl for
// zoom and exposure, IAMVideoProcAmp for gain (the same flags: 1 automatic,
// 2 manual).
class Win32Camera final : public CameraCapture {
public:
    Win32Camera(ComPtr<IMFSourceReader> reader, ComPtr<IMFMediaSource> source, std::string device, CameraPixels format,
                uint32_t width, uint32_t height, uint32_t stride)
        : reader_(std::move(reader)), device_(std::move(device)), format_(format), width_(width), height_(height),
          stride_(stride) {
        source.As(&camera_control_);
        source.As(&proc_amp_);
        // What the player set for this camera last time.
        std::vector<SavedCameraControl> saved;
        { std::lock_guard guard(saved_controls_lock); saved = read_saved_controls(); }
        for (const SavedCameraControl& setting : saved)
            if (setting.device == device_) {
                const bool applied = apply(setting.control, setting.value);
                std::cerr << "NATIVE_CAMERA_CONTROL restored " << camera_control_name(setting.control) << '='
                          << (setting.value.automatic ? std::string("auto") : std::to_string(setting.value.value))
                          << " applied=" << applied << '\n';
            }
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

    bool control_range(CameraControl control, CameraControlRange& range) override {
        long minimum = 0, maximum = 0, step = 0, standard = 0, flags = 0;
        HRESULT result = E_NOINTERFACE;
        if (control == CameraControl::gain) {
            if (proc_amp_) result = proc_amp_->GetRange(VideoProcAmp_Gain, &minimum, &maximum, &step, &standard, &flags);
        } else if (camera_control_) {
            result = camera_control_->GetRange(control == CameraControl::zoom ? CameraControl_Zoom : CameraControl_Exposure,
                                               &minimum, &maximum, &step, &standard, &flags);
        }
        if (FAILED(result) || maximum <= minimum) return false;
        range = {minimum, maximum, step > 0 ? step : 1, standard, (flags & 1) != 0};
        return true;
    }

    bool control(CameraControl control, CameraControlValue& value) override {
        long number = 0, flags = 0;
        HRESULT result = E_NOINTERFACE;
        if (control == CameraControl::gain) {
            if (proc_amp_) result = proc_amp_->Get(VideoProcAmp_Gain, &number, &flags);
        } else if (camera_control_) {
            result = camera_control_->Get(control == CameraControl::zoom ? CameraControl_Zoom : CameraControl_Exposure,
                                          &number, &flags);
        }
        if (FAILED(result)) return false;
        value = {number, (flags & 1) != 0};
        return true;
    }

    bool set_control(CameraControl control, const CameraControlValue& value) override {
        if (!apply(control, value)) return false;
        std::lock_guard guard(saved_controls_lock);
        std::vector<SavedCameraControl> saved = read_saved_controls();
        remember_camera_control(saved, {device_, control, value});
        std::ofstream(saved_controls_path(), std::ios::binary | std::ios::trunc) << format_camera_controls(saved);
        std::cerr << "NATIVE_CAMERA_CONTROL set " << camera_control_name(control) << '='
                  << (value.automatic ? std::string("auto") : std::to_string(value.value)) << '\n';
        return true;
    }

private:
    bool apply(CameraControl control, const CameraControlValue& value) {
        // Automatic keeps the value the camera has; held needs the value.
        CameraControlValue now{};
        long number = value.value;
        if (value.automatic && this->control(control, now)) number = now.value;
        const long flags = value.automatic ? 1 : 2;
        if (control == CameraControl::gain) return proc_amp_ && SUCCEEDED(proc_amp_->Set(VideoProcAmp_Gain, number, flags));
        return camera_control_ &&
               SUCCEEDED(camera_control_->Set(control == CameraControl::zoom ? CameraControl_Zoom : CameraControl_Exposure,
                                              number, flags));
    }

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
    ComPtr<IAMCameraControl> camera_control_;
    ComPtr<IAMVideoProcAmp> proc_amp_;
    std::string device_;
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

namespace {
// The capture devices Media Foundation lists, with their friendly names.
// The caller releases nothing: the activators are freed here.
bool for_each_device(const std::function<void(uint32_t, IMFActivate&, const std::wstring&)>& visit) {
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(attributes.GetAddressOf(), 1)) ||
        FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
        return false;
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attributes.Get(), &devices, &count)) || !count) {
        if (devices) CoTaskMemFree(devices);
        return false;
    }
    for (UINT32 i = 0; i < count; ++i) {
        std::wstring name;
        WCHAR* friendly = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendly, &length))) {
            name.assign(friendly, length);
            CoTaskMemFree(friendly);
        }
        visit(i, *devices[i], name);
    }
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);
    return true;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size_t(length), 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}
}

std::vector<std::string> CameraCapture::devices() {
    static MediaFoundation media;
    std::vector<std::string> names;
    if (!media.ready) return names;
    for_each_device([&](uint32_t, IMFActivate&, const std::wstring& name) { names.push_back(narrow(name)); });
    return names;
}

std::unique_ptr<CameraCapture> CameraCapture::open(uint32_t width, uint32_t height, const std::string& wanted_device) {
    static MediaFoundation media;
    if (!media.ready) {
        std::cerr << "NATIVE_CAMERA unavailable=media-foundation\n";
        return nullptr;
    }
    // SFR_CAMERA_DEVICE names the camera to open, out of the same list the
    // launcher shows. The list is walked once here, so the name that matches
    // belongs to the activator that is opened, however Media Foundation
    // happened to order them this time.
    std::vector<std::string> names;
    std::vector<ComPtr<IMFActivate>> activators;
    for_each_device([&](uint32_t, IMFActivate& device, const std::wstring& listed) {
        names.push_back(narrow(listed));
        activators.emplace_back(&device);
    });
    if (activators.empty()) {
        std::cerr << "NATIVE_CAMERA unavailable=no-device" << std::endl;
        return nullptr;
    }
    const size_t wanted = chosen_camera_device(names, wanted_device);
    const std::string chosen = names[wanted];
    ComPtr<IMFMediaSource> source;
    if (FAILED(activators[wanted]->ActivateObject(IID_PPV_ARGS(source.GetAddressOf())))) {
        std::cerr << "NATIVE_CAMERA unavailable=busy device=" << chosen << std::endl;
        return nullptr;
    }

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
    std::cerr << "NATIVE_CAMERA device=" << chosen << " of " << names.size();
    std::cerr << " size=" << actual_width << 'x' << actual_height << " format="
              << (format == CameraPixels::bgra ? "bgra" : format == CameraPixels::yuy2 ? "yuy2" : "nv12")
              << " stride=" << stride << '\n';
    return std::make_unique<Win32Camera>(std::move(reader), std::move(source), chosen, format, actual_width, actual_height,
                                         uint32_t(stride));
}
}
