// A full-screen triangle sampling the framebuffer, with the sampled area as a
// push constant so one pipeline serves every back buffer size. Compiled at
// build time to DXIL and SPIR-V (CMakeLists.txt, sfr_embed_shader).
// The pixel shader also draws the touch controls (touch_controls.h) over the
// image: per circle (x, y, radius, state) in image coordinates, state 0 for
// none; the stick's knob is at g_AreaKnob.zw.
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
