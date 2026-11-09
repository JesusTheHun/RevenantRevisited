// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *     tile.hlsl.h  - HLSL tile G-buffer fill (SM5 / D3D11 variant)      *
// *************************************************************************
//
// Windows / SOKOL_D3D11 port of tile.metal.h.
//
// *************************************************************************

#pragma once

inline constexpr const char* kTileVsHlsl = R"HLSL(
cbuffer params : register(b0) {
    float4 rect;
    float4 zparams;
    float4 tile_root;
    float4 tile_sprite;
    float4 filter_;
    float4 cb_obj_id;
    float4 camera;
    float4 proj;
    float4 tile_rect;
    float4 cam_tile;
    float4 debug;
    float4 raycast;
};
struct vs_in  { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
vs_out main_vs(vs_in i) {
    vs_out o;
    float sx = i.uv.x * tile_sprite.z - tile_sprite.x;
    float sy = i.uv.y * tile_sprite.w - tile_sprite.y;
    float lz = (-2.0 * sy * 0.867) / 2.003378;
    float su = 2.0 * (sy + lz * 0.867);
    float wx = tile_root.x + 0.5 * (su + sx);
    float wy = tile_root.y + 0.5 * (su - sx);
    float wz = tile_root.z + lz;
    float relx = wx - camera.x;
    float rely = wy - camera.y;
    float sum = relx + rely;
    float S = relx - rely;
    float T = 0.5 * sum - wz * 0.867;
    float scene_z_wu = camera.z - 0.867 * sum - 0.5 * wz;
    float zoom = max(zparams.w, 0.0001);
    float persp_scale = ((camera.w > 0.5) ? (camera.z / max(scene_z_wu, 1.0)) : 1.0) * zoom;
    float spx = rect.x + S * persp_scale;
    float spy = rect.y + T * persp_scale;
    o.pos = float4(2.0 * spx / max(rect.z, 1.0) - 1.0,
                   1.0 - 2.0 * spy / max(rect.w, 1.0),
                   0.0, 1.0);
    o.uv  = i.uv;
    return o;
}
)HLSL";

inline constexpr const char* kTileFsHlsl = R"HLSL(
#define ISO_COS30 0.867
#define ISO_WZ_DENOM 2.003378
#define EDGE_EPS 1e-6
cbuffer params : register(b0) {
    float4 rect;
    float4 zparams;
    float4 tile_root;
    float4 tile_sprite;
    float4 filter_;
    float4 cb_obj_id;
    float4 camera;
    float4 proj;
    float4 tile_rect;
    float4 cam_tile;
    float4 debug;
    float4 raycast;
};
Texture2D    color_tex : register(t0);
Texture2D    depth_tex : register(t1);
SamplerState smp       : register(s0);
struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
struct fs_out {
    float4 albedo  : SV_Target0;
    float4 normal  : SV_Target1;
    float4 scene_z : SV_Target2;
    float4 obj_id  : SV_Target3;
    float  depth   : SV_Depth;
};
float tile_depth_read(float2 uv) {
    uint w, h;
    depth_tex.GetDimensions(w, h);
    int2 p = int2(clamp(uv * float2(float(w), float(h)),
                         float2(0.0, 0.0),
                         float2(float(w - 1), float(h - 1))));
    return depth_tex.Load(int3(p, 0)).r;
}
fs_out main_ps(vs_out in_) {
    fs_out o;
    float4 c = color_tex.Sample(smp, in_.uv);
    float albedo_pad = max(raycast.z, 0.0);
    float2 gbuf_size = max(proj.zw, float2(1.0, 1.0));
    bool present_albedo =
        in_.pos.x >= albedo_pad && in_.pos.x < (gbuf_size.x - albedo_pad) &&
        in_.pos.y >= albedo_pad && in_.pos.y < (gbuf_size.y - albedo_pad);
    // Decoded alpha is coverage; flat sprites have valid zero local depth.
    if (c.a < 0.01) discard;
    float zraw = tile_depth_read(in_.uv);
    float d    = zparams.x + zraw * zparams.y;
    if (d < 0.0 || d > 1.0) discard;
    float nr  = max(filter_.x, 0.5);
    float thr = max(filter_.y, 0.0);
    float tw, th; depth_tex.GetDimensions(tw, th);
    float2 tex = float2(nr / max(tw, 1.0), nr / max(th, 1.0));
    float sx = in_.uv.x * tile_sprite.z - tile_sprite.x;
    float sy = in_.uv.y * tile_sprite.w - tile_sprite.y;
    float zl  = tile_depth_read(in_.uv - float2(tex.x, 0.0));
    float zrs = tile_depth_read(in_.uv + float2(tex.x, 0.0));
    float zt  = tile_depth_read(in_.uv - float2(0.0, tex.y));
    float zb  = tile_depth_read(in_.uv + float2(0.0, tex.y));
    if (abs(zl  - zraw) > thr + EDGE_EPS) zl  = zraw;
    if (abs(zrs - zraw) > thr + EDGE_EPS) zrs = zraw;
    if (abs(zt  - zraw) > thr + EDGE_EPS) zt  = zraw;
    if (abs(zb  - zraw) > thr + EDGE_EPS) zb  = zraw;
    float dzl = zl  * tile_root.w;
    float dzr = zrs * tile_root.w;
    float dzt = zt  * tile_root.w;
    float dzb = zb  * tile_root.w;
    float nrx = nr;
    float nry = nr;
    float lz_l = (-dzl - 2.0 * sy * ISO_COS30) / ISO_WZ_DENOM;
    float su_l = 2.0 * (sy + lz_l * ISO_COS30);
    float3 Wl = float3(tile_root.x + 0.5 * (su_l + (sx - nrx)),
                       tile_root.y + 0.5 * (su_l - (sx - nrx)),
                       tile_root.z + lz_l);
    float lz_r = (-dzr - 2.0 * sy * ISO_COS30) / ISO_WZ_DENOM;
    float su_r = 2.0 * (sy + lz_r * ISO_COS30);
    float3 Wr = float3(tile_root.x + 0.5 * (su_r + (sx + nrx)),
                       tile_root.y + 0.5 * (su_r - (sx + nrx)),
                       tile_root.z + lz_r);
    float lz_t = (-dzt - 2.0 * (sy - nry) * ISO_COS30) / ISO_WZ_DENOM;
    float su_t = 2.0 * ((sy - nry) + lz_t * ISO_COS30);
    float3 Wt = float3(tile_root.x + 0.5 * (su_t + sx),
                       tile_root.y + 0.5 * (su_t - sx),
                       tile_root.z + lz_t);
    float lz_b = (-dzb - 2.0 * (sy + nry) * ISO_COS30) / ISO_WZ_DENOM;
    float su_b = 2.0 * ((sy + nry) + lz_b * ISO_COS30);
    float3 Wb = float3(tile_root.x + 0.5 * (su_b + sx),
                       tile_root.y + 0.5 * (su_b - sx),
                       tile_root.z + lz_b);
    float3 t_u = Wr - Wl;
    float3 t_v = Wb - Wt;
    float3 N   = normalize(cross(t_v, t_u));
    float3 Vc  = normalize(float3(ISO_COS30, ISO_COS30, 1.0));
    if (dot(N, Vc) < 0.0) N = -N;
    if (!present_albedo) c = float4(0.0, 0.0, 0.0, 0.0);
    o.albedo  = c;
    o.normal  = float4(N * 0.5 + 0.5, 1.0);
    o.scene_z = float4(d, 0.0, 0.0, 1.0);
    o.obj_id  = cb_obj_id;
    o.depth   = d;
    return o;
}
)HLSL";
