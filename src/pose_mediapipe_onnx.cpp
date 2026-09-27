#include "pose_mediapipe.h"
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace sfr {
namespace {
Ort::Session load_detector(Ort::Env& env,const std::filesystem::path& file) {
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);options.SetInterOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    return Ort::Session(env,file.c_str(),options);
}
// Validate the pinned tensor contract before passing a camera frame to it.
class PoseNetwork {
public:
    PoseNetwork(Ort::Session session,std::vector<int64_t> shape,std::vector<std::vector<int64_t>> outputs)
        : session_(std::move(session)),shape_(std::move(shape)),output_shapes_(std::move(outputs)) {
        if(session_.GetInputCount()!=1) throw std::runtime_error("pose model input count");
        const auto info=session_.GetInputTypeInfo(0);
        const auto tensor=info.GetTensorTypeAndShapeInfo();
        if(tensor.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || tensor.GetShape()!=shape_)
            throw std::runtime_error("pose model input shape/type");
        Ort::AllocatorWithDefaultOptions allocator;
        input_=session_.GetInputNameAllocated(0,allocator).get();
        for(const auto& wanted : output_shapes_) {
            std::string name;
            for(size_t n=0;n<session_.GetOutputCount();++n) {
                const auto type=session_.GetOutputTypeInfo(n);
                const auto candidate=type.GetTensorTypeAndShapeInfo();
                if(candidate.GetElementType()==ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT && candidate.GetShape()==wanted) {
                    if(!name.empty()) throw std::runtime_error("ambiguous pose model output");
                    name=session_.GetOutputNameAllocated(n,allocator).get();
                }
            }
            if(name.empty()) throw std::runtime_error("missing pose model output");
            outputs_.push_back(std::move(name));
        }
    }
    std::vector<Ort::Value> run(std::vector<float>& data) {
        size_t expected=1;for(const auto dimension : shape_) expected*=size_t(dimension);
        if(data.size()!=expected) throw std::runtime_error("pose input buffer size");
        auto memory=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
        auto input=Ort::Value::CreateTensor<float>(memory,data.data(),data.size(),shape_.data(),shape_.size());
        const char* input_name=input_.c_str();
        std::vector<const char*> names;for(const auto& name : outputs_) names.push_back(name.c_str());
        auto values=session_.Run(Ort::RunOptions{nullptr},&input_name,&input,1,names.data(),names.size());
        for(size_t i=0;i<values.size();++i) {
            if(!values[i].IsTensor() || values[i].GetTensorTypeAndShapeInfo().GetShape()!=output_shapes_[i])
                throw std::runtime_error("pose output shape changed");
        }
        return values;
    }
private:
    Ort::Session session_;
    std::vector<int64_t> shape_;
    std::vector<std::vector<int64_t>> output_shapes_;
    std::string input_;
    std::vector<std::string> outputs_;
};
class MediaPipePose final : public PoseEstimator {
public:
    MediaPipePose(Ort::Env env,Ort::Session pose,const std::filesystem::path& detector)
        : environment_(std::move(env)),
          pose_(std::move(pose),{1,256,256,3},{{1,195},{1,1},{1,117}}),
          detector_(load_detector(environment_,detector),{1,3,224,224},{{1,2254,12},{1,2254,1}}) {
        if(const char* text=std::getenv("SFR_POSE_CONFIDENCE")) {
            char* end=nullptr;const float value=std::strtof(text,&end);
            if(end!=text && *end=='\0' && value>0 && value<1) threshold_=value;
        }
    }
    bool estimate(const CameraFrame& frame,PoseLandmarks& landmarks) override {
        if(!frame.width || !frame.height || frame.bgra.size()/4<size_t(frame.width)*frame.height) return false;
        if(frame.width!=width_ || frame.height!=height_) { region_.reset();width_=frame.width;height_=frame.height; }
        try {
            if(!region_) {
                const PoseRegion full{frame.width/2.0f,frame.height/2.0f,std::max(frame.width,frame.height)/2.0f,0};
                auto pixels=mediapipe_input(frame,full,true);
                auto values=detector_.run(pixels);
                region_=decode_person({values[0].GetTensorData<float>(),2254*12},
                    {values[1].GetTensorData<float>(),2254},frame.width,frame.height,threshold_);
                if(!region_) return false;
            }
            auto pixels=mediapipe_input(frame,*region_,false);
            auto values=pose_.run(pixels);
            std::optional<PoseRegion> next;
            const bool found=decode_mediapipe({values[0].GetTensorData<float>(),195},
                {values[2].GetTensorData<float>(),117},values[1].GetTensorData<float>()[0],*region_,threshold_,landmarks,next);
            if(trace_) {
                std::cerr<<"NATIVE_POSE_3D confidence="<<values[1].GetTensorData<float>()[0]
                    <<" found="<<found<<" crop="<<region_->x<<','<<region_->y<<','<<region_->radius<<','<<region_->angle;
                if(next) std::cerr<<" next="<<next->x<<','<<next->y<<','<<next->radius<<','<<next->angle;
                std::cerr<<'\n';
            }
            // Bad/lost crops trigger the detector on the next image, never a
            // second pose inference on this frame. No state survives loss.
            region_=found?next:std::nullopt;
            if(region_ && region_->radius>2*std::max(width_,height_)) region_.reset();
            return found;
        } catch(const std::exception& error) {
            region_.reset();
            if(!reported_) { std::cerr<<"NATIVE_POSE mediapipe-failed="<<error.what()<<'\n';reported_=true; }
            return false;
        }
    }
private:
    Ort::Env environment_;
    PoseNetwork pose_,detector_;
    std::optional<PoseRegion> region_;
    uint32_t width_=0,height_=0;
    float threshold_=.5f;
    bool reported_=false;
    bool trace_=[] {const char* text=std::getenv("SFR_POSE_TRACE");return text && *text=='1';}();
};
}
std::unique_ptr<PoseEstimator> open_mediapipe_pose(Ort::Env env,Ort::Session session,const std::filesystem::path& model) {
    try {
        auto detector=model.parent_path()/"person_detection_mediapipe_2023mar.onnx";
        if(const char* chosen=std::getenv("SFR_POSE_DETECTOR");chosen && *chosen) detector=chosen;
        auto result=std::make_unique<MediaPipePose>(std::move(env),std::move(session),detector);
        std::cerr<<"NATIVE_POSE model=\""<<model.string()<<"\" backend=mediapipe world=1 detector=\""<<detector.string()<<"\"\n";
        return result;
    } catch(const std::exception& error) {
        std::cerr<<"NATIVE_POSE unavailable="<<error.what()<<'\n';return nullptr;
    }
}
}
