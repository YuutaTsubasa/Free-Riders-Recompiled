// native_raster_state_test: a triangle with the vertex output of a
// XenosRecomp vertex shader (src/shaders/depth_flip.hlsl), with depth and w
// different at each corner, and interpolators the pixel shader shows.
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

Interpolators vertexMain(uint id : SV_VertexID) {
    const float2 corners[3] = {float2(-1, -1), float2(-1, 3), float2(3, -1)};
    const float depths[3] = {0.125, 0.75, 0.4};
    const float ws[3] = {1.0, 2.0, 0.5};
    Interpolators output = (Interpolators)0;
    const float w = ws[id];
    output.oPos = float4(corners[id] * w, depths[id] * w, w);
    output.oTexCoord15 = float4(0, 1, 0, 0);
    output.oColor0 = float4(1, 0, 0, 1);
    output.oColor1 = float4(0, 0, 1, 1);
    output.clipDistance = 1;
    return output;
}

float4 pixelMain(Interpolators input) : SV_Target {
    return float4(input.oColor0.r, input.oTexCoord15.g, input.oColor1.b, 1);
}
