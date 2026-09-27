#include "pose_mediapipe.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
namespace {
void require(bool ok,const char* reason) { if(!ok) throw std::runtime_error(reason); }
bool near(float a,float b) { return std::abs(a-b)<0.001f; }
void detector() {
    require(near(sfr::person_anchor(0)[0],0.5f/28),"first detector anchor centre");
    require(sfr::person_anchor(0)==sfr::person_anchor(1),"two anchors at first grid cell");
    require(near(sfr::person_anchor(1568)[0],0.5f/14),"second detector stride");
    require(near(sfr::person_anchor(2253)[0],6.5f/7),"last detector stride");
    std::vector<float> boxes(2254*12),scores(2254,-100);
    const size_t index=800;
    const auto a=sfr::person_anchor(index);scores[index]=10;
    boxes[index*12+4]=(0.5f-a[0])*224;boxes[index*12+5]=(0.5f-a[1])*224;
    boxes[index*12+6]=(0.5f-a[0])*224;boxes[index*12+7]=(0.125f-a[1])*224;
    auto region=sfr::decode_person(boxes,scores,640,480,.5f);
    require(region && near(region->x,320) && near(region->y,240),"detector removes landscape letterbox padding");
    require(near(region->radius,300) && near(region->angle,0),"hip/full-body landmarks define upright crop");
    scores[index]=-100;
    require(!sfr::decode_person(boxes,scores,640,480,.5f),"no confident person is not a crop");
    require(!sfr::decode_person({},scores,640,480,.5f),"refuse malformed tensor");
}
void pixels() {
    sfr::CameraFrame f;f.width=f.height=2;f.bgra={0,0,255,255,0,0,255,255,0,0,255,255,0,0,255,255};
    const sfr::PoseRegion region{1,1,1,0};
    auto pose=sfr::mediapipe_input(f,region,false);
    require(pose.size()==256*256*3,"pose input is NHWC RGB");
    const size_t p=(128*256+128)*3;
    require(near(pose[p],1) && near(pose[p+1],0) && near(pose[p+2],0),"BGRA converts to normalized RGB");
    auto detector=sfr::mediapipe_input(f,region,true);
    const size_t d=112*224+112;
    require(detector.size()==224*224*3 && near(detector[d],1) && near(detector[224*224+d],-1),"detector input has different layout and range");
    f.bgra.clear();require(sfr::mediapipe_input(f,region,false).empty(),"reject incomplete camera frames");
}
void world_decode() {
    std::vector<float> image(195,0),world(117,0);
    for(size_t i=0;i<39;++i) {image[i*5]=128;image[i*5+1]=128;image[i*5+3]=image[i*5+4]=10;}
    // Right wrist: a forward reach, independently of the 2D picture position.
    world[16*3]=.2f;world[16*3+1]=-.1f;world[16*3+2]=-.4f;
    image[33*5]=128;image[33*5+1]=128;image[34*5]=128;image[34*5+1]=0;
    sfr::PoseLandmarks out{};std::optional<sfr::PoseRegion> next;
    const sfr::PoseRegion crop{320,240,240,0};
    require(sfr::decode_mediapipe(image,world,.9f,crop,.5f,out,next),"confident 3D output decodes");
    const auto& wrist=out[sfr::pose_point::wrist_right];
    require(wrist.has_world && near(wrist.world[2],-.4f),"retain world Z in metres, not crop-scaled or flattened");
    require(near(wrist.x,320) && near(wrist.y,240),"map 2D crop coordinates to source pixels");
    require(next && near(next->radius,300) && near(next->angle,0),"auxiliary points include training margin to prevent shrinking next-frame crop");
    require(sfr::decode_mediapipe(image,world,.9f,{320,240,240,std::numbers::pi_v<float>/2},.5f,out,next),"rotated body decodes");
    require(near(wrist.world[0],.1f) && near(wrist.world[1],.2f) && near(wrist.world[2],-.4f),"undo crop rotation for XY without rotating Z");
    world[16*3+2]=std::numeric_limits<float>::quiet_NaN();
    const auto old=out[sfr::pose_point::wrist_right].world;
    require(!sfr::decode_mediapipe(image,world,.9f,crop,.5f,out,next),"invalid depth is not sent to the game");
    require(out[sfr::pose_point::wrist_right].world==old && !next,"failed decode leaves pose intact and clears tracking crop");
    world[16*3+2]=-.4f;image[11*5+4]=-10;
    require(!sfr::decode_mediapipe(image,world,.9f,crop,.5f,out,next),"missing torso rejects full body");
}
}
int main() { try {detector();pixels();world_decode();std::cout<<"MediaPipe decode checks passed\n";} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
