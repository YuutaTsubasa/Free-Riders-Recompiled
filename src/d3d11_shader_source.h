#pragma once
#include <optional>
#include <string>

namespace sfr {
// The translated shader's D3D branch (the HLSL its DXIL is compiled from)
// rewritten for Direct3D 11's shader model 5.0, which FXC (d3dcompiler_47)
// compiles (Issue #359):
//  - the texture heaps become fixed arrays of the renderer's per-draw layout,
//    2D textures at t0..t15, 2D arrays at t16..t31, cubes at t32..t47 and
//    samplers at s0..s15, element s for fetch constant s; shader model 5.0
//    indexes a resource array only with a literal, so each fetch constant's
//    descriptor index becomes its number (the renderer binds the textures
//    the shared constants name at those slots) and the resolved-framebuffer
//    flag (the index's top bit) is read from the shared constants instead;
//  - register spaces go (b0..b4), as do the survey UAV and vk:: attributes;
//  - HLSL 2021's select() becomes ?:, the specialization constant is the
//    SFR_SPEC_CONSTANTS macro given at compile time, and macros without
//    parameters, which FXC's preprocessor refuses, lose their ();
//  - FXC refuses `continue` inside a switch, so XenosRecomp's dispatch loop
//    ("while (true) switch (pc) { case n: ... }") becomes one guarded block
//    per case in case order, a jump continuing the loop;
//  - an array the translator also names as a macro reading it is renamed
//    (FXC does not expand such a macro), `texture` (an FXC keyword) is
//    renamed, and the 64-bit swap mask shift becomes 32-bit, as for SPIR-V.
// Empty, with the reason in why, for a shader this does not understand.
std::optional<std::string> d3d11_shader_source(std::string text, std::string* why = nullptr);
}
