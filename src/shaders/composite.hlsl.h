// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *     composite.hlsl.h  - HLSL composite quad (SM5 / D3D11 variant)     *
// *************************************************************************
//
// Windows / SOKOL_D3D11 port of composite.metal.h. Vertex entry: main_vs;
// pixel entry: main_ps (matches the sh.vs.entry / sh.fs.entry strings set
// in the D3D11 branch of renderer.cpp's pipeline init).
//
// *************************************************************************

#pragma once

inline constexpr const char* kCompositeVsHlsl = R"HLSL(
cbuffer params : register(b0) {
    float4 rect;
    float4 uv_rect;
    float4 chroma_key;
    float4 color_tint;
};
struct vs_in  { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
vs_out main_vs(vs_in i) {
    vs_out o;
    o.pos = float4(rect.xy + i.pos * rect.zw, 0.0, 1.0);
    o.uv  = uv_rect.xy + i.uv * uv_rect.zw;
    return o;
}
)HLSL";

inline constexpr const char* kCompositeFsHlsl = R"HLSL(
cbuffer params : register(b0) {
    float4 rect;
    float4 uv_rect;
    float4 chroma_key;
    float4 color_tint;
};
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 main_ps(vs_out i) : SV_Target0 {
    float4 c = tex.Sample(smp, i.uv);
    if (chroma_key.x > 0.5 && c.r > 0.55 && c.g < 0.30 && c.b < 0.30)
        discard;
    return c * color_tint;
}
)HLSL";

// The 2:1 reductions; see kCompositeReduceFsMetal.
inline constexpr const char* kCompositeReduceFsHlsl = R"HLSL(
cbuffer params : register(b0) {
    float4 rect;
    float4 uv_rect;
    float4 texel;
    float4 color_tint;
};
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 main_ps(vs_out i) : SV_Target0 {
    float2 h = 0.5 * texel.xy;
    float4 block[4] = { tex.Sample(smp, i.uv + float2(-h.x, -h.y)),
                        tex.Sample(smp, i.uv + float2( h.x, -h.y)),
                        tex.Sample(smp, i.uv + float2(-h.x,  h.y)),
                        tex.Sample(smp, i.uv + float2( h.x,  h.y)) };
    const float3 steps = float3(8.0, 4.0, 8.0);
    float3 mean;
    if (texel.z > 0.5)
    {
        float3 sum = float3(0.0, 0.0, 0.0);
        for (int k = 0; k < 4; ++k)
        {
            float3 c = block[k].a > 0.5 ? floor(round(block[k].rgb * 255.0) / steps) : float3(0.0, 0.0, 0.0);
            sum += float3(c.r, floor(c.g / 2.0), c.b);
        }
        mean = floor(sum / float3(4.0, 2.0, 4.0));
        if (all(mean == float3(0.0, 0.0, 0.0)))
            discard;
    }
    else
    {
        float3 sum = float3(0.0, 0.0, 0.0);
        float opaque = 0.0;
        for (int k = 0; k < 4; ++k)
            if (block[k].a > 0.5)
            {
                float3 c = floor(round(block[k].rgb * 255.0) / steps);
                sum += float3(c.r * 8.0 + floor(c.g / 8.0), c.g, c.b);
                opaque += 1.0;
            }
        if (opaque < 3.0)
            discard;
        mean = floor(sum / (opaque * float3(8.0, 1.0, 1.0)));
    }
    const float3 levels = float3(32.0, 64.0, 32.0);
    return float4((mean * steps + floor(mean / (levels / steps))) / 255.0, 1.0) * color_tint;
}
)HLSL";

// The coloured triangles; see kColorMeshVsMetal.
inline constexpr const char* kColorMeshVsHlsl = R"HLSL(
cbuffer params : register(b0) {
    float4 target;
};
struct vs_in  { float2 pos : POSITION; float4 color : COLOR0; };
struct vs_out { float4 pos : SV_Position; float4 color : COLOR0; };
vs_out main_vs(vs_in i) {
    vs_out o;
    o.pos = float4(i.pos.x * 2.0 / target.x - 1.0, 1.0 - i.pos.y * 2.0 / target.y, 0.0, 1.0);
    o.color = i.color;
    return o;
}
)HLSL";

inline constexpr const char* kColorMeshFsHlsl = R"HLSL(
struct vs_out { float4 pos : SV_Position; float4 color : COLOR0; };
float4 main_ps(vs_out i) : SV_Target0 {
    return i.color;
}
)HLSL";
