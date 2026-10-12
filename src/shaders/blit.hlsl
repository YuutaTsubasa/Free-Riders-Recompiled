// A full-screen triangle sampling the framebuffer, with the sampled area as a
// push constant so one pipeline serves every back buffer size. Compiled at
// build time to DXIL and SPIR-V (CMakeLists.txt, sfr_embed_shader).
// The pixel shader also draws the touch controls (touch_controls.h) over the
// image: per circle (x, y, radius, state) in image coordinates, state 0 for
// none; the stick's knob is at g_AreaKnob.zw. A state of 3 to 4 is the Pause
// Gesture's gauge instead (state - 3 is how far the hold has got): a
// ring that fills clockwise from the top, on a dark disc, around a figure in
// the pose.

struct BlitConstants {
    float4 areaKnob;   // sampled area (xy), stick knob (zw)
    float4 circles[7];
};
#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<BlitConstants> g_Blit;
#define g_AreaKnob g_Blit.areaKnob
#define g_Circles g_Blit.circles
#else
cbuffer Blit : register(b0, space2) { float4 g_AreaKnob; float4 g_Circles[7]; };
#endif
struct Vertex { float4 position : SV_Position; float2 texCoord : TEXCOORD0; float2 image : TEXCOORD1; };
Vertex vertexMain(uint index : SV_VertexID) {
    const float2 corner = float2((index << 1) & 2, index & 2);
    Vertex vertex;
    vertex.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    vertex.texCoord = corner * g_AreaKnob.xy;
    vertex.image = corner;
    return vertex;
}
Texture2D<float4> g_Source : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

static const float3 g_Colors[7] = {
    float3(1.0, 1.0, 1.0),     // stick
    float3(0.25, 0.85, 0.3),   // A
    float3(0.95, 0.25, 0.25),  // B
    float3(0.25, 0.5, 1.0),    // X
    float3(1.0, 0.85, 0.2),    // Y
    float3(0.75, 0.75, 0.75),  // RT
    float3(1.0, 1.0, 1.0),     // START
};

// The smooth union of two distances: rounded where they meet, by k.
float smooth_min(float a, float b, float k) {
    const float h = saturate(0.5 + 0.5 * (b - a) / k);
    return lerp(b, a, h) - k * h * (1.0 - h);
}

// Distance from p to a capsule from a to b that narrows from radius ra to rb.
float limb(float2 p, float2 a, float2 b, float ra, float rb) {
    const float2 pa = p - a, ba = b - a;
    const float h = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * h) - lerp(ra, rb, h);
}

// A smooth mannequin in the pose, seen as in a mirror (the arm the player
// raises reaches to the screen's left, the other hangs at the right), in
// units of the gauge's radius, +y down: rounded limbs blended into the body,
// the head a neck's gap above it.
float figure(float2 p, float edge) {
    float body = limb(p, float2(0.0, -0.2), float2(0.0, 0.07), 0.144, 0.126);
    body = smooth_min(body, limb(p, float2(-0.055, 0.06), float2(-0.085, 0.5), 0.085, 0.065), 0.03);  // legs
    body = smooth_min(body, limb(p, float2(0.055, 0.06), float2(0.085, 0.5), 0.085, 0.065), 0.03);
    body = smooth_min(body, limb(p, float2(0.1, -0.2), float2(0.16, 0.13), 0.075, 0.058), 0.035);   // arm hanging
    body = smooth_min(body, limb(p, float2(-0.1, -0.2), float2(-0.4, 0.07), 0.075, 0.058), 0.035);  // arm out at 45
    const float d = min(body, length(p - float2(0.0, -0.455)) - 0.15);                          // head
    const float inside = 1.0 - smoothstep(0.62 - edge, 0.62 + edge, length(p));
    return (1.0 - smoothstep(-edge, edge, d)) * inside;
}

// The Pause Gesture's gauge at c (x, y, radius, 3 + progress).
float3 gauge(float3 color, float2 image, float4 c, float2 pixel) {
    const float2 d = (image - c.xy) * float2(16.0 / 9.0, 1.0);
    const float r = length(d);
    const float edge = pixel.y;
    const float disc_alpha = 1.0 - smoothstep(c.z - edge, c.z + edge, r);
    const float ring = smoothstep(c.z * 0.68 - edge, c.z * 0.68 + edge, r) *
                       (1.0 - smoothstep(c.z * 0.88 - edge, c.z * 0.88 + edge, r));
    float turn = atan2(d.x, -d.y) / 6.2831853;  // 0 at the top, clockwise
    if (turn < 0.0) turn += 1.0;
    const float filled = step(turn, c.w - 3.0);
    color = lerp(color, float3(0.05, 0.07, 0.1), disc_alpha * 0.55);
    color = lerp(color, float3(0.55, 0.6, 0.65), ring * (1.0 - filled) * 0.6);
    color = lerp(color, float3(0.45, 0.9, 1.0), ring * filled * 0.95);
    return lerp(color, float3(1.0, 1.0, 1.0), figure(d / c.z, edge / c.z) * 0.9);
}

// A soft disc with a brighter rim; alpha 0 outside.
float disc(float2 image, float2 centre, float radius, float2 pixel) {
    const float d = length((image - centre) * float2(16.0 / 9.0, 1.0)) - radius;
    return 1.0 - smoothstep(-pixel.y, pixel.y, d);
}

float4 pixelMain(Vertex vertex) : SV_Target {
    float4 color = g_Source.Sample(g_Sampler, vertex.texCoord);
    const float2 pixel = fwidth(vertex.image);
    [unroll] for (int i = 0; i < 7; ++i) {
        const float4 c = g_Circles[i];
        if (c.w <= 0.0) continue;
        if (c.w >= 3.0) {
            color.rgb = gauge(color.rgb, vertex.image, c, pixel);
            continue;
        }
        const float inside = disc(vertex.image, c.xy, c.z, pixel);
        const float core = disc(vertex.image, c.xy, c.z * 0.82, pixel);
        const float alpha = (c.w > 1.5 ? 0.55 : 0.22) * core + (inside - core) * (c.w > 1.5 ? 0.9 : 0.5);
        color.rgb = lerp(color.rgb, g_Colors[i], alpha);
        if (i == 0) {  // the stick's knob
            const float knob = disc(vertex.image, g_AreaKnob.zw, c.z * 0.45, pixel);
            color.rgb = lerp(color.rgb, float3(1.0, 1.0, 1.0), knob * (c.w > 1.5 ? 0.6 : 0.3));
        }
    }
    return color;
}
