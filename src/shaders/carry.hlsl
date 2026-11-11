// What a target's colour or depth held, drawn into the fresh texture that
// takes its place (NativePresentation::carry_target_attachments, Issue #176):
// a full-screen triangle that reads each pixel back exactly (Load, no
// filtering) as the first draw of the target's next pass. A tiling GPU then
// starts that pass from the old texture as it would from a load, without a
// separate copy. Compiled at build time to DXIL and SPIR-V (CMakeLists.txt,
// sfr_embed_shader); only Vulkan uses it.

Texture2D<float4> g_Color : register(t0, space0);
Texture2D<float> g_Depth : register(t1, space1);

struct Vertex { float4 position : SV_Position; };

Vertex vertexMain(uint id : SV_VertexID) {
    Vertex v;
    const float2 corner = float2((id << 1) & 2, id & 2);
    v.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return v;
}

float4 colorMain(Vertex v) : SV_Target {
    return g_Color.Load(int3(int2(v.position.xy), 0));
}

float depthMain(Vertex v) : SV_Depth {
    return g_Depth.Load(int3(int2(v.position.xy), 0));
}

struct ColorAndDepth { float4 color : SV_Target; float depth : SV_Depth; };
ColorAndDepth bothMain(Vertex v) {
    ColorAndDepth out_;
    const int3 texel = int3(int2(v.position.xy), 0);
    out_.color = g_Color.Load(texel);
    out_.depth = g_Depth.Load(texel);
    return out_;
}
