#include "pad_devices.h"
#include "pad_assignment.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void identities_cross_processes() {
    const std::vector<sfr::SdlPadIdentity> initial{{10, "Same pad", "guid", "one"}, {20, "Same pad", "guid", "two"}};
    const auto devices = sfr::identify_sdl_pads(initial);
    const auto restarted = sfr::identify_sdl_pads(std::vector<sfr::SdlPadIdentity>{{77, "Same pad", "guid", "two"}, {88, "Same pad", "guid", "one"}});
    require(devices[0].identity != devices[1].identity && devices[0].name != devices[1].name, "identical models have distinct identifiers and labels");
    require(sfr::pad_with_name(devices[0].identity, restarted) == 88, "serial identity survives reordered enumeration and instance IDs");
    require(sfr::pad_with_name("Same pad", devices) == sfr::PadAssignment::absent_pad, "ambiguous legacy names do not select the wrong controller");
    require(sfr::pad_with_name("Same pad", std::span(devices).first(1)) == 10, "unambiguous legacy names still work");
    require(sfr::pad_with_name("", devices) == sfr::PadAssignment::no_pad, "empty preference stays automatic");
    const auto numbered = sfr::identify_sdl_pads(std::vector<sfr::SdlPadIdentity>{{10, "Pad", "guid", ""}, {20, "Pad", "guid", ""}});
    const auto renumbered = sfr::identify_sdl_pads(std::vector<sfr::SdlPadIdentity>{{77, "Pad", "guid", ""}, {88, "Pad", "guid", ""}});
    require(numbered[0].identity != numbered[1].identity && numbered[1].identity == renumbered[1].identity, "serial-less controllers use a process-independent occurrence ordinal");
}
void capture_reads_edges_from_each_pad() {
    sfr::PadButtonCapture capture;
    sfr::GamepadState held; held.buttons = sfr::gamepad_button::a;
    capture.reset(std::vector<sfr::PadSample>{{0, held}, {1, {}}});
    require(!capture.update(std::vector<sfr::PadSample>{{0, held}, {1, {}}}), "activation press is not captured again");
    sfr::GamepadState second; second.buttons = sfr::gamepad_button::y;
    require(capture.update(std::vector<sfr::PadSample>{{0, held}, {1, second}}) == sfr::InputAction::y, "a second controller can bind a button");
    require(!capture.update(std::vector<sfr::PadSample>{{0, held}, {1, second}}), "held button is not a repeated press");
    capture.update(std::vector<sfr::PadSample>{{0, {}}, {1, {}}});
    second = {};
    second.left_trigger = 180;
    require(capture.update(std::vector<sfr::PadSample>{{1, second}, {0, {}}}) == sfr::InputAction::left_trigger, "triggers and reordered devices use independent edges");
    require(!capture.update(std::vector<sfr::PadSample>{{2, held}}), "a newly connected held button must be released before capture");
}
}
int main() {
    try { identities_cross_processes(); capture_reads_edges_from_each_pad(); std::cout << "Pad device checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
