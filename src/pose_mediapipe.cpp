#include "pose_mediapipe.h"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace sfr {
namespace {
float probability(float logit) { return 1/(1+std::exp(-std::clamp(logit,-80.0f,80.0f))); }
bool valid(const PoseRegion& r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.radius) && std::isfinite(r.angle)
        && std::abs(r.x)<100000 && std::abs(r.y)<100000 && r.radius>=1 && r.radius<100000;
}
PoseRegion region_from_points(float x,float y,float end_x,float end_y) {
    // MediaPipe's PoseDetectionToRoi and PoseLandmarksToRoi both apply the
    // 1.25 training margin. Omitting it shrinks tracked crops every frame.
    return {x,y,1.25f*std::hypot(end_x-x,end_y-y),
            std::remainder(std::numbers::pi_v<float>/2+std::atan2(end_y-y,end_x-x),2*std::numbers::pi_v<float>)};
}
std::array<float,2> source_point(const PoseRegion& r,float x,float y) {
    const float c=std::cos(r.angle),s=std::sin(r.angle);
    return {r.x+c*x-s*y,r.y+s*x+c*y};
}
}
std::array<float,2> person_anchor(size_t index) {
    for(const auto [grid,copies] : {std::pair{28,2},std::pair{14,2},std::pair{7,6}}) {
        const size_t count=size_t(grid)*grid*copies;
        if(index<count) { const size_t cell=index/copies; return {(float(cell%grid)+.5f)/grid,(float(cell/grid)+.5f)/grid}; }
        index-=count;
    }
    return {};
}
std::optional<PoseRegion> decode_person(std::span<const float> boxes,std::span<const float> scores,
                                       uint32_t width,uint32_t height,float threshold) {
    if(boxes.size()!=person_anchor_count*12 || scores.size()!=person_anchor_count || !width || !height) return {};
    // One player: the highest scoring valid detection survives NMS as well.
    float best=threshold;
    std::optional<PoseRegion> result;
    const float scale=float(std::max(width,height));
    for(size_t i=0;i<person_anchor_count;++i) {
        const float score=probability(scores[i]);
        if(!std::isfinite(scores[i]) || score<best) continue;
        const auto anchor=person_anchor(i);
        const auto point=[&](size_t at) { return std::array{
            (boxes[i*12+at]/224+anchor[0])*scale-(scale-width)/2,
            (boxes[i*12+at+1]/224+anchor[1])*scale-(scale-height)/2}; };
        const auto hip=point(4),full=point(6);
        const auto crop=region_from_points(hip[0],hip[1],full[0],full[1]);
        if(!valid(crop) || crop.radius>scale*2) continue;
        best=score;result=crop;
    }
    return result;
}
std::vector<float> mediapipe_input(const CameraFrame& frame,const PoseRegion& region,bool detector) {
    if(!frame.width || !frame.height || frame.bgra.size()/4<size_t(frame.width)*frame.height || !valid(region)) return {};
    const uint32_t size=detector?224:256;
    std::vector<float> out(size_t(size)*size*3);
    const float step=2*region.radius/size,c=std::cos(region.angle),s=std::sin(region.angle);
    for(uint32_t y=0;y<size;++y) for(uint32_t x=0;x<size;++x) {
        const float dx=(float(x)+.5f)*step-region.radius,dy=(float(y)+.5f)*step-region.radius;
        const float sx=region.x+c*dx-s*dy-.5f,sy=region.y+s*dx+c*dy-.5f;
        const int ix=int(std::floor(sx)),iy=int(std::floor(sy));
        const float fx=sx-ix,fy=sy-iy;
        for(uint32_t channel=0;channel<3;++channel) {
            const auto sample=[&](int xx,int yy) {
                // Detector padding is zero *after* normalization; pose padding is black.
                if(xx<0 || yy<0 || xx>=int(frame.width) || yy>=int(frame.height)) return 0.0f;
                const float value=frame.bgra[(size_t(yy)*frame.width+xx)*4+(2-channel)]/255.0f;
                return detector?value*2-1:value;
            };
            const float upper=std::lerp(sample(ix,iy),sample(ix+1,iy),fx);
            const float lower=std::lerp(sample(ix,iy+1),sample(ix+1,iy+1),fx);
            const size_t at=detector?size_t(channel)*size*size+y*size+x:(size_t(y)*size+x)*3+channel;
            out[at]=std::lerp(upper,lower,fy);
        }
    }
    return out;
}
bool decode_mediapipe(std::span<const float> image,std::span<const float> world,float confidence,
                      const PoseRegion& region,float threshold,PoseLandmarks& output,std::optional<PoseRegion>& next) {
    next.reset();
    if(image.size()!=195 || world.size()!=117 || !std::isfinite(confidence) || confidence<threshold || !valid(region)) return false;
    constexpr uint32_t indices[pose_point::count]={0,2,5,7,8,11,12,13,14,15,16,23,24,25,26,27,28};
    PoseLandmarks landmarks{};
    const float scale=2*region.radius/256,c=std::cos(region.angle),s=std::sin(region.angle);
    for(uint32_t i=0;i<pose_point::count;++i) {
        const size_t n=indices[i];
        for(size_t j=0;j<5;++j) if(!std::isfinite(image[n*5+j])) return false;
        for(size_t j=0;j<3;++j) if(!std::isfinite(world[n*3+j]) || std::abs(world[n*3+j])>10) return false;
        const auto xy=source_point(region,(image[n*5]-128)*scale,(image[n*5+1]-128)*scale);
        auto& p=landmarks[i];p.x=xy[0];p.y=xy[1];
        p.score=std::min(probability(image[n*5+3]),probability(image[n*5+4]));
        // Undo the crop's rotation for world XY only; model Z is already metric.
        p.world={c*world[n*3]-s*world[n*3+1],s*world[n*3]+c*world[n*3+1],world[n*3+2]};
        p.has_world=true;
    }
    for(const auto n : {pose_point::shoulder_left,pose_point::shoulder_right,pose_point::hip_left,pose_point::hip_right})
        if(landmarks[n].score<threshold) return false;
    // Auxiliary landmarks 33/34 predict the next hip-centred full-body crop.
    const auto hip=source_point(region,(image[33*5]-128)*scale,(image[33*5+1]-128)*scale);
    const auto full=source_point(region,(image[34*5]-128)*scale,(image[34*5+1]-128)*scale);
    const auto crop=region_from_points(hip[0],hip[1],full[0],full[1]);
    if(valid(crop) && crop.radius>region.radius*.5f && crop.radius<region.radius*2) next=crop;
    output=landmarks;
    return true;
}
}
