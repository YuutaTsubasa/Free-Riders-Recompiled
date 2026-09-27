#include "camera_debug.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <array>
namespace {
void require(bool ok,const char* text) { if(!ok) throw std::runtime_error(text); }
void states() {
    using namespace sfr;
    const auto now=CameraDebugClock::time_point{}+std::chrono::seconds(10);
    CameraDebugFrame f;
    require(camera_debug_state(f,now)==CameraDebugState::waiting,"wait for the first guest frame");
    f.game_frame=1; f.submitted=now;
    require(camera_debug_state(f,now)==CameraDebugState::unavailable,"camera/model failure must be visible");
    f.camera_available=true;
    require(camera_debug_state(f,now)==CameraDebugState::waiting,"do not invent a camera skeleton before detection");
    f.has_pose=f.detected=true; f.observation_age_ms=f.pose_age_ms=0;
    require(camera_debug_state(f,now)==CameraDebugState::live,"delivered fresh camera skeleton is live");
    f.detected=false; f.pose_age_ms=200;
    require(camera_debug_state(f,now)==CameraDebugState::held,"lost detection must label the pose retained by the game");
    f.detected=true; f.observation_age_ms=700;
    require(camera_debug_state(f,now)==CameraDebugState::held,"stopped capture is not live even if last detection succeeded");
    require(camera_debug_state(f,now+std::chrono::seconds(1))==CameraDebugState::stale,"stopped guest submissions must be distinguished from live updates");
}
void projection() {
    using namespace sfr;
    auto origin=camera_debug_project({0,0,2.5f},CameraDebugView::front,500,500);
    auto hand=camera_debug_project({0.4f,0.7f,2.5f},CameraDebugView::front,500,500);
    require(origin.visible && hand.visible && hand.x>origin.x && hand.y<origin.y,"positive X goes right; raised Y goes up");
    auto side=camera_debug_project({0,0,2.5f},CameraDebugView::side,500,500);
    auto foot=camera_debug_project({0,-0.7f,2.42f},CameraDebugView::side,500,500);
    require(foot.visible && foot.x<side.x && foot.y>side.y,"side view must preserve actual Z rather than infer extra depth");
    require(!camera_debug_project({std::numeric_limits<float>::quiet_NaN(),0,0},CameraDebugView::side,500,500).visible,"reject invalid data in every axis");
    require(!camera_debug_project({0,0,2.5f},CameraDebugView::front,0,500).visible,"zero-size minimized panel must not divide by zero");
    auto wide=camera_debug_project({0.5f,0.5f,2.5f},CameraDebugView::front,800,400);
    auto centre=camera_debug_project({0,0,2.5f},CameraDebugView::front,800,400);
    require(std::abs((wide.x-centre.x)-(centre.y-wide.y))<0.01f,"resizing must preserve equal metre scales");
}
void controller_handoff() {
    using namespace sfr;
    const auto now=CameraDebugClock::time_point{}+std::chrono::seconds(10);
    CameraDebugFrame f;
    require(!f.controller_active,"legacy camera frames default to camera source");
    f.game_frame=1; f.submitted=now; f.has_pose=true;
    f.camera_available=f.detected=true; f.observation_age_ms=f.pose_age_ms=0;
    f.controller_active=true;
    require(camera_debug_state(f,now)==CameraDebugState::controller,"controller source must not label its submitted joints camera-live");
    f.camera_available=false; f.detected=false; f.pose_age_ms=-1;
    require(camera_debug_state(f,now)==CameraDebugState::controller,"controller fallback remains visible when the camera is unavailable");
    require(camera_debug_state(f,now+std::chrono::milliseconds(500))==CameraDebugState::controller,"guest submission remains current at the stale boundary");
    require(camera_debug_state(f,now+std::chrono::milliseconds(501))==CameraDebugState::stale,"paused guest submissions take priority over the controller source");
    f.camera_available=true; f.pose_age_ms=700;
    require(camera_debug_state(f,now)==CameraDebugState::controller,"lost camera tracking must not label controller joints as held camera pose");
    f.controller_active=false; f.detected=true; f.pose_age_ms=0;
    require(camera_debug_state(f,now)==CameraDebugState::live,"returning control to a fresh camera restores camera-live state");
}
void bones() {
    std::array<unsigned,sfr::nui_joint_count> uses{};
    auto edges=sfr::camera_debug_bones();
    require(edges.size()==19,"20-joint skeleton needs its 19 anatomical connections");
    for(auto b:edges) {
        require(b.from<uses.size() && b.to<uses.size() && b.from!=b.to,"bone references valid distinct joints");
        ++uses[b.from]; ++uses[b.to];
    }
    for(auto n:uses) require(n>0,"every joint including hands/feet must be visible");
}
}
int main() { try { states();controller_handoff();projection();bones();std::cout<<"Camera debug checks passed\n"; } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
