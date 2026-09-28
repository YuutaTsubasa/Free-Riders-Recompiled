// A model of the player's own -- the VRM chosen for the Avatar -- drawn in
// the title's scene pass. Positions, normals and texture coordinates come from
// the vertex buffer (gltf_model.h); the transform, the lighting and the
// material's own colour are push constants, so one pipeline serves any model,
// any part of it and any place to put it.
//
// Compiled at build time to DXIL and SPIR-V (CMakeLists.txt, sfr_embed_shader).
struct ModelConstants {
    float4x4 transform;  // model to clip space
    float4 light;        // xyz the direction light comes from, w the ambient part
    float4 tint;         // rgb multiplied into the shade, a the opacity
    float4 options;      // x: anything less see-through than this is not drawn
};
#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<ModelConstants> g_Model;
#define g_Transform g_Model.transform
#define g_Light g_Model.light
#define g_Tint g_Model.tint
#define g_Options g_Model.options
#else
cbuffer Model : register(b0, space2) { float4x4 g_Transform; float4 g_Light; float4 g_Tint; float4 g_Options; };
#endif

// The part's own picture, or a single white pixel for a part that has none,
// so that both take the same path.
Texture2D<float4> g_Picture : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

struct Input { float3 position : POSITION; float3 normal : NORMAL; float2 texcoord : TEXCOORD0; };
struct Vertex { float4 position : SV_Position; float3 normal : TEXCOORD0; float2 texcoord : TEXCOORD1; };

Vertex vertexMain(Input input) {
    Vertex vertex;
    vertex.position = mul(g_Transform, float4(input.position, 1.0));
    vertex.normal = input.normal;
    vertex.texcoord = input.texcoord;
    return vertex;
}

float3 srgbToLinear(float3 value) {
    return lerp(value / 12.92, pow((value + 0.055) / 1.055, 2.4), step(0.04045, value));
}

float3 linearToSrgb(float3 value) {
    return lerp(value * 12.92, 1.055 * pow(max(value, 0), 1.0 / 2.4) - 0.055, step(0.0031308, value));
}

float4 pixelMain(Vertex vertex, bool front : SV_IsFrontFace) : SV_Target {
    const float4 sampleColour = g_Picture.Sample(g_Sampler, vertex.texcoord);
    const float4 painted = float4(srgbToLinear(sampleColour.rgb) * g_Tint.rgb, sampleColour.a * g_Tint.a);
    // A cut-out, not a blend: hair and eyelashes are drawn as cards with
    // see-through corners, and without this they are opaque rectangles.
    if (painted.a < g_Options.x) discard;
    // VRM files may declare MToon and its standard unlit fallback together.
    // Until full MToon is available, preserve that fallback instead of adding
    // an unrelated directional light to already authored character colours.
    if (g_Options.y > 0.5) return float4(linearToSrgb(painted.rgb), painted.a);
    const float3 normal = normalize(front ? vertex.normal : -vertex.normal);
    // Lambert, with enough ambient that a face turned away is still a shape
    // rather than a silhouette.
    const float lit = saturate(dot(normal, -normalize(g_Light.xyz)));
    const float shade = g_Light.w + (1.0 - g_Light.w) * lit;
    return float4(linearToSrgb(painted.rgb * shade), painted.a);
}
