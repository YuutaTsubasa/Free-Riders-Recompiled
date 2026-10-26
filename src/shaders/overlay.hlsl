// One picture of the port's own screen overlay (screen_overlay.h: the
// achievement notification and page) drawn over the presented frame: a
// rectangle in clip space and an opacity as push constants, the picture's
// colours premultiplied by its alpha. Compiled at build time to DXIL and
// SPIR-V (CMakeLists.txt, sfr_embed_shader).
struct OverlayConstants {
    float4 rect;     // left, top, right, bottom in clip space
    float4 opacity;  // x: the whole picture's opacity
};
#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<OverlayConstants> g_Overlay;
#define g_Rect g_Overlay.rect
#define g_Opacity g_Overlay.opacity
#else
cbuffer Overlay : register(b0, space2) { float4 g_Rect; float4 g_Opacity; };
#endif
struct Vertex { float4 position : SV_Position; float2 texCoord : TEXCOORD0; };
Vertex vertexMain(uint index : SV_VertexID) {
    // Two triangles: 0 1 2, 2 1 3 over the corners (0,0) (1,0) (0,1) (1,1).
    const uint corners[6] = {0, 1, 2, 2, 1, 3};
    const uint corner = corners[index];
    const float2 at = float2(corner & 1, corner >> 1);
    Vertex vertex;
    vertex.position = float4(lerp(g_Rect.x, g_Rect.z, at.x), lerp(g_Rect.y, g_Rect.w, at.y), 0.0, 1.0);
    vertex.texCoord = at;
    return vertex;
}
Texture2D<float4> g_Picture : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);
float4 pixelMain(Vertex vertex) : SV_Target {
    return g_Picture.Sample(g_Sampler, vertex.texCoord) * g_Opacity.x;
}
