// The pose estimate with ONNX Runtime and the pinned RTMPose model
// (tools/onnx, Apache 2.0). The whole picture is the box: the player stands
// in front of the camera, so no person detector is run.
#include "pose_estimator.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace sfr {
namespace {
constexpr uint32_t model_width = 192, model_height = 256;
// RTMPose's SimCC head: the score rows are twice the input's pixels.
constexpr float simcc_split_ratio = 2.0f;

class OnnxPose final : public PoseEstimator {
public:
    OnnxPose(Ort::Env environment, Ort::Session session, std::string input_name, std::string x_name,
             std::string y_name)
        : environment_(std::move(environment)), session_(std::move(session)), input_name_(std::move(input_name)),
          x_name_(std::move(x_name)), y_name_(std::move(y_name)),
          input_(size_t(model_width) * model_height * 3) {}

    bool estimate(const CameraFrame& frame, PoseLandmarks& landmarks) override {
        if (!frame.width || !frame.height || frame.bgra.size() < size_t(frame.width) * frame.height * 4) return false;
        const PoseCrop crop = pose_crop(frame.width, frame.height, model_width, model_height);
        fill_input(frame, crop);

        const std::array<int64_t, 4> shape{1, 3, model_height, model_width};
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(memory, input_.data(), input_.size(), shape.data(),
                                                          shape.size());
        const char* const inputs[] = {input_name_.c_str()};
        const char* const outputs[] = {x_name_.c_str(), y_name_.c_str()};
        std::vector<Ort::Value> results;
        try {
            results = session_.Run(Ort::RunOptions{nullptr}, inputs, &input, 1, outputs, 2);
        } catch (const Ort::Exception& error) {
            static bool reported = false;
            if (!reported) { reported = true; std::cerr << "NATIVE_POSE failed=" << error.what() << '\n'; }
            return false;
        }
        if (results.size() != 2) return false;
        const auto x_shape = results[0].GetTensorTypeAndShapeInfo().GetShape();
        const auto y_shape = results[1].GetTensorTypeAndShapeInfo().GetShape();
        if (x_shape.size() != 3 || y_shape.size() != 3) return false;
        decode_simcc(results[0].GetTensorData<float>(), results[1].GetTensorData<float>(), uint32_t(x_shape[1]),
                     uint32_t(x_shape[2]), uint32_t(y_shape[2]), simcc_split_ratio, crop, landmarks);
        // Something was found when the shoulders and hips are believable: a
        // body only half in the picture is worse than no body at all.
        const float torso = (std::min)((std::min)(landmarks[pose_point::shoulder_left].score,
                                                  landmarks[pose_point::shoulder_right].score),
                                       (std::min)(landmarks[pose_point::hip_left].score,
                                                  landmarks[pose_point::hip_right].score));
        return torso > confidence_;
    }

private:
    void fill_input(const CameraFrame& frame, const PoseCrop& crop) {
        // Nearest neighbour: the model's input is small and the camera's
        // pictures are noisy enough that a smoother sample buys nothing.
        constexpr float mean[3] = {123.675f, 116.28f, 103.53f};      // RGB
        constexpr float inverse[3] = {1.0f / 58.395f, 1.0f / 57.12f, 1.0f / 57.375f};
        const uint32_t plane = model_width * model_height;
        for (uint32_t y = 0; y < model_height; ++y) {
            const float source_y = crop.origin_y + (float(y) + 0.5f) * crop.scale_y;
            const int row = (std::clamp)(int(source_y), 0, int(frame.height) - 1);
            for (uint32_t x = 0; x < model_width; ++x) {
                const float source_x = crop.origin_x + (float(x) + 0.5f) * crop.scale_x;
                const int column = (std::clamp)(int(source_x), 0, int(frame.width) - 1);
                const uint8_t* pixel = frame.bgra.data() + (size_t(row) * frame.width + column) * 4;
                const float rgb[3] = {float(pixel[2]), float(pixel[1]), float(pixel[0])};
                for (uint32_t channel = 0; channel < 3; ++channel)
                    input_[size_t(channel) * plane + size_t(y) * model_width + x] =
                        (rgb[channel] - mean[channel]) * inverse[channel];
            }
        }
    }

    Ort::Env environment_;
    Ort::Session session_;
    std::string input_name_, x_name_, y_name_;
    std::vector<float> input_;
    float confidence_ = [] {
        const char* text = std::getenv("SFR_POSE_CONFIDENCE");
        const double value = text ? std::strtod(text, nullptr) : 0.0;
        return float(value > 0 ? value : 0.3);
    }();
};
}

std::filesystem::path PoseEstimator::default_model() {
    if (const char* chosen = std::getenv("SFR_POSE_MODEL"); chosen && *chosen) return chosen;
    namespace fs = std::filesystem;
    std::error_code error;
    // Beside the program in a release, or in the checkout while developing.
    for (const fs::path candidate : {fs::path("pose/rtmpose.onnx"), fs::path("tools/onnx/rtmpose/end2end.onnx")})
        if (fs::is_regular_file(candidate, error)) return candidate;
    return "pose/rtmpose.onnx";
}

std::unique_ptr<PoseEstimator> PoseEstimator::open(const std::filesystem::path& model) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(model, error)) {
        std::cerr << "NATIVE_POSE unavailable=no-model path=" << model.string() << '\n';
        return nullptr;
    }
    try {
        Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "sfr-pose");
        Ort::SessionOptions options;
        // One thread: this runs beside the game, which wants the cores.
        options.SetIntraOpNumThreads(1);
        options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        Ort::Session session(environment, model.wstring().c_str(), options);
#else
        Ort::Session session(environment, model.string().c_str(), options);
#endif
        Ort::AllocatorWithDefaultOptions allocator;
        if (session.GetInputCount() != 1 || session.GetOutputCount() != 2) {
            std::cerr << "NATIVE_POSE unavailable=unexpected-model inputs=" << session.GetInputCount()
                      << " outputs=" << session.GetOutputCount() << '\n';
            return nullptr;
        }
        const auto input_name = session.GetInputNameAllocated(0, allocator);
        const auto x_name = session.GetOutputNameAllocated(0, allocator);
        const auto y_name = session.GetOutputNameAllocated(1, allocator);
        std::cerr << "NATIVE_POSE model=\"" << model.string() << "\" input=" << input_name.get()
                  << " outputs=" << x_name.get() << ',' << y_name.get() << '\n';
        return std::make_unique<OnnxPose>(std::move(environment), std::move(session), input_name.get(), x_name.get(),
                                          y_name.get());
    } catch (const Ort::Exception& error) {
        std::cerr << "NATIVE_POSE unavailable=" << error.what() << '\n';
        return nullptr;
    }
}
}
