#include "camera_input.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
void handoff() {
    using namespace sfr;
    using namespace std::chrono;
    CameraInputSelection select;
    const auto start=CameraInputSelection::Clock::time_point{}+seconds(5);
    require(!select.update({},-1,start),"startup without a pose must keep the controller");
    require(select.update({},0,start),"fresh pose should control an idle player");
    GamepadState pad; pad.thumb_rx=32767;
    require(!select.update(pad,0,start),"intentional stick input must take priority over a fresh camera pose");
    require(!select.update({},0,start+milliseconds(1499)),"menu dwell and release gestures need a controller grace period");
    require(select.update({},0,start+milliseconds(1500)),"idle controller must return to a fresh camera");
    pad.buttons=gamepad_button::a;
    require(!select.update(pad,0,start+seconds(2)),"held input extends controller ownership");
    require(!select.update(pad,0,start+seconds(4)),"held input cannot expire back to the camera");
    require(!select.update({},0,start+seconds(5)),"release must retain controller ownership briefly");
    require(select.update({},0,start+milliseconds(5500)),"released controller must eventually return to camera");
    require(select.update({},250,start+seconds(6)),"one briefly missed camera frame must not switch sources");
    require(!select.update({},501,start+seconds(6)),"a frozen camera stream must fall back to the controller");
    require(!select.update({},-1,start+seconds(7)),"camera loss must not reuse its last pose forever");
    require(select.update({},20,start+seconds(8)),"tracking recovery should resume camera after controller idle");
    require(!select.update({},std::numeric_limits<double>::quiet_NaN(),start+seconds(9)),"invalid pose age is not tracked");
    require(!select.update({},std::numeric_limits<double>::infinity(),start+seconds(9)),"infinite pose age is not tracked");
}
void controls() {
    using namespace sfr;
    const auto now=CameraInputSelection::Clock::time_point{};
    GamepadState drift; drift.thumb_lx=7849; drift.thumb_ly=-7849;
    drift.thumb_rx=7849; drift.thumb_ry=-7849; drift.left_trigger=drift.right_trigger=30;
    CameraInputSelection idle;
    require(idle.update(drift,0,now),"stick/trigger drift inside dead zones must not steal camera control");
    for(unsigned field=0;field<7;++field) {
        CameraInputSelection select;
        GamepadState pad;
        switch(field) {
        case 0:pad.buttons=gamepad_button::back;break;
        case 1:pad.left_trigger=31;break;
        case 2:pad.right_trigger=31;break;
        case 3:pad.thumb_lx=-32768;break;
        case 4:pad.thumb_ly=7850;break;
        case 5:pad.thumb_rx=-7850;break;
        case 6:pad.thumb_ry=32767;break;
        }
        require(!select.update(pad,0,now),"every mapped controller input can take over");
    }
    require(camera_controls_player(0,true) && !camera_controls_player(1,true),"camera source selection must leave P2 under controller control");
    require(!camera_controls_player(0,false),"camera loss restores P1 controller race overrides");
}
}
int main() {try {handoff();controls();std::cout<<"Camera input handoff checks passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
