// Inverted viewport depth on a D3D12 device that cannot do it (Issue #177).
//
// The title draws its 3D scenes with MinDepth=1, MaxDepth=0: a viewport that
// stores depth = 1 - z/w. D3D12 flips z for such a viewport only where the
// driver reports InvertedViewportDepthFlipsZSupported (OPTIONS13); an older
// driver gets the viewport with its depth range swapped (0..1), and these
// geometry shaders flip clip-space z instead, z' = w - z. The rasterizer then
// stores 0 + 1 * (w - z)/w = 1 - z/w: the depth an inverted viewport stores,
// so depth tests, clears, shadow maps sampled later and SV_Position.z in the
// pixel shader all see the values they see on a capable device.
//
// The input is the vertex output every XenosRecomp vertex shader declares
// (shader_recompiler.cpp: SV_Position, TEXCOORD0..15, COLOR0..1 and
// SV_ClipDistance, in that order), passed through unchanged otherwise.
// Compiled at build time to DXIL (CMakeLists.txt, sfr_embed_shader).
struct Interpolators {
    precise float4 oPos : SV_Position;
    float4 oTexCoord0 : TEXCOORD0;
    float4 oTexCoord1 : TEXCOORD1;
    float4 oTexCoord2 : TEXCOORD2;
    float4 oTexCoord3 : TEXCOORD3;
    float4 oTexCoord4 : TEXCOORD4;
    float4 oTexCoord5 : TEXCOORD5;
    float4 oTexCoord6 : TEXCOORD6;
    float4 oTexCoord7 : TEXCOORD7;
    float4 oTexCoord8 : TEXCOORD8;
    float4 oTexCoord9 : TEXCOORD9;
    float4 oTexCoord10 : TEXCOORD10;
    float4 oTexCoord11 : TEXCOORD11;
    float4 oTexCoord12 : TEXCOORD12;
    float4 oTexCoord13 : TEXCOORD13;
    float4 oTexCoord14 : TEXCOORD14;
    float4 oTexCoord15 : TEXCOORD15;
    float4 oColor0 : COLOR0;
    float4 oColor1 : COLOR1;
    float clipDistance : SV_ClipDistance;
};

Interpolators flipped(Interpolators vertex) {
    vertex.oPos.z = vertex.oPos.w - vertex.oPos.z;
    return vertex;
}

[maxvertexcount(3)]
void triangleMain(triangle Interpolators input[3], inout TriangleStream<Interpolators> output) {
    [unroll] for (uint i = 0; i < 3; ++i) output.Append(flipped(input[i]));
}

[maxvertexcount(2)]
void lineMain(line Interpolators input[2], inout LineStream<Interpolators> output) {
    [unroll] for (uint i = 0; i < 2; ++i) output.Append(flipped(input[i]));
}

[maxvertexcount(1)]
void pointMain(point Interpolators input[1], inout PointStream<Interpolators> output) {
    output.Append(flipped(input[0]));
}
