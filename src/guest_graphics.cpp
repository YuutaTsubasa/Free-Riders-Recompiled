#include "guest_graphics.h"
#include <cstdlib>
#include "guest_memory.h"
#include "native_graphics.h"
#include "native_shaders.h"
#include "native_presentation.h"
#include "native_raster_state.h"
#include "native_blend_control.h"
#include "native_render_state.h"
#include "native_renderer.h"
#include <plume_render_interface.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <iostream>
#include <utility>
#include <vector>

namespace sfr {
GuestGraphics* active_guest_graphics = nullptr;

namespace {
[[noreturn]] void unsupported(uint32_t address, const char* detail) {
    throw RuntimeStop("native-graphics", address, detail);
}
constexpr std::array<uint32_t, 31> initial_parameters{
    1280,720,0x18280186,1,0,0,1,0,0,1,0x1A220197,0,0,1,0,0,0x28280106,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0};
struct StateRecord { uint32_t metadata, function, value; };
SamplerFilterState decode_sampler_filters(uint32_t word3, uint32_t word4) {
    return {word3, word4, uint8_t((word3 >> 19) & 3), uint8_t((word3 >> 21) & 3),
        uint8_t((word3 >> 23) & 3), uint8_t((word3 >> 25) & 7),
        uint8_t(word4 & 1), uint8_t((word4 >> 1) & 1), bool(word4 & 0x400), bool(word4 & 0x800)};
}
struct BlendDefaults { uint32_t requested, flags, effective; };
struct SamplerDefaults {
    uint32_t word3, word4;
    uint8_t anisotropy, volume_control, min_mip, max_mip;
};
SamplerDefaults read_sampler_defaults(const std::array<StateRecord, 20>& table) {
    // All20 original defaults, applied to all26 zeroed fetch records by
    // 82500B38. Prior setup only changes dword0; surrounding buffer copies
    // do not alias these words. See sampler-initialization-provenance.md.
    constexpr std::array<std::array<uint32_t, 2>, 20> profile{{
        {0x824E89C0,0}, {0x824E8A10,0}, {0x824E8A60,0}, {0x824E8950,0},
        {0x824E83F0,0}, {0x824E8248,0}, {0x824E8590,2}, {0x824E87B0,0},
        {0x824E8850,0}, {0x824E8690,1}, {0x824E84F0,0}, {0x824E8348,0},
        {0x824E85E8,0}, {0x824E88D0,13}, {0x824E8AB0,0}, {0x824E8708,0},
        {0x824E8B08,0}, {0x824E8B60,0}, {0x824E8BB8,0}, {0x824E8C10,1}
    }};
    for (size_t i = 0; i < table.size(); ++i)
        if (table[i].function != profile[i][0] || table[i].value != profile[i][1])
            unsupported(0x82AD0F20 + uint32_t(i) * 12, "unknown original sampler initialization profile");
    return {(table[4].value << 19) | (table[5].value << 21) | (table[6].value << 23),
        0, uint8_t(table[9].value), uint8_t(table[10].value | (table[11].value << 1) | (table[12].value << 2)),
        uint8_t(table[8].value), uint8_t(table[13].value)};
}
BlendDefaults read_blend_defaults(const std::array<StateRecord, 101>& table) {
    // Original 824F4B28 zeroes the allocation. 82500B38 then calls all
    // 101 default setters in table order. These are ALL writers of the two
    // requested cache words in that verified profile; see blend-requests.md.
    constexpr std::array<std::array<uint32_t, 3>, 11> profile{{
        {0x3C,0x824E6A40,0}, {0x40,0x824E6DD0,0},
        {0x48,0x824E6B60,1}, {0x4C,0x824E6BF0,0},
        {0x50,0x824E6AD0,0}, {0x54,0x824E6CF0,1},
        {0x58,0x824E6D60,0}, {0x5C,0x824E6C80,0},
        {0x17C,0x824E8228,0}, {0x180,0x824E80D0,2}, {0x184,0x824E8100,2}
    }};
    for (auto row : profile) {
        const auto& actual = table[row[0] / 4];
        if (actual.function != row[1] || actual.value != row[2])
            unsupported(0x82AD0A60 + row[0] * 3, "unknown original blend initialization profile");
    }
    const auto value = [&](uint32_t state) { return table[state / 4].value; };
    const uint32_t requested = value(0x48) | (value(0x4C) << 8) | (value(0x50) << 5) |
        (value(0x54) << 16) | (value(0x58) << 24) | (value(0x5C) << 21);
    const uint32_t flags = (value(0x3C) << 31) | (value(0x40) << 30) |
        (value(0x17C) << 23) | (value(0x180) << 20) | (value(0x184) << 17);
    // Enable is zero in this verified profile, so all target controls are copy.
    // Requested alpha/source fields are retained independently of that result.
    return {requested, flags, 0x00010001};
}
template<size_t N>
std::array<StateRecord, N> read_table(GuestMemory& memory, uint32_t address) {
    memory.check(address, N * 12);
    std::array<StateRecord, N> result{};
    for (size_t i = 0; i < N; ++i) {
        auto& r = result[i];
        r = {memory.load<uint32_t>(address + i*12), memory.load<uint32_t>(address+i*12+4),
             memory.load<uint32_t>(address+i*12+8)};
        if ((r.function & 3) || r.function < 0x82210000 || r.function >= 0x82ACC700)
            unsupported(r.function, "invalid original graphics state function");
    }
    return result;
}
int32_t viewport_integer(GuestMemory& memory, uint32_t address) {
    const float value = std::bit_cast<float>(memory.load<uint32_t>(address));
    if (!std::isfinite(value) || double(value) < INT32_MIN || double(value) > INT32_MAX)
        unsupported(address, "unsupported viewport value");
    return static_cast<int32_t>(value);
}
}

struct GuestGraphics::Impl {
    std::array<StateRecord, 101> render_defaults;
    std::array<StateRecord, 20> sampler_defaults;
    std::unique_ptr<NativePresentation> presentation;
    std::unique_ptr<NativeShaders> shaders;
    std::array<std::optional<NativeBlendControl>, 4> blend_controls;
    NativeRenderState render_state;
    std::array<std::optional<uint32_t>, 26> texture_bindings;
    std::unique_ptr<NativeRenderer> renderer;
};
NativeRenderer& GuestGraphics::renderer() {
    if(!impl_) unsupported(device_address,"native guest device is not created");
    if(!impl_->renderer) impl_->renderer = std::make_unique<NativeRenderer>(graphics_,*impl_->presentation);
    return *impl_->renderer;
}
bool GuestGraphics::foreign_render_targets = false;
GuestGraphics::GuestGraphics(GuestMemory& memory, NativeGraphics& graphics)
    : memory_(memory), graphics_(graphics) {}
GuestGraphics::~GuestGraphics() = default;
bool GuestGraphics::created() const noexcept { return bool(impl_); }
bool GuestGraphics::owns_object(uint32_t address) const noexcept {
    return impl_ && (address == device_address || address == color_handle ||
                    address == depth_handle || address == presentation_handle || impl_->shaders->owns(address));
}
void GuestGraphics::present_front_buffer(uint32_t device) {
    if(!impl_ || device != device_address) unsupported(device,"present requested for a non-native device");
    // The original resolves the device's back buffer (+0x3AC4) bound as RT0
    // (+0x3148) into its front buffer (+0x3AC0) and swaps that texture.
    if(memory_.load<uint32_t>(uint64_t(device)+0x3AC4) != color_handle ||
       memory_.load<uint32_t>(uint64_t(device)+0x3148) != color_handle)
        unsupported(device,"present source is not the native back buffer");
    if(memory_.load<uint32_t>(uint64_t(device)+0x3AC0) != presentation_handle)
        unsupported(device,"present destination is not the native front buffer");
    // A console scales the title's back buffer to the display. A race resets it
    // to 880x720 but renders at 1280x720, so only a frame whose last pass
    // covered exactly the back buffer (the loading screens do) is stretched;
    // one that covered the whole framebuffer is presented as it is.
    // SFR_PRESENT_STRETCH=0 presents the framebuffer either way.
    static const bool stretch=[]{ const char* t=std::getenv("SFR_PRESENT_STRETCH"); return !t || *t!='0'; }();
    const uint32_t back_width=memory_.load<uint32_t>(uint64_t(device)+0x35BC);
    const uint32_t back_height=memory_.load<uint32_t>(uint64_t(device)+0x35C0);
    const auto& viewport=impl_->presentation->raster_state().viewport();
    const bool back_buffer_sized=stretch && viewport.x==0 && viewport.y==0 &&
        uint32_t(viewport.width)==back_width && uint32_t(viewport.height)==back_height;
    impl_->presentation->present(back_buffer_sized?back_width:0,back_buffer_sized?back_height:0);
}
uint32_t GuestGraphics::discard_commands(uint32_t device, uint32_t& discarded_bytes) {
    if(!impl_ || device != device_address) unsupported(device,"command space requested for a non-native device");
    const auto write = memory_.load<uint32_t>(uint64_t(device)+48);
    // The pointer addresses the last stored word; the scratch start minus 4 is empty.
    if(write < command_scratch-4 || write >= command_scratch+command_scratch_size || (write & 3))
        unsupported(write,"native command write pointer left the scratch buffer");
    discarded_bytes = write-(command_scratch-4);
    memory_.store<uint32_t>(uint64_t(device)+48,command_scratch-4);
    return command_scratch-4;
}
uint32_t GuestGraphics::create_shader(ShaderStage stage,uint32_t container) {
    if(!impl_) unsupported(container,"native guest device is not created");
    return impl_->shaders->create(stage,container);
}
void GuestGraphics::attach_shader(uint32_t handle, uint32_t object) {
    if(!impl_) unsupported(object,"native guest device is not created");
    impl_->shaders->attach(handle,object);
}
const NativeShaders& GuestGraphics::shaders() const {
    if(!impl_) unsupported(device_address,"native guest device is not created");
    return *impl_->shaders;
}
NativePresentation& GuestGraphics::presentation() {
    if (!impl_) unsupported(device_address, "native guest device is not created");
    return *impl_->presentation;
}

void GuestGraphics::set_blend_control(uint32_t device, uint32_t target, uint32_t packed) {
    if (!impl_ || device != device_address)
        unsupported(device, "unknown native device for blend control");
    if (target >= impl_->blend_controls.size())
        unsupported(target, "blend control target is outside the four-target profile");
    NativeBlendControl next;
    try { next = decode_blend_control(packed); }
    catch (const std::invalid_argument& error) {
        throw RuntimeStop("native-blend-control", packed, error.what());
    }
    constexpr std::array<uint32_t,4> offsets{0x2938,0x2958,0x295C,0x2960};
    constexpr std::array<uint64_t,4> dirty_bits{0x20400,0x20004,0x20002,0x20001};
    // Preflight every guest effect before publishing either representation.
    // The original setter always marks dirty, even for an unchanged word.
    memory_.check_write(device + offsets[target], 4);
    memory_.check_write(device + 16, 8);
    const uint64_t dirty = memory_.load<uint64_t>(device + 16) | dirty_bits[target];
    memory_.store<uint32_t>(device + offsets[target], packed);
    memory_.store<uint64_t>(device + 16, dirty);
    impl_->blend_controls[target] = next;
}
BlendRequestUpdate GuestGraphics::set_blend_request(uint32_t device, BlendRequest request, uint32_t value) {
    if (!impl_ || device != device_address)
        unsupported(device, "unknown native device for blend request");
    if (request != BlendRequest::enable && request != BlendRequest::source &&
        request != BlendRequest::destination && request != BlendRequest::operation)
        unsupported(static_cast<uint32_t>(request), "unsupported blend request setter");

    const bool enable = request == BlendRequest::enable;
    const uint32_t cache_offset = enable ? 0x2EFC : 0x2EF8;
    memory_.check_write(device + cache_offset, 4);
    uint32_t flags = memory_.load<uint32_t>(device + 0x2EFC);
    uint32_t requested = memory_.load<uint32_t>(device + 0x2EF8);
    if (enable) flags = (flags & 0x7FFFFFFF) | ((value & 1) << 31);
    else if (request == BlendRequest::source) requested = (requested & ~31u) | (value & 31);
    // 824E6AD0 differs from the destination setter only in rlwimi r10,r4,5,24,26.
    else if (request == BlendRequest::operation) requested = (requested & ~0xE0u) | ((value & 7) << 5);
    else requested = (requested & ~0x1F00u) | ((value & 31) << 8);

    // Disabled factor setters only change requested state. They do not read,
    // validate or dirty target controls, and need not decode dormant factors.
    if (!enable && !(flags & 0x80000000)) {
        memory_.store<uint32_t>(device + cache_offset, requested);
        return {requested, flags, 0, false};
    }
    uint32_t effective = requested;
    if (!(flags & 0x40000000))
        effective = (requested & 0xFFFF) |
            (((requested << 16) | ((requested & 0x1010) << 12)) & 0xEFEF0000);
    // Preserve the original full DWORD zero comparison: value=2 clears the
    // requested enable bit but still publishes the nonzero effective branch.
    if (enable && value == 0) effective = 0x00010001;
    NativeBlendControl next;
    try { next = decode_blend_control(effective); }
    catch (const std::invalid_argument& error) {
        throw RuntimeStop("native-blend-control", effective, error.what());
    }
    const std::array<uint32_t,4> ordered_offsets = enable
        ? std::array<uint32_t,4>{0x295C,0x2938,0x2958,0x2960}
        : std::array<uint32_t,4>{0x2938,0x2958,0x295C,0x2960};
    for (uint32_t offset : ordered_offsets) memory_.check_write(device + offset, 4);
    memory_.check_write(device + 16, 8);
    uint64_t dirty = memory_.load<uint64_t>(device + 16);

    // Preflight/decode above prevent partial publication at unsupported state
    // boundaries. Successful stores retain the original ordering and OR407.
    memory_.store<uint32_t>(device + cache_offset, enable ? flags : requested);
    for (uint32_t offset : ordered_offsets) memory_.store<uint32_t>(device + offset, effective);
    for (uint64_t bits : {0x400ull,0x4ull,0x2ull,0x1ull}) {
        dirty |= bits;
        memory_.store<uint64_t>(device + 16, dirty);
    }
    for (auto& control : impl_->blend_controls) control = next;
    return {requested, flags, effective, true};
}
SamplerFilterState GuestGraphics::set_sampler_filter(uint32_t device, uint32_t slot, SamplerFilter filter, uint32_t value) {
    if (!impl_ || device != device_address || slot >= 26)
        unsupported(device, "unknown native device or sampler outside original 26-slot domain");
    if (filter != SamplerFilter::magnification && filter != SamplerFilter::minification)
        unsupported(static_cast<uint32_t>(filter), "unsupported sampler filter setter");
    const uint32_t fetch = device + 0x480 + slot * 24;
    memory_.check_write(fetch + 12, 4);
    memory_.check_write(fetch + 16, 4);
    memory_.check_write(device + 24, 8);
    const uint32_t word3 = memory_.load<uint32_t>(fetch + 12);
    const uint32_t word4 = memory_.load<uint32_t>(fetch + 16);
    const uint32_t anisotropy = memory_.load<uint8_t>(device + 0x2F44 + slot);
    const uint32_t volume_control = memory_.load<uint8_t>(device + 0x2F92 + slot);
    constexpr uint32_t lookup[]{0,0,2,2,3,3,3,4,4,4,4,4,4,5,5,5,5};
    if (anisotropy >= std::size(lookup))
        unsupported(anisotropy, "sampler anisotropy index exceeds verified original lookup");
    const uint32_t table_address = 0x82001608 + anisotropy * 4;
    const uint32_t encoded_anisotropy = memory_.load<uint32_t>(table_address);
    if (encoded_anisotropy != lookup[anisotropy])
        unsupported(table_address, "unknown original sampler anisotropy lookup value");
    const uint64_t dirty = memory_.load<uint64_t>(device + 24) | (uint64_t{1} << (31 - slot));
    const bool mag = filter == SamplerFilter::magnification;
    const uint32_t q = value >> 2;
    const uint32_t other_walk = (word4 >> (mag ? 11 : 10)) & 1;
    const uint32_t walk_mask = mag ? 0x400 : 0x800;
    const uint32_t intermediate4 = (word4 & ~walk_mask) | ((q << (mag ? 10 : 11)) & walk_mask);
    const uint32_t combined = ((encoded_anisotropy & ~((other_walk | q) - 1u)) << (mag ? 6 : 4)) | q | value;
    const uint32_t filter_mask = mag ? 0x0E180000 : 0x0E600000;
    const uint32_t next3 = (word3 & ~filter_mask) | (std::rotl(combined, mag ? 19 : 21) & filter_mask);
    uint32_t volume = (next3 & ~0x0007FFFFu) | (std::rotl(next3, 31) & 0x0007FFFF);
    volume = (volume & ~0x7FF00000u) | (std::rotl(next3, 31) & 0x7FF00000);
    const uint32_t volume_mask = (volume_control >> 2) - 1u;
    const uint32_t volume_bits = ((std::rotl(volume, 13) & 0xFFF) & volume_mask) + (volume_control & ~volume_mask);
    const uint32_t next4 = (intermediate4 & 0xFFFFFFFC) | (volume_bits & 3);
    // Complete unsigned original CPU effects, including raw DWORD inputs.
    // Retention does not imply that every raw encoding is a valid native
    // sampler descriptor. Preserve both original W4 writes and their order.
    memory_.store<uint32_t>(fetch + 16, intermediate4);
    memory_.store<uint32_t>(fetch + 12, next3);
    memory_.store<uint32_t>(fetch + 16, next4);
    memory_.store<uint64_t>(device + 24, dirty);
    return decode_sampler_filters(next3, next4);
}
SamplerFilterState GuestGraphics::sampler_filter_state(uint32_t slot) const {
    if (!impl_ || slot >= 26) unsupported(slot, "unknown or uninitialized original sampler slot");
    const uint32_t fetch = device_address + 0x480 + slot * 24;
    // Original game code also writes fetch fields inline. Never return a
    // setter-only cache; descriptor/shader consumers must use current fields.
    const uint32_t word3 = memory_.load<uint32_t>(fetch + 12);
    const uint32_t word4 = memory_.load<uint32_t>(fetch + 16);
    return decode_sampler_filters(word3, word4);
}
const NativeBlendControl& GuestGraphics::blend_control(uint32_t target) const {
    if (!impl_ || target >= impl_->blend_controls.size() || !impl_->blend_controls[target])
        unsupported(target, "native blend control has not been initialized for this target");
    return *impl_->blend_controls[target];
}
void GuestGraphics::set_render_state(uint32_t device, RenderState state, uint32_t value) {
    if (!impl_ || device != device_address)
        unsupported(device, "unknown native device for render state");
    auto next = impl_->render_state;
    constexpr std::array comparisons{
        plume::RenderComparisonFunction::NEVER, plume::RenderComparisonFunction::LESS,
        plume::RenderComparisonFunction::EQUAL, plume::RenderComparisonFunction::LESS_EQUAL,
        plume::RenderComparisonFunction::GREATER, plume::RenderComparisonFunction::NOT_EQUAL,
        plume::RenderComparisonFunction::GREATER_EQUAL, plume::RenderComparisonFunction::ALWAYS};
    const auto boolean = [&] {
        if (value > 1) unsupported(value, "noncanonical render-state boolean");
        return value != 0;
    };
    const auto comparison = [&] {
        if (value >= comparisons.size()) unsupported(value, "unsupported render-state comparison");
        return comparisons[value];
    };
    uint32_t offset = 0, mask = 0, bits = 0;
    uint64_t dirty_bits = 0;
    switch (state) {
    case RenderState::alpha_test_enable:
        next.alpha_test_enabled = boolean();
        offset = 0x293C; mask = 8; bits = value << 3; dirty_bits = 0x40200;
        break;
    case RenderState::alpha_function:
        next.alpha_function = comparison();
        offset = 0x293C; mask = 7; bits = value; dirty_bits = 0x200;
        break;
    case RenderState::alpha_reference: {
        if (value > 255) unsupported(value, "alpha reference exceeds the 8-bit state domain");
        const auto scale_bits = memory_.load<uint32_t>(0x82001658);
        if (scale_bits != 0x3B808081) unsupported(0x82001658, "original alpha-reference scale is not the verified constant");
        // Match original fcfid/frsp/fmuls: exact small integer, original
        // single-precision constant, product rounded back to single precision.
        const float scaled = float(double(float(value)) * double(std::bit_cast<float>(scale_bits)));
        next.alpha_reference = scaled;
        offset = 0x2904; mask = UINT32_MAX; bits = std::bit_cast<uint32_t>(scaled); dirty_bits = 0x08000000;
        break;
    }
    case RenderState::depth_enable: {
        next.depth_enable_requested = boolean();
        const uint32_t attachment = memory_.load<uint32_t>(device + 0x3158);
        if (attachment && attachment != depth_handle && !foreign_render_targets)
            unsupported(attachment, "unknown native depth attachment for render state");
        offset = 0x2934; mask = 2; bits = (value && attachment) ? 2u : 0u; dirty_bits = 0x20800;
        break;
    }
    case RenderState::depth_function:
        next.depth_function = comparison();
        offset = 0x2934; mask = 0x70; bits = value << 4; dirty_bits = 0x20800;
        break;
    case RenderState::depth_write:
        next.depth_write_enabled = boolean();
        offset = 0x2934; mask = 4; bits = value << 2; dirty_bits = 0x800;
        break;
    case RenderState::cull_mode: {
        if (value > 6 || value == 3) unsupported(value, "unsupported native cull/winding encoding");
        constexpr std::array modes{plume::RenderCullMode::NONE, plume::RenderCullMode::FRONT, plume::RenderCullMode::BACK};
        next.cull = NativeCullState{modes[value & 3], (value & 4) == 0};
        offset = 0x2948; mask = 7; bits = value; dirty_bits = 0x40;
        break;
    }
    default:
        unsupported(static_cast<uint32_t>(state), "render state has no audited native adapter");
    }
    memory_.check_write(device + offset, 4);
    memory_.check_write(device + 16, 8);
    if (state == RenderState::depth_enable) memory_.check_write(device + 0x2F14, 4);
    const uint32_t cache = mask == UINT32_MAX ? bits : (memory_.load<uint32_t>(device + offset) & ~mask) | bits;
    const uint64_t dirty = memory_.load<uint64_t>(device + 16) | dirty_bits;
    // No guest/native effects until all validation and source reads succeed.
    if (state == RenderState::depth_enable) memory_.store<uint32_t>(device + 0x2F14, value);
    memory_.store<uint32_t>(device + offset, cache);
    memory_.store<uint64_t>(device + 16, dirty);
    impl_->render_state = next;
}
void GuestGraphics::set_primitive_restart(uint32_t device, uint32_t value) {
    if (!impl_ || device != device_address)
        unsupported(device, "unknown native device for primitive restart");
    memory_.check_write(device + 0x2948, 4);
    memory_.check_write(device + 16, 8);
    const uint32_t cache = (memory_.load<uint32_t>(device + 0x2948) & ~0x00200000u) |
                           ((value & 1u) << 21);
    const uint64_t dirty = memory_.load<uint64_t>(device + 16) | 0x40;
    // The original consumes only bit0 and marks dirty even on an equal value.
    memory_.store<uint32_t>(device + 0x2948, cache);
    memory_.store<uint64_t>(device + 16, dirty);
    impl_->render_state.primitive_restart_enabled = (value & 1u) != 0;
}
const NativeRenderState& GuestGraphics::render_state() const {
    if (!impl_) unsupported(device_address, "native device is not created");
    return impl_->render_state;
}
bool GuestGraphics::depth_enabled() const {
    if (!impl_ || !impl_->render_state.depth_enable_requested.has_value())
        unsupported(device_address, "native depth-enable request has not been initialized");
    const uint32_t attachment = memory_.load<uint32_t>(device_address + 0x3158);
    if (attachment && attachment != depth_handle && !foreign_render_targets)
        unsupported(attachment, "unknown native depth attachment for effective state");
    return *impl_->render_state.depth_enable_requested && attachment != 0;
}
void GuestGraphics::set_texture(uint32_t device, uint32_t slot, uint32_t texture, uint64_t dirty_mask) {
    if (!impl_ || device != device_address)
        unsupported(device, "unknown native device for texture binding");
    if (slot >= impl_->texture_bindings.size())
        unsupported(slot, "texture slot is outside the audited initialization profile");
    if (dirty_mask != (0x80000000ull >> slot))
        unsupported(slot, "texture dirty mask does not match the original slot");
    const uint32_t fetch = device + 0x480 + 24 * slot;
    const uint32_t binding = device + 0x31B0 + 4 * slot;
    memory_.check_write(fetch, 24);
    memory_.check_write(binding, 4);
    const uint32_t old = memory_.load<uint32_t>(binding);
    // The original retires the replaced texture: it stamps the device's current
    // fence (+0x2A9C) into old+8, or queues the texture when +0x2AA0 flags it.
    // The queued path needs native fence retirement.
    const uint32_t fence = memory_.load<uint32_t>(uint64_t(device) + 0x2A9C);
    if (old && !fence && (memory_.load<uint32_t>(uint64_t(device) + 0x2AA0) & memory_.load<uint32_t>(old)))
        unsupported(old, "queued retirement of a replaced texture requires native fences");
    if (old && fence) memory_.check_write(uint64_t(old) + 8, 4);
    if (texture) {
        FetchWords texture_words, slot_words;
        memory_.check(uint64_t(texture) + 28, 24);
        for (uint32_t i = 0; i < 6; ++i) {
            texture_words[i] = memory_.load<uint32_t>(uint64_t(texture) + 28 + i * 4);
            slot_words[i] = memory_.load<uint32_t>(uint64_t(fetch) + i * 4);
        }
        const auto merged = merge_texture_fetch(texture_words, slot_words,
            memory_.load<uint8_t>(uint64_t(device) + 0x2F5E + slot), memory_.load<uint8_t>(uint64_t(device) + 0x2F78 + slot));
        const uint64_t dirty = memory_.load<uint64_t>(uint64_t(device) + 24) | dirty_mask;
        for (uint32_t i = 0; i < 6; ++i) memory_.store<uint32_t>(uint64_t(fetch) + i * 4, merged[i]);
        memory_.store<uint64_t>(uint64_t(device) + 24, dirty);
    } else {
        // Original null path only disables the fetch type and binding.
        // In particular it neither reads nor modifies the texture dirty word.
        memory_.store<uint32_t>(fetch, memory_.load<uint32_t>(fetch) & 0xFFFFFFFCu);
    }
    memory_.store<uint32_t>(binding, texture);
    if (old && fence) memory_.store<uint32_t>(uint64_t(old) + 8, fence);
    impl_->texture_bindings[slot] = texture;
}
FetchWords GuestGraphics::texture_fetch(uint32_t slot) const {
    if (!impl_ || slot >= 26) unsupported(slot, "fetch slot outside the original 26-slot domain");
    FetchWords words;
    for (uint32_t i = 0; i < 6; ++i)
        words[i] = memory_.load<uint32_t>(uint64_t(device_address) + 0x480 + 24 * slot + i * 4);
    return words;
}
uint32_t GuestGraphics::texture_binding(uint32_t slot) const {
    if (!impl_ || slot >= impl_->texture_bindings.size() || !impl_->texture_bindings[slot])
        unsupported(slot, "native texture binding has not been initialized for this slot");
    return *impl_->texture_bindings[slot];
}

uint32_t GuestGraphics::create_device(uint32_t adapter, uint32_t mode, uint32_t reserved,
                                      uint32_t flags, uint32_t parameters, uint32_t output) {
    if (impl_) unsupported(device_address, "a native guest device already exists");
    if (adapter || mode != 1 || reserved || flags)
        unsupported(mode, "unsupported native device creation mode/flags");
    if (!parameters || !output || (parameters & 3) || (output & 3))
        unsupported(parameters, "invalid native device creation pointers");
    memory_.check(parameters, initial_parameters.size()*4);
    memory_.check_write(output, 4);
    std::array<uint32_t, 31> words{};
    for (size_t i=0; i<words.size(); ++i) {
        words[i] = memory_.load<uint32_t>(uint64_t(parameters)+i*4);
        if (i >= 2 && words[i] != initial_parameters[i])
            unsupported(parameters+uint32_t(i*4), "unsupported presentation parameter profile");
    }
    if (!words[0] || !words[1] || words[0] > 8192 || words[1] > 8192)
        unsupported(parameters, "unsupported presentation dimensions");
    // With foreign render targets the title binds and unbinds the native back
    // buffer through its own SetRenderTarget, which reads surface fields
    // (+24, +28) that the native handles do not have. They are mapped larger
    // and left zero, so those fields read as unspecified instead of faulting.
    const uint32_t handle_size = foreign_render_targets ? 64u : 8u;
    for (auto [address,size] : {std::pair{device_address,device_size},
                              std::pair{color_handle,handle_size},std::pair{depth_handle,handle_size},
                              std::pair{presentation_handle,handle_size},
                              std::pair{command_scratch,command_scratch_size}})
        if (!memory_.available(address,size)) unsupported(address,"native graphics address is occupied");
    auto next = std::make_unique<Impl>();
    next->render_defaults = read_table<101>(memory_,0x82AD0A60);
    next->sampler_defaults = read_table<20>(memory_,0x82AD0F20);
    const auto blend_defaults = read_blend_defaults(next->render_defaults);
    const auto sampler_defaults = read_sampler_defaults(next->sampler_defaults);
    for (auto& control : next->blend_controls) control = decode_blend_control(blend_defaults.effective);
    graphics_.initialize();
    next->presentation = std::make_unique<NativePresentation>(graphics_,words[0],words[1]);
    next->shaders = std::make_unique<NativeShaders>(memory_,graphics_);

    memory_.map(device_address, device_size);
    for (uint32_t handle : {color_handle,depth_handle,presentation_handle}) {
        memory_.map(handle,handle_size);
        memory_.store<uint32_t>(handle+4,1);
        // Native handles own real host resources. They do not claim an Xbox
        // eDRAM header or expose guessed packed resource metadata.
        memory_.add_read_only_word(handle,[handle]() -> uint32_t {
            unsupported(handle,"raw access to an unimplemented native resource header");
        });
    }
    for (uint32_t i=0;i<5;++i) memory_.store<uint64_t>(device_address+i*8,UINT64_MAX);
    memory_.map(command_scratch,command_scratch_size);
    memory_.store<uint32_t>(device_address+48,command_scratch-4);
    memory_.store<uint32_t>(device_address+56,command_scratch+command_scratch_size-command_scratch_margin);
    memory_.store<uint32_t>(device_address+0x3C,1);
    memory_.store<uint32_t>(device_address+0x2EF8,blend_defaults.requested);
    memory_.store<uint32_t>(device_address+0x2EFC,blend_defaults.flags);
    for (uint32_t offset : {0x295Cu,0x2938u,0x2958u,0x2960u})
        memory_.store<uint32_t>(device_address+offset,blend_defaults.effective);
    for (uint32_t slot = 0; slot < 26; ++slot) {
        memory_.store<uint32_t>(device_address + 0x48C + slot * 24,sampler_defaults.word3);
        memory_.store<uint32_t>(device_address + 0x490 + slot * 24,sampler_defaults.word4);
        memory_.store<uint8_t>(device_address + 0x2F44 + slot,sampler_defaults.anisotropy);
        memory_.store<uint8_t>(device_address + 0x2F92 + slot,sampler_defaults.volume_control);
        memory_.store<uint8_t>(device_address + 0x2F5E + slot,sampler_defaults.min_mip);
        memory_.store<uint8_t>(device_address + 0x2F78 + slot,sampler_defaults.max_mip);
    }
    for (size_t i=0;i<next->render_defaults.size();++i) {
        memory_.store<uint32_t>(device_address+0x40+i*4,next->render_defaults[i].function);
        memory_.store<uint32_t>(device_address+0x224+i*4,next->render_defaults[i].metadata);
    }
    for (size_t i=0;i<next->sampler_defaults.size();++i) {
        memory_.store<uint32_t>(device_address+0x1D4+i*4,next->sampler_defaults[i].function);
        memory_.store<uint32_t>(device_address+0x3B8+i*4,next->sampler_defaults[i].metadata);
    }
    memory_.store<uint32_t>(device_address+0x3148,color_handle);
    memory_.store<uint32_t>(device_address+0x3158,depth_handle);
    const std::array<float,6> viewport{0,0,float(words[0]),float(words[1]),0,1};
    for (size_t i=0;i<viewport.size();++i)
        memory_.store<uint32_t>(device_address+0x3218+i*4,std::bit_cast<uint32_t>(viewport[i]));
    memory_.store<uint32_t>(device_address+0x323C,65535);
    memory_.store<uint32_t>(device_address+0x3240,65535);
    for (size_t i=0;i<words.size();++i)
        memory_.store<uint32_t>(device_address+0x35BC+i*4,words[i]);
    // Display refresh rate read by GetDisplayMode (NTSC 60 Hz output).
    memory_.store<uint32_t>(device_address+0x550C,60);
    memory_.store<uint32_t>(device_address+0x3ABC,depth_handle);
    memory_.store<uint32_t>(device_address+0x3AC0,presentation_handle);
    memory_.store<uint32_t>(device_address+0x3AC4,color_handle);
    memory_.store<uint32_t>(device_address+0x5E88,0x0C000000);
    memory_.store<uint32_t>(output,device_address);
    impl_ = std::move(next);
    std::cerr << "NATIVE_GRAPHICS backend=" << graphics_backend_name(graphics_.backend()) << " adapter="
              << graphics_.device().getDescription().name << '\n';
    std::cerr << "NATIVE_BLEND_DEFAULTS table=0x82ad0a60 requested=0x" << std::hex
              << blend_defaults.requested << " flags=0x" << blend_defaults.flags
              << " effective=0x" << blend_defaults.effective << std::dec << " targets=4\n";
    std::cerr << "NATIVE_SAMPLER_DEFAULTS table=0x82ad0f20 slots=26 word3=0x" << std::hex
              << sampler_defaults.word3 << " word4=0x" << sampler_defaults.word4 << std::dec
              << " requested_anisotropy=" << unsigned(sampler_defaults.anisotropy)
              << " volume_control=" << unsigned(sampler_defaults.volume_control) << '\n';
    return 0;
}

bool GuestGraphics::clear(uint32_t device, uint32_t rectangle_count, uint32_t rectangles,
                          uint32_t flags, uint32_t argb, float depth_value, uint32_t stencil_value) {
    if (!impl_ || device != device_address) unsupported(device,"unknown native device for clear");
    if (rectangles && !rectangle_count) return false;
    if (flags & ~0x3Fu) unsupported(flags,"unsupported clear flags");
    if (memory_.load<uint8_t>(device+0x2ABC) & 0x30)
        unsupported(device+0x2ABC,"special clear rectangle mode is unsupported");
    NativeClear clear;
    for (uint32_t i=0;i<4;++i) {
        if (!(flags & (1u<<i))) continue;
        const uint32_t binding=memory_.load<uint32_t>(device+0x3148+i*4);
        if (!binding) continue;
        if (i || (binding != color_handle && !foreign_render_targets))
            unsupported(binding,"unsupported clear render target");
        clear.color=true;
    }
    // The original routine does not enter its depth-only fallback if color
    // targets were requested but all selected targets are unbound.
    if ((flags & 15) && !clear.color) return false;
    const uint32_t depth_binding=memory_.load<uint32_t>(device+0x3158);
    if ((flags & 0x30) && depth_binding) {
        if (depth_binding != depth_handle && !foreign_render_targets)
            unsupported(depth_binding,"unsupported clear depth target");
        clear.depth=bool(flags&0x10); clear.stencil=bool(flags&0x20);
    }
    if (!clear.color && !clear.depth && !clear.stencil) return false;
    clear.color_value={float((argb>>16)&255)/255.0f,float((argb>>8)&255)/255.0f,
                       float(argb&255)/255.0f,float(argb>>24)/255.0f};
    clear.depth_value=depth_value;
    clear.stencil_value=static_cast<uint8_t>(stencil_value);
    const int64_t x=viewport_integer(memory_,device+0x3218), y=viewport_integer(memory_,device+0x321C);
    const int64_t w=viewport_integer(memory_,device+0x3220), h=viewport_integer(memory_,device+0x3224);
    int64_t left=std::max<int64_t>(0,x),top=std::max<int64_t>(0,y);
    int64_t right=std::min<int64_t>(impl_->presentation->width(),x+w);
    int64_t bottom=std::min<int64_t>(impl_->presentation->height(),y+h);
    if (memory_.load<uint32_t>(device+0x2F00)) {
        left=std::max<int64_t>(left,std::bit_cast<int32_t>(memory_.load<uint32_t>(device+0x3234)));
        top=std::max<int64_t>(top,std::bit_cast<int32_t>(memory_.load<uint32_t>(device+0x3238)));
        right=std::min<int64_t>(right,std::bit_cast<int32_t>(memory_.load<uint32_t>(device+0x323C)));
        bottom=std::min<int64_t>(bottom,std::bit_cast<int32_t>(memory_.load<uint32_t>(device+0x3240)));
    }
    if (left>=right || top>=bottom) return false;
    std::vector<plume::RenderRect> clipped;
    if (!rectangles) clipped.emplace_back(int32_t(left),int32_t(top),int32_t(right),int32_t(bottom));
    else {
        if (rectangle_count>4096) unsupported(rectangle_count,"clear rectangle count exceeds supported bound");
        memory_.check(rectangles,uint64_t(rectangle_count)*16);
        clipped.reserve(rectangle_count);
        for (uint32_t i=0;i<rectangle_count;++i) {
            const uint64_t p=uint64_t(rectangles)+uint64_t(i)*16;
            auto l=std::max<int64_t>(left,std::bit_cast<int32_t>(memory_.load<uint32_t>(p)));
            auto t=std::max<int64_t>(top,std::bit_cast<int32_t>(memory_.load<uint32_t>(p+4)));
            auto r=std::min<int64_t>(right,std::bit_cast<int32_t>(memory_.load<uint32_t>(p+8)));
            auto b=std::min<int64_t>(bottom,std::bit_cast<int32_t>(memory_.load<uint32_t>(p+12)));
            if (l<r && t<b) clipped.emplace_back(int32_t(l),int32_t(t),int32_t(r),int32_t(b));
        }
    }
    if (clipped.empty()) return false;
    impl_->presentation->clear(clear,clipped);
    return true;
}
}
